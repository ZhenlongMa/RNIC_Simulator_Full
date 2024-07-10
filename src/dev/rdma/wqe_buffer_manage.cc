#include "dev/rdma/hangu_rnic.hh"

#include "base/trace.hh"
#include "debug/HanGu.hh"

using namespace HanGuRnicDef;
using namespace Net;
using namespace std;

HanGuRnic::WqeBufferManage::WqeBufferManage(HanGuRnic *rNic, const std::string name, int wqeCacheNum):
    rNic(rNic),
    _name(name),
    descBufferCap(wqeCacheNum),
    wqeReqReturnEvent([this]{wqeReqReturn();}, name),
    wqeReadReqProcessEvent([this]{wqeReadReqProcess();}, name),
    wqeBufferUpdateEvent([this]{wqeBufferUpdate();}, name),
    wqePrefetchProcEvent([this]{wqePrefetchProc();}, name),
    wqeReadRspEvent([this]{wqeReadRspProcess();}, name) {
    for (int i = 0; i < descBufferCap; i++) {
        vacantAddr.push(i);
    }
}

void HanGuRnic::WqeBufferManage::wqeReadReqProcess()
{
    accessNum++;
    assert(rNic->descScheduler.wqeFetchInfoQue.size());
    uint32_t descNum = rNic->descScheduler.wqeFetchInfoQue.front().first;
    QPStatusPtr qpStatus = rNic->descScheduler.wqeFetchInfoQue.front().second;
    rNic->descScheduler.wqeFetchInfoQue.pop();
    if (descNum > wqeBufferMetadataTable[qpStatus->qpn]->avaiNum + wqeBufferMetadataTable[qpStatus->qpn]->pendingReqNum) { // WQEs in the buffer is not sufficient
        missNum++;
        int fetchNum;
        int fetchByte;
        int fetchOffset;
        fetchNum = descNum - wqeBufferMetadataTable[qpStatus->qpn]->avaiNum;
        wqeBufferMetadataTable[qpStatus->qpn]->fetchReqNum += descNum;
        fetchByte = fetchNum * sizeof(TxDesc);
        // In descScheduler.wqePrefetch it is assured that fetchByte does not exceed the border of Work Queue
        int sqWqeCap = sqSize / sizeof(TxDesc);
        int tailOffset = qpStatus->tail_ptr % sqWqeCap;
        assert(fetchNum + tailOffset <= sqWqeCap);
        fetchOffset = (qpStatus->tail_ptr + wqeBufferMetadataTable[qpStatus->qpn]->avaiNum) * sizeof(TxDesc) % sqSize;
        MrReqRspPtr descReq = make_shared<MrReqRsp>(DMA_TYPE_RREQ, MR_RCHNL_TX_DESC_FETCH, qpStatus->key, fetchByte, fetchOffset);
        descReq->txDescRsp = new TxDesc[descNum];
        rNic->descReqFifo.push(descReq);
        wqeBufferMetadataTable[qpStatus->qpn]->pendingReqNum += fetchNum;
        if (!rNic->mrRescModule.transReqEvent.scheduled()) { /* Schedule MrRescModule.transReqProcessing */
            rNic->schedule(rNic->mrRescModule.transReqEvent, curTick() + rNic->clockPeriod());
        }
    }
    else if (descNum > wqeBufferMetadataTable[qpStatus->qpn]->avaiNum) {
        wqeBufferMetadataTable[qpStatus->qpn]->fetchReqNum += descNum;
    }
    else { // WQEs in the buffer is sufficient
        hitNum++;
        WqeRspPtr wqeRsp = std::make_shared<WqeRsp>(descNum, qpStatus->qpn);
        for (int i = 0; i < descNum; i++) {
            wqeRsp->descList.push(wqeBuffer[qpStatus->qpn]->descArray[i]);
        }
        wqeReturnQue.push(wqeRsp);
        if (!wqeReqReturnEvent.scheduled()) {
            rNic->schedule(wqeReqReturnEvent, curTick() + rNic->clockPeriod());
        }
        if (!rNic->descScheduler.wqeProcEvent.scheduled()) {
            rNic->schedule(rNic->descScheduler.wqeProcEvent, curTick() + rNic->clockPeriod());
        }
    }
    if (rNic->descScheduler.wqeFetchInfoQue.size() != 0 && !wqeReadReqProcessEvent.scheduled()) {
        rNic->schedule(wqeReadReqProcessEvent, curTick() + rNic->clockPeriod());
    }
}

