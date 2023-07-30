
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
    HANGU_PRINT(DescScheduler, "QPC Response received by DescScheduler! QPN: 0x%x, index: %d\n", qpc->num, qpc->idx);
    assert(rNic->doorbellVector[qpc->idx] != nullptr);
    DoorbellPtr db = rNic->doorbellVector[qpc->idx];
    rNic->df2ccuIdxFifo.push(qpc->idx);
    rNic->doorbellVector[qpc->idx] = nullptr;
    assert(db->qpn == qpc->txQpcRsp->srcQpn);

    sqSize = pow(2, qpc->txQpcRsp->sqSizeLog);

    QPStatusPtr qpStatus;
    if (qpStatusTable.find(db->qpn) == qpStatusTable.end())
    {
        panic("Cannot find qpn: %d\n", db->qpn);
    }
    qpStatus = qpStatusTable[db->qpn];
    std::pair<DoorbellPtr, QPStatusPtr> qpStatusDbPair(db, qpStatus);
    dbQpStatusRspQue.push(qpStatusDbPair);
    HANGU_PRINT(DescScheduler, "QP status doorbell pair sent to QP status proc! QP number: 0x%x, desc num: %d\n", qpStatus->qpn, db->num);
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
 * This function checks QP status and push WQE prefetch request into prefetch QPN queue.
