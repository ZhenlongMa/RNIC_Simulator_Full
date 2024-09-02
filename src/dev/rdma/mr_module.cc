
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

///////////////////////////// HanGuRnic::Translation & Protection Table {begin}//////////////////////////////
HanGuRnic::MrRescModule::MrRescModule (HanGuRnic *i, const std::string n, 
        uint32_t mptCacheNum, uint32_t mttCacheNum)
  : rnic(i),
    _name(n),
    chnlIdx(0),
    dmaRrspEvent ([this]{ dmaRrspProcessing(); }, n),
    mptRspEvent  ([this]{ mptRspProcessing();  }, n),
    mttRspEvent  ([this]{ mttRspProcessing();  }, n),
    mrRspProcEvent([this]{mrRspProcessing();}, n),
    onFlyDataMrRdReqNum(0),
    onFlyDescMrRdReqNum(0),
    onFlyDataDmaRdReqNum(0),
    onFlyDescDmaRdReqNum(0),
    onFlyMptRdReqNum(0),
    onFlyMttRdReqNum(0),
    onFlyMptPrefetchReqNum(0),
    transReqEvent([this]{ transReqProcessing();}, n),
    mptCache(i, mptCacheNum, n + ".MptCache"),
    mttCache(i, mttCacheNum, n + ".MttCache")
    { }


bool 
HanGuRnic::MrRescModule::isMRMatching (MptResc * mptResc, MrReqRspPtr mrReq) {
    if (mptResc->key != mrReq->lkey) {
        return false;
    }
    return true;
}


void 
HanGuRnic::MrRescModule::mptReqProcess (MrReqRspPtr mrReq) {
    mrReq->reqTick = curTick();

    /* Read MPT entry */
    // mptCache.rescRead(mrReq->lkey, &mptRspEvent, mrReq);
    if (
        #ifdef CACHE_ALL_CQ_MPT
        mrReq->chnl == TPT_WCHNL_TX_CQUE || mrReq->chnl == TPT_WCHNL_RX_CQUE ||
        #endif
        #ifdef CACHE_ALL_QP_MPT
        mrReq->chnl == MR_RCHNL_RX_DESC || mrReq->chnl == MR_RCHNL_TX_DESC ||
        mrReq->chnl == MR_RCHNL_TX_DESC_PREFETCH || mrReq->chnl == MR_RCHNL_TX_DESC_FETCH ||
        #endif
        0
    ) {
        if (cqMpt.find(mrReq->lkey) == cqMpt.end()) {
            mptCache.rescRead(mrReq->lkey, &mptRspEvent, mrReq);
        }
        else {
            HANGU_PRINT(MrResc, "CQ MPT!\n");
            cqMptRspQue.emplace(mrReq, cqMpt[mrReq->lkey]);
            if (!mptRspEvent.scheduled()) {
                rnic->schedule(mptRspEvent, curTick() + rnic->clockPeriod());
            }
        }
    }
    else {
        mptCache.rescRead(mrReq->lkey, &mptRspEvent, mrReq);
    }

    if (onFlyMptNum.find(mrReq->chnl) == onFlyMptNum.end()) {
        onFlyMptNum[mrReq->chnl] = 1;
    }
    else {
        onFlyMptNum[mrReq->chnl]++;
    }

    onFlyMptRdReqNum++;
    HANGU_PRINT(MrResc, " mptReqProcess! qpn: 0x%x, onFlyMptRdReqNum: %d, MPT[%d]: %d\n", 
        mrReq->qpn, onFlyMptRdReqNum, mrReq->chnl, onFlyMptNum[mrReq->chnl]);
}

void 
HanGuRnic::MrRescModule::mttReqProcess (uint64_t mttIdx, MrReqRspPtr mrReq) {
    
    /* Read MTT entry */
    mttCache.rescRead(mttIdx, &mttRspEvent, mrReq);
    onFlyMttRdReqNum++;
    HANGU_PRINT(MrResc, " mttReqProcess! qpn: 0x%x, onFlyMttRdReqNum: %d\n", mrReq->qpn, onFlyMttRdReqNum);
}