void HanGuRnic::WqeBufferManage::wqeReadRspProcess() {
    assert(wqeRspQue.size() != 0);
    MrReqRspPtr resp = wqeRspQue.front();
    // assert(resp->chnl == MR_RCHNL_TX_DESC_UPDATE);
    uint32_t qpn = resp->qpn;
    assert(wqeBufferMetadataTable[qpn]->replaceLock == true);
    // store WQEs
    uint32_t replaceQpn;
    int min = 0;
    TxDescPtr txDesc;
    while (descBufferCap - descBufferUsed < resp->length / sizeof(TxDesc)) {
        // replace
        assert(wqeBufferMetadataTable.size() < 500);
        for (auto it = wqeBufferMetadataTable.begin(); it != wqeBufferMetadataTable.end(); it++) {
            if (min > it->second->replaceParam && it->second->replaceLock == false) {
                min = it->second->replaceParam;
                replaceQpn = it->first;
                assert(it->second->avaiNum > 0);
            }
        }
        descBufferUsed -= wqeBufferMetadataTable[replaceQpn]->avaiNum;
        wqeBufferMetadataTable.erase(replaceQpn);
    }
    for (uint32_t i = 0; (i * sizeof(TxDesc)) < resp->length; ++i) {
        txDesc = make_shared<TxDesc>(resp->txDescRsp + i);
        HANGU_PRINT(MrResc, "txDesc length: %d, lVaddr: 0x%x, opcode: %d\n", txDesc->len, txDesc->lVaddr, txDesc->opcode);
        assert(txDesc->len != 0);
        assert(txDesc->lVaddr != 0);
        assert(txDesc->opcode != 0);
        wqeBuffer[qpn]->descArray.push_back(txDesc);
        descBufferUsed++;
        wqeBufferMetadataTable[qpn]->avaiNum++;
        wqeBufferMetadataTable[qpn]->pendingReqNum--;
    }
    // trigger wqeReqReturn
    if (wqeBufferMetadataTable[qpn]->avaiNum >= wqeBufferMetadataTable[qpn]->fetchReqNum && wqeBufferMetadataTable[qpn]->fetchReqNum > 0) {
        WqeRspPtr wqeRsp = std::make_shared<WqeRsp>(wqeBufferMetadataTable[qpn]->fetchReqNum, qpn);
        for (int i = 0; i < wqeBufferMetadataTable[qpn]->fetchReqNum; i++) {
            TxDescPtr txDesc = wqeBuffer[qpn]->descArray[i];
            wqeRsp->descList.push(txDesc);
        }
        wqeReturnQue.push(wqeRsp);
        if (!wqeReqReturnEvent.scheduled()) {
            rNic->schedule(wqeReqReturnEvent, curTick() + rNic->clockPeriod());
        }
        wqeBufferMetadataTable[qpn]->fetchReqNum = 0;
    }
    // trigger memory metadata prefetch
    if (wqeBufferMetadataTable[qpn]->avaiNum > 0 && wqeBufferMetadataTable[qpn]->fetchReqNum == 0) {
        // uint32_t lkey;
        // for (int i = 0; i < wqeBufferMetadataTable[qpn]->avaiNum; i++) {
        //     lkey = wqeBuffer[qpn].second->descArray[i]->lkey;
        //     memPrefetchQue.push(qpn, lkey);
        // }
        // if (!rNic->rescPrefetcher.prefetchMemProcEvent.scheduled()) {
        //     rNic->schedule(rNic->rescPrefetcher.prefetchMemProcEvent, curTick() + rNic->clockPeriod());
        // }
        triggerMemPrefetch(qpn);
    }
    // unlock replace
    if (wqeBufferMetadataTable[qpn]->pendingReqNum == 0) {
        wqeBufferMetadataTable[qpn]->replaceLock = false;
    }
    if (wqeRspQue.size() != 0 && !wqeReadRspEvent.scheduled()) {
        rNic->schedule(wqeReadRspEvent, curTick() + rNic->clockPeriod());
    }
}

