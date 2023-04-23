
#include "dev/rdma/hangu_rnic.hh"

// #include "debug/DescScheduler.hh"
#include "base/trace.hh"
#include "debug/HanGu.hh"

using namespace HanGuRnicDef;
using namespace Net;
using namespace std;

HanGuRnic::DescScheduler::DescScheduler(HanGuRnic *rNic, const std::string name):
    rNic(rNic),
    _name(name),
    totalWeight(0),
    qpStatusRspEvent([this]{qpStatusProc();}, name),
    wqePrefetchEvent([this]{wqePrefetch();}, name),
    getPrefetchQpnEvent([this]{wqePrefetchSchedule();}, name),
    launchWqeEvent([this]{launchWQE();}, name),
    updateEvent([this]{rxUpdate();}, name),
    createQpStatusEvent([this]{createQpStatus();}, name),
    qpcRspEvent([this]{qpcRspProc();}, name),
    wqeRspEvent([this]{wqeProc();}, name)
{
    // HANGU_PRINT(DescScheduler, "init\n");
}

/**
 * @note
 * Process QPC response triggered by doorbell processing. 
 * This function requests for QP status.
*/
void HanGuRnic::DescScheduler::qpcRspProc()
{
    CxtReqRspPtr qpc;
    assert(rNic->qpcModule.txQpAddrRspFifo.size());
    qpc = rNic->qpcModule.txQpAddrRspFifo.front();
    rNic->qpcModule.txQpAddrRspFifo.pop();
    HANGU_PRINT(DescScheduler, "QPC Response received by DescScheduler! QPN: %d, index: %d\n", qpc->num, qpc->idx);
    assert(rNic->doorbellVector[qpc->idx] != nullptr);
    DoorbellPtr db = rNic->doorbellVector[qpc->idx];
    rNic->df2ccuIdxFifo.push(qpc->idx);
    rNic->doorbellVector[qpc->idx] = nullptr;
    assert(db->qpn == qpc->txQpcRsp->srcQpn);

    QPStatusPtr qpStatus;
    if (qpStatusTable.find(db->qpn) == qpStatusTable.end())
    {
        panic("Cannot find qpn: %d\n", db->qpn);
    }
    qpStatus = qpStatusTable[db->qpn];
    std::pair<DoorbellPtr, QPStatusPtr> qpStatusDbPair(db, qpStatus);
    dbQpStatusRspQue.push(qpStatusDbPair);
    HANGU_PRINT(DescScheduler, "QP status doorbell pair sent to QP status proc! QP number: %d\n", qpStatus->qpn);
    if (!qpStatusRspEvent.scheduled())
    {
        rNic->schedule(qpStatusRspEvent, curTick() + rNic->clockPeriod());
    }
    if (rNic->qpcModule.txQpAddrRspFifo.size())
    {
        if (!qpcRspEvent.scheduled())
        {
            rNic->schedule(qpcRspEvent, curTick() + rNic->clockPeriod());
        }
    }
}

/**
 * @note
 * This method checks QP status and push WQE prefetch request into prefetch queue.
*/
void HanGuRnic::DescScheduler::qpStatusProc()
{
    assert(dbQpStatusRspQue.size());
    DoorbellPtr db = dbQpStatusRspQue.front().first;
    QPStatusPtr qpStatus = dbQpStatusRspQue.front().second;
    dbQpStatusRspQue.pop();
    assert(db->qpn == qpStatus->qpn);
    HANGU_PRINT(DescScheduler, "QP status doorbell pair received by QP status proc!\n");
    if (qpStatus->head_ptr == qpStatus->tail_ptr)
    {
        // If head pointer equals to tail pointer, this QP is absent in the prefetch queue,
        // so it should be pushed into prefetch queue.
        // TODO: high pirority queue is not supported yet!
        lowPriorityQpnQue.push(db->qpn);
        // totalWeight += qpStatus->weight;
        HANGU_PRINT(DescScheduler, "Unactive QP!\n");
    }
    qpStatus->head_ptr += db->num;

    // schedule WQE prefetch event
    if (!getPrefetchQpnEvent.scheduled())
    {
        rNic->schedule(getPrefetchQpnEvent, curTick() + rNic->clockPeriod());
    }

    // update QP status
    // WARNING: QP status update could lead to QP death
    qpStatusTable[qpStatus->qpn]->head_ptr = qpStatus->head_ptr;
    HANGU_PRINT(DescScheduler, "QP[%d] status updated!\n", qpStatus->qpn);

    if (dbQpStatusRspQue.size())
    {
        if (!qpStatusRspEvent.scheduled())
        {
            rNic->schedule(qpStatusRspEvent, curTick() + rNic->clockPeriod());
        }
    }
}