void 
HanGuRnic::MrRescModule::dmaReqProcess (uint64_t pAddr, MrReqRspPtr mrReq, uint32_t offset, uint32_t length) {
    
    HANGU_PRINT(MrResc, "dmaReqProcess! qpn: 0x%x\n", mrReq->qpn);
    
    if (mrReq->type == DMA_TYPE_WREQ) {

        /* Post dma req to DMA engine */
        DmaReqPtr dmaWreq;
        switch (mrReq->chnl) {
          case TPT_WCHNL_TX_CQUE:
          case TPT_WCHNL_RX_CQUE:
            dmaWreq = make_shared<DmaReq>(rnic->pciToDma(pAddr), mrReq->length, 
                    nullptr, mrReq->data + offset, 0); /* last parameter is useless here */
            rnic->cqDmaWriteFifo.push(dmaWreq);
            break;
          case TPT_WCHNL_TX_DATA:
          case TPT_WCHNL_RX_DATA:
            dmaWreq = make_shared<DmaReq>(rnic->pciToDma(pAddr), length, 
                    nullptr, mrReq->data + offset, 0); /* last parameter is useless here */
            rnic->dataDmaWriteFifo.push(dmaWreq);

            break;
        }
        
        /* Schedule DMA write Engine */
        if (!rnic->dmaEngine.dmaWriteEvent.scheduled()) {
            rnic->schedule(rnic->dmaEngine.dmaWriteEvent, curTick() + rnic->clockPeriod());
        }


    } else if (mrReq->type == DMA_TYPE_RREQ) {
        DmaReqPtr dmaRdReq;
        switch (mrReq->chnl) {
            case MR_RCHNL_TX_DESC:
            case MR_RCHNL_RX_DESC:
            case MR_RCHNL_TX_DESC_FETCH:
            case MR_RCHNL_TX_DESC_PREFETCH:
                /* Post desc dma req to DMA engine */
                dmaRdReq = make_shared<DmaReq>(rnic->pciToDma(pAddr), mrReq->length, 
                        &dmaRrspEvent, mrReq->data + offset, 0); /* last parameter is useless here */
                rnic->descDmaReadFifo.push(dmaRdReq);
                // update on fly request count
                onFlyDescDmaRdReqNum++;
                HANGU_PRINT(MrResc, "desc DMA request sent! on-fly count: %d\n", onFlyDescDmaRdReqNum);
                assert(onFlyDescDmaRdReqNum > 0);
                break;
            case MR_RCHNL_TX_DATA:
            case MR_RCHNL_RX_DATA:
                /* Post data dma req to DMA engine */
                dmaRdReq = make_shared<DmaReq>(rnic->pciToDma(pAddr), length, 
                        &dmaRrspEvent, mrReq->data + offset, 0); /* last parameter is useless here */
                rnic->dataDmaReadFifo.push(dmaRdReq);
                // update on fly request count
                onFlyDataDmaRdReqNum++;
                HANGU_PRINT(MrResc, "data DMA request sent! on-fly count: %d\n", onFlyDataDmaRdReqNum);
                assert(onFlyDataDmaRdReqNum > 0);
                break;
            default:
                panic("Illegal MR request channel: %d!\n", mrReq->chnl);
        }

        /* Push to Fifo, and dmaRrspProcessing 
         * will fetch for processing */   
        dmaReq2RspFifo.emplace(mrReq, dmaRdReq);
        HANGU_PRINT(MrResc, "push DMA read req into dmaReq2RspFifo, fifo asize: %d, type: %d, mttnum: %d\n", dmaReq2RspFifo.size(), mrReq->chnl, mrReq->mttNum);
        assert(dmaRdReq->size != 0);

        /* Schedule for fetch cached resources through dma read. */
        if (!rnic->dmaEngine.dmaReadEvent.scheduled()) {
            rnic->schedule(rnic->dmaEngine.dmaReadEvent, curTick() + rnic->clockPeriod());
        }
    }
    else {
        panic("invalid MR request type: %d\n", mrReq->type);
    }
}