// return WQEs to desc scheduler
void HanGuRnic::WqeBufferManage::wqeReqReturn() {
    assert(wqeReturnQue.size() != 0);
    WqeRspPtr wqeRsp = wqeReturnQue.front();
    wqeReturnQue.pop();
    rNic->wqeRspInfoQue.push(make_pair(wqeRsp->descNum, wqeRsp->qpn));
    for (int i = 0; i < wqeRsp->descNum; i++) {
        rNic->txdescRspFifo.push(wqeRsp->descList.front());
        wqeRsp->descList.pop();
    }
    assert(wqeRsp->descList.size() == 0);
    if (!rNic->descScheduler.wqeProcEvent.scheduled()) {
        rNic->schedule(rNic->descScheduler.wqeProcEvent, curTick() + rNic->clockPeriod());
    }
    if (wqeReturnQue.size() != 0 && !wqeReqReturnEvent.scheduled()) {
        rNic->schedule(wqeReqReturnEvent, curTick() + rNic->clockPeriod());
    }
}

void HanGuRnic::WqeBufferManage::wqePrefetchProc() {
    assert(prefetchQpnQue.size() != 0);
    uint32_t qpn = prefetchQpnQue.front();
    prefetchQpnQue.pop();
    wqeBufferMetadataTable[qpn]->replaceParam = maxReplaceParam;
    maxReplaceParam++;
    if (wqeBufferMetadataTable[qpn]->keepNum > wqeBufferMetadataTable[qpn]->avaiNum) { 
        // launch prefetch request
        QPStatusPtr qpStatus = rNic->descScheduler.qpStatusTable[qpn];
        wqeBufferMetadataTable[qpn]->replaceLock = true;
        int sqWqeCap = sqSize / sizeof(TxDesc);
        // int tailOffset = qpStatus->tail_ptr % sqWqeCap;
        int prefetchNum = wqeBufferMetadataTable[qpn]->keepNum - wqeBufferMetadataTable[qpn]->avaiNum;
        if ((qpStatus->tail_ptr + wqeBufferMetadataTable[qpn]->avaiNum) % sqWqeCap + prefetchNum > sqWqeCap) { 
            // WQE request exceeds the border of a QP, needs to send TWO MR request
            assert((qpStatus->tail_ptr + wqeBufferMetadataTable[qpn]->avaiNum) % sqWqeCap + prefetchNum < sqWqeCap * 2);
            int tempPrefetchNum = sqWqeCap - (qpStatus->tail_ptr + wqeBufferMetadataTable[qpn]->avaiNum) % sqWqeCap;
            int tempPrefetchOffset = (qpStatus->tail_ptr + wqeBufferMetadataTable[qpn]->avaiNum) % sqWqeCap;
            int tempPrefetchByte = tempPrefetchNum * sizeof(TxDesc);
            MrReqRspPtr descPrefetchReq = make_shared<MrReqRsp>(DMA_TYPE_RREQ, MR_RCHNL_TX_DESC_PREFETCH, qpStatus->key, tempPrefetchByte, tempPrefetchOffset, qpStatus->qpn);
            descPrefetchReq->txDescRsp = new TxDesc[tempPrefetchNum];
            rNic->descReqFifo.push(descPrefetchReq);
            tempPrefetchNum = prefetchNum - tempPrefetchNum;
            tempPrefetchOffset = 0;
            tempPrefetchByte = tempPrefetchNum * sizeof(TxDesc);
            descPrefetchReq = make_shared<MrReqRsp>(DMA_TYPE_RREQ, MR_RCHNL_TX_DESC_PREFETCH, qpStatus->key, tempPrefetchByte, tempPrefetchOffset, qpStatus->qpn);
            descPrefetchReq->txDescRsp = new TxDesc[tempPrefetchNum];
            rNic->descReqFifo.push(descPrefetchReq);
        } else {
            // WQE request does not exceed the border of a QP
            int tempPrefetchOffset = (qpStatus->tail_ptr + wqeBufferMetadataTable[qpn]->avaiNum) % sqWqeCap;
            int prefetchByte = prefetchNum * sizeof(TxDesc);
            MrReqRspPtr descPrefetchReq = make_shared<MrReqRsp>(DMA_TYPE_RREQ, MR_RCHNL_TX_DESC_PREFETCH, qpStatus->key, prefetchByte, tempPrefetchOffset, qpStatus->qpn);
            descPrefetchReq->txDescRsp = new TxDesc[prefetchNum];
            rNic->descReqFifo.push(descPrefetchReq);
        }
        wqeBufferMetadataTable[qpn]->pendingReqNum += prefetchNum;
        if (!rNic->mrRescModule.transReqEvent.scheduled()) { /* Schedule MrRescModule.transReqProcessing */
            rNic->schedule(rNic->mrRescModule.transReqEvent, curTick() + rNic->clockPeriod());
        }
    }
    else {
        // trigger MPT and MTT prefetch
        triggerMemPrefetch(qpn);
    }
    if (prefetchQpnQue.size() != 0 && !wqePrefetchProcEvent.scheduled()) {
        rNic->schedule(wqePrefetchProcEvent, curTick() + rNic->clockPeriod());
    }
}

