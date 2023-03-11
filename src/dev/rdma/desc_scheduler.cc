
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
    qpcRspEvent([this]{qpcRspProc();}, name)
{

}

/**
 * @note
 * Process QPC response triggered by doorbell processing. This function 1) check and update
 * QP status, judge if it is needed to push QPN to prefetch queue.
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
    MrReqRspPtr descReq;
    // label QP status
    // judge pre-fetch
    if (qpc->txQpcRsp->indicator == 1) // In case of latency-sensitive QP, fetch all descriptors
    {
        descReq = make_shared<MrReqRsp>(DMA_TYPE_RREQ, MR_RCHNL_TX_DESC,
            qpc->txQpcRsp->sndWqeBaseLkey, db->num << 5, db->offset);
    }
    else
    {
        if (db->num > MAX_PREFETCH_NUM)
        {
            descReq = make_shared<MrReqRsp>(DMA_TYPE_RREQ, MR_RCHNL_TX_DESC,
                qpc->txQpcRsp->sndWqeBaseLkey, ((uint32_t)MAX_PREFETCH_NUM) << 5, db->offset);
        }
        else
        {
            descReq = make_shared<MrReqRsp>(DMA_TYPE_RREQ, MR_RCHNL_TX_DESC,
                qpc->txQpcRsp->sndWqeBaseLkey, ((uint32_t)db->num) << 5, db->offset);
        }
    }
}

void HanGuRnic::DescScheduler::qpStatusProc()
{
    
}