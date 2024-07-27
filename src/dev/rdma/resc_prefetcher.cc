#include "dev/rdma/hangu_rnic.hh"

// #include "debug/DescScheduler.hh"
#include "base/trace.hh"
#include "debug/HanGu.hh"

using namespace HanGuRnicDef;
using namespace Net;
using namespace std;

HanGuRnic::RescPrefetcher::RescPrefetcher(HanGuRnic *rNic, const std::string name):
    rNic(rNic),
    _name(name),
    prefetchCnt(0),
    prefetchProcEvent([this]{prefetchProc();}, name),
    prefetchMemProcEvent([this]{prefetchMemProc();}, name),
    qpcPfetchRspProcEvent([this]{qpcPfetchRspProc();}, name) {
}

// launch prefetch signal, prefetch QPC, WQE, MPT
void HanGuRnic::RescPrefetcher::prefetchProc() {
    assert(prefetchQue.size() > 0);
    uint32_t qpn = prefetchQue.front();
    HANGU_PRINT(RescPrefetcher, "prefetchProc: launch prefetch! qpn: 0x%x\n", qpn);
    prefetchQue.pop();
    prefetchCnt++;
    // triggerPrefetch();
    // prefetch QPC
    CxtReqRspPtr qpcRdReq = make_shared<CxtReqRsp>(CXT_PFCH_QP, CXT_CHNL_TX, qpn, 1, 0);
    qpcRdReq->txQpcRsp = new QpcResc;
    rNic->qpcModule.postQpcReq(qpcRdReq);
    // prefetch WQE
    rNic->wqeBufferManage.prefetchQpnQue.push(qpn);
    if (!rNic->wqeBufferManage.wqePrefetchProcEvent.scheduled()) {
        rNic->schedule(rNic->wqeBufferManage.wqePrefetchProcEvent, curTick() + rNic->clockPeriod());
    }
    triggerPrefetch();
}

void HanGuRnic::RescPrefetcher::prefetchMemProc() {
    assert(rNic->memPrefetchInfoQue.size() != 0);
    uint32_t qpn = rNic->memPrefetchInfoQue.front().first;
    uint32_t prefetchNum = rNic->memPrefetchInfoQue.front().second;
    rNic->memPrefetchInfoQue.pop();
    // get LKEY
    MrReqRspPtr mrReq;
    uint32_t lkey;
    int offset;
    HANGU_PRINT(RescPrefetcher, "prefetchMemProc: prefetch memory metadata! qpn: 0x%x, prefetchNum: %d\n", qpn, prefetchNum);
    for (int i = 0; i < prefetchNum; i++) {
        lkey = rNic->memPrefetchLkeyQue.front();
        rNic->memPrefetchLkeyQue.pop();
        if (i == 0) {
            offset = rNic->descScheduler.qpStatusTable[qpn]->fetch_offset;
        }
        else {
            offset = 0;
        }
        mrReq = make_shared<MrReqRsp>(DMA_TYPE_RREQ, MR_RCHNL_TX_MPT_PREFETCH, lkey, 0, offset, qpn);
        rNic->mptPrefetchQue.push(mrReq);
    }
    if (!rNic->mrRescModule.transReqEvent.scheduled()) {
        rNic->schedule(rNic->mrRescModule.transReqEvent, curTick() + rNic->clockPeriod());
    }
}

void HanGuRnic::RescPrefetcher::triggerPrefetch() {
    if ((rNic->descScheduler.lowPriorityQpnQue.size() < prefetchQue.size() + PREFETCH_WINDOW) && 
        prefetchQue.size() != 0 &&
        !prefetchProcEvent.scheduled()) {
        rNic->schedule(prefetchProcEvent, curTick() + rNic->clockPeriod());
    }
}

void HanGuRnic::RescPrefetcher::qpcPfetchRspProc() {
    // do nothing
}