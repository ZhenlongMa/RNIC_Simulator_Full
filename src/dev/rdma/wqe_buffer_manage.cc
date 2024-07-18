#include "dev/rdma/hangu_rnic.hh"

#include "base/trace.hh"
#include "debug/HanGu.hh"

using namespace HanGuRnicDef;
using namespace Net;
using namespace std;

HanGuRnic::WqeBufferManage::WqeBufferManage(HanGuRnic *rNic, const std::string name, int wqeCacheNum):
    rNic(rNic),
    _name(name),
    maxReplaceParam(0),
    descBufferCap(wqeCacheNum),
    wqeReqReturnEvent([this]{wqeReqReturn();}, name),
    wqeReadReqProcessEvent([this]{wqeReadReqProcess();}, name),
    wqeBufferUpdateEvent([this]{wqeBufferUpdate();}, name),
    wqePrefetchProcEvent([this]{wqePrefetchProc();}, name),
    wqeReadRspEvent([this]{wqeReadRspProcess();}, name),
    createWqeBufferEvent([this]{createWqeBuffer();}, name) {
    for (int i = 0; i < descBufferCap; i++) {
        vacantAddr.push(i);
    }
    // HANGU_PRINT(WqeBufferManage, "init!\n");
}

void HanGuRnic::WqeBufferManage::wqeReadReqProcess() {
    HANGU_PRINT(WqeBufferManage, "enter wqeReadReqProcess!\n");
    accessNum++;
    assert(rNic->descScheduler.wqeFetchInfoQue.size());
    uint32_t descNum = rNic->descScheduler.wqeFetchInfoQue.front().first;
    QPStatusPtr qpStatus = rNic->descScheduler.wqeFetchInfoQue.front().second;
    rNic->descScheduler.wqeFetchInfoQue.pop();
    // wqeBufferMetadataTable[qpStatus->qpn]->replaceParam = maxReplaceParam;
    // maxReplaceParam++;
    if (descNum > wqeBufferMetadataTable[qpStatus->qpn]->avaiNum + wqeBufferMetadataTable[qpStatus->qpn]->pendingReqNum) { // WQEs in the buffer is not sufficient
        HANGU_PRINT(WqeBufferManage, "wqeReadReqProcess: WQEs in the buffer and pending requests is not sufficient! Launch more WQE request! qpn: 0x%x\n", qpStatus->qpn);
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
        MrReqRspPtr descReq = make_shared<MrReqRsp>(DMA_TYPE_RREQ, MR_RCHNL_TX_DESC_FETCH, qpStatus->key, fetchByte, fetchOffset, qpStatus->qpn);
        HANGU_PRINT(WqeBufferManage, "wqeReadReqProcess: read WQE! qpn: 0x%x, offset: %d, length: %d\n", qpStatus->qpn, fetchOffset, fetchByte);
        descReq->txDescRsp = new TxDesc[descNum];
        rNic->descReqFifo.push(descReq);
        wqeBufferMetadataTable[qpStatus->qpn]->pendingReqNum += fetchNum;
        if (!rNic->mrRescModule.transReqEvent.scheduled()) { /* Schedule MrRescModule.transReqProcessing */
            rNic->schedule(rNic->mrRescModule.transReqEvent, curTick() + rNic->clockPeriod());
        }
        wqeBufferMetadataTable[qpStatus->qpn]->replaceLock = true;
    }
    else if (descNum > wqeBufferMetadataTable[qpStatus->qpn]->avaiNum) {
        HANGU_PRINT(WqeBufferManage, "wqeReadReqProcess: WQEs in the buffer is not sufficient, wait for pending requests! qpn: 0x%x\n", qpStatus->qpn);
        wqeBufferMetadataTable[qpStatus->qpn]->fetchReqNum += descNum;
        wqeBufferMetadataTable[qpStatus->qpn]->replaceLock = true;
    }
    else { // WQEs in the buffer is sufficient
        hitNum++;
        HANGU_PRINT(WqeBufferManage, "wqeReadReqProcess: WQEs in the buffer is sufficient! qpn: 0x%x, hitNum: %ld\n", qpStatus->qpn, hitNum);
        WqeRspPtr wqeRsp = std::make_shared<WqeRsp>(descNum, qpStatus->qpn);
        for (int i = 0; i < descNum; i++) {
            wqeRsp->descList.push(wqeBuffer[qpStatus->qpn]->descArray[i]);
        }
        wqeReturnQue.push(wqeRsp);
        if (!wqeReqReturnEvent.scheduled()) {
            rNic->schedule(wqeReqReturnEvent, curTick() + rNic->clockPeriod());
        }
    }
    if (rNic->descScheduler.wqeFetchInfoQue.size() != 0 && !wqeReadReqProcessEvent.scheduled()) {
        rNic->schedule(wqeReadReqProcessEvent, curTick() + rNic->clockPeriod());
    }
}