void HanGuRnic::MrRescModule::mrRspProcessing() {
    int chnl;
    int flag = 0;

    // determine the channel to process
    for (auto it = pendingMrReqQueue.begin(); it != pendingMrReqQueue.end(); it++) {
        if (it->second.size() == 0) {
            continue;
        }
        if (it->second.front()->dmaRspNum == it->second.front()->mttNum) {
            chnl = it->first;
            flag = 1;
        }
    }

    MrReqRspPtr mrReqRsp = pendingMrReqQueue[chnl].front();

    assert(flag == 1);
    assert(mrReqRsp->dmaRspNum == mrReqRsp->mttNum);

    HANGU_PRINT(MrResc, "Trigger QP 0x%x MR request response! pendingMrReqQueue[%d] size: %d\n", 
        mrReqRsp->qpn, chnl, pendingMrReqQueue[chnl].size());
    pendingMrReqQueue[chnl].pop();
    Event *event;
    RxDescPtr rxDesc;
    TxDescPtr txDesc;
    switch (mrReqRsp->chnl) {
        // WQE fetching and prefetching are impossible to be out-of-order
        case MR_RCHNL_TX_DESC:
        case MR_RCHNL_TX_DESC_PREFETCH:
        case MR_RCHNL_TX_DESC_FETCH:
            event = &rnic->wqeBufferManage.wqeReadRspEvent;
            rnic->wqeBufferManage.wqeRspQue.push(mrReqRsp);
            for (uint32_t i = 0; (i * sizeof(TxDesc)) < mrReqRsp->length; ++i) {
                txDesc = make_shared<TxDesc>(mrReqRsp->txDescRsp + i);
                HANGU_PRINT(MrResc, "channel: %d, txDesc length: %d, lVaddr: 0x%x, opcode: %d, qpn: 0x%x\n", 
                    mrReqRsp->chnl, txDesc->len, txDesc->lVaddr, txDesc->opcode, mrReqRsp->qpn);
                assert(txDesc->len != 0);
                assert(txDesc->lVaddr != 0);
                assert(txDesc->opcode != 0);
            }
            onFlyDescMrRdReqNum--;
            HANGU_PRINT(MrResc, "MR module receives a complete desc MR response, on-fly request count: %d\n", onFlyDescMrRdReqNum);
            assert(onFlyDescMrRdReqNum >= 0);
            HANGU_PRINT(MrResc, "dmaRrspProcessing: size is %d, desc total len is %d!\n", 
                rnic->txdescRspFifo.size(), mrReqRsp->length);
            break;
        case MR_RCHNL_RX_DESC:
            event = &rnic->rdmaEngine.rcvRpuEvent;
            for (uint32_t i = 0; (i * sizeof(RxDesc)) < mrReqRsp->length; ++i) {
                rxDesc = make_shared<RxDesc>(mrReqRsp->rxDescRsp + i);
                assert((rxDesc->len != 0) && (rxDesc->lVaddr != 0));
                rnic->rxdescRspFifo.push(rxDesc);
            }
            delete mrReqRsp->rxDescRsp;
            HANGU_PRINT(MrResc, "dmaRrspProcessing: rnic->rxdescRspFifo.size() is %d!\n", 
                    rnic->rxdescRspFifo.size());
            break;
        case MR_RCHNL_TX_DATA:
            event = &rnic->rdmaEngine.rgrrEvent;
            rnic->txdataRspFifo.push(mrReqRsp);
            onFlyDataMrRdReqNum--;
            HANGU_PRINT(MrResc, "MR module receives a complete data MR response, on-fly request count: %d, txdataRspFifo size: %d\n", 
                onFlyDataMrRdReqNum, rnic->txdataRspFifo.size());
            assert(onFlyDataMrRdReqNum >= 0);
            break;
        case MR_RCHNL_RX_DATA:
            event = &rnic->rdmaEngine.rdCplRpuEvent;
            rnic->rxdataRspFifo.push(mrReqRsp);
            break;
        default:
            panic("TPT CHNL error, there should only exist RCHNL type!\n");
            return;
    }

    /* Schedule relevant event in REQ */
    if (!event->scheduled()) {
        rnic->schedule(*event, curTick() + rnic->clockPeriod());
    }

    // if any channel has valid response left, schedule my self in the next tick
    for (auto it = pendingMrReqQueue.begin(); it != pendingMrReqQueue.end(); it++) {
        if (it->second.size() == 0) {
            continue;
        }
        if (it->second.front()->dmaRspNum == it->second.front()->mttNum && !mrRspProcEvent.scheduled()) {
            rnic->schedule(mrRspProcEvent, curTick() + rnic->clockPeriod());
        }
    }
}


