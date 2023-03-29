
#include "dev/rdma/hangu_rnic.hh"

#include <algorithm>
#include <memory>
#include <queue>

#include "base/inet.hh"
#include "base/trace.hh"
#include "base/random.hh"
#include "debug/Drain.hh"
#include "dev/net/etherpkt.hh"
#include "debug/HanGu.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "params/HanGuRnic.hh"
#include "sim/stats.hh"
#include "sim/system.hh"

using namespace HanGuRnicDef;
using namespace Net;
using namespace std;

HanGuRnic::DescScheduler::DescScheduler(HanGuRnic *rNic, std::string name):
    rNic(rNic),
    qpcRspEvent([this]{qpcRspProc();}, name),
    qpStatusRspEvent([this]{qpStatusProc();}, name),
    wqeRspEvent([this]{wqeProc();}, name),
    // rxUpdateEvent([this]{rxUpdate();}, name),
    // qpStatusReqEvent([this]{qpStatusReqProc();}, name),
    wqePrefetchEvent([this]{wqePrefetch();}, name),
    getPrefetchQpnEvent([this]{wqePrefetchSchedule();}, name),
    launchWqeEvent([this]{launchWQE();}, name),
    updateEvent([this]{rxUpdate();}, name),
    createQpStatusEvent([this]{createQpStatus();}, name)
{

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
    if (qpStatus->head_ptr == qpStatus->tail_ptr)
    {
        // If head pointer equals to tail pointer, this QP is absent in the prefetch queue,
        // so it should be pushed into prefetch queue and totoal weight should be updated.
        lowPriorityQpnQue.push(db->qpn);
        totalWeight += qpStatus->weight;
    }
    qpStatus->head_ptr += db->num;

    // schedule WQE prefetch event
    if (!getPrefetchQpnEvent.scheduled())
    {
        rNic->schedule(getPrefetchQpnEvent, curTick() + rNic->clockPeriod());
    }

    // update QP status
    // WARNING: QP status update could lead to QP death
    qpStatusTable[db->num]->head_ptr = qpStatus->head_ptr;

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
    allowNewHWQE = highPriorityDescQue.size() > 0;
    allowNewLWQE = lowPriorityDescQue.size() < WQE_PREFETCH_THRESHOLD;
    allowNewWQE = (allowNewHWQE && highPriorityQpnQue.size()) || (allowNewLWQE && lowPriorityQpnQue.size());
    if (!allowNewWQE)
    {
        return;
    }
    if (highPriorityQpnQue.size() < WQE_PREFETCH_THRESHOLD)
    {
        uint32_t qpn = highPriorityQpnQue.front();
        highPriorityQpnQue.pop();
        // QPStatusPtr = 
        wqePrefetchQpStatusRReqQue.push(qpn);
    }
    else if (lowPriorityQpnQue.size() < WQE_PREFETCH_THRESHOLD)
    {
        uint32_t qpn = lowPriorityQpnQue.front();
        lowPriorityQpnQue.pop();
        wqePrefetchQpStatusRReqQue.push(qpn);
    }
    if (!wqePrefetchEvent.scheduled())
    {
        rNic->schedule(wqePrefetchEvent, curTick() + rNic->clockPeriod());
    }
    if (highPriorityQpnQue.size() || lowPriorityQpnQue.size())
    {
        if (!getPrefetchQpnEvent.scheduled())
        {
            rNic->schedule(getPrefetchQpnEvent, curTick() + rNic->clockPeriod());
        }
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
    if (qpStatus->head_ptr - qpStatus->fetch_ptr > MAX_PREFETCH_NUM)
    {
        descNum = MAX_PREFETCH_NUM;
    }
    else 
    {
        descNum = qpStatus->head_ptr - qpStatus->fetch_ptr;
    }

    MrReqRspPtr descReq = make_shared<MrReqRsp>(DMA_TYPE_RREQ, MR_RCHNL_TX_DESC,
            qpStatus->key, descNum << 5, qpStatus->fetch_ptr);

    descReq->txDescRsp = new TxDesc[descNum];
    rNic->descReqFifo.push(descReq);
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
 * Process WQE responses and produce sub WQEs.
*/
void HanGuRnic::DescScheduler::wqeProc()
{
    assert(rNic->txdescRspFifo.size());
    uint32_t descNum = wqeFetchInfoQue.front().first;
    QPStatusPtr qpStatus = wqeFetchInfoQue.front().second;
    wqeFetchInfoQue.pop();
    TxDescPtr desc;

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
        assert(descNum == 1);
        assert(qpStatus->wnd_start <= qpStatus->wnd_fetch);
        assert(qpStatus->wnd_fetch < qpStatus->wnd_end);
        // set wnd_end to the size of the whole message
        if (qpStatus->wnd_start == 0)
        {
            qpStatus->wnd_end = rNic->txdescRspFifo.front()->len;
        }
        TxDescPtr desc = rNic->txdescRspFifo.front();
        TxDescPtr subDesc = make_shared<TxDesc>(desc);
        subDesc->opcode = desc->opcode;

        // WARNING: SEND/RECV are currently not supported!
        subDesc->lVaddr = desc->lVaddr + qpStatus->wnd_start;
        subDesc->rdmaType.rVaddr_l = desc->rdmaType.rVaddr_l + qpStatus->wnd_start;
        subDesc->len = qpStatus->wnd_end - qpStatus->wnd_fetch > MAX_COMMIT_SZ ? 
            MAX_COMMIT_SZ : qpStatus->wnd_end - qpStatus->wnd_fetch;
        subDesc->qpn = qpStatus->qpn;
        
        // If this subDesc doesn't finish the whole message, don't generate CQE
        if (qpStatus->wnd_fetch + subDesc->len >= qpStatus->wnd_end)
        {
            subDesc->flags = 0;
        }
        lowPriorityDescQue.push(subDesc);
        qpStatus->wnd_fetch += subDesc->len;
    }
    else
    {
        panic("Illegal QP type!\n");
    }
    
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
    if (!rNic->rdmaEngine.dduEvent.scheduled())
    {
        rNic->schedule(rNic->rdmaEngine.dduEvent, curTick() + rNic->clockPeriod());
    }
    if (!launchWqeEvent.scheduled())
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
    assert(status->wnd_fetch + len <= status->wnd_end);
    status->wnd_fetch += len;
    if (status->wnd_fetch >= status->wnd_end)
    {
        if (status->tail_ptr != status->head_ptr)
        {
            status->tail_ptr++;
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
    qpStatusTable.emplace(status->qpn, status);
    if (rNic->createQue.size())
    {
        rNic->schedule(createQpStatusEvent, curTick() + rNic->clockPeriod());
    }
}