void HanGuRnic::WqeBufferManage::wqeReadRspProcess() {
    assert(wqeRspQue.size() != 0);
    MrReqRspPtr resp = wqeRspQue.front();
    wqeRspQue.pop();
    uint32_t qpn = resp->qpn;
    uint32_t respQueOffset = resp->offset / sizeof(TxDesc);
    uint32_t modTail = rNic->descScheduler.qpStatusTable[qpn]->tail_ptr % (sqSize / sizeof(TxDesc));
    HANGU_PRINT(WqeBufferManage, "wqeReadRspProcess: enter wqeReadRspProcess! qpn: 0x%x\n", qpn);
    HANGU_PRINT(WqeBufferManage, "wqeReadRspProcess: respQueOffset: %d, modTail: %d, avai num: %d, rsp num: %d, queue cap: %d, rsp chnl: %d\n", 
        respQueOffset, modTail, wqeBufferMetadataTable[qpn]->avaiNum, resp->length / sizeof(TxDesc), sqSize / sizeof(TxDesc), resp->chnl);
    HANGU_PRINT(WqeBufferManage, "wqeReadRspProcess: pending num: %d\n", wqeBufferMetadataTable[qpn]->pendingReqNum);
    assert(wqeBufferMetadataTable[qpn]->replaceLock == true);
    // make sure the response is the correct next WQEs
    assert(modTail + wqeBufferMetadataTable[qpn]->avaiNum == respQueOffset);
    assert(wqeBufferMetadataTable[qpn]->pendingReqNum >= resp->length / sizeof(TxDesc));
    // store WQEs
    uint32_t replaceQpn;
    int min = 0;
    TxDescPtr txDesc;
    // in case of desc buffer capacity is not enough, pick some descriptors and replace
    while (descBufferCap - descBufferUsed < resp->length / sizeof(TxDesc)) {
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
        HANGU_PRINT(WqeBufferManage, "wqeReadRspProcess: erase qpn: 0x%x\n", replaceQpn);
    }
    for (uint32_t i = 0; (i * sizeof(TxDesc)) < resp->length; ++i) {
        txDesc = make_shared<TxDesc>(resp->txDescRsp + i);
        HANGU_PRINT(WqeBufferManage, "txDesc length: %d, lVaddr: 0x%x, opcode: %d, qpn: 0x%x, cq tag: %s\n", 
            txDesc->len, txDesc->lVaddr, txDesc->opcode, resp->qpn, txDesc->isSignaled() ? "true" : "false");
        assert(txDesc->len != 0);
        assert(txDesc->lVaddr != 0);
        assert(txDesc->opcode != 0);
        assert(wqeBuffer.find(qpn) != wqeBuffer.end());
        assert(wqeBufferMetadataTable.find(qpn) != wqeBufferMetadataTable.end());
        wqeBuffer[qpn]->descArray.push_back(txDesc);
        descBufferUsed++;
        wqeBufferMetadataTable[qpn]->avaiNum++;
        wqeBufferMetadataTable[qpn]->pendingReqNum--;
    }
    // trigger wqeReqReturn
    if (wqeBufferMetadataTable[qpn]->avaiNum >= wqeBufferMetadataTable[qpn]->fetchReqNum && wqeBufferMetadataTable[qpn]->fetchReqNum > 0) {
        HANGU_PRINT(WqeBufferManage, "wqeReadRspProcess: trigger WQE request return! qpn: 0x%x\n", qpn);
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
        triggerMemPrefetch(qpn);
    }
    // unlock replace
    if (wqeBufferMetadataTable[qpn]->pendingReqNum == 0) {
        wqeBufferMetadataTable[qpn]->replaceLock = false;
        HANGU_PRINT(WqeBufferManage, "wqeReadRspProcess: unlock replace! qpn: 0x%x\n", qpn);
    }
    if (wqeRspQue.size() != 0 && !wqeReadRspEvent.scheduled()) {
        rNic->schedule(wqeReadRspEvent, curTick() + rNic->clockPeriod());
    }
    HANGU_PRINT(WqeBufferManage, "wqeReadRspProcess: leave wqeReadRspProcess!\n");
}

