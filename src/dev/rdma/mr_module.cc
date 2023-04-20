
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
    transReqEvent([this]{ transReqProcessing();}, n),
    mptCache(i, mptCacheNum, n),
    mttCache(i, mttCacheNum, n) { }


bool 
HanGuRnic::MrRescModule::isMRMatching (MptResc * mptResc, MrReqRspPtr mrReq) {
    if (mptResc->key != mrReq->lkey) {
        return false;
    }
    return true;
}


void 
HanGuRnic::MrRescModule::mptReqProcess (MrReqRspPtr mrReq) {
    
    HANGU_PRINT(MrResc, " mptReqProcess enter\n");

    /* Read MPT entry */
    mptCache.rescRead(mrReq->lkey, &mptRspEvent, mrReq);
}

void 
HanGuRnic::MrRescModule::mttReqProcess (uint64_t mttIdx, MrReqRspPtr mrReq) {

    HANGU_PRINT(MrResc, " mttReqProcess enter\n");
    
    /* Read MTT entry */
    mttCache.rescRead(mttIdx, &mttRspEvent, mrReq);
}

void 
HanGuRnic::MrRescModule::dmaReqProcess (uint64_t pAddr, MrReqRspPtr mrReq, uint32_t offset, uint32_t length) {
    
    HANGU_PRINT(MrResc, " MrRescModule.dmaReqProcess!\n");
    
    if (mrReq->type == DMA_TYPE_WREQ) {

        /* Post dma req to DMA engine */
        DmaReqPtr dmaWreq;
        switch (mrReq->chnl) {
          case TPT_WCHNL_TX_CQUE:
          case TPT_WCHNL_RX_CQUE:
            dmaWreq = make_shared<DmaReq>(rnic->pciToDma(pAddr), mrReq->length, 
                    nullptr, mrReq->data + offset, 0); /* last parameter is useless here */
            rnic->cqDmaWriteFifo.push(dmaWreq);

            HANGU_PRINT(MrResc, " MrRescModule.dmaReqProcess: write CQ Request, dmaReq->paddr is 0x%lx, offset %d\n", 
                    dmaWreq->addr, mrReq->offset);
            break;
          case TPT_WCHNL_TX_DATA:
          case TPT_WCHNL_RX_DATA:
            HANGU_PRINT(MrResc, " MrRescModule.dmaReqProcess: write Data request!\n");
            // for (int i = 0; i < mrReq->length; ++i) {
            //     HANGU_PRINT(MrResc, " MrRescModule.dmaReqProcess: data[%d] 0x%x\n", i, mrReq->data[i]);
            // }

            dmaWreq = make_shared<DmaReq>(rnic->pciToDma(pAddr), length, 
                    nullptr, mrReq->data + offset, 0); /* last parameter is useless here */
            HANGU_PRINT(MrResc, " MrRescModule.dmaReqProcess: write data Request, dmaReq->paddr is 0x%lx, offset %d, size %d\n", 
                    dmaWreq->addr, offset, length);
            rnic->dataDmaWriteFifo.push(dmaWreq);

            break;
        }
        
        /* Schedule DMA write Engine */
        if (!rnic->dmaEngine.dmaWriteEvent.scheduled()) {
            rnic->schedule(rnic->dmaEngine.dmaWriteEvent, curTick() + rnic->clockPeriod());
            HANGU_PRINT(MrResc, " MrRescModule.dmaReqProcess: schedule dmaWriteEvent\n");
        }


    } else if (mrReq->type == DMA_TYPE_RREQ) {

        DmaReqPtr dmaRdReq;
        switch (mrReq->chnl) {
          case MR_RCHNL_TX_DESC:
          case MR_RCHNL_RX_DESC:

            HANGU_PRINT(MrResc, " MrRescModule.dmaReqProcess: read desc request lkey 0x%x len %d offset %d\n",
                    mrReq->lkey, mrReq->length, mrReq->offset);

            /* Post desc dma req to DMA engine */
            dmaRdReq = make_shared<DmaReq>(rnic->pciToDma(pAddr), mrReq->length, 
                    &dmaRrspEvent, mrReq->data + offset, 0); /* last parameter is useless here */
            rnic->descDmaReadFifo.push(dmaRdReq);
            
            break;
          case MR_RCHNL_TX_DATA:
          case MR_RCHNL_RX_DATA:

            HANGU_PRINT(MrResc, " MrRescModule.dmaReqProcess: read data request\n");

            /* Post data dma req to DMA engine */
            dmaRdReq = make_shared<DmaReq>(rnic->pciToDma(pAddr), length, 
                    &dmaRrspEvent, mrReq->data + offset, 0); /* last parameter is useless here */
            rnic->dataDmaReadFifo.push(dmaRdReq);

            break;
        }

        /* Push to Fifo, and dmaRrspProcessing 
         * will fetch for processing */   
        dmaReq2RspFifo.emplace(mrReq, dmaRdReq);

        /* Schedule for fetch cached resources through dma read. */
        if (!rnic->dmaEngine.dmaReadEvent.scheduled()) {
            rnic->schedule(rnic->dmaEngine.dmaReadEvent, curTick() + rnic->clockPeriod());
        }
    }
    HANGU_PRINT(MrResc, " MrRescModule.dmaReqProcess: out!\n");
}