/**
 * @note
 * This method sends QP status read request to prefetch WQE. Scheduling policy is FIFO.
*/
void HanGuRnic::DescScheduler::wqePrefetchSchedule()
{
    bool allowNewWQE;
    bool allowNewHWQE;
    bool allowNewLWQE;
    assert(highPriorityQpnQue.size() || lowPriorityQpnQue.size());
    // assert(highPriorityQpnQue.size() <= WQE_BUFFER_CAPACITY && lowPriorityQpnQue.size() <= WQE_BUFFER_CAPACITY);
    allowNewHWQE = highPriorityDescQue.size() < WQE_BUFFER_CAPACITY;
    allowNewLWQE = lowPriorityDescQue.size() < WQE_BUFFER_CAPACITY;
    allowNewWQE = (allowNewHWQE && highPriorityQpnQue.size()) || (allowNewLWQE && lowPriorityQpnQue.size());

    if (!allowNewWQE)
    {
        HANGU_PRINT(DescScheduler, "New WQE not allowed!\n");
        return;
    }

    if (highPriorityQpnQue.size() > 0)
    {
        HANGU_PRINT(DescScheduler, "High priority QPN Queue size: %d!\n", highPriorityQpnQue.size());
        uint32_t qpn = highPriorityQpnQue.front();
        highPriorityQpnQue.pop();
        wqePrefetchQpStatusRReqQue.push(qpn);
        HANGU_PRINT(DescScheduler, "High priority QPN fetched! qpn: %d\n", qpn);
    }
    else if (lowPriorityQpnQue.size() < WQE_PREFETCH_THRESHOLD)
    {
        HANGU_PRINT(DescScheduler, "Low priority QPN Queue size: %d!\n", lowPriorityQpnQue.size());
        uint32_t qpn = lowPriorityQpnQue.front();
        lowPriorityQpnQue.pop();
        wqePrefetchQpStatusRReqQue.push(qpn);
        HANGU_PRINT(DescScheduler, "Low priority QPN fetched! qpn: %d\n", qpn);
    }

    if (!wqePrefetchEvent.scheduled())
    {
        rNic->schedule(wqePrefetchEvent, curTick() + rNic->clockPeriod());
    }
    if ((highPriorityQpnQue.size() || lowPriorityQpnQue.size()) && !getPrefetchQpnEvent.scheduled())
    {
        rNic->schedule(getPrefetchQpnEvent, curTick() + rNic->clockPeriod());
    }
}

/**
 * @note
 * Get QP status response and request for descriptors.
*/
void HanGuRnic::DescScheduler::wqePrefetch()
{
    assert(wqePrefetchQpStatusRReqQue.size());
    uint32_t qpn = wqePrefetchQpStatusRReqQue.front();
    wqePrefetchQpStatusRReqQue.pop();
    QPStatusPtr qpStatus = qpStatusTable[qpn];

    uint32_t descNum;
    HANGU_PRINT(DescScheduler, "head_ptr: 0x%x, fetch_ptr: 0x%x\n", qpStatus->head_ptr, qpStatus->fetch_ptr);
    HANGU_PRINT(DescScheduler, "QP num: %d, group ID: %d, weight: %d, group granularity: %d\n", 
        qpStatus->qpn, qpStatus->group_id, qpStatus->weight, groupTable[qpStatus->group_id]);
    if (qpStatus->head_ptr - qpStatus->fetch_ptr > MAX_PREFETCH_NUM)
    {
        descNum = MAX_PREFETCH_NUM;
    }
    else 
    {
        descNum = qpStatus->head_ptr - qpStatus->fetch_ptr;
    }

    // MrReqRspPtr descReq = make_shared<MrReqRsp>(DMA_TYPE_RREQ, MR_RCHNL_TX_DESC,
    //         qpStatus->key, descNum << 5, qpStatus->fetch_ptr);
    MrReqRspPtr descReq = make_shared<MrReqRsp>(DMA_TYPE_RREQ, MR_RCHNL_TX_DESC,
            qpStatus->key, descNum * sizeof(TxDesc), qpStatus->fetch_ptr);

    descReq->txDescRsp = new TxDesc[descNum];
    rNic->descReqFifo.push(descReq);
    HANGU_PRINT(DescScheduler, "WQE req sent! QPN: %d, WQE num: %d, req size: %d\n", qpStatus->qpn, descNum, descNum * sizeof(TxDesc));
    std::pair<uint32_t, QPStatusPtr> wqeFetchInfoPair(descNum, qpStatus);
    wqeFetchInfoQue.push(wqeFetchInfoPair);

    if (!rNic->mrRescModule.transReqEvent.scheduled()) { /* Schedule MrRescModule.transReqProcessing */
        rNic->schedule(rNic->mrRescModule.transReqEvent, curTick() + rNic->clockPeriod());
    }

    if (wqePrefetchQpStatusRReqQue.size())
    {
        if (!wqePrefetchEvent.scheduled())
        {
            rNic->schedule(wqePrefetchEvent, curTick() + rNic->clockPeriod());
        }
    }
}