void 
HanGuRnic::MrRescModule::dmaRrspProcessing() {

    HANGU_PRINT(MrResc, "dmaRrspProcessing! FIFO size: %d\n", dmaReq2RspFifo.size());

    /* If empty, just return */
    if (dmaReq2RspFifo.empty() || 0 == dmaReq2RspFifo.front().second->rdVld) {
        HANGU_PRINT(MrResc, "DMA read response not ready!\n");
        return;
    }

    /* Get dma rrsp data */
    MrReqRspPtr tptRsp = dmaReq2RspFifo.front().first;
    HANGU_PRINT(MrResc, "DMA read response received by MR module, MR request length: %d, DMA request length: %d, dmaRspNum: %d, mttNum: %d, mttRspNum: %d, qpn: 0x%x\n", 
        tptRsp->length, dmaReq2RspFifo.front().second->size, tptRsp->dmaRspNum, tptRsp->mttNum, tptRsp->mttRspNum, tptRsp->qpn);
    assert(tptRsp->dmaRspNum < tptRsp->mttNum);
    assert(tptRsp->dmaRspNum < tptRsp->mttRspNum);
    tptRsp->dmaRspNum++;
    dmaReq2RspFifo.pop();

    if (tptRsp->type == DMA_TYPE_WREQ) {
        panic("mrReq type error, write type req cannot put into dmaReq2RspFifo\n");
    }
    tptRsp->type = DMA_TYPE_RRSP;

    HANGU_PRINT(MrResc, "dmaRrspProcessing: tptRsp lkey %d length %d offset %d!\n", 
                tptRsp->lkey, tptRsp->length, tptRsp->offset);

    // update DMA on fly request count
    switch (tptRsp->chnl) {
        case MR_RCHNL_TX_DESC:
        case MR_RCHNL_RX_DESC:
        case MR_RCHNL_TX_DESC_FETCH:
        case MR_RCHNL_TX_DESC_PREFETCH:
            onFlyDescDmaRdReqNum--;
            HANGU_PRINT(MrResc, "descriptor DMA response received by MR module! on-fly count: %d, qpn: 0x%x\n", 
                onFlyDescDmaRdReqNum, tptRsp->qpn);
            break;
        case MR_RCHNL_TX_DATA:
        case MR_RCHNL_RX_DATA:
            onFlyDataDmaRdReqNum--;
            HANGU_PRINT(MrResc, "data DMA response received by MR module! on-fly count: %d, qpn: 0x%x\n", 
                onFlyDataDmaRdReqNum, tptRsp->qpn);
            break;
    }

    MrReqRspPtr chnlFirstReq = pendingMrReqQueue[tptRsp->chnl].front();

    if (tptRsp->dmaRspNum == tptRsp->mttNum && chnlFirstReq->dmaRspNum != chnlFirstReq->mttNum) {
        HANGU_PRINT(MrResc, "MR response blocked! chnl: %d, qpn 0x%x lkey 0x%x blocked by qpn 0x%x lkey 0x%x\n", 
            tptRsp->chnl, tptRsp->qpn, tptRsp->lkey, chnlFirstReq->qpn, chnlFirstReq->lkey);
    }

    for (auto it = pendingMrReqQueue.begin(); it != pendingMrReqQueue.end(); it++) {
        if (it->second.size() == 0) {
            continue;
        }
        if (it->second.front()->dmaRspNum == it->second.front()->mttNum && !mrRspProcEvent.scheduled()) {
            rnic->schedule(mrRspProcEvent, curTick() + rnic->clockPeriod());
        }
    }

    /* Schedule myself if next elem in FIFO is ready */
    if (dmaReq2RspFifo.size() && dmaReq2RspFifo.front().second->rdVld) {
        if (!dmaRrspEvent.scheduled()) { /* Schedule myself */
            rnic->schedule(dmaRrspEvent, curTick() + rnic->clockPeriod());
        }
    }

    HANGU_PRINT(MrResc, "dmaRrspProcessing: out!\n");

}