*/
void HanGuRnic::DescScheduler::qpStatusProc()
{
    assert(dbQpStatusRspQue.size());
    DoorbellPtr db = dbQpStatusRspQue.front().first;
    QPStatusPtr qpStatus = dbQpStatusRspQue.front().second;
    bool schedule = false;
    dbQpStatusRspQue.pop();
    // delete this line in the future
    assert(db->qpn == qpStatus->qpn);
    assert(qpStatus->type == BW_QP || qpStatus->type == UD_QP);
    HANGU_PRINT(DescScheduler, "Before updating head. qpn: 0x%x, head: %d, tail:  %d\n", 
        qpStatus->qpn, qpStatus->head_ptr, qpStatus->tail_ptr);
    if (qpStatus->head_ptr == qpStatus->tail_ptr) // WARNING: consider corner case!
    {
        // If head pointer equals to tail pointer, this QP is absent in the prefetch queue,
        // so it should be pushed into prefetch queue.
        // TODO: high pirority queue is not supported yet!
        lowPriorityQpnQue.push(db->qpn);
        // totalWeight += qpStatus->weight;
        schedule = true;
        qpStatus->in_que++;
        HANGU_PRINT(DescScheduler, "Inactive QP! qpn: 0x%x, in que: %d\n", db->qpn, qpStatus->in_que);
    }
    else
    {
        HANGU_PRINT(DescScheduler, "Active QP! Do not push QPN into QPN queue! qpn: 0x%x\n", db->qpn);
    }
    qpStatus->head_ptr += db->num;

    // If this QP has no unfinished WQE, schedule WQE prefetch event
    if (!getPrefetchQpnEvent.scheduled() && schedule)
    {
        rNic->schedule(getPrefetchQpnEvent, curTick() + rNic->clockPeriod());
    }

    // update QP status
    // WARNING: QP status update could lead to QP death
    qpStatusTable[qpStatus->qpn]->head_ptr = qpStatus->head_ptr;
    HANGU_PRINT(DescScheduler, "QP[0x%x] status updated!\n", qpStatus->qpn);

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
 * Pop QPN from QPN queue to prefetch WQE. Scheduling policy is FIFO.
*/
void HanGuRnic::DescScheduler::wqePrefetchSchedule()
{
    bool allowNewWQE;
    bool allowNewHWQE;
    bool allowNewLWQE;
    assert(highPriorityQpnQue.size() || lowPriorityQpnQue.size());
    allowNewHWQE = highPriorityDescQue.size() < WQE_BUFFER_CAPACITY;
    allowNewLWQE = lowPriorityDescQue.size() < WQE_BUFFER_CAPACITY;
    allowNewWQE = (allowNewHWQE && highPriorityQpnQue.size()) || (allowNewLWQE && (lowPriorityQpnQue.size()));

    if (!allowNewWQE)
    {
        HANGU_PRINT(DescScheduler, "New WQE not allowed! lowPriorityDescQue size: %d, lowPriorityQpnQue size: %d\n", 
            lowPriorityDescQue.size(), lowPriorityQpnQue.size());
        return;
    }

    if (highPriorityQpnQue.size() > 0)
    {
        HANGU_PRINT(DescScheduler, "High priority QPN Queue size: %d!\n", highPriorityQpnQue.size());
        uint32_t qpn = highPriorityQpnQue.front();
        highPriorityQpnQue.pop();
        wqePrefetchQpStatusRReqQue.push(qpn);
        HANGU_PRINT(DescScheduler, "High priority QPN fetched! qpn: 0x%x\n", qpn);
    }
    else if (lowPriorityQpnQue.size() > 0)
    {
        if (lowPriorityQpnQue.size() < WQE_PREFETCH_THRESHOLD)
        {
            HANGU_PRINT(DescScheduler, "Low priority QPN queue size: %d!\n", lowPriorityQpnQue.size());
            uint32_t qpn = lowPriorityQpnQue.front();
            lowPriorityQpnQue.pop();
            wqePrefetchQpStatusRReqQue.push(qpn);
            qpStatusTable[qpn]->in_que--;
            qpStatusTable[qpn]->fetch_count++;
            HANGU_PRINT(DescScheduler, "Low priority QPN fetched! qpn: 0x%x, in que: %d, fetch count: %ld\n", 
                qpn, qpStatusTable[qpn]->in_que, qpStatusTable[qpn]->fetch_count);
        }
    }

    if (lowPriorityQpnQue.size() == 0 && highPriorityQpnQue.size() == 0)
    {
        HANGU_PRINT(DescScheduler, "Empty QPN queue!\n");
    }

    if (!wqePrefetchEvent.scheduled())
    {
        rNic->schedule(wqePrefetchEvent, curTick() + rNic->clockPeriod());
    }
    if ((highPriorityQpnQue.size() || lowPriorityQpnQue.size() || leastPriorityQpnQue.size()) 
        && !getPrefetchQpnEvent.scheduled())
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
    // HANGU_PRINT(DescScheduler, "wqePrefetch in!\n");
    assert(wqePrefetchQpStatusRReqQue.size());
    uint32_t qpn = wqePrefetchQpStatusRReqQue.front();
    wqePrefetchQpStatusRReqQue.pop();
    QPStatusPtr qpStatus = qpStatusTable[qpn];

    if (qpStatus->fetch_lock == 0)
    {
        uint32_t descNum;
        HANGU_PRINT(DescScheduler, "wqe prefetch! qpn: 0x%x, head_ptr: 0x%x, tail pointer: 0x%x, fetch offset: 0x%x\n", 
            qpStatus->qpn, qpStatus->head_ptr, qpStatus->tail_ptr, qpStatus->fetch_offset);
        HANGU_PRINT(DescScheduler, "QP num: 0x%x, type: %d, group ID: %d, weight: %d, group granularity: %d\n", 
            qpStatus->qpn, qpStatus->type, qpStatus->group_id, qpStatus->weight, groupTable[qpStatus->group_id]);
        
        if (qpStatus->head_ptr - qpStatus->tail_ptr > MAX_PREFETCH_NUM)
        {
            descNum = MAX_PREFETCH_NUM;
        }
        else 
        {
            descNum = qpStatus->head_ptr - qpStatus->tail_ptr;
        }

        // In case of going back in circular queue
        uint32_t tailOffset = qpStatus->tail_ptr % (sqSize / sizeof(TxDesc));
        if (descNum + tailOffset > sqSize / sizeof(TxDesc))
        {
            HANGU_PRINT(DescScheduler, "Loopback Send Queue! descNum: %d, tail pointer: %d, tailOffset: %d, sqSize: %d, size of desc: %d\n", 
                descNum, qpStatus->tail_ptr, tailOffset, sqSize, sizeof(TxDesc));
            descNum = sqSize / sizeof(TxDesc) - tailOffset;
            assert(tailOffset + descNum <= sqSize / sizeof(TxDesc));
        }

        if (descNum != 0)
        {
            MrReqRspPtr descReq = make_shared<MrReqRsp>(DMA_TYPE_RREQ, MR_RCHNL_TX_DESC,
                qpStatus->key, descNum * sizeof(TxDesc), qpStatus->tail_ptr * sizeof(TxDesc) % sqSize);

            descReq->txDescRsp = new TxDesc[descNum];
            rNic->descReqFifo.push(descReq);
            HANGU_PRINT(DescScheduler, "WQE req sent! QPN: %d, WQE num: %d, req size: %d, tail ptr: %d\n", 
                qpStatus->qpn, descNum, descNum * sizeof(TxDesc), qpStatus->tail_ptr);
            std::pair<uint32_t, QPStatusPtr> wqeFetchInfoPair(descNum, qpStatus);
            wqeFetchInfoQue.push(wqeFetchInfoPair);
            if (!rNic->mrRescModule.transReqEvent.scheduled()) { /* Schedule MrRescModule.transReqProcessing */
                rNic->schedule(rNic->mrRescModule.transReqEvent, curTick() + rNic->clockPeriod());
            }
        }
        else 
        {
            HANGU_PRINT(DescScheduler, "useless least-priority qpn! qpn: %d\n", qpStatus->qpn);
        }
        qpStatus->fetch_lock = 1;
    }
    else
    {
        HANGU_PRINT(DescScheduler, "Fetch locked! QPN: %d\n", qpStatus->qpn);
    }

    if (wqePrefetchQpStatusRReqQue.size() && !wqePrefetchEvent.scheduled())
    {
        rNic->schedule(wqePrefetchEvent, curTick() + rNic->clockPeriod());
    }
}

/**
 * @note
 * Process WQE responses and produce sub WQEs. Do not use wnd_start after this stage.
*/
void HanGuRnic::DescScheduler::wqeProc()
{
    // HANGU_PRINT(DescScheduler, "wqeProc in!\n");
    assert(rNic->txdescRspFifo.size());
    uint32_t descNum = wqeFetchInfoQue.front().first;
    QPStatusPtr qpStatus = wqeFetchInfoQue.front().second;
    wqeFetchInfoQue.pop();
    TxDescPtr desc;
    uint8_t subDescNum = 0;

    HANGU_PRINT(DescScheduler, "WQE processing begin! QPN: 0x%x, type: %d, group: %d, QP weight: %d, group granularity: %d\n", 
        qpStatus->qpn, qpStatus->type, qpStatus->group_id, qpStatus->weight, groupTable[qpStatus->group_id]);

    assert(qpStatus->head_ptr >= qpStatus->tail_ptr);
    if (qpStatus->head_ptr == qpStatus->tail_ptr)
    {
        HANGU_PRINT(DescScheduler, "invalid WQE! QPN: %d, head_ptr: 0x%x, tail_ptr: 0x%x\n", 
            qpStatus->qpn, qpStatus->head_ptr, qpStatus->tail_ptr);
        for (int i = 0; i < descNum; i++)
        {
            rNic->txdescRspFifo.pop();
        }
        if (rNic->txdescRspFifo.size() && !wqeRspEvent.scheduled())
        {
            rNic->schedule(wqeRspEvent, curTick() + rNic->clockPeriod());
        }
        return;
    }

    // check QP type
    if (qpStatus->type == LAT_QP)// warning: modify here
    {
        // For latency sensitive QP, commit all WQEs
        assert(0);
        for (int i = 0; i < descNum; i++)
        {
            desc = rNic->txdescRspFifo.front();
            rNic->txdescRspFifo.pop();
            highPriorityDescQue.push(desc);
        }
        qpStatus->tail_ptr += descNum;
    }
    else if (qpStatus->type == RATE_QP) // WARNING: modify here
    {
        assert(0);
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
        qpStatus->tail_ptr += descNum;
    }
    else if (qpStatus->type == BW_QP || qpStatus->type == UD_QP)
    {
        assert(descNum >= 1);
        uint32_t procSize = 0; // data size been processed in this schedule period
        uint32_t batchSize; // the size of data that should be transmitted in this schedule period
        batchSize = qpStatus->weight * groupTable[qpStatus->group_id];
        assert(batchSize > 0);
        for (int i = 0; i < descNum; i++)
        {
            HANGU_PRINT(DescScheduler, "new BW/UD desc received by wqe proc! qpn: 0x%x\n", qpStatus->qpn);
            assert(rNic->txdescRspFifo.size());
            if (procSize < batchSize)
            {
                TxDescPtr desc = rNic->txdescRspFifo.front();
                HANGU_PRINT(DescScheduler, "WQE splitting! qpn: 0x%x, fetch offset: %d, current desc len: %d, batch size: %d, descNum: %d, group granularity: %d\n", 
                    qpStatus->qpn, qpStatus->fetch_offset, desc->len, batchSize, descNum, groupTable[qpStatus->group_id]);
                assert(qpStatus->fetch_offset < desc->len);
                HANGU_PRINT(DescScheduler, "ready to split WQE! qpn: 0x%x, tail pointer: %d, head pointer: %d, fetch offset: 0x%x\n", 
                    qpStatus->qpn, qpStatus->tail_ptr, qpStatus->head_ptr, qpStatus->fetch_offset);
                assert(qpStatus->tail_ptr < qpStatus->head_ptr);

                TxDescPtr subDesc = make_shared<TxDesc>(desc);
                subDesc->opcode = desc->opcode;
                // WARNING: SEND/RECV are currently not supported!
                subDesc->lVaddr = desc->lVaddr + qpStatus->fetch_offset;
                subDesc->rdmaType.rVaddr_l = desc->rdmaType.rVaddr_l + qpStatus->fetch_offset;
                // set submessage length
                if (desc->len - qpStatus->fetch_offset > batchSize - procSize)
                {
                    subDesc->len = batchSize - procSize;
                }
                else
                {
                    subDesc->len = desc->len - qpStatus->fetch_offset;
                }

                // If this subDesc doesn't finish the whole message, don't generate CQE,
                // or otherwise generate CQE, and switch to the next descriptor
                if (qpStatus->fetch_offset + subDesc->len >= desc->len)
                {
                    // update tail pointer
                    HANGU_PRINT(DescScheduler, "qpn: 0x%x, tail pointer to update: %d, head pointer: %d\n", qpStatus->qpn, qpStatus->tail_ptr, qpStatus->head_ptr);
                    assert(qpStatus->tail_ptr < qpStatus->head_ptr);
                    qpStatus->tail_ptr++;
                    // if the original WQE is signaled, signal the sub WQE
                    if (desc->isSignaled())
                    {
                        subDesc->setCompleteSignal();
                        HANGU_PRINT(DescScheduler, "Signal the sub desc! QPN: 0x%x, flag: 0x%x\n", qpStatus->qpn, subDesc->flags);
                    }
                    else
                    {
                        HANGU_PRINT(DescScheduler, "Do not signal the sub desc because WQE is unsignaled! QPN: 0x%x, flag: 0x%x\n", qpStatus->qpn, subDesc->flags);
                    }
                    qpStatus->fetch_offset = 0;
                }
                else
                {
                    subDesc->cancelCompleteSignal();
                    qpStatus->fetch_offset += subDesc->len;
                    HANGU_PRINT(DescScheduler, "Do not signal the sub desc! QPN: 0x%x, flag: 0x%x\n", qpStatus->qpn, subDesc->flags);
                }
                procSize += subDesc->len;
                HANGU_PRINT(DescScheduler, "finish WQE split: type: %d, sub WQE length: %d, qpn: 0x%x, descNum: %d, sub WQE flag: 0x%x\n", 
                    qpStatus->type, subDesc->len, qpStatus->qpn, descNum, subDesc->flags);

                // if this is the last sub WQE in this period, mark it as prefetch queue update
                if (procSize >= batchSize || i + 1 >= descNum)
                {
                    subDesc->setQueUpdate();
                }

                lowPriorityDescQue.push(subDesc);
                subDescNum++;
            }
            rNic->txdescRspFifo.pop();
        }
        // if (qpStatus->tail_ptr != qpStatus->head_ptr)
        // {
        //     while (leastPriorityQpnQue.size() >= LEAST_QPN_QUE_CAP)
        //     {
        //         leastPriorityQpnQue.pop();
        //     }
        //     leastPriorityQpnQue.push(qpStatus->qpn);
        //     qpStatus->in_least_que = 1;
        //     HANGU_PRINT(DescScheduler, "push back QPN into least priority queue! qpn: %d\n", qpStatus->qpn);
        //     if (!getPrefetchQpnEvent.scheduled())
        //     {
        //         rNic->schedule(getPrefetchQpnEvent, curTick() + rNic->clockPeriod());
        //     }
        // }
        if (qpStatus->tail_ptr != qpStatus->head_ptr)
        {
            lowPriorityQpnQue.push(qpStatus->qpn);
            qpStatus->in_que++;
            HANGU_PRINT(DescScheduler, "push back qpn into low qpn queue, qpn: 0x%x, in_que: %d\n", qpStatus->qpn, qpStatus->in_que);
            assert(qpStatus->in_que == 1);
            // assert(qpStatus->in_que == 0);
            // qpStatus->in_least_que = 1;
            // HANGU_PRINT(DescScheduler, "push back QPN into low priority queue! qpn: %d\n", qpStatus->qpn);
            if (!getPrefetchQpnEvent.scheduled())
            {
                rNic->schedule(getPrefetchQpnEvent, curTick() + rNic->clockPeriod());
            }
        }
        else
        {
            HANGU_PRINT(DescScheduler, "qp[0x%x] is idle! in_que: %d\n", qpStatus->qpn, qpStatus->in_que);
            assert(qpStatus->in_que == 0);
        }
    }
    else
    {
        panic("Illegal QP type!\n");
    }

    // write QPN back to low QPN queue in case of UD and UC QP
    // TODO: check here
    if ((qpStatus->type == UD_QP || qpStatus->type == UC_QP) && (qpStatus->tail_ptr < qpStatus->head_ptr))
    {
        lowPriorityQpnQue.push(qpStatus->qpn);
        HANGU_PRINT(DescScheduler, "UD or UC QP prefetch: type: %d\n", qpStatus->type);
        if (!getPrefetchQpnEvent.scheduled())
        {
            rNic->schedule(getPrefetchQpnEvent, curTick() + rNic->clockPeriod());
        }
    }

    // unlock WQE fetching
    assert(qpStatus->fetch_lock == 1);
    qpStatus->fetch_lock = 0;

    DoorbellPtr doorbell = make_shared<DoorbellFifo>(subDescNum, qpStatus->qpn, qpStatus->type);
    wqeProcToLaunchWqeQue.push(doorbell);
    HANGU_PRINT(DescScheduler, "pseudo doorbell pushed into queue to launchWQE, QPN: 0x%x, num: %d\n", qpStatus->qpn, subDescNum);
    
    if (!launchWqeEvent.scheduled())
    {
        rNic->schedule(launchWqeEvent, curTick() + rNic->clockPeriod());
    }
    if (rNic->txdescRspFifo.size() && !wqeRspEvent.scheduled())
    {
        rNic->schedule(wqeRspEvent, curTick() + rNic->clockPeriod());
    }
    // HANGU_PRINT(DescScheduler, "wqeProc out!\n");
}

/**
 * @note: send sub-wqe to DDU in RDMA engine
*/
void HanGuRnic::DescScheduler::launchWQE()
{
    // HANGU_PRINT(DescScheduler, "launchWQE in!\n");

    // get pseudo doorbell
    DoorbellPtr doorbell = wqeProcToLaunchWqeQue.front();
    rNic->rdmaEngine.df2ddFifo.push(doorbell); 
    HANGU_PRINT(DescScheduler, "pseudo doorbell get by launchWQE, QPN: 0x%x, num: %d, type: %d, desc launch queue size: %d\n", 
        doorbell->qpn, doorbell->num, doorbell->opcode, rNic->txDescLaunchQue.size());
    wqeProcToLaunchWqeQue.pop();

    if (doorbell->opcode == BW_QP || doorbell->opcode == UC_QP || doorbell->opcode == UD_QP)
    {
        // assert(lowPriorityDescQue.size() == doorbell->num); // WHY?
        assert(lowPriorityDescQue.size() >= doorbell->num);
        assert(doorbell->num != 0);
        for (int i = 0; i < doorbell->num; i++)
        {
            rNic->txDescLaunchQue.push(lowPriorityDescQue.front());
            lowPriorityDescQue.pop();
        }
    }
    else
    {
        assert(highPriorityDescQue.size() == doorbell->num);
        assert(doorbell->num != 0);
        for (int i = 0; i < doorbell->num; i++)
        {
            rNic->txDescLaunchQue.push(highPriorityDescQue.front());
            highPriorityDescQue.pop();
        }
    }

    if (!rNic->rdmaEngine.dduEvent.scheduled())
    {
        rNic->schedule(rNic->rdmaEngine.dduEvent, curTick() + rNic->clockPeriod());
    }
    if (!launchWqeEvent.scheduled() && wqeProcToLaunchWqeQue.size())
    {
        rNic->schedule(launchWqeEvent, curTick() + rNic->clockPeriod());
    }
    // HANGU_PRINT(DescScheduler, "launch WQE out!\n");
}

/**
 * @note
 * Update win_fetch and tail_ptr in QP status. Add QPN back to QPN queue in case of RC QP
*/
void HanGuRnic::DescScheduler::rxUpdate()
{
    // HANGU_PRINT(DescScheduler, "rxUpdate in!\n");
    assert(rNic->updateQue.size());
    uint32_t qpn = rNic->updateQue.front().first;
    uint32_t len = rNic->updateQue.front().second;
    bool schedule = false; 
    rNic->updateQue.pop();
    QPStatusPtr status = qpStatusTable[qpn];
    HANGU_PRINT(DescScheduler, "rx received! qpn: %d, len: %d\n", qpn, len);
    HANGU_PRINT(DescScheduler, "rx received! type: %d, head_ptr: 0x%x, tail_ptr: 0x%x\n", status->type, status->head_ptr, status->tail_ptr);
    assert(status->type == LAT_QP || status->type == RATE_QP || status->type == BW_QP);
    if (status->type == LAT_QP || status->type == RATE_QP)
    {
        status->tail_ptr++;
        if (status->head_ptr > status->tail_ptr)
        {
            // TODO: push back QPN
            schedule = true;
        }
    }
    else if (status->type == BW_QP)
    {
        // assert(status->fetch_offset + len <= status->wnd_end);
        if (status->tail_ptr < status->head_ptr)
        {
            // lowPriorityQpnQue.push(status->qpn);
            // schedule = true;
            schedule = false;
        }
        else if (status->tail_ptr == status->head_ptr)
        {
            HANGU_PRINT(DescScheduler, "do not push back QPN\n");
        }
        else
        {
            panic("tail pointer exceeds head pointer! qpn: %d", status->qpn);
        }
    }

    if (!getPrefetchQpnEvent.scheduled() && schedule)
    {
        rNic->schedule(getPrefetchQpnEvent, curTick() + rNic->clockPeriod());
    }
    
    if (rNic->updateQue.size() && !updateEvent.scheduled())
    {
        rNic->schedule(updateEvent, curTick() + rNic->clockPeriod());
    }
    // HANGU_PRINT(DescScheduler, "rxUpdate out!\n");
}

void HanGuRnic::DescScheduler::createQpStatus()
{
    assert(rNic->createQue.size());
    QPStatusPtr status = rNic->createQue.front();
    rNic->createQue.pop();
    qpStatusTable[status->qpn] = status;
    HANGU_PRINT(DescScheduler, "new QP created! type: %d\n", status->type);
    assert(status->type == LAT_QP   || 
           status->type == BW_QP    || 
           status->type == RATE_QP  || 
           status->type == UC_QP    || 
           status->type == UD_QP);
    if (rNic->createQue.size())
    {
        rNic->schedule(createQpStatusEvent, curTick() + rNic->clockPeriod());
    }
}