/**
 * @note
 * Process WQE responses and produce sub WQEs. Do not use wnd_start after this stage.
*/
void HanGuRnic::DescScheduler::wqeProc()
{
    assert(rNic->txdescRspFifo.size());
    uint32_t descNum = wqeFetchInfoQue.front().first;
    QPStatusPtr qpStatus = wqeFetchInfoQue.front().second;
    wqeFetchInfoQue.pop();
    TxDescPtr desc;
    uint8_t procDescNum = 0;

    HANGU_PRINT(DescScheduler, "WQE processing begin! QPN: %d, type: %d\n", qpStatus->qpn, qpStatus->type);

    // check QP type
    if (qpStatus->type == LAT_QP)
    {
        // For latency sensitive QP, commit all WQEs
        for (int i = 0; i < descNum; i++)
        {
            desc = rNic->txdescRspFifo.front();
            rNic->txdescRspFifo.pop();
            highPriorityDescQue.push(desc);
        }
        qpStatus->fetch_ptr += descNum;
    }
    else if (qpStatus->type == RATE_QP)
    {
        // For message rate sensitive QP, commit up to 8 WQEs
        int fetchNum;
        if (descNum > MAX_PREFETCH_NUM)
        {
            fetchNum = MAX_PREFETCH_NUM;
        }
        else
        {
            fetchNum = descNum;
        }
        for (int i = 0; i < fetchNum; i++)
        {
            desc = rNic->txdescRspFifo.front();
            rNic->txdescRspFifo.pop();
            lowPriorityDescQue.push(desc);
        }
        qpStatus->fetch_ptr += descNum;
    }
    else if (qpStatus->type == BW_QP)
    {
        assert(descNum >= 1);
        assert(qpStatus->wnd_start <= qpStatus->wnd_fetch);
        assert(qpStatus->wnd_fetch < qpStatus->wnd_end);
        
        // set wnd_end to the size of the whole message
        if (qpStatus->wnd_start == 0)
        {
            qpStatus->wnd_end = rNic->txdescRspFifo.front()->len;
        }

        TxDescPtr desc = rNic->txdescRspFifo.front();
        HANGU_PRINT(DescScheduler, "BW desc received by wqe proc! qpn: %d\n", qpStatus->qpn);

        uint32_t batchSize; // the size of data that should be transmitted in this period
        batchSize = qpStatus->weight * groupTable[qpStatus->group_id];
        uint32_t procSize = 0;

        while(procSize < batchSize)
        {
            TxDescPtr subDesc = make_shared<TxDesc>(desc);
            subDesc->opcode = desc->opcode;
            // subDesc->qpn = qpStatus->qpn;
            // WARNING: SEND/RECV are currently not supported!
            subDesc->lVaddr = desc->lVaddr + qpStatus->fetch_offset;
            subDesc->rdmaType.rVaddr_l = desc->rdmaType.rVaddr_l + qpStatus->fetch_offset;
            if (desc->len - qpStatus->fetch_offset > MAX_COMMIT_SZ)
            {
                subDesc->len = MAX_COMMIT_SZ;
            }
            else
            {
                subDesc->len = desc->len - qpStatus->fetch_offset;
            }

            // If this subDesc doesn't finish the whole message, don't generate CQE,
            // or otherwise generate CQE, and switch to the next descriptor
            if (qpStatus->fetch_offset + subDesc->len >= desc->len)
            {
                subDesc->flags = 1;
                rNic->txdescRspFifo.pop();
                qpStatus->fetch_offset = 0;
                // If there are still descriptors left , switch to the next descriptor
                if (procDescNum + 1 < descNum)
                {
                    desc = rNic->txdescRspFifo.front();
                    lowPriorityDescQue.push(subDesc);
                }
                else 
                {
                    lowPriorityDescQue.push(subDesc);
                    break;
                }
                procDescNum++;
            }
            else
            {
                subDesc->flags = 0;
                qpStatus->fetch_offset += subDesc->len;
                lowPriorityDescQue.push(subDesc);
            }
            procSize += subDesc->len;
        }
    }
    else
    {
        panic("Illegal QP type!\n");
    }

    DoorbellPtr doorbell = make_shared<DoorbellFifo>(procDescNum, qpStatus->qpn);
    wqeProcToLaunchWqeQue.push(doorbell);
    // HANGU_PRINT("pseudo doorbell pushed into queue, QPN: %d, num: %d\n", qpStatus->qpn, procDescNum);
    
    if (!launchWqeEvent.scheduled())
    {
        rNic->schedule(launchWqeEvent, curTick() + rNic->clockPeriod());
    }
    if (rNic->txdescRspFifo.size())
    {
        if (!wqeRspEvent.scheduled())
        {
            rNic->schedule(wqeRspEvent, curTick() + rNic->clockPeriod());
        }
    }
}