void 
HanGuRnic::MrRescModule::dmaRrspProcessing() {

    HANGU_PRINT(MrResc, " MrRescModule.dmaRrspProcessing! FIFO size: %d\n", dmaReq2RspFifo.size());
    
    /* If empty, just return */
    if (dmaReq2RspFifo.empty() || 
            0 == dmaReq2RspFifo.front().second->rdVld) {
        return;
    }

    /* Get dma rrsp data */
    MrReqRspPtr tptRsp = dmaReq2RspFifo.front().first;
    dmaReq2RspFifo.pop();

    assert(tptRsp->dmaRspNum != tptRsp->mttNum);

    if (tptRsp->type == DMA_TYPE_WREQ) {
        panic("mrReq type error, write type req cannot put into dmaReq2RspFifo\n");
        return;
    }
    tptRsp->type = DMA_TYPE_RRSP;

    HANGU_PRINT(MrResc, " MrRescModule.dmaRrspProcessing: tptRsp lkey %d length %d offset %d!\n", 
                tptRsp->lkey, tptRsp->length, tptRsp->offset);

    Event *event;
    RxDescPtr rxDesc;
    TxDescPtr txDesc;
    switch (tptRsp->chnl) {
      case MR_RCHNL_TX_DESC:
        // event = &rnic->rdmaEngine.dduEvent;
        event = &rnic->descScheduler.wqeRspEvent;

        for (uint32_t i = 0; (i * sizeof(TxDesc)) < tptRsp->length; ++i) {
            txDesc = make_shared<TxDesc>(tptRsp->txDescRsp + i);
            assert(txDesc->len != 0);
            assert(txDesc->lVaddr != 0);
            assert(txDesc->opcode != 0);
            rnic->txdescRspFifo.push(txDesc);
        }
        // assert((tptRsp->txDescRsp->len != 0) && (tptRsp->txDescRsp->lVaddr != 0));
        // rnic->txdescRspFifo.push(tptRsp->txDescRsp);
        // rnic->txdescRspFifo.push(tptRsp);

        HANGU_PRINT(MrResc, " MrRescModule.dmaRrspProcessing: size is %d, desc total len is %d!\n", 
                rnic->txdescRspFifo.size(), tptRsp->length);

        break;
      case MR_RCHNL_RX_DESC:
        event = &rnic->rdmaEngine.rcvRpuEvent;
        for (uint32_t i = 0; (i * sizeof(RxDesc)) < tptRsp->length; ++i) {
            rxDesc = make_shared<RxDesc>(tptRsp->rxDescRsp + i);
            assert((rxDesc->len != 0) && (rxDesc->lVaddr != 0));
            rnic->rxdescRspFifo.push(rxDesc);
        }
        // rnic->rxdescRspFifo.push(tptRsp);
        delete tptRsp->rxDescRsp;

        HANGU_PRINT(MrResc, " MrRescModule.dmaRrspProcessing: rnic->rxdescRspFifo.size() is %d!\n", 
                rnic->rxdescRspFifo.size());

        break;
      case MR_RCHNL_TX_DATA:
        event = &rnic->rdmaEngine.rgrrEvent;
        rnic->txdataRspFifo.push(tptRsp);
      
        break;
      case MR_RCHNL_RX_DATA:
        event = &rnic->rdmaEngine.rdCplRpuEvent;
        rnic->rxdataRspFifo.push(tptRsp);
      
        break;
      default:
        panic("TPT CHNL error, there should only exist RCHNL type!\n");
        return;
    }

    /* Schedule relevant event in REQ */
    if (!event->scheduled()) {
        rnic->schedule(*event, curTick() + rnic->clockPeriod());
    }

    /* Schedule myself if next elem in FIFO is ready */
    if (dmaReq2RspFifo.size() && dmaReq2RspFifo.front().second->rdVld) {

        if (!dmaRrspEvent.scheduled()) { /* Schedule myself */
            rnic->schedule(dmaRrspEvent, curTick() + rnic->clockPeriod());
        }
    }

    HANGU_PRINT(MrResc, " MrRescModule.dmaRrspProcessing: out!\n");

}