void
HanGuRnic::MrRescModule::mptRspProcessing() {
    MptResc *mptResc;
    MrReqRspPtr reqPkt;
    assert(mptCache.rrspFifo.size() || cqMptRspQue.size());
    /* Get mpt resource & MR req pkt from mptCache rsp fifo */
    if (cqMptRspQue.size() != 0) {
        reqPkt = cqMptRspQue.front().first;
        mptResc = cqMptRspQue.front().second;
        cqMptRspQue.pop();
    }
    else {
        mptResc = mptCache.rrspFifo.front().first;
        reqPkt = mptCache.rrspFifo.front().second;
        mptCache.rrspFifo.pop();
    }

    reqPkt->mpt = mptResc;

    onFlyMptRdReqNum--;
    onFlyMptNum[reqPkt->chnl]--;

    HANGU_PRINT(MrResc, "mptRspProcessing! onFlyMptRdReqNum: %d, MPT[%d]: %d, req time: %ld\n", 
        onFlyMptRdReqNum, reqPkt->chnl, onFlyMptNum[reqPkt->chnl], curTick() - reqPkt->reqTick);
    assert(onFlyMptNum[reqPkt->chnl] >= 0);
    assert(onFlyMptRdReqNum >= 0);

    // cache all CQ MPT
    #ifdef CACHE_ALL_CQ_MPT
    if (reqPkt->chnl == TPT_WCHNL_TX_CQUE || reqPkt->chnl == TPT_WCHNL_RX_CQUE) {
        cqMpt[mptResc->key] = mptResc;
    }
    #endif
    #ifdef CACHE_ALL_QP_MPT
    if (reqPkt->chnl == MR_RCHNL_TX_DESC || 
        reqPkt->chnl == MR_RCHNL_RX_DESC || 
        reqPkt->chnl == MR_RCHNL_TX_DESC_PREFETCH || 
        reqPkt->chnl == MR_RCHNL_TX_DESC_FETCH
    ) {
        qpMpt[mptResc->key] = mptResc;
    }
    #endif

    if (reqPkt->chnl == MR_RCHNL_TX_MPT_PREFETCH) {
        assert(rnic->rescPrefetcher.mrPrefetchFlag[mptResc->key] == true);
        rnic->rescPrefetcher.mrPrefetchFlag[mptResc->key] = false;
        onFlyMptPrefetchReqNum--;
        HANGU_PRINT(MrResc, "mptRspProcessing: receive MPT prefetch! onFlyMptPrefetchReqNum: %d\n", onFlyMptPrefetchReqNum);
    }

    assert(mptResc->startVAddr % PAGE_SIZE == 0);

    HANGU_PRINT(MrResc, "mptRspProcessing: qpn: 0x%x, mptResc->lkey 0x%x, len %d, chnl 0x%x, type 0x%x, offset %d\n", 
            reqPkt->qpn, mptResc->key, reqPkt->length, reqPkt->chnl, reqPkt->type, reqPkt->offset);

    /* Match the info in MR req and mptResc */
    if (!isMRMatching(mptResc, reqPkt)) {
        panic("[MrRescModule] mpt resc in MR is not match with reqPkt, \n");
    }

    // Calculate MTT index, modified by mazhenlong
    uint64_t mttIdx = mptResc->mttSeg + ((reqPkt->offset + (mptResc->startVAddr & 0xFFF)) >> PAGE_SIZE_LOG);

    // Calculate mttNum
    if ((reqPkt->length + (reqPkt->offset % PAGE_SIZE)) % PAGE_SIZE == 0) {
        reqPkt->mttNum = (reqPkt->length + (reqPkt->offset % PAGE_SIZE)) / PAGE_SIZE;
    }
    else {
        reqPkt->mttNum = (reqPkt->length + (reqPkt->offset % PAGE_SIZE)) / PAGE_SIZE + 1;
    }
    reqPkt->mttRspNum   = 0;
    reqPkt->dmaRspNum   = 0;
    reqPkt->sentPktNum  = 0;
    HANGU_PRINT(MrResc, "mptRspProcessing: reqPkt->offset 0x%x, mptResc->startVAddr 0x%x, mptResc->mttSeg 0x%x, mttIdx 0x%x, mttNum: %d\n", 
        reqPkt->offset, mptResc->startVAddr, mptResc->mttSeg, mttIdx, reqPkt->mttNum);

    if (reqPkt->chnl == MR_RCHNL_TX_DESC) {
        HANGU_PRINT(MrResc, "tx desc MR request! mttNum: %d\n", reqPkt->mttNum);
        // The size of the Work Queue is up to one page.
        assert(reqPkt->mttNum == 1);
    }

    /* Post mtt req */
    // modified by mazhenlong
    for (int i = 0; i < reqPkt->mttNum; i++) {
        mttReqProcess(mttIdx + i, reqPkt);
    }

    /* Schedule myself */
    if (mptCache.rrspFifo.size() || cqMptRspQue.size()) {
        if (!mptRspEvent.scheduled()) {
            rnic->schedule(mptRspEvent, curTick() + rnic->clockPeriod());
        }
    }

    HANGU_PRINT(MrResc, "mptRspProcessing: out!\n");
}

