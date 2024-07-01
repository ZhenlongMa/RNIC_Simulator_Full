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
    prefetchProcEvent([this]{prefetchProc();}, name), {
}

// launch prefetch signal, prefetch QPC, WQE, MPT
void HanGuRnic::RescPrefetcher::prefetchProc() {
    uint32_t qpn = prefetchQue.front();
    prefetchQue.pop();
    triggerPrefetch();
    // prefetch QPC
    CxtReqRspPtr qpcRdReq = make_shared<CxtReqRsp>(CXT_PFCH_QP, CXT_CHNL_TX, qpn, 1, 0);
    rNic->qpcModule.postQpcReq(qpcRdReq);
    // prefetch WQE
    rNic->wqeBuffManage.prefetchQpnQue.push(qpn);
    if (!rNic->wqeBuffManage.prefetchProcEvent.scheduled()) {
        rNic->schedule(rNic->wqeBuffManage.prefetchProcEvent, curTick() + rNic->clockPeriod());
    }
    triggerPrefetch();
}

void HanGuRnic::RescPrefetcher::prefetchMemProc() {
    assert(rNic->memPrefetchInfoQue.size() != 0);
    // uint32_t qpn = memPrefetchInfoQue.front().first();
    // uint32_t prefetchNum = memPrefetchInfoQue.front().second();
    // memPrefetchInfoQue.pop();
    // for (int i = 0; i < prefechNum; i++) {

    // }
}

void HanGuRnic::RescPrefetcher::updateWqeBuffer() {
    
}

void HanGuRnic::RescPrefetcher::triggerPrefetch() {
    if ((rnic->descScheduler.lowPriorityQpnQue.size() < prefetchQue.size() + PREFETCH_WINDOW) && 
        !prefetchProcEvent.scheduled()) {
        rNic->schedule(prefetchProcEvent, curTick() + rNic->clockPeriod);
    }
}