void HanGuRnic::WqeBufferManage::wqeBufferUpdate() {
    assert(rNic->wqeBufferUpdateQue.size() != 0);
    uint32_t qpn = rNic->wqeBufferUpdateQue.front().first;
    uint32_t eraseNum = rNic->wqeBufferUpdateQue.front().second;
    rNic->wqeBufferUpdateQue.pop();
    assert(wqeBuffer[qpn]->descArray.size() == wqeBufferMetadataTable[qpn]->avaiNum);
    for (int i = 0; i < eraseNum; i++) {
        wqeBuffer[qpn]->descArray.erase(wqeBuffer[qpn]->descArray.begin());
        wqeBufferMetadataTable[qpn]->avaiNum--;
        descBufferUsed--;
        if (wqeBufferMetadataTable[qpn]->avaiNum == 0) {
            wqeBufferMetadataTable.erase(qpn);
        }
    }
    if (rNic->wqeBufferUpdateQue.size() != 0 && !wqeBufferUpdateEvent.scheduled()) {
        rNic->schedule(wqeBufferUpdateEvent, curTick() + rNic->clockPeriod());
    }
}

void HanGuRnic::WqeBufferManage::triggerMemPrefetch(uint32_t qpn) {
    assert(wqeBufferMetadataTable[qpn]->avaiNum > 0);
    uint32_t lkey;
    rNic->memPrefetchInfoQue.push(make_pair(qpn, wqeBufferMetadataTable[qpn]->avaiNum));
    for (int i = 0; i < wqeBufferMetadataTable[qpn]->avaiNum; i++) {
        lkey = wqeBuffer[qpn]->descArray[i]->lkey;
        rNic->memPrefetchLkeyQue.push(lkey);
    }
    if (!rNic->rescPrefetcher.prefetchMemProcEvent.scheduled()) {
        rNic->schedule(rNic->rescPrefetcher.prefetchMemProcEvent, curTick() + rNic->clockPeriod());
    }
}