void
HanGuRnic::MrRescModule::mttRspProcessing() {
    // HANGU_PRINT(MrResc, "mttRspProcessing!\n");
    /* Get mttResc from mttCache Rsp fifo */
    assert(mttCache.rrspFifo.size() > 0);
    MttResc *mttResc   = mttCache.rrspFifo.front().first;
    MrReqRspPtr reqPkt = mttCache.rrspFifo.front().second;
    mttCache.rrspFifo.pop();
    HANGU_PRINT(MrResc, "mttRspProcessing: qpn: 0x%x, reqPkt chnl: %d\n", reqPkt->chnl, reqPkt->qpn);

    if (reqPkt->chnl != MR_RCHNL_TX_MPT_PREFETCH) {
        /* Post dma req */
        uint32_t offset; // relative to MR request
        uint32_t length;
        uint64_t dmaAddr;

        // set DMA address
        if (reqPkt->mttRspNum == 0) {
            dmaAddr = mttResc->pAddr + reqPkt->offset % PAGE_SIZE; // WARNING: suppose MR is page-aligned
        }
        else {
            dmaAddr = mttResc->pAddr;
        }

        // set offset
        if (reqPkt->mttRspNum == 0) {
            offset = 0;
        }
        else {
            offset = (reqPkt->mttRspNum - 1) * PAGE_SIZE + (PAGE_SIZE - (reqPkt->offset % PAGE_SIZE));
        }

        // set length
        if (reqPkt->mttRspNum == 0) { // if this is the first MTT response
            if (reqPkt->mttRspNum + 1 == reqPkt->mttNum) {
                length = reqPkt->length;
            }
            else {
                length = PAGE_SIZE - reqPkt->offset;
            }
        }
        else if (reqPkt->mttRspNum + 1 == reqPkt->mttNum) {
            if ((reqPkt->length + reqPkt->offset) % PAGE_SIZE == 0) {
                length = PAGE_SIZE;
            }
            else {
                length = (reqPkt->length + reqPkt->offset) % PAGE_SIZE; 
            }
        }
        else if (reqPkt->mttRspNum + 1 < reqPkt->mttNum) {
            length = PAGE_SIZE;
        }
        else {
            panic("Wrong mtt rsp num and mtt num! mtt rsp num: %d, mtt num: %d", reqPkt->mttRspNum, reqPkt->mttNum);
        }
        assert(length != 0);
        dmaReqProcess(dmaAddr, reqPkt, offset, length);

        assert(reqPkt->mttRspNum < reqPkt->mttNum);
        reqPkt->mttRspNum++;
    }
    else {
        HANGU_PRINT(MrResc, "mttRspProcessing: finish memory metadata prefetch! qpn: 0x%x\n", reqPkt->qpn);
    }

    /* Schedule myself */
    if (mttCache.rrspFifo.size()) {
        if (!mttRspEvent.scheduled()) {
            rnic->schedule(mttRspEvent, curTick() + rnic->clockPeriod());
        }
    }
    onFlyMttRdReqNum--;
    HANGU_PRINT(MrResc, "mttRspProcessing: out! onFlyMttRdReqNum: %d\n", onFlyMttRdReqNum);
    assert(onFlyMttRdReqNum >= 0);
}