void
HanGuRnic::MrRescModule::mptRspProcessing() {
    HANGU_PRINT(MrResc, " MrRescModule.mptRspProcessing!\n");

    /* Get mpt resource & MR req pkt from mptCache rsp fifo */
    MptResc *mptResc   = mptCache.rrspFifo.front().first;
    MrReqRspPtr reqPkt = mptCache.rrspFifo.front().second;
    mptCache.rrspFifo.pop();

    reqPkt->mpt = mptResc;

    assert(mptResc->startVAddr % PAGE_SIZE == 0);

    HANGU_PRINT(MrResc, " MrRescModule.mptRspProcessing: mptResc->lkey 0x%x, len %d, chnl 0x%x, type 0x%x, offset %d\n", 
            mptResc->key, reqPkt->length, reqPkt->chnl, reqPkt->type, reqPkt->offset);
    if (reqPkt->type == 1) {
        for (int i = 0; i < reqPkt->length; ++i) {
            HANGU_PRINT(MrResc, " MrRescModule.mptRspProcessing: data[%d] 0x%x\n", i, reqPkt->data[i]);
        }
    }

    /* Match the info in MR req and mptResc */
    if (!isMRMatching(mptResc, reqPkt)) {
        panic("[MrRescModule] mpt resc in MR is not match with reqPkt, \n");
    }

    /* Calculate required MTT index */
    // uint64_t mttIdx = mptResc->mttSeg + ((reqPkt->offset - (mptResc->startVAddr & 0xFFFF)) >> PAGE_SIZE_LOG);
    // reqPkt->offset = reqPkt->offset & 0xFFF;
    // uint64_t mttIdx = mptResc->mttSeg;
    // HANGU_PRINT(MrResc, " MrRescModule.mptRspProcessing: reqPkt->offset 0x%x, mptResc->startVAddr 0x%x, mptResc->mttSeg 0x%x, mttIdx 0x%x\n", 
    //         reqPkt->offset, mptResc->startVAddr, mptResc->mttSeg, mttIdx);

    // Calculate MTT index, modified by mazhenlong
    // TO DO: change offset bitwidth
    uint64_t mttIdx = mptResc->mttSeg + ((reqPkt->offset + (mptResc->startVAddr & 0xFFF)) >> PAGE_SIZE_LOG);
    HANGU_PRINT(MrResc, " MrRescModule.mptRspProcessing: reqPkt->offset 0x%x, mptResc->startVAddr 0x%x, mptResc->mttSeg 0x%x, mttIdx 0x%x\n", 
            reqPkt->offset, mptResc->startVAddr, mptResc->mttSeg, mttIdx);

    // Calculate mttNum
    if ((reqPkt->length + (reqPkt->offset % PAGE_SIZE)) % PAGE_SIZE == 0)
    {
        reqPkt->mttNum = (reqPkt->length + (reqPkt->offset % PAGE_SIZE)) / PAGE_SIZE;
    }
    else
    {
        reqPkt->mttNum = (reqPkt->length + (reqPkt->offset % PAGE_SIZE)) / PAGE_SIZE + 1;
    }
    reqPkt->mttRspNum   = 0;
    reqPkt->dmaRspNum   = 0;
    reqPkt->sentPktNum  = 0;

    /* Post mtt req */
    // mttReqProcess(mttIdx, reqPkt);

    /* Post mtt req */
    // modified by mazhenlong
    for (int i = 0; i < reqPkt->mttNum; i++)
    {
        mttReqProcess(mttIdx + i, reqPkt);
    }

    /* Schedule myself */
    if (mptCache.rrspFifo.size()) {
        if (!mptRspEvent.scheduled()) {
            rnic->schedule(mptRspEvent, curTick() + rnic->clockPeriod());
        }
    }

    HANGU_PRINT(MrResc, " MrRescModule.mptRspProcessing: out!\n");
}