// return WQEs to desc scheduler
void HanGuRnic::WqeBufferManage::wqeReqReturn() {
    assert(wqeReturnQue.size() != 0);
    WqeRspPtr wqeRsp = wqeReturnQue.front();
    wqeReturnQue.pop();
    HANGU_PRINT(WqeBufferManage, "wqeReqReturn: descNum: %d, qpn: 0x%x!\n", wqeRsp->descNum, wqeRsp->qpn);
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
    HANGU_PRINT(WqeBufferManage, "wqePrefetchProc: into wqePrefetchProc! qpn: 0x%x\n", qpn);
    assert(rNic->descScheduler.qpStatusTable.find(qpn) != rNic->descScheduler.qpStatusTable.end());
    int keepNum, activeNum;
    activeNum = rNic->descScheduler.qpStatusTable[qpn]->head_ptr - rNic->descScheduler.qpStatusTable[qpn]->tail_ptr;
    HANGU_PRINT(WqeBufferManage, "wqePrefetchProc: active num: %d\n", activeNum);
    if (activeNum > MAX_PREFETCH_NUM) {
        keepNum = MAX_PREFETCH_NUM;
    }
    else {
        keepNum = activeNum; // WARNING: HOW TO UPDATE KEEPNUM IN TIME?
    }
    HANGU_PRINT(WqeBufferManage, "wqePrefetchProc: keepNum: %d!\n", keepNum);
    if (wqeBufferMetadataTable.find(qpn) == wqeBufferMetadataTable.end()) {
        wqeBufferMetadataTable[qpn] = std::make_shared<WqeBufferMetadata>();
        HANGU_PRINT(WqeBufferManage, "wqePrefetchProc: create WQE buffer metadata!\n");
    }
    wqeBufferMetadataTable[qpn]->keepNum = keepNum;
    wqeBufferMetadataTable[qpn]->replaceParam = maxReplaceParam;
    maxReplaceParam++;
    HANGU_PRINT(WqeBufferManage, "wqePrefetchProc: maxReplaceParam: %d\n", maxReplaceParam);
    if (wqeBufferMetadataTable[qpn]->keepNum > wqeBufferMetadataTable[qpn]->avaiNum) { 
        HANGU_PRINT(WqeBufferManage, "wqePrefetchProc: prefetch not enough!\n");
        // launch prefetch request
        QPStatusPtr qpStatus = rNic->descScheduler.qpStatusTable[qpn];
        wqeBufferMetadataTable[qpn]->replaceLock = true;
        int sqWqeCap = sqSize / sizeof(TxDesc);
        int prefetchNum = wqeBufferMetadataTable[qpn]->keepNum - wqeBufferMetadataTable[qpn]->avaiNum;
        assert(wqeBufferMetadataTable[qpn]->pendingReqNum == 0);
        assert(prefetchNum > 0);
        // WQE request exceeds the border of a QP, needs to send TWO MR request
        if ((qpStatus->tail_ptr + wqeBufferMetadataTable[qpn]->avaiNum) % sqWqeCap + prefetchNum > sqWqeCap) { 
            HANGU_PRINT(WqeBufferManage, "wqePrefetchProc: fetch two MR for WQE!\n");
            assert((qpStatus->tail_ptr + wqeBufferMetadataTable[qpn]->avaiNum) % sqWqeCap + prefetchNum < sqWqeCap * 2);
            int tempPrefetchNum = sqWqeCap - (qpStatus->tail_ptr + wqeBufferMetadataTable[qpn]->avaiNum) % sqWqeCap;
            int tempPrefetchOffset = ((qpStatus->tail_ptr + wqeBufferMetadataTable[qpn]->avaiNum) % sqWqeCap) * sizeof(TxDesc);
            int tempPrefetchByte = tempPrefetchNum * sizeof(TxDesc);
            MrReqRspPtr descPrefetchReq = make_shared<MrReqRsp>(DMA_TYPE_RREQ, MR_RCHNL_TX_DESC_PREFETCH, qpStatus->key, tempPrefetchByte, tempPrefetchOffset, qpStatus->qpn);
            HANGU_PRINT(WqeBufferManage, "wqePrefetchProc: first prefetch num: %d, prefetch byte: %d, offset: %d, avai num: %d!\n", 
                tempPrefetchNum, tempPrefetchByte, tempPrefetchOffset, wqeBufferMetadataTable[qpn]->avaiNum);
            descPrefetchReq->txDescRsp = new TxDesc[tempPrefetchNum];
            rNic->descReqFifo.push(descPrefetchReq);
            tempPrefetchNum = prefetchNum - tempPrefetchNum;
            tempPrefetchOffset = 0;
            tempPrefetchByte = tempPrefetchNum * sizeof(TxDesc);
            descPrefetchReq = make_shared<MrReqRsp>(DMA_TYPE_RREQ, MR_RCHNL_TX_DESC_PREFETCH, qpStatus->key, tempPrefetchByte, tempPrefetchOffset, qpStatus->qpn);
            HANGU_PRINT(WqeBufferManage, "wqePrefetchProc: second prefetch num: %d, prefetch byte: %d, offset: %d, avai num: %d!\n", 
                tempPrefetchNum, tempPrefetchByte, tempPrefetchOffset, wqeBufferMetadataTable[qpn]->avaiNum);
            descPrefetchReq->txDescRsp = new TxDesc[tempPrefetchNum];
            rNic->descReqFifo.push(descPrefetchReq);
        } else {
            // WQE request does not exceed the border of a QP
            HANGU_PRINT(WqeBufferManage, "wqePrefetchProc: fetch one MR for WQE!\n");
            int tempPrefetchOffset = ((qpStatus->tail_ptr + wqeBufferMetadataTable[qpn]->avaiNum) % sqWqeCap) * sizeof(TxDesc);
            int prefetchByte = prefetchNum * sizeof(TxDesc);
            MrReqRspPtr descPrefetchReq = make_shared<MrReqRsp>(DMA_TYPE_RREQ, MR_RCHNL_TX_DESC_PREFETCH, qpStatus->key, prefetchByte, tempPrefetchOffset, qpStatus->qpn);
            descPrefetchReq->txDescRsp = new TxDesc[prefetchNum];
            rNic->descReqFifo.push(descPrefetchReq);
            HANGU_PRINT(WqeBufferManage, "wqePrefetchProc: prefetch num: %d, prefetch byte: %d, offset: %d, avai num: %d!\n", 
                prefetchNum, prefetchByte, tempPrefetchOffset, wqeBufferMetadataTable[qpn]->avaiNum);
        }
        wqeBufferMetadataTable[qpn]->pendingReqNum += prefetchNum;
        if (!rNic->mrRescModule.transReqEvent.scheduled()) { /* Schedule MrRescModule.transReqProcessing */
            rNic->schedule(rNic->mrRescModule.transReqEvent, curTick() + rNic->clockPeriod());
        }
        wqeBufferMetadataTable[qpn]->replaceLock = true;
    }
    else {
        HANGU_PRINT(WqeBufferManage, "wqePrefetchProc: prefetch enough, trigger mem prefetch!\n");
        // trigger MPT and MTT prefetch
        triggerMemPrefetch(qpn);
    }
    if (prefetchQpnQue.size() != 0 && !wqePrefetchProcEvent.scheduled()) {
        rNic->schedule(wqePrefetchProcEvent, curTick() + rNic->clockPeriod());
    }
    HANGU_PRINT(WqeBufferManage, "wqePrefetchProc: end wqePrefetchProc! qpn: 0x%x\n", qpn);
}