void
HanGuRnic::MrRescModule::transReqProcessing() {

    HANGU_PRINT(MrResc, "transReqProcessing! desc: %d, cq: %d, data: %d, pfetch: %d\n", 
        rnic->descReqFifo.size(), rnic->cqWreqFifo.size(), rnic->dataReqFifo.size(), rnic->mptPrefetchQue.size());

    uint8_t CHNL_NUM = 4;
    bool isEmpty[CHNL_NUM];
    isEmpty[0] = rnic->descReqFifo.empty();
    isEmpty[1] = rnic->cqWreqFifo.empty() ;
    isEmpty[2] = rnic->dataReqFifo.empty();
    isEmpty[3] = rnic->mptPrefetchQue.empty();
    
    MrReqRspPtr mrReq;
    for (uint8_t cnt = 0; cnt < CHNL_NUM; ++cnt) {
        if (isEmpty[chnlIdx] == false) {
            switch (chnlIdx) {
                case 0:
                    mrReq = rnic->descReqFifo.front();
                    rnic->descReqFifo.pop();
                    onFlyDescMrRdReqNum++;
                    assert(onFlyDescMrRdReqNum > 0);
                    HANGU_PRINT(MrResc, "transReqProcessing: Desc read request! qpn: 0x%x, on-fly request count: %d\n", 
                        mrReq->qpn, onFlyDescMrRdReqNum);
                    assert(mrReq->type == DMA_TYPE_RREQ);
                    break;
                case 1:
                    mrReq = rnic->cqWreqFifo.front();
                    rnic->cqWreqFifo.pop();
                    HANGU_PRINT(MrResc, "transReqProcessing: CQ write request, offset %d\n", mrReq->offset);
                    break;
                case 2:
                    mrReq = rnic->dataReqFifo.front();
                    rnic->dataReqFifo.pop();
                    if (mrReq->type == DMA_TYPE_RREQ) {
                        onFlyDataMrRdReqNum++;
                        assert(onFlyDataMrRdReqNum > 0);
                        HANGU_PRINT(MrResc, "transReqProcessing: MR module receive a data MR request! qpn: 0x%x, on-fly request count: %d\n", 
                            mrReq->qpn, onFlyDataMrRdReqNum);
                    }
                    HANGU_PRINT(MrResc, "transReqProcessing: Data read/Write request! data addr 0x%lx\n", (uintptr_t)(mrReq->data));
                    break;
                case 3:
                    mrReq = rnic->mptPrefetchQue.front();
                    rnic->mptPrefetchQue.pop();
                    assert(rnic->rescPrefetcher.mrPrefetchFlag[mrReq->lkey] == true);
                    onFlyMptPrefetchReqNum++;
                    HANGU_PRINT(MrResc, "transReqProcessing: MPT prefetch request! lkey: 0x%lx, qpn: 0x%x, on-fly: %d\n", 
                        mrReq->lkey, mrReq->qpn, onFlyMptPrefetchReqNum);
                    break;
                default:
                    panic("Illegal Channel!\n");
                    break;
            }

            // for (int i = 0; i < mrReq->length; ++i) {
            //     HANGU_PRINT(MrResc, "transReqProcessing: data[%d] 0x%x\n", i, (mrReq->data)[i]);
            // }

            HANGU_PRINT(MrResc, "transReqProcessing: lkey 0x%x, offset 0x%x, length %d, type: %d\n", 
                        mrReq->lkey, mrReq->offset, mrReq->length, mrReq->type);
            assert(mrReq->type == DMA_TYPE_WREQ || mrReq->type == DMA_TYPE_RREQ);

            /* Point to next chnl */
            ++chnlIdx;
            chnlIdx = chnlIdx % CHNL_NUM;

            /* Schedule this module again if there still has elem in fifo */
            if (!rnic->descReqFifo.empty() || 
                !rnic->cqWreqFifo.empty()  || 
                !rnic->dataReqFifo.empty() ||
                !rnic->mptPrefetchQue.empty()) {
                if (!transReqEvent.scheduled()) {
                    rnic->schedule(transReqEvent, curTick() + rnic->clockPeriod());
                }
            }
            
            if (mrReq->chnl != MR_RCHNL_TX_MPT_PREFETCH) {
                pendingMrReqQueue[mrReq->chnl].push(mrReq);
            }
            /* Read MPT entry */
            mptReqProcess(mrReq);

            HANGU_PRINT(MrResc, "transReqProcessing: out!\n");
            
            return;
        } else {
            /* Point to next chnl */
            ++chnlIdx;
            chnlIdx = chnlIdx % CHNL_NUM;
        }
    }
}
///////////////////////////// HanGuRnic::Translation & Protection Table {end}//////////////////////////////