void
HanGuRnic::MrRescModule::mttRspProcessing() {
    HANGU_PRINT(MrResc, " MrRescModule.mttRspProcessing!\n");
    
    /* Get mttResc from mttCache Rsp fifo */
    MttResc *mttResc   = mttCache.rrspFifo.front().first;
    MrReqRspPtr reqPkt = mttCache.rrspFifo.front().second;
    mttCache.rrspFifo.pop();
    HANGU_PRINT(MrResc, " MrRescModule.mttRspProcessing: mttResc->paddr 0x%lx size %d mttCache.rrspFifo %d\n", 
            mttResc->pAddr, reqPkt->length, mttCache.rrspFifo.size());

    /* Post dma req */
    // dmaReqProcess(mttResc->pAddr + reqPkt->offset, reqPkt);

    /* Post dma req */
    uint32_t offset; // relative to MR request
    uint32_t length;
    uint64_t dmaAddr;

    // set DMA address
    if (reqPkt->mttRspNum == 0)
    {
        dmaAddr = mttResc->pAddr + reqPkt->offset % PAGE_SIZE; // warning: suppose MR is page-aligned
    }
    else
    {
        dmaAddr = mttResc->pAddr;
    }

    // set offset
    if (reqPkt->mttRspNum == 0)
    {
        offset = 0;
    }
    else
    {
        offset = (reqPkt->mttRspNum - 1) * PAGE_SIZE + (PAGE_SIZE - (reqPkt->offset % PAGE_SIZE));
    }

    // set length
    if (reqPkt->mttRspNum == 0)
    {
        if (reqPkt->mttRspNum + 1 == reqPkt->mttNum)
        {
            length = reqPkt->length;
        }
        else
        {
            length = PAGE_SIZE - reqPkt->offset;
        }
    }
    else if (reqPkt->mttRspNum + 1 == reqPkt->mttNum)
    {
        length = (reqPkt->length + reqPkt->offset) % PAGE_SIZE;
    }
    else
    {
        length = PAGE_SIZE;
    }
    // dmaReqProcess(mttResc->pAddr + reqPkt->offset, reqPkt);
    dmaReqProcess(dmaAddr, reqPkt, offset, length);

    assert(reqPkt->mttRspNum < reqPkt->mttNum);
    reqPkt->mttRspNum++;

    /* Schedule myself */
    if (mttCache.rrspFifo.size()) {
        
        if (!mttRspEvent.scheduled()) {
            rnic->schedule(mttRspEvent, curTick() + rnic->clockPeriod());
        }
    }

    HANGU_PRINT(MrResc, " MrRescModule.mttRspProcessing: out!\n");
}

void
HanGuRnic::MrRescModule::transReqProcessing() {

    HANGU_PRINT(MrResc, " MrRescModule.transReqProcessing!\n");

    uint8_t CHNL_NUM = 3;
    bool isEmpty[CHNL_NUM];
    isEmpty[0] = rnic->descReqFifo.empty();
    isEmpty[1] = rnic->cqWreqFifo.empty() ;
    isEmpty[2] = rnic->dataReqFifo.empty();
    
    HANGU_PRINT(MrResc, " MrRescModule.transReqProcessing isEmpty[0] %d, isEmpty[1] %d, isEmpty[2] %d\n", 
            isEmpty[0], isEmpty[1], isEmpty[2]);
    
    MrReqRspPtr mrReq;
    for (uint8_t cnt = 0; cnt < CHNL_NUM; ++cnt) {
        if (isEmpty[chnlIdx] == false) {
            switch (chnlIdx) {
              case 0:
                mrReq = rnic->descReqFifo.front();
                rnic->descReqFifo.pop();
                HANGU_PRINT(MrResc, " MrRescModule.transReqProcessing: Desc read request!\n");
                break;
              case 1:
                mrReq = rnic->cqWreqFifo.front();
                rnic->cqWreqFifo.pop();
                HANGU_PRINT(MrResc, " MrRescModule.transReqProcessing: Completion Queue write request, offset %d\n", mrReq->offset);
                break;
              case 2:
                mrReq = rnic->dataReqFifo.front();
                rnic->dataReqFifo.pop();
                HANGU_PRINT(MrResc, " MrRescModule.transReqProcessing: Data read/Write request! data addr 0x%lx\n", (uintptr_t)(mrReq->data));
                
                break;
            }

            // for (int i = 0; i < mrReq->length; ++i) {
            //     HANGU_PRINT(MrResc, " MrRescModule.transReqProcessing: data[%d] 0x%x\n", i, (mrReq->data)[i]);
            // }

            HANGU_PRINT(MrResc, " MrRescModule.transReqProcessing: lkey 0x%x, offset 0x%x, length %d\n", 
                        mrReq->lkey, mrReq->offset, mrReq->length);

            /* Point to next chnl */
            ++chnlIdx;
            chnlIdx = chnlIdx % CHNL_NUM;

            /* Schedule this module again if there still has elem in fifo */
            if (!rnic->descReqFifo.empty() || 
                !rnic->cqWreqFifo.empty()  || 
                !rnic->dataReqFifo.empty()) {
                if (!transReqEvent.scheduled()) {
                    rnic->schedule(transReqEvent, curTick() + rnic->clockPeriod());
                }
            }

            /* Read MPT entry */
            mptReqProcess(mrReq);

            HANGU_PRINT(MrResc, " MrRescModule.transReqProcessing: out!\n");
            
            return;
        } else {
            /* Point to next chnl */
            ++chnlIdx;
            chnlIdx = chnlIdx % CHNL_NUM;
        }
    }
}
///////////////////////////// HanGuRnic::Translation & Protection Table {end}//////////////////////////////