// erase empty wqe buffer, pop used wqe, update keepNum
void HanGuRnic::WqeBufferManage::wqeBufferUpdate() {
    assert(rNic->wqeBufferUpdateQue.size() != 0);
    uint32_t qpn = rNic->wqeBufferUpdateQue.front().first;
    uint32_t eraseNum = rNic->wqeBufferUpdateQue.front().second;
    rNic->wqeBufferUpdateQue.pop();
    assert(wqeBuffer[qpn]->descArray.size() == wqeBufferMetadataTable[qpn]->avaiNum);
    assert(wqeBuffer[qpn]->descArray.size() != 0);
    HANGU_PRINT(WqeBufferManage, "wqeBufferUpdate: erase! qpn: 0x%x, eraseNum: %d\n", qpn, eraseNum);
    for (int i = 0; i < eraseNum; i++) {
        wqeBuffer[qpn]->descArray.erase(wqeBuffer[qpn]->descArray.begin());
        wqeBufferMetadataTable[qpn]->avaiNum--;
        descBufferUsed--;
        if (wqeBufferMetadataTable[qpn]->avaiNum == 0 && wqeBufferMetadataTable[qpn]->pendingReqNum == 0) {
            wqeBufferMetadataTable.erase(qpn);
            HANGU_PRINT(WqeBufferManage, "wqeBufferUpdate: erase qpn: 0x%x\n", qpn);
        }
    }
    if (rNic->wqeBufferUpdateQue.size() != 0 && !wqeBufferUpdateEvent.scheduled()) {
        rNic->schedule(wqeBufferUpdateEvent, curTick() + rNic->clockPeriod());
    }
}

void HanGuRnic::WqeBufferManage::triggerMemPrefetch(uint32_t qpn) {
    HANGU_PRINT(WqeBufferManage, "into triggerMemPrefetch! qpn: 0x%x\n", qpn);
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

void HanGuRnic::WqeBufferManage::createWqeBuffer() {
    while (rNic->createWqeBufferQue.size() != 0) {
        uint32_t qpn = rNic->createWqeBufferQue.front();
        rNic->createWqeBufferQue.pop();
        wqeBuffer[qpn] = std::make_shared<WqeBufferUnit>();
        HANGU_PRINT(WqeBufferManage, "create wqe buffer! qpn: 0x%x\n", qpn);
    }
}