/**
 * @note: send sub-wqe to DDU in RDMA engine
*/
void HanGuRnic::DescScheduler::launchWQE()
{
    assert(lowPriorityDescQue.size() || highPriorityDescQue.size());
    if (highPriorityDescQue.size())
    {
        rNic->txDescLaunchQue.push(highPriorityDescQue.front());
        highPriorityDescQue.pop();
    }
    else
    {
        rNic->txDescLaunchQue.push(lowPriorityDescQue.front());
        lowPriorityDescQue.pop();
    }

    DoorbellPtr doorbell = wqeProcToLaunchWqeQue.front();
    rNic->rdmaEngine.df2ddFifo.push(doorbell); 
    HANGU_PRINT(DescScheduler, "pseudo doorbell pushed into queue, QPN: %d, num: %d\n", doorbell->qpn, doorbell->num);
    wqeProcToLaunchWqeQue.pop();

    if (!rNic->rdmaEngine.dduEvent.scheduled())
    {
        rNic->schedule(rNic->rdmaEngine.dduEvent, curTick() + rNic->clockPeriod());
    }
    if (!launchWqeEvent.scheduled() && (lowPriorityDescQue.size() || highPriorityDescQue.size()))
    {
        rNic->schedule(launchWqeEvent, curTick() + rNic->clockPeriod());
    }
}

/**
 * @note
 * Update win_fetch and tail_ptr in QP status. Maybe tail_ptr updating should be implemented in 
 * CQ processing.
*/
void HanGuRnic::DescScheduler::rxUpdate()
{
    assert(rNic->updateQue.size());
    uint32_t qpn = rNic->updateQue.front().first;
    uint32_t len = rNic->updateQue.front().second;
    rNic->updateQue.pop();
    QPStatusPtr status = qpStatusTable[qpn];
    HANGU_PRINT(DescScheduler, "rx received! tail_ptr: 0x%x\n", status->tail_ptr);
    if (status->type == LAT_QP || status->type == RATE_QP)
    {
        status->tail_ptr++;
    }
    else if (status->type == BW_QP)
    {
        assert(status->wnd_fetch + len <= status->wnd_end);
        status->wnd_fetch += len;
        if (status->wnd_fetch >= status->wnd_end)
        {
            if (status->tail_ptr != status->head_ptr)
            {
                status->tail_ptr++;
            }
        }
        if (status->tail_ptr < status->head_ptr)
        {
            lowPriorityQpnQue.push(status->qpn);
        }
        else if (status->tail_ptr == status->head_ptr)
        {
        }
        else
        {
            panic("tail pointer exceeds head pointer! qpn: %d", status->qpn);
        }
    }
    
    if (rNic->updateQue.size())
    {
        if (!updateEvent.scheduled())
        {
            rNic->schedule(updateEvent, curTick() + rNic->clockPeriod());
        }
    }
}

void HanGuRnic::DescScheduler::createQpStatus()
{
    assert(rNic->createQue.size());
    QPStatusPtr status = rNic->createQue.front();
    rNic->createQue.pop();
    qpStatusTable[status->qpn] = status;
    HANGU_PRINT(DescScheduler, "new QP created! type: %d\n", status->type);
    assert(status->type == LAT_QP || status->type == BW_QP || status->type == RATE_QP);
    if (rNic->createQue.size())
    {
        rNic->schedule(createQpStatusEvent, curTick() + rNic->clockPeriod());
    }
}