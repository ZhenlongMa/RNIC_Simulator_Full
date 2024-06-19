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
    uint32_t qpn = prefetchQue.front()
    // prefetch QPC
    CxtReqRspPtr qpcRdReq = make_shared<CxtReqRsp>(CXT_PFCH_QP, CXT_CHNL_TX, qpn, 1, 0);
    rNic->qpcModule.postQpcReq(qpcRdReq);
    // prefetch WQE
}
