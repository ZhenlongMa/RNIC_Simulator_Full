/*
 *======================= START OF LICENSE NOTICE =======================
 *  Copyright (C) 2021 Kang Ning, NCIC, ICT, CAS.
 *  All Rights Reserved.
 *
 *  NO WARRANTY. THE PRODUCT IS PROVIDED BY DEVELOPER "AS IS" AND ANY
 *  EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 *  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL DEVELOPER BE LIABLE FOR
 *  ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 *  GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 *  IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 *  OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THE PRODUCT, EVEN
 *  IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *======================== END OF LICENSE NOTICE ========================
 *  Primary Author: Kang Ning
 *  <kangning18z@ict.ac.cn>
 */

/**
 * @file
 * Device model for Han Gu RNIC.
 */

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

HanGuRnic::HanGuRnic(const Params *p)
  : RdmaNic(p), etherInt(NULL),
    doorbellVector(p->reorder_cap),
    // coreQpn(p->core_num),
    ceuProcEvent      ([this]{ ceuProc();      }, name()),
    doorbellProcEvent ([this]{ doorbellProc(); }, name()),
    mboxEvent([this]{ mboxFetchCpl();    }, name()),
    //rdmaEngine  (this, name() + ".RdmaEngine", p->reorder_cap),
    rdmaArray(this, p->rdma_core_num, p->reorder_cap, name() + ".RdmaArray"),
    mrRescModule(this, name() + ".MrRescModule", p->mpt_cache_num, p->mtt_cache_num),
    cqcModule   (this, name() + ".CqcModule", p->cqc_cache_num),
    qpcModule   (this, name() + ".QpcModule", p->qpc_cache_cap, p->reorder_cap),
    dmaEngine   (this, name() + ".DmaEngine"),

    dmaReadDelay(p->dma_read_delay), dmaWriteDelay(p->dma_write_delay),
    pciBandwidth(p->pci_speed),
    etherBandwidth(p->ether_speed),
    LinkDelay     (p->link_delay),
    ethRxPktProcEvent([this]{ ethRxPktProc(); }, name()), 
    coreNum(p->rdma_core_num)
{

    HANGU_PRINT(HanGuRnic, " qpc_cache_cap %d  reorder_cap %d cpuNum 0x%x\n", p->qpc_cache_cap, p->reorder_cap, p->cpu_num);

    cpuNum = p->cpu_num;
    syncCnt = 0;
    syncSucc = 0;

    // for (int i = 0; i < p->reorder_cap; ++i) {
    //     df2ccuIdxFifo.push(i);
    // }

    etherInt = new HanGuRnicInt(name() + ".int", this);

    mboxBuf = new uint8_t[4096];

    // Set the MAC address
    memset(macAddr, 0, ETH_ADDR_LEN);
    for (int i = 0; i < ETH_ADDR_LEN; ++i) {
        macAddr[ETH_ADDR_LEN - 1 - i] = (p->mac_addr >> (i * 8)) & 0xff;
        // HANGU_PRINT(PioEngine, " mac[%d] 0x%x\n", ETH_ADDR_LEN - 1 - i, macAddr[ETH_ADDR_LEN - 1 - i]);
    }

    BARSize[0]  = (1 << 12);
    BARAddrs[0] = 0xc000000000000000;
}

HanGuRnic::~HanGuRnic() {
    delete etherInt;
}

void
HanGuRnic::init() {
    PciDevice::init();
}

Port &
HanGuRnic::getPort(const std::string &if_name, PortID idx) {
    if (if_name == "interface")
        return *etherInt;
    return RdmaNic::getPort(if_name, idx);
}

///////////////////////////// HanGuRnic::PIO relevant {begin}//////////////////////////////

Tick
HanGuRnic::writeConfig(PacketPtr pkt) {
    int offset = pkt->getAddr() & PCI_CONFIG_SIZE;
    if (offset < PCI_DEVICE_SPECIFIC) {
        PciDevice::writeConfig(pkt);
    }
    else {
        panic("Device specific PCI config space not implemented.\n");
    }

    /* !TODO: We will implement PCI configuration here.
     * Some work may need to be done here based for the pci 
     * COMMAND bits, we don't realize now. */

    return configDelay;
}


Tick
HanGuRnic::read(PacketPtr pkt) {
    int bar;
    Addr daddr;

    if (!getBAR(pkt->getAddr(), bar, daddr)) {
        panic("Invalid PCI memory access to unmapped memory.\n");
    }

    /* Only HCR Space (BAR0-1) is allowed */
    assert(bar == 0);

    /* Only 32bit accesses allowed */
    assert(pkt->getSize() == 4);

    // HANGU_PRINT(PioEngine, " Read device addr 0x%x, pioDelay: %d\n", daddr, pioDelay);


    /* Handle read of register here.
     * Here we only implement read go bit */
    if (daddr == (Addr)&(((HanGuRnicDef::Hcr*)0)->goOpcode)) {/* Access `GO` bit */
        pkt->setLE<uint32_t>(regs.cmdCtrl.go()<<31 | regs.cmdCtrl.op());
    } else if (daddr == 0x20) {/* Access `sync` reg */
        pkt->setLE<uint32_t>(syncSucc);
    } else {
        pkt->setLE<uint32_t>(0);
    }

    pkt->makeAtomicResponse();
    return pioDelay;
}

Tick
HanGuRnic::write(PacketPtr pkt) {
    int bar;
    Addr daddr;

    HANGU_PRINT(PioEngine, " PioEngine.write: pkt addr 0x%x, size 0x%x\n",
            pkt->getAddr(), pkt->getSize());

    if (!getBAR(pkt->getAddr(), bar, daddr)) {
        panic("Invalid PCI memory access to unmapped memory.\n");
    }

    /* Only BAR 0 is allowed */
    assert(bar == 0);
    
    if (daddr == 0 && pkt->getSize() == sizeof(Hcr)) {
        HANGU_PRINT(PioEngine, " PioEngine.write: HCR, inparam: 0x%x\n", pkt->getLE<Hcr>().inParam_l);

        regs.inParam.iparaml(pkt->getLE<Hcr>().inParam_l);
        regs.inParam.iparamh(pkt->getLE<Hcr>().inParam_h);
        regs.modifier = pkt->getLE<Hcr>().inMod;
        regs.outParam.oparaml(pkt->getLE<Hcr>().outParam_l);
        regs.outParam.oparamh(pkt->getLE<Hcr>().outParam_h);
        regs.cmdCtrl = pkt->getLE<Hcr>().goOpcode;

        /* Schedule CEU */
        if (!ceuProcEvent.scheduled()) { 
            schedule(ceuProcEvent, curTick() + clockPeriod());
        }

    } else if (daddr == 0x18 && pkt->getSize() == sizeof(uint64_t)) {

        /*  Used to Record start of time */
        HANGU_PRINT(HanGuRnic, " PioEngine.write: Doorbell, value %#X pio interval %ld\n", pkt->getLE<uint64_t>(), curTick() - this->tick); 
        
        regs.db._data = pkt->getLE<uint64_t>();
        
        DoorbellPtr dbell = make_shared<DoorbellFifo>(regs.db.opcode(), 
            regs.db.num(), regs.db.qpn(), regs.db.offset());
        pio2ccuDbFifo.push(dbell);

        /* Record last tick */
        this->tick = curTick();

        /* Schedule doorbellProc */
        if (!doorbellProcEvent.scheduled()) { 
            schedule(doorbellProcEvent, curTick() + clockPeriod());
        }

        HANGU_PRINT(HanGuRnic, " PioEngine.write: qpn %d, opcode %x, num %d\n", 
                regs.db.qpn(), regs.db.opcode(), regs.db.num());
    } else if (daddr == 0x20 && pkt->getSize() == sizeof(uint32_t)) { /* latency sync */
        
        HANGU_PRINT(HanGuRnic, " PioEngine.write: sync bit, value %#X, syncCnt %d\n", pkt->getLE<uint32_t>(), syncCnt); 
        
        if (pkt->getLE<uint32_t>() == 1) {
            syncCnt += 1;
            assert(syncCnt <= cpuNum);
            if (syncCnt == cpuNum) {
                syncSucc = 1;
            }
        } else {
            assert(syncCnt > 0);
            syncCnt -= 1;
            if (syncCnt == 0) {
                syncSucc = 0;
            }
        }

        HANGU_PRINT(HanGuRnic, " PioEngine.write: sync bit end, value %#X, syncCnt %d\n", pkt->getLE<uint32_t>(), syncCnt); 
    } else {
        panic("Write request to unknown address : %#x && size 0x%x\n", daddr, pkt->getSize());
    }

    pkt->makeAtomicResponse();
    return pioDelay;
}
///////////////////////////// HanGuRnic::PIO relevant {end}//////////////////////////////

///////////////////////////// HanGuRnic::CCU relevant {begin}//////////////////////////////

void
HanGuRnic::mboxFetchCpl () {

    HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl!\n");
    switch (regs.cmdCtrl.op()) {
      case INIT_ICM :
        HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl: INIT_ICM command!\n");
        regs.mptBase   = ((InitResc *)mboxBuf)->mptBase;
        regs.mttBase   = ((InitResc *)mboxBuf)->mttBase;
        regs.qpcBase   = ((InitResc *)mboxBuf)->qpcBase;
        regs.cqcBase   = ((InitResc *)mboxBuf)->cqcBase;
        regs.mptNumLog = ((InitResc *)mboxBuf)->mptNumLog;
        regs.qpcNumLog = ((InitResc *)mboxBuf)->qpsNumLog;
        regs.cqcNumLog = ((InitResc *)mboxBuf)->cqsNumLog;
        mrRescModule.mptCache.setBase(regs.mptBase);
        mrRescModule.mttCache.setBase(regs.mttBase);
        qpcModule.setBase(regs.qpcBase);
        cqcModule.cqcCache.setBase(regs.cqcBase);
        break;
      case WRITE_ICM:
        // HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl: WRITE_ICM command! outparam %d, mod %d\n", 
        //         regs.outParam.oparaml(), regs.modifier);
        
        switch (regs.outParam.oparaml()) {
          case ICMTYPE_MPT:
            HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl: ICMTYPE_MPT command!\n");
            mrRescModule.mptCache.icmStore((IcmResc *)mboxBuf, regs.modifier);
            break;
          case ICMTYPE_MTT:
            HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl: ICMTYPE_MTT command!\n");
            mrRescModule.mttCache.icmStore((IcmResc *)mboxBuf, regs.modifier);
            break;
          case ICMTYPE_QPC:
            HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl: ICMTYPE_QPC command!\n");
            qpcModule.icmStore((IcmResc *)mboxBuf, regs.modifier);
            break;
          case ICMTYPE_CQC:
            HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl: ICMTYPE_CQC command!\n");
            cqcModule.cqcCache.icmStore((IcmResc *)mboxBuf, regs.modifier);
            break;
          default: /* ICM mapping do not belong any Resources. */
            panic("ICM mapping do not belong any Resources.\n");
        }
        break;
      case WRITE_MPT:
        HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl: WRITE_MPT command! mod %d ouParam %d\n", regs.modifier, regs.outParam._data);
        for (int i = 0; i < regs.outParam._data; ++i) {
            MptResc *tmp = (((MptResc *)mboxBuf) + i);
            HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl: WRITE_MPT command! mpt_index %d tmp_addr 0x%lx\n", tmp->key, (uintptr_t)tmp);
            mrRescModule.mptCache.rescWrite(tmp->key, tmp);
        }
        break;
      case WRITE_MTT:
        HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl: WRITE_MTT command!\n");
        for (int i = 0; i < regs.outParam._data; ++i) {
            mrRescModule.mttCache.rescWrite(regs.modifier + i, ((MttResc *)mboxBuf) + i);
        }
        break;
      case WRITE_QPC:
        HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl: WRITE_QPC command! 0x%lx\n", (uintptr_t)mboxBuf);
        for (int i = 0; i < regs.outParam._data; ++i) {
            CxtReqRspPtr qpcReq = make_shared<CxtReqRsp>(CXT_CREQ_QP, CXT_CHNL_TX, 0); /* last param is useless here */
            qpcReq->txQpcReq = new QpcResc;
            memcpy(qpcReq->txQpcReq, (((QpcResc *)mboxBuf) + i), sizeof(QpcResc));
            HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl: WRITE_QPC command! i %d qpn 0x%x(%d), addr 0x%lx\n", 
                    i, qpcReq->txQpcReq->srcQpn, qpcReq->txQpcReq->srcQpn&QPN_MASK, (uintptr_t)qpcReq->txQpcReq);
            qpcReq->num = qpcReq->txQpcReq->srcQpn;
            qpcModule.postQpcReq(qpcReq); /* post create request to qpcModule */
        }
        delete[] mboxBuf;
        break;
      case WRITE_CQC:
        HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl: WRITE_CQC command! regs_mod %d mb 0x%lx\n", regs.modifier,  (uintptr_t)mboxBuf);
        cqcModule.cqcCache.rescWrite(regs.modifier, (CqcResc *)mboxBuf);
        break;
      default:
        panic("Bad inputed command.\n");
    }
    regs.cmdCtrl.go(0); // Set command indicator as finished.

    HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl: `GO` bit is down!\n");

    // delete[] mboxBuf;
}

void
HanGuRnic::ceuProc () {
    
    HANGU_PRINT(CcuEngine, " CcuEngine.ceuProc!\n");

    int size;
    switch (regs.cmdCtrl.op()) {
      case INIT_ICM :
        size = sizeof(InitResc); // MBOX_INIT_SZ;
        mboxBuf = (uint8_t *)new InitResc;
        HANGU_PRINT(CcuEngine, " CcuEngine.ceuProc: INIT_ICM command!\n");
        break;
      case WRITE_ICM:
        HANGU_PRINT(CcuEngine, " CcuEngine.ceuProc: WRITE_ICM command!\n");
        size = regs.modifier * sizeof(IcmResc); // regs.modifier * MBOX_ICM_ENTRY_SZ;
        mboxBuf = (uint8_t *)new IcmResc[regs.modifier];
        break;
      case WRITE_MPT:
        HANGU_PRINT(CcuEngine, " CcuEngine.ceuProc: WRITE_MPT command!\n");
        size = regs.outParam._data * sizeof(MptResc);
        mboxBuf = (uint8_t *)new MptResc[regs.outParam._data];
        break;
      case WRITE_MTT:
        HANGU_PRINT(CcuEngine, " CcuEngine.ceuProc: WRITE_MTT command!\n");
        size = regs.outParam._data * sizeof(MttResc);
        mboxBuf = (uint8_t *)new MttResc[regs.outParam._data];
        break;
      case WRITE_QPC:
        HANGU_PRINT(CcuEngine, " CcuEngine.ceuProc: WRITE_QPC command! batch_size %ld\n", regs.outParam._data);
        size = regs.outParam._data * sizeof(QpcResc);
        mboxBuf = (uint8_t *)new QpcResc[regs.outParam._data];
        break;
      case WRITE_CQC:
        HANGU_PRINT(CcuEngine, " CcuEngine.ceuProc: WRITE_CQC command!\n");
        size = sizeof(CqcResc); // MBOX_CQC_ENTRY_SZ;
        mboxBuf = (uint8_t *)new CqcResc;
        break;
      default:
        size = 0;
        panic("Bad input command.\n");
    }

    assert(size > 0 && size <= (MAILBOX_PAGE_NUM << 12)); /* size should not be zero */

    /* read mailbox through dma engine */
    DmaReqPtr dmaReq = make_shared<DmaReq>(pciToDma(regs.inParam._data), size, 
            &mboxEvent, mboxBuf, 0); /* last param is useless here */
    ccuDmaReadFifo.push(dmaReq);
    if (!dmaEngine.dmaReadEvent.scheduled()) {
        schedule(dmaEngine.dmaReadEvent, curTick() + clockPeriod());
    }

    /* We don't schedule it here, cause it should be 
     * scheduled by DMA Engine. */
    // if (!mboxEvent.scheduled()) { /* Schedule mboxFetchCpl */
    //     schedule(mboxEvent, curTick() + clockPeriod());
    // }
}

/**
 * @brief Doorbell Forwarding Unit
 * Forwarding doorbell to RDMAEngine.DFU.
 * Post QPC read request to read relatived QPC information.
 */
void
HanGuRnic::doorbellProc () {

    HANGU_PRINT(HanGuRnic, " CCU.doorbellProc! db_size: %d\n", pio2ccuDbFifo.size());

    /* read doorbell info */
    assert(pio2ccuDbFifo.size());
    // HANGU_PRINT(CcuEngine, "before pio2ccuDbFifo front!\n");
    DoorbellPtr dbell = pio2ccuDbFifo.front();
    assert(dbell != nullptr);
    // HANGU_PRINT(CcuEngine, "RDMA Array name: %s!\n", rdmaArray.name());
    // HANGU_PRINT(CcuEngine, "after pio2ccuDbFifo front!\n");

    uint8_t CoreID;

    // HANGU_PRINT(CcuEngine, "Before Allocating core!\n");
    CoreID = rdmaArray.AllocCore(dbell->qpn);
    HANGU_PRINT(CcuEngine, "RDMA Core Allocated, Core ID: %d!\n", CoreID);

    if (CoreID == INVALID_CORE)
    {
        HANGU_PRINT(CcuEngine, "CCU.doorbellProc, RDMA Core allocation failed, exit the schedule\n");
        return;
    }

    /* If there's no valid idx, exit the schedule */
    if (rdmaArray.df2ccuIdxFifoVec[CoreID].size() == 0) {
        HANGU_PRINT(CcuEngine, "CCU.doorbellProc, If there's no valid idx, exit the schedule\n");
        return;
    }

    // HANGU_PRINT(CcuEngine, " before pio2ccuDbFifo pop!\n");
    pio2ccuDbFifo.pop();
    // HANGU_PRINT(CcuEngine, " pio2ccuDbFifo pop!\n");

    /* Push doorbell to doorbell fifo */
    uint8_t idx = rdmaArray.df2ccuIdxFifoVec[CoreID].front();
    rdmaArray.df2ccuIdxFifoVec[CoreID].pop();
    HANGU_PRINT(CcuEngine, "index got by ccu! rdmaArray.df2ccuIdxFifoVec[%d].size: %d\n", 
        CoreID, rdmaArray.df2ccuIdxFifoVec[CoreID].size());
    rdmaArray.doorbellVectorVec[CoreID][idx] = dbell;
    /* We don't schedule it here, cause it should be 
     * scheduled by Context Module. */
    // if (!rdmaEngine.dfuEvent.scheduled()) { /* Schedule RdmaEngine.dfuProcessing */
    //     schedule(rdmaEngine.dfuEvent, curTick() + clockPeriod());
    // }

    /* Post QP addr request to QpcModule */
    CxtReqRspPtr qpAddrReq = make_shared<CxtReqRsp>(CXT_RREQ_SQ, 
            CXT_CHNL_TX, dbell->qpn, 1, idx, CoreID);
    qpAddrReq->txQpcRsp = new QpcResc;
    // qpcModule.postQpcReq(qpAddrReq); // add core information
    rdmaArray.postQpcReq(qpAddrReq);

    // HANGU_PRINT(CcuEngine, " CCU.doorbellProc: db.qpn %d df2ccuIdxFifo.size %d idx %d\n", 
    //         dbell->qpn, df2ccuIdxFifo.size(), idx);

    /* If there still has elem in fifo, schedule myself again */
    // if (df2ccuIdxFifo.size() && pio2ccuDbFifo.size()) {
    if (pio2ccuDbFifo.size()) {
        if (!doorbellProcEvent.scheduled()) {
            schedule(doorbellProcEvent, curTick() + clockPeriod());
        }
    }

    HANGU_PRINT(CcuEngine, " CCU.doorbellProc: out!\n");
}
///////////////////////////// HanGuRnic::CCU relevant {end}//////////////////////////////

///////////////////////////// HanGuRnic::Resource Cache {begin}//////////////////////////////
template <class T, class S>
uint32_t
HanGuRnic::RescCache<T, S>::replaceScheme() {
    
    uint32_t cnt = random_mt.random(0, (int)cache.size() - 1);
    
    uint32_t rescNum = cache.begin()->first;
    for (auto iter = cache.begin(); iter != cache.end(); ++iter, --cnt) {
        // HANGU_PRINT(RescCache, " RescCache.replaceScheme: num %d, cnt %d\n", iter->first, cnt);
        if (cnt == 0) {
            rescNum = iter->first;
        }
    }

    return rescNum;
}

template <class T, class S>
void 
HanGuRnic::RescCache<T, S>::storeReq(uint64_t addr, T *resc) {

    HANGU_PRINT(RescCache, " storeReq enter\n");
    
    DmaReqPtr dmaReq = make_shared<DmaReq>(rnic->pciToDma(addr), rescSz, 
            nullptr, (uint8_t *)resc, 0); /* rnic->dmaWriteDelay is useless here */
    dmaReq->reqType = 1; /* this is a write request */
    rnic->cacheDmaAccessFifo.push(dmaReq);
    /* Schedule for fetch cached resources through dma read. */
    if (!rnic->dmaEngine.dmaWriteEvent.scheduled()) {
        rnic->schedule(rnic->dmaEngine.dmaWriteEvent, curTick() + rnic->clockPeriod());
    }
}

template <class T, class S>
void 
HanGuRnic::RescCache<T, S>::fetchReq(uint64_t addr, Event *cplEvent, 
        uint32_t rescIdx, S reqPkt, T *rspResc, const std::function<bool(T&)> &rescUpdate) {
    
    HANGU_PRINT(RescCache, "fetchReq enter\n");
    
    T *rescDma = new T; /* This is the origin of resc pointer in cache */
    
    /* Post dma read request to DmaEngine.dmaReadProcessing */
    DmaReqPtr dmaReq = make_shared<DmaReq>(rnic->pciToDma(addr), rescSz, 
            &fetchCplEvent, (uint8_t *)rescDma, 0); /* last parameter is useless here */
    rnic->cacheDmaAccessFifo.push(dmaReq);
    if (!rnic->dmaEngine.dmaReadEvent.scheduled()) {
        rnic->schedule(rnic->dmaEngine.dmaReadEvent, curTick() + rnic->clockPeriod());
    }

    /* push event to fetchRsp */
    rreq2rrspFifo.emplace(cplEvent, rescIdx, rescDma, reqPkt, dmaReq, rspResc, rescUpdate);

    HANGU_PRINT(RescCache, " RescCache.fetchReq: fifo size %d\n", rreq2rrspFifo.size());
}

template <class T, class S>
void 
HanGuRnic::RescCache<T, S>::fetchRsp() {

    HANGU_PRINT(RescCache, " RescCache.fetchRsp! capacity: %d, size %d, rescSz %d\n", capacity, cache.size(), sizeof(T));
    
    if (rreq2rrspFifo.empty()) {
        return ;
    }

    CacheRdPkt rrsp = rreq2rrspFifo.front();
    if (rrsp.dmaReq->rdVld == 0) {
        return;
    }

    rreq2rrspFifo.pop();
    HANGU_PRINT(RescCache, " RescCache.fetchRsp: rescNum %d, dma_addr 0x%lx, rsp_addr 0x%lx, fifo size %d\n", 
            rrsp.rescIdx, (uint64_t)rrsp.rescDma, (uint64_t)rrsp.rspResc, rreq2rrspFifo.size());
    
    
    if (cache.find(rrsp.rescIdx) != cache.end()) { /* It has already been fetched */
        
        /* Abandon fetched resource, and put cache resource 
         * to FIFO. */ 
        memcpy((void *)rrsp.rescDma, (void *)(&(cache[rrsp.rescIdx])), sizeof(T));
        if (rrsp.rspResc) {
            memcpy((void *)rrsp.rspResc, (void *)(&(cache[rrsp.rescIdx])), sizeof(T));
        }
    
    } else { /* rsp Resc is not in cache */

        /* Write new fetched entry to cache */
        if (cache.size() < capacity) {
            cache.emplace(rrsp.rescIdx, *(rrsp.rescDma));
            HANGU_PRINT(RescCache, " RescCache.fetchRsp: capacity %d size %d\n", capacity, cache.size());
        } else { /* Cache is full */

            HANGU_PRINT(RescCache, " RescCache.fetchRsp: Cache is full!\n");
            
            uint32_t wbRescNum = replaceScheme();
            uint64_t pAddr = rescNum2phyAddr(wbRescNum);
            T *wbReq = new T;
            memcpy(wbReq, &(cache[wbRescNum]), sizeof(T));
            storeReq(pAddr, wbReq);

            // Output printing
            if (sizeof(T) == sizeof(struct QpcResc)) {
                struct QpcResc *val = (struct QpcResc *)(rrsp.rescDma);
                struct QpcResc *rep = (struct QpcResc *)(wbReq);
                HANGU_PRINT(RescCache, " RescCache.fetchRsp: qpn 0x%x, sndlkey 0x%x \n\n", 
                        val->srcQpn, val->sndWqeBaseLkey);
                HANGU_PRINT(RescCache, " RescCache.fetchRsp: replaced qpn 0x%x, sndlkey 0x%x \n\n", 
                        rep->srcQpn, rep->sndWqeBaseLkey);
            }
            // T *cptr = rrsp.rescDma;
            // for (int i = 0; i < sizeof(T); ++i) {
            //     HANGU_PRINT(RescCache, " RescCache.fetchRsp: data[%d] 0x%x\n", i, ((uint8_t *)cptr)[i]);
            // }

            cache.erase(wbRescNum);
            cache.emplace(rrsp.rescIdx, *(rrsp.rescDma));
            HANGU_PRINT(RescCache, " RescCache.fetchRsp: capacity %d size %d, replaced idx %d pAddr 0x%lx\n", 
                    capacity, cache.size(), wbRescNum, pAddr);
        }
    
        /* Push fetched resource to FIFO */ 
        if (rrsp.rspResc) {
            memcpy(rrsp.rspResc, rrsp.rescDma, sizeof(T));
        }
    }

    /* Schedule read response cplEvent */
    if (rrsp.cplEvent == nullptr) { // this is a write request
        HANGU_PRINT(RescCache, " RescCache.fetchRsp: this is a write request!\n");
    } else { // this is a read request
        if (!rrsp.cplEvent->scheduled()) {
            rnic->schedule(rrsp.cplEvent, curTick() + rnic->clockPeriod());
        }
        rrspFifo.emplace(rrsp.rescDma, rrsp.reqPkt);
    }
    HANGU_PRINT(RescCache, " RescCache.fetchRsp: Push fetched resource to FIFO!\n");

    /* Update content in cache entry
     * Note that this should be called last because 
     * we hope get older resource, not updated resource */
    if (rrsp.rescUpdate == nullptr) {
        HANGU_PRINT(RescCache, " RescCache.fetchRsp: rescUpdate is null!\n");
    } else {
        HANGU_PRINT(RescCache, " RescCache.fetchRsp: rescUpdate is not null\n");
        rrsp.rescUpdate(cache[rrsp.rescIdx]);
    }

    /* Schdeule myself if we have valid elem */
    if (rreq2rrspFifo.size()) {
        CacheRdPkt rrsp = rreq2rrspFifo.front();
        if (rrsp.dmaReq->rdVld) {
            if (!fetchCplEvent.scheduled()) {
                rnic->schedule(fetchCplEvent, curTick() + rnic->clockPeriod());
            }
        }
    } else { /* schedule readProc if it do not has pending read req **/
        if (!readProcEvent.scheduled()) {
            rnic->schedule(readProcEvent, curTick() + rnic->clockPeriod());
        }
    }

    HANGU_PRINT(RescCache, " RescCache.fetchRsp: out\n");
}

template <class T, class S>
void 
HanGuRnic::RescCache<T, S>::setBase(uint64_t base) {
    baseAddr = base;
}

template <class T, class S>
void 
HanGuRnic::RescCache<T, S>::icmStore(IcmResc *icmResc, uint32_t chunkNum) {
    HANGU_PRINT(RescCache, "icmStore enter\n");

    for (int i = 0; i < chunkNum; ++i) {
        
        uint32_t idx = (icmResc[i].vAddr - baseAddr) >> 12;
        DPRINTF(HanGuRnic, "[HanGuRnic] mbox content: baseAddr 0x%lx, idx 0x%lx\n", baseAddr, idx);
        DPRINTF(HanGuRnic, "[HanGuRnic] mbox content: vaddr 0x%lx\n", icmResc[i].vAddr);
        while (icmResc[i].pageNum) {
            icmPage[idx] = icmResc[i].pAddr;
            DPRINTF(HanGuRnic, "[HanGuRnic] mbox content: pAddr 0x%lx\n", icmResc[i].pAddr);
            
            /* Update param */
            --icmResc[i].pageNum;
            icmResc[i].pAddr += (1 << 12);
            ++idx;
        }
    }

    delete[] icmResc;
}

template <class T, class S>
uint64_t
HanGuRnic::RescCache<T, S>::rescNum2phyAddr(uint32_t num) {
    uint32_t vAddr = num * rescSz;
    uint32_t icmIdx = vAddr >> 12;
    uint32_t offset = vAddr & 0xfff;
    uint64_t pAddr = icmPage[icmIdx] + offset;

    return pAddr;
}

/**
 * @note This Func write back resource and put it back to rrspFifo
 * @param resc resource to be written
 * @param rescIdx resource num
 * @param rescUpdate This is a function pointer, if it is not 
 * nullptr, we execute the function to update cache entries.
 * 
 */
template <class T, class S>
void 
HanGuRnic::RescCache<T, S>::rescWrite(uint32_t rescIdx, T *resc, const std::function<bool(T&, T&)> &rescUpdate) {

    HANGU_PRINT(RescCache, " RescCache.rescWrite! capacity: %d, size: %d rescSz %d, rescIndex %d\n", 
            capacity, cache.size(), sizeof(T), rescIdx);
    // if (sizeof(T) == sizeof(struct QpcResc)) {
    //     for (auto &item : cache) {
    //         uint32_t key = item.first;
    //         struct QpcResc *val = (struct QpcResc *)&(item.second);
    //         HANGU_PRINT(RescCache, " RescCache.rescWrite: cache elem is key 0x%x qpn 0x%x, sndlkey 0x%x \n\n", 
    //                 key, val->srcQpn, val->sndWqeBaseLkey);
    //     }
    // }
    
    if (cache.find(rescIdx) != cache.end()) { /* Cache hit */

        HANGU_PRINT(RescCache, " RescCache.rescWrite: Cache hit\n");
        
        /* If there's specified update function */
        if (rescUpdate == nullptr) {
            T tmp = cache[rescIdx];
            cache.erase(rescIdx);
            delete &tmp;
            cache.emplace(rescIdx, *resc);
            HANGU_PRINT(RescCache, " RescCache.rescWrite: Resc is written\n");
        } else {
            rescUpdate(cache[rescIdx], *resc);
            HANGU_PRINT(RescCache, " RescCache.rescWrite: Desc updated\n");
        }

        // T *cptr = &(cache[rescIdx]);
        // for (int i = 0; i < sizeof(T); ++i) {
        //     HANGU_PRINT(RescCache, " RescCache.rescWrite: data[%d] 0x%x resc 0x%x\n", i, ((uint8_t *)cptr)[i], ((uint8_t *)resc)[i]);
        // }

        HANGU_PRINT(RescCache, " RescCache: capacity %d size %d\n", capacity, cache.size());
    } else if (cache.size() < capacity) { /* Cache miss & insert elem directly */
        HANGU_PRINT(RescCache, " RescCache.rescWrite: Cache miss\n");

        cache.emplace(rescIdx, *resc);
        
        HANGU_PRINT(RescCache, " RescCache: capacity %d size %d\n", capacity, cache.size());
    } else if (cache.size() == capacity) { /* Cache miss & replace */

        HANGU_PRINT(RescCache, " RescCache.rescWrite: Cache miss & replace\n");

        /* Select one elem in cache to evict */
        uint32_t wbRescNum = replaceScheme();
        uint64_t pAddr = rescNum2phyAddr(wbRescNum);
        T *writeReq = new T;
        memcpy(writeReq, &(cache[wbRescNum]), sizeof(T));
        storeReq(pAddr, writeReq);

        // T *cptr = &(cache[wbRescNum]);
        // HANGU_PRINT(RescCache, " RescCache.rescWrite: cptr 0x%lx\n", (uint64_t)cptr);
        // for (int i = 0; i < sizeof(T); ++i) {
        //     HANGU_PRINT(RescCache, " RescCache.rescWrite: data[%d] 0x%x resc 0x%x\n", i, ((uint8_t *)cptr)[i], ((uint8_t *)resc)[i]);
        // }

        // delete &(cache[wbRescNum]); /* It has been written to host memory */
        cache.erase(wbRescNum);
        cache.emplace(rescIdx, *resc);
        HANGU_PRINT(RescCache, " RescCache.rescWrite: wbRescNum %d, ICM_paddr_base 0x%x, new_index %d\n", wbRescNum, pAddr, rescIdx);
        HANGU_PRINT(RescCache, " RescCache: capacity %d size %d\n", capacity, cache.size());
    } else {
        panic(" RescCache.rescWrite: mismatch! capacity %d size %d\n", capacity, cache.size());
    }
}


/**
 * @note This Func get resource and put it back to rrspFifo.
 *      Note that this function returns resc in two data struct:
 *      1. rrspFifo, this Fifo stores reference to the cache.
 *      2. T *rspResc, this input is an optional, which may be "nullptr"
 * @param rescIdx resource num
 * @param cplEvent event to call wehn get desired data.
 * @param rspResc the address to which copy the cache entry
 * @param rescUpdate This is a function pointer, if it is not 
 * nullptr, we execute the function to update cache entries.
 * 
 */
template <class T, class S>
void 
HanGuRnic::RescCache<T, S>::rescRead(uint32_t rescIdx, Event *cplEvent, S reqPkt, T *rspResc, const std::function<bool(T&)> &rescUpdate) {

    HANGU_PRINT(RescCache, " RescCache.rescRead! capacity: %d, rescIdx %d, is_write %d, rescSz: %d, size: %d\n", 
            capacity, rescIdx, (cplEvent == nullptr), sizeof(T), cache.size());

    /* push event to fetchRsp */
    reqFifo.emplace(cplEvent, rescIdx, nullptr, reqPkt, nullptr, rspResc, rescUpdate);

    if (!readProcEvent.scheduled()) {
        rnic->schedule(readProcEvent, curTick() + rnic->clockPeriod());
    }

    HANGU_PRINT(RescCache, " RescCache.rescRead: out!\n");
}

/**
 * @note This Func get resource req and put it back to rrspFifo.
 *      Note that this function returns resc in two data struct:
 *      1. rrspFifo, this Fifo stores reference to the cache.
 *      2. T *rspResc, this input is an optional, which may be "nullptr"
 */
template <class T, class S>
void 
HanGuRnic::RescCache<T, S>::readProc() {

    /* If there's pending read req or there's no req in reqFifo, 
     * do not process next rquest */
    if (rreq2rrspFifo.size() || reqFifo.empty()) {
        return;
    }

    /* Get cache rd req pkt from reqFifo */
    CacheRdPkt rreq = reqFifo.front();
    uint32_t rescIdx = rreq.rescIdx;
    reqFifo.pop();

    /* only used to dump information */
    HANGU_PRINT(RescCache, " RescCache.readProc! capacity: %d, rescIdx %d, is_write %d, rescSz: %d, size: %d\n", 
            capacity, rescIdx, (rreq.cplEvent == nullptr), sizeof(T), cache.size());
    // if (sizeof(T) == sizeof(struct QpcResc)) {
    //     for (auto &item : cache) {
    //         uint32_t key = item.first;
    //         struct QpcResc *val = (struct QpcResc *)&(item.second);
    //         HANGU_PRINT(RescCache, " RescCache.readProc0: cache elem is key 0x%x qpn 0x%x, sndlPsn %d \n\n", 
    //                 key, val->srcQpn, val->sndPsn);
    //     }
    // }

    if (cache.find(rescIdx) != cache.end()) { /* Cache hit */
        HANGU_PRINT(RescCache, " RescCache.readProc: Cache hit\n");
        
        /** 
         * If rspResc is not nullptr, which means 
         * it need to put resc to rspResc, copy 
         * data in cache entry.
         */
        if (rreq.rspResc) {
            memcpy(rreq.rspResc, &cache[rescIdx], sizeof(T));
        }

        if (rreq.cplEvent == nullptr) { // This is write request
            HANGU_PRINT(RescCache, " RescCache.readProc: This is write request\n");
        } else { // This is read request
            HANGU_PRINT(RescCache, " RescCache.readProc: This is read request\n");
            
            T *rescBack = new T;
             memcpy(rescBack, &cache[rescIdx], sizeof(T));

            /* Schedule read response event */
            if (!rreq.cplEvent->scheduled()) {
                rnic->schedule(*(rreq.cplEvent), curTick() + rnic->clockPeriod());
            }
            rrspFifo.emplace(rescBack, rreq.reqPkt);
        }

        /* Note that this should be called last because 
         * we hope get older resource, not updated resource */
        if (rreq.rescUpdate) {
            rreq.rescUpdate(cache[rescIdx]);
        }

        /* cache hit, so we can schedule next request in reqFifo */
        if (reqFifo.size()) {
            if (!readProcEvent.scheduled()) {
                rnic->schedule(readProcEvent, curTick() + rnic->clockPeriod());
            }
        }

        // T *cptr = &(cache[rescIdx]);
        // for (int i = 0; i < sizeof(T); ++i) {
        //     HANGU_PRINT(RescCache, " RescCache.rescRead: data[%d] 0x%x\n", i, ((uint8_t *)cptr)[i]);
        // }

    } else if (cache.size() <= capacity) { /* Cache miss & read elem */
        HANGU_PRINT(RescCache, " RescCache.readProc: Cache miss & read elem!\n");
        
        /* Fetch required data */
        uint64_t pAddr = rescNum2phyAddr(rescIdx);
        fetchReq(pAddr, rreq.cplEvent, rescIdx, rreq.reqPkt, rreq.rspResc, rreq.rescUpdate);

        HANGU_PRINT(RescCache, " RescCache.readProc: resc_index %d, ICM paddr 0x%lx\n", rescIdx, pAddr);

    } else {
        panic(" RescCache.readProc: mismatch! capacity %d size %d\n", capacity, cache.size());
    }

    HANGU_PRINT(RescCache, " RescCache.readProc: out! capacity: %d, size: %d\n", capacity, cache.size());
}

///////////////////////////// HanGuRnic::Resource Cache {end}//////////////////////////////


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
HanGuRnic::MrRescModule::dmaReqProcess (uint64_t pAddr, MrReqRspPtr mrReq) {
    
    HANGU_PRINT(MrResc, " MrRescModule.dmaReqProcess!\n");
    
    if (mrReq->type == DMA_TYPE_WREQ) {

        /* Post dma req to DMA engine */
        DmaReqPtr dmaWreq;
        switch (mrReq->chnl) {
          case TPT_WCHNL_TX_CQUE:
          case TPT_WCHNL_RX_CQUE:
            dmaWreq = make_shared<DmaReq>(rnic->pciToDma(pAddr), mrReq->length, 
                    nullptr, mrReq->data, 0); /* last parameter is useless here */
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

            dmaWreq = make_shared<DmaReq>(rnic->pciToDma(pAddr), mrReq->length, 
                    nullptr, mrReq->data, 0); /* last parameter is useless here */
            HANGU_PRINT(MrResc, " MrRescModule.dmaReqProcess: write data Request, dmaReq->paddr is 0x%lx, offset %d, size %d\n", 
                    dmaWreq->addr, mrReq->offset, mrReq->length);
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
                    &dmaRrspEvent, mrReq->data, 0); /* last parameter is useless here */
            rnic->descDmaReadFifo.push(dmaRdReq);
            
            break;
          case MR_RCHNL_TX_DATA:
          case MR_RCHNL_RX_DATA:

            HANGU_PRINT(MrResc, " MrRescModule.dmaReqProcess: read data request\n");

            /* Post data dma req to DMA engine */
            dmaRdReq = make_shared<DmaReq>(rnic->pciToDma(pAddr), mrReq->length, 
                    &dmaRrspEvent, mrReq->data, 0); /* last parameter is useless here */
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
            event = &rnic->rdmaArray.txdduDescRspEvent;

            for (uint32_t i = 0; (i * sizeof(TxDesc)) < tptRsp->length; ++i) {
                txDesc = make_shared<TxDesc>(tptRsp->txDescRsp + i);
                assert((txDesc->len != 0) && (txDesc->lVaddr != 0) && (txDesc->opcode != 0));
                // rnic->txdescRspFifo.push(txDesc);
            }
            assert(tptRsp->coreID != INVALID_CORE);
            rnic->txdescRspFifo.push(tptRsp);
            // assert((tptRsp->txDescRsp->len != 0) && (tptRsp->txDescRsp->lVaddr != 0));
            // rnic->txdescRspFifo.push(tptRsp->txDescRsp);

            HANGU_PRINT(MrResc, " MrRescModule.dmaRrspProcessing: size is %d, desc total len is %d!\n", 
                    rnic->txdescRspFifo.size(), tptRsp->length);

            break;
        case MR_RCHNL_RX_DESC:
            // event = &rnic->rdmaEngine.rcvRpuEvent;
            event = &rnic->rdmaArray.rxDescRdRspEvent;
            for (uint32_t i = 0; (i * sizeof(RxDesc)) < tptRsp->length; ++i) {
                rxDesc = make_shared<RxDesc>(tptRsp->rxDescRsp + i);
                assert((rxDesc->len != 0) && (rxDesc->lVaddr != 0));
                // rnic->rxdescRspFifo.push(rxDesc);
            }
            rnic->rxdescRspFifo.push(tptRsp);
            // delete tptRsp->rxDescRsp;

            HANGU_PRINT(MrResc, " MrRescModule.dmaRrspProcessing: rnic->rxdescRspFifo.size() is %d!\n", 
                    rnic->rxdescRspFifo.size());

            break;
        case MR_RCHNL_TX_DATA:
            // event = &rnic->rdmaEngine.rgrrEvent;
            event = &rnic->rdmaArray.txDataRdRspEvent;
            rnic->txdataRspFifo.push(tptRsp);
        
            break;
        case MR_RCHNL_RX_DATA:
            // event = &rnic->rdmaEngine.rdCplRpuEvent;
            event = &rnic->rdmaArray.rdRpuDataRdRspEvent;
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
    reqPkt->offset = reqPkt->offset & 0xFFF;
    uint64_t mttIdx = mptResc->mttSeg;
    HANGU_PRINT(MrResc, " MrRescModule.mptRspProcessing: reqPkt->offset 0x%x, mptResc->startVAddr 0x%x, mptResc->mttSeg 0x%x, mttIdx 0x%x\n", 
            reqPkt->offset, mptResc->startVAddr, mptResc->mttSeg, mttIdx);

    /* Post mtt req */
    mttReqProcess(mttIdx, reqPkt);

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
    dmaReqProcess(mttResc->pAddr + reqPkt->offset, reqPkt);

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


///////////////////////////// HanGuRnic::CqcModule {begin}//////////////////////////////
bool 
HanGuRnic::CqcModule::postCqcReq(CxtReqRspPtr cqcReq) {

    assert(cqcReq->type == CXT_RREQ_CQ);

    if (cqcReq->chnl == CXT_CHNL_TX) {
        rnic->txCqcReqFifo.push(cqcReq);
    } else if (cqcReq->chnl == CXT_CHNL_RX) {
        rnic->rxCqcReqFifo.push(cqcReq);
    } else {
        panic("[CqcModule]: cqcReq->chnl error! %d", cqcReq->chnl);
    }

    if (!cqcReqProcEvent.scheduled()) { /* Schedule cqcReqProc() */
        rnic->schedule(cqcReqProcEvent, curTick() + rnic->clockPeriod());
    }

    return true;
}

void 
HanGuRnic::CqcModule::cqcRspProc() {

    HANGU_PRINT(CxtResc, " CqcModule.cqcRspProc!\n");

    assert(cqcCache.rrspFifo.size());
    CxtReqRspPtr cqcRsp = cqcCache.rrspFifo.front().second;
    cqcCache.rrspFifo.pop();
    uint8_t type = cqcRsp->type, chnl = cqcRsp->chnl;
    Event *e;

    /* Get event and push cqcRsp to relevant Fifo */
    if (type == CXT_RREQ_CQ && chnl == CXT_CHNL_TX) {
        cqcRsp->type = CXT_RRSP_CQ;
        // e = &rnic->rdmaEngine.scuEvent;
        e = &rnic->rdmaArray.sendCqcRspEvent;

        rnic->txCqcRspFifo.push(cqcRsp);
    } else if (type == CXT_RREQ_CQ && chnl == CXT_CHNL_RX) {
        cqcRsp->type = CXT_RRSP_CQ;
        // e = &rnic->rdmaEngine.rcuEvent;
        
        rnic->rxCqcRspFifo.push(cqcRsp);
        e = &rnic->rdmaArray.rcvCqcRdRspEvent;
    } else {
        panic("[CqcModule]: cxtReq type error! type: %d, chnl %d", type, chnl);
    }
    
    /* schedule related module to retruen read rsp cqc */
    if (!e->scheduled()) {
        rnic->schedule(*e, curTick() + rnic->clockPeriod());
    }

    /* If there's still has elem to be 
     * processed, reschedule myself */
    if (cqcCache.rrspFifo.size()) {
        if (!cqcRspProcEvent.scheduled()) {/* Schedule myself */
            rnic->schedule(cqcRspProcEvent, curTick() + rnic->clockPeriod());
        }
    }

    HANGU_PRINT(CxtResc, " CqcModule.cqcRspProc: out!\n");
}

/* cqc update function used in lambda expression */
bool cqcReadUpdate(CqcResc &resc) {

    resc.offset += sizeof(CqDesc);

    if (resc.offset + sizeof(CqDesc) > (1 << resc.sizeLog)) {
        resc.offset = 0;
    }
    return true;
}

void
HanGuRnic::CqcModule::cqcReqProc() {

    HANGU_PRINT(CxtResc, " CqcModule.cqcReqProc!\n");

    uint8_t CHNL_NUM = 2;
    bool isEmpty[CHNL_NUM];
    isEmpty[0] = rnic->txCqcReqFifo.empty();
    isEmpty[1] = rnic->rxCqcReqFifo.empty();

    CxtReqRspPtr cqcReq;
    for (uint8_t cnt = 0; cnt < CHNL_NUM; ++cnt) {
        if (isEmpty[chnlIdx] == false) {
            switch (chnlIdx) {
              case 0:
                cqcReq = rnic->txCqcReqFifo.front();
                rnic->txCqcReqFifo.pop();
                assert(cqcReq->chnl == CXT_CHNL_TX);

                HANGU_PRINT(CxtResc, " CqcModule.cqcReqProc: tx CQC read req posted!\n");
                break;
              case 1:
                cqcReq = rnic->rxCqcReqFifo.front();
                rnic->rxCqcReqFifo.pop();
                assert(cqcReq->chnl == CXT_CHNL_RX);

                HANGU_PRINT(CxtResc, " CqcModule.cqcReqProc: rx CQC read req posted!\n");
                break;
            }

            assert(cqcReq->type == CXT_RREQ_CQ);

            /* Read CQC from CQC Cache */
            cqcCache.rescRead(cqcReq->num, &cqcRspProcEvent, cqcReq, cqcReq->txCqcRsp, [](CqcResc &resc) -> bool { return cqcReadUpdate(resc); });

            /* Point to next chnl */
            ++chnlIdx;
            chnlIdx = chnlIdx % CHNL_NUM;

            /* Schedule myself again if there still has elem in fifo */
            if (rnic->txCqcReqFifo.size() || rnic->rxCqcReqFifo.size()) {
                if (!cqcReqProcEvent.scheduled()) { /* schedule myself */
                    rnic->schedule(cqcReqProcEvent, curTick() + rnic->clockPeriod());
                }
            }
            
            HANGU_PRINT(CxtResc, " CqcModule.cqcReqProc: out!\n");

            return;
        } else {
            /* Point to next chnl */
            ++chnlIdx;
            chnlIdx = chnlIdx % CHNL_NUM;
        }
    }
}
///////////////////////////// HanGuRnic::CqcModule {end}//////////////////////////////

///////////////////////////// HanGuRnic::Cache {begin}//////////////////////////////
template<class T>
uint32_t 
HanGuRnic::Cache<T>::replaceEntry() {

    uint64_t min = seq_end;
    uint32_t rescNum = cache.begin()->first;
    for (auto iter = cache.begin(); iter != cache.end(); ++iter) { // std::unordered_map<uint32_t, std::pair<T*, uint64_t>>::iterator
        if (min >= iter->second.second) {
            rescNum = iter->first;
        }
    }
    HANGU_PRINT(CxtResc, " HanGuRnic.Cache.replaceEntry: out! %d\n", rescNum);
    return rescNum;

    // uint32_t cnt = random_mt.random(0, (int)cache.size() - 1);
    
    // uint32_t rescNum = cache.begin()->first;
    // for (auto iter = cache.begin(); iter != cache.end(); ++iter, --cnt) {
    //     if (cnt == 0) {
    //         rescNum = iter->first;
    //     }
    // }
    // HANGU_PRINT(CxtResc, " HanGuRnic.Cache.replaceEntry: out!\n");
    // return rescNum;
}

template<class T>
bool 
HanGuRnic::Cache<T>::lookupHit(uint32_t entryNum) {
    bool res = (cache.find(entryNum) != cache.end());
    if (res) { /* if hit update the state of the entry */
        cache[entryNum].second = seq_end++;
    }
    return res;
}

template<class T>
bool 
HanGuRnic::Cache<T>::lookupFull(uint32_t entryNum) {
    return cache.size() == capacity;
}

template<class T>
bool 
HanGuRnic::Cache<T>::readEntry(uint32_t entryNum, T* entry) {
    assert(cache.find(entryNum) != cache.end());

    memcpy(entry, cache[entryNum].first, sizeof(T));
    return true;
}

template<class T>
bool 
HanGuRnic::Cache<T>::updateEntry(uint32_t entryNum, const std::function<bool(T&)> &update) {
    assert(cache.find(entryNum) != cache.end());
    assert(update != nullptr);

    return update(*cache[entryNum].first);
}

template<class T>
bool 
HanGuRnic::Cache<T>::writeEntry(uint32_t entryNum, T* entry) {
    assert(cache.find(entryNum) == cache.end()); /* could not find this entry in default */

    T *val = new T;
    memcpy(val, entry, sizeof(T));
    cache.emplace(entryNum, make_pair(val, seq_end++));

    // for (auto &item : cache) {
    //     uint32_t key = item.first;
    //     QpcResc *val  = (QpcResc *)item.second;
    //     HANGU_PRINT(CxtResc, " HanGuRnic.Cache.writeEntry: key %d srcQpn %d firstPsn %d\n\n", 
    //             key, val->srcQpn, val->sndPsn);
    // }
    return true;
}

/* delete entry in cache */
template<class T>
T* 
HanGuRnic::Cache<T>::deleteEntry(uint32_t entryNum) {
    assert(cache.find(entryNum) != cache.end());
    
    T *rtnResc = cache[entryNum].first;
    cache.erase(entryNum);
    assert(cache.size() == capacity - 1);
    return rtnResc;
}
///////////////////////////// HanGuRnic::Cache {end}//////////////////////////////

///////////////////////////// HanGuRnic::PendingStruct {begin}//////////////////////////////
void 
HanGuRnic::PendingStruct::swapIdx() {
    uint8_t tmp = onlineIdx;
    onlineIdx   = offlineIdx;
    offlineIdx  = tmp;
    assert(onlineIdx != offlineIdx);
}

void 
HanGuRnic::PendingStruct::pushElemProc() {
    
    assert(pushFifo.size());
    PendingElemPtr pElem = pushFifo.front();
    CxtReqRspPtr qpcReq = pElem->reqPkt;
    pushFifo.pop();

    HANGU_PRINT(CxtResc, " PendingStruct.pushElemProc: qpn %d idx %d chnl %d\n", pElem->qpn, pElem->idx, pElem->chnl);
    assert((pElem->qpn & QPN_MASK) <= QPN_NUM);
    assert((pElem->reqPkt->num & QPN_MASK) <= QPN_NUM);
    assert(qpcReq != nullptr);

    /* post pElem to pendingFifo */
    ++elemNum;
    if (pendingFifo[offlineIdx].size()) {
        pendingFifo[offlineIdx].push(pElem);
    } else {
        pendingFifo[onlineIdx].push(pElem);
    }

    /* schedule loadMem to post qpcReq dma pkt to dma engine */
    HANGU_PRINT(CxtResc, " PendingStruct.pushElemProc: has_dma %d\n", pElem->has_dma);
    if (pElem->has_dma) {
        rnic->qpcModule.loadMem(qpcReq);
    }

    /* If there are elem in fifo, schedule myself again */
    if (pushFifo.size()) {
        if (!pushElemProcEvent.scheduled()) {
            rnic->schedule(pushElemProcEvent, curTick() + rnic->clockPeriod());
        }
    }
    HANGU_PRINT(CxtResc, " PendingStruct.pushElemProc: out!\n");
}

bool 
HanGuRnic::PendingStruct::push_elem(PendingElemPtr pElem) {
    pushFifo.push(pElem);
    if (!pushElemProcEvent.scheduled()) {
        rnic->schedule(pushElemProcEvent, curTick() + rnic->clockPeriod());
    }
    return true;
}

// return first elem in the fifo, the elem is not removed
PendingElemPtr 
HanGuRnic::PendingStruct::front_elem() {
    assert(pendingFifo[onlineIdx].size());
    /* read first pElem from pendingFifo */
    return pendingFifo[onlineIdx].front();
}

/* return first elem in the fifo, the elem is removed.
 * Note that if onlinePending is empty && offlinePending 
 * has elem, swap onlineIdx && offlineIdx */
PendingElemPtr 
HanGuRnic::PendingStruct::pop_elem() {
    
    assert(pendingFifo[onlineIdx].size() > 0);
    PendingElemPtr pElem = pendingFifo[onlineIdx].front();
    pendingFifo[onlineIdx].pop();
    --elemNum;

    /* if onlinePend empty, and offlinePend has elem, swap onlineIdx and offlineIdx */
    if (pendingFifo[onlineIdx].empty() && pendingFifo[offlineIdx].size()) {
        /* swap onlineIdx and offlineIdx */
        swapIdx();
    }

    HANGU_PRINT(CxtResc, " PendingStruct.pop_elem: exit, get_size() %d elemNum %d\n", get_size(), elemNum);
    
    return pElem;
}

/* read && pop one elem from offlinePending (to check the element) */
PendingElemPtr 
HanGuRnic::PendingStruct::get_elem_check() {

    assert(pendingFifo[offlineIdx].size() || pendingFifo[onlineIdx].size());
    
    if (pendingFifo[offlineIdx].empty()) {
        /* swap onlineIdx and offlineIdx */
        swapIdx();
    }
    PendingElemPtr pElem = pendingFifo[offlineIdx].front();
    pendingFifo[offlineIdx].pop();
    --elemNum;

    HANGU_PRINT(CxtResc, " QpcModule.PendingStruct.get_elem_check: exit\n");
    return pElem;
}

/* and push to the online pendingFifo */
void 
HanGuRnic::PendingStruct::ignore_elem_check(PendingElemPtr pElem) {
    pendingFifo[onlineIdx].push(pElem);
    ++elemNum;

    HANGU_PRINT(CxtResc, " QpcModule.PendingStruct.ignore_elem_check: exit\n");
}

/* if it is the first, swap online and offline pendingFifo */
void 
HanGuRnic::PendingStruct::succ_elem_check() {
    if (pendingFifo[onlineIdx].size() == 0) {
        /* swap onlineIdx and offlineIdx */
        swapIdx();
    }
    HANGU_PRINT(CxtResc, " QpcModule.PendingStruct.succ_elem_check: get_size %d, elemNum %d\n", 
            get_size(), elemNum);
}

/* call push_elem */
void 
HanGuRnic::PendingStruct::push_elem_check(PendingElemPtr pElem) {
    if (pendingFifo[onlineIdx].size() == 0) {
        /* swap onlineIdx and offlineIdx */
        swapIdx();
    }
    push_elem(pElem);
    HANGU_PRINT(CxtResc, " QpcModule.PendingStruct.push_elem_check: exit\n");
}
///////////////////////////// HanGuRnic::PendingStruct {end}//////////////////////////////

///////////////////////////// HanGuRnic::QpcModule {begin}//////////////////////////////
bool 
HanGuRnic::QpcModule::postQpcReq(CxtReqRspPtr qpcReq) {

    assert( (qpcReq->type == CXT_WREQ_QP) || 
            (qpcReq->type == CXT_RREQ_QP) || 
            (qpcReq->type == CXT_RREQ_SQ) ||
            (qpcReq->type == CXT_CREQ_QP));
    assert( (qpcReq->chnl == CXT_CHNL_TX) || 
            (qpcReq->chnl == CXT_CHNL_RX));

    if (qpcReq->type == CXT_CREQ_QP) {
        ccuQpcWreqFifo.push(qpcReq);
    } else if (qpcReq->chnl == CXT_CHNL_TX && qpcReq->type == CXT_RREQ_SQ) {
        txQpAddrRreqFifo.push(qpcReq);
    } else if (qpcReq->chnl == CXT_CHNL_TX && qpcReq->type == CXT_RREQ_QP) {
        txQpcRreqFifo.push(qpcReq);
    } else if (qpcReq->chnl == CXT_CHNL_RX && qpcReq->type == CXT_RREQ_QP) {
        rxQpcRreqFifo.push(qpcReq);
    } else {
        panic("[QpcModule.postQpcReq] invalid chnl %d or type %d", qpcReq->chnl, qpcReq->type);
    }

    if (!qpcReqProcEvent.scheduled()) { /* Schedule qpcReqProc() */
        rnic->schedule(qpcReqProcEvent, curTick() + rnic->clockPeriod());
    }

    return true;
}

bool qpcTxUpdate (QpcResc &resc, uint32_t sz) {
    if (resc.qpType == QP_TYPE_RC) {
        resc.ackPsn += sz;
        resc.sndPsn += sz;
    }
    resc.sndWqeOffset += sz * sizeof(TxDesc);
    if (resc.sndWqeOffset + sizeof(TxDesc) > (1 << resc.sqSizeLog)) {
        resc.sndWqeOffset = 0; /* Same as in userspace drivers */
    }
    
    return true;
}

bool qpcRxUpdate (QpcResc &resc) {
    if (resc.qpType == QP_TYPE_RC) {
        resc.expPsn += 1;
    }
    resc.rcvWqeOffset += sizeof(RxDesc);
    if (resc.rcvWqeOffset + sizeof(RxDesc) > (1 << resc.rqSizeLog)) {
        resc.rcvWqeOffset = 0; /* Same as in userspace drivers */
    }
    
    assert(resc.rqSizeLog == 12);
    return true;
}

void 
HanGuRnic::QpcModule::hitProc(uint8_t chnlNum, CxtReqRspPtr qpcReq) {
    qpcCache.readEntry(qpcReq->num, qpcReq->txQpcRsp);
    assert(qpcReq->num == qpcReq->txQpcRsp->srcQpn);

    HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.readProc.hitProc: qpn %d hit, chnlNum %d idx %d\n", 
            qpcReq->txQpcRsp->srcQpn, chnlNum, qpcReq->idx);

    /* Post rsp to related fifo, schedule related rsp receiving module */
    Event *e;
    if (chnlNum == 0) { // txQpAddrRspFifo
        txQpAddrRspFifo.push(qpcReq);
        // e = &rnic->rdmaEngine.dfuEvent;
        e = &rnic->rdmaArray.txQpAddrRspEvent;
    } else if (chnlNum == 1) { // txQpcRspFifo
        /* update after read */
        uint32_t sz = qpcReq->sz;
        qpcCache.updateEntry(qpcReq->num, [sz](QpcResc &qpc) { return qpcTxUpdate(qpc, sz); });

        txQpcRspFifo.push(qpcReq);
        // e = &rnic->rdmaEngine.dpuEvent;
        e = &rnic->rdmaArray.txQpcRspEvent;
    } else if (chnlNum == 2) { // rxQpcRspFifo
        /* update after read */
        qpcCache.updateEntry(qpcReq->num, [](QpcResc &qpc) { return qpcRxUpdate(qpc); });

        rxQpcRspFifo.push(qpcReq);
        e = &rnic->rdmaArray.recvQpcRspEvent;
        // e = &rnic->rdmaEngine.rpuEvent;
    } else {
        panic("[QpcModule.readProc.hitProc] Unrecognized chnl %d or type %d", qpcReq->chnl, qpcReq->type);
    }

    if (!e->scheduled()) {
        rnic->schedule(*e, curTick() + rnic->clockPeriod());
    }
}

bool 
HanGuRnic::QpcModule::readProc(uint8_t chnlNum, CxtReqRspPtr qpcReq) {

    HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.readProc!\n");

    /* Lookup qpnHashMap to learn that if there's 
     * pending elem for this qpn in this channel. */
    if (qpnHashMap.find(qpcReq->num) != qpnHashMap.end()) { /* related qpn is fond in qpnHashMap */
        qpnHashMap[qpcReq->num]->reqCnt += 1;

        HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.readProc: related qpn is found in qpnHashMap qpn %d idx %d\n", 
                qpcReq->num, qpcReq->idx);

        HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.readProc: qpnMap.size() %d get_size() %d\n", 
                qpnHashMap.size(), pendStruct.get_size());
        /* save req to pending fifo */
        PendingElemPtr pElem =  make_shared<PendingElem>(qpcReq->idx, chnlNum, qpcReq, false); // new PendingElem(qpcReq->idx, chnlNum, qpcReq, false);
        pendStruct.push_elem(pElem);
        HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.readProc: qpnMap.size() %d get_size() %d\n", 
                qpnHashMap.size(), pendStruct.get_size());

        return true;
    }

    /* Lookup QPC in QPC Cache */
    if (qpcCache.lookupHit(qpcReq->num)) { /* cache hit, and return related rsp to related fifo */

        HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.readProc: cache hit, qpn %d\n", qpcReq->num);

        hitProc(chnlNum, qpcReq);
    } else { /* cache miss */

        HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.readProc: cache miss, qpn %d, rtnCnt %d\n", qpcReq->num, rtnCnt);

        /* save req to pending fifo */
        HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.readProc: qpnMap.size() %d get_size() %d\n", 
                qpnHashMap.size(), pendStruct.get_size());
        PendingElemPtr pElem = make_shared<PendingElem>(qpcReq->idx, chnlNum, qpcReq, true); // new PendingElem(qpcReq->idx, chnlNum, qpcReq, true);
        pendStruct.push_elem(pElem);
        HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.readProc: qpnMap.size() %d get_size() %d\n", 
                qpnHashMap.size(), pendStruct.get_size());

        /* write an entry to qpnHashMap */
        QpnInfoPtr qpnInfo = make_shared<QpnInfo>(qpcReq->num); // new QpnInfo(qpcReq->num);
        qpnHashMap.emplace(qpcReq->num, qpnInfo);
        HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.readProc: qpnHashMap.size %d rtnCnt %d\n", qpnHashMap.size(), rtnCnt);
    }

    HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.readProc: out!\n");
    return true;
}

void 
HanGuRnic::QpcModule::qpcCreate() {
    CxtReqRspPtr qpcReq = ccuQpcWreqFifo.front();
    ccuQpcWreqFifo.pop();
    assert(qpcReq->type == CXT_CREQ_QP);
    assert(qpcReq->num == qpcReq->txQpcReq->srcQpn);

    HANGU_PRINT(CxtResc, " QpcModule.qpcCreate: srcQpn %d sndBaseLkey %d\n", qpcReq->txQpcReq->srcQpn, qpcReq->txQpcReq->sndWqeBaseLkey);
    writeOne(qpcReq);

    /* delete useless qpc, cause writeEntry use memcpy 
     * to build cache entry. */
    delete qpcReq->txQpcReq;
}

void 
HanGuRnic::QpcModule::qpcAccess() {

    uint8_t CHNL_NUM = 3;
    bool isEmpty[CHNL_NUM];
    isEmpty[0] = txQpAddrRreqFifo.empty();
    isEmpty[1] = txQpcRreqFifo.empty();
    isEmpty[2] = rxQpcRreqFifo.empty();

    HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.qpcAccess: empty[0] %d empty[1] %d empty[2] %d\n", isEmpty[0], isEmpty[1], isEmpty[2]);

    CxtReqRspPtr qpcReq;
    for (uint8_t cnt = 0; cnt < CHNL_NUM; ++cnt) {
        if (isEmpty[this->chnlIdx] == false) {
            switch (this->chnlIdx) {
                case 0:
                    qpcReq = txQpAddrRreqFifo.front();
                    txQpAddrRreqFifo.pop();
                    assert(qpcReq->chnl == CXT_CHNL_TX && qpcReq->type == CXT_RREQ_SQ);
                    
                    break;
                case 1:
                    qpcReq = txQpcRreqFifo.front();
                    txQpcRreqFifo.pop();
                    assert(qpcReq->chnl == CXT_CHNL_TX && qpcReq->type == CXT_RREQ_QP);
                    
                    break;
                case 2:
                    qpcReq = rxQpcRreqFifo.front();
                    rxQpcRreqFifo.pop();
                    assert(qpcReq->chnl == CXT_CHNL_RX && qpcReq->type == CXT_RREQ_QP);
                    
                    break;
                default:
                    panic("[QpcModule.qpcReqProc.qpcAccess] chnlIdx error! %d", this->chnlIdx);
                    
                    break;
            }

            HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.qpcAccess: qpn: %d, chnlIdx %d, idx %d rtnCnt %d\n", 
                    qpcReq->num, this->chnlIdx, qpcReq->idx, rtnCnt);
            assert((qpcReq->num & QPN_MASK) <= QPN_NUM);

            readProc(this->chnlIdx, qpcReq);

            /* Point to next chnl */
            ++this->chnlIdx;
            this->chnlIdx = this->chnlIdx % CHNL_NUM;

            HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.qpcAccess: out!\n");
            return;
        } else {
            /* Point to next chnl */
            ++this->chnlIdx;
            this->chnlIdx = this->chnlIdx % CHNL_NUM;
        }
    }
}

void 
HanGuRnic::QpcModule::writeOne(CxtReqRspPtr qpcReq) {
    HANGU_PRINT(CxtResc, " QpcModule.writeOne!\n");

    HANGU_PRINT(CxtResc, " QpcModule.writeOne: srcQpn 0x%x, num %d, idx %d, chnl %d, sndBaseLkey %d\n", qpcReq->txQpcReq->srcQpn, qpcReq->num, qpcReq->idx, qpcReq->chnl, qpcReq->txQpcReq->sndWqeBaseLkey);
    assert(qpcReq->num == qpcReq->txQpcReq->srcQpn);

    if (qpcCache.lookupFull(qpcReq->num)) {

        /* get replaced qpc */
        uint32_t wbQpn = qpcCache.replaceEntry();
        QpcResc* qpc = qpcCache.deleteEntry(wbQpn);
        HANGU_PRINT(CxtResc, " QpcModule.writeOne: get replaced qpc 0x%x(%d)\n", wbQpn, (wbQpn & RESC_LIM_MASK));
        
        /* get related icm addr */
        uint64_t paddr = qpcIcm.num2phyAddr(wbQpn);

        /* store replaced qpc back to memory */
        storeMem(paddr, qpc);
    }

    /* write qpc entry back to cache */
    qpcCache.writeEntry(qpcReq->num, qpcReq->txQpcRsp);
    HANGU_PRINT(CxtResc, " QpcModule.writeOne: out!\n");
}

void 
HanGuRnic::QpcModule::storeMem(uint64_t paddr, QpcResc *qpc) {
    DmaReqPtr dmaReq = make_shared<DmaReq>(paddr, sizeof(QpcResc), 
            nullptr, (uint8_t *)qpc, 0); /* last param is useless here */
    dmaReq->reqType = 1; /* this is a write request */
    rnic->cacheDmaAccessFifo.push(dmaReq);
    if (!rnic->dmaEngine.dmaWriteEvent.scheduled()) {
        rnic->schedule(rnic->dmaEngine.dmaWriteEvent, curTick() + rnic->clockPeriod());
    }
}

DmaReqPtr 
HanGuRnic::QpcModule::loadMem(CxtReqRspPtr qpcReq) {

    HANGU_PRINT(CxtResc, " QpcModule.loadMem: Post qpn %d to dmaEngine, idx %d, pending size %d\n", 
            qpcReq->num, qpcReq->idx, pendStruct.get_size());
    
    PendingElemPtr pElem = pendStruct.front_elem();
    HANGU_PRINT(CxtResc, " QpcModule.loadMem: qpn %d chnl %d has_dma %d, idx %d\n", 
            pElem->qpn, pElem->chnl, pElem->has_dma, pElem->idx);
    assert((pElem->qpn & QPN_MASK) <= QPN_NUM);

    /* get qpc request icm addr, and post read request to ICM memory */
    uint64_t paddr = qpcIcm.num2phyAddr(qpcReq->num);
    DmaReqPtr dmaReq = make_shared<DmaReq>(paddr, sizeof(QpcResc), 
            &qpcRspProcEvent, (uint8_t *)qpcReq->txQpcReq, 0); /* last param is useless here */
    rnic->cacheDmaAccessFifo.push(dmaReq);
    if (!rnic->dmaEngine.dmaReadEvent.scheduled()) {
        rnic->schedule(rnic->dmaEngine.dmaReadEvent, curTick() + rnic->clockPeriod());
    }

    return dmaReq;
}

uint8_t 
HanGuRnic::QpcModule::checkNoDmaElem(PendingElemPtr pElem, uint8_t chnlNum, uint32_t qpn) {

    HANGU_PRINT(CxtResc, " QpcModule.qpcRspProc.checkNoDmaElem!\n");
    QpnInfoPtr qInfo = qpnHashMap[qpn];
    assert(qInfo->reqCnt);

    /* check if qpn attached to this elem is in cache */
    if (qpcCache.lookupHit(qpn)) { /* cache hit */

        HANGU_PRINT(CxtResc, " QpcModule.qpcRspProc.checkNoDmaElem: qpcCache.lookupHit. qInfo->reqCnt %d\n", qInfo->reqCnt);

        /* update qpnHashMap, and delete invalid elem in qpnMap */
        qInfo->reqCnt -= 1;
        if (qInfo->reqCnt == 0) {
            --rtnCnt;
            qpnHashMap.erase(qpn);
        }

        /* return rsp to qpcRspFifo */
        hitProc(chnlNum, pElem->reqPkt);

        /* update pendingFifo */
        pendStruct.succ_elem_check();

        return 0;
    } else if (qInfo->isReturned) { /* cache miss && accordingly qpc is returned */

        HANGU_PRINT(CxtResc, " QpcModule.qpcRspProc.checkNoDmaElem: cache miss && accordingly qpc is returned\n");

        /* delete isReturned && --rtnCnt */
        qInfo->reqRePosted();
        --rtnCnt;
        
        /* repost this request to pendingFifo */
        pElem->has_dma = 1; /* This request needs to post dma read request this time */
        pendStruct.push_elem_check(pElem);

        return 0;
    }

    return 1;
}

bool 
HanGuRnic::QpcModule::isRspValidRun() {
    return (((rtnCnt != 0) && pendStruct.get_size()) || rnic->qpcDmaRdCplFifo.size());
}

void 
HanGuRnic::QpcModule::qpcRspProc() {

    HANGU_PRINT(CxtResc, " QpcModule.qpcRspProc! rtnCnt %d qpnMap.size %d get_size() %d\n", 
            rtnCnt, qpnHashMap.size(), pendStruct.get_size());
    assert(rtnCnt <= qpnHashMap.size());
    assert(rtnCnt <= pendStruct.get_size());
    
    for (auto &item : qpnHashMap) {
        uint32_t   key = item.first;
        QpnInfoPtr val = item.second;
        HANGU_PRINT(CxtResc, " QpcModule.qpcRspProc: key %d qpn %d reqCnt %d\n\n", 
                key, val->qpn, val->reqCnt);
    }

    if (rnic->qpcDmaRdCplFifo.size()) { /* processing dmaRsp pkt */

        HANGU_PRINT(CxtResc, " QpcModule.qpcRspProc: processing dmaRsp pkt!\n");

        PendingElemPtr pElem = pendStruct.front_elem();
        uint32_t qpn = pElem->reqPkt->num;
        uint8_t  chnlNum = pElem->chnl;
        QpnInfoPtr qInfo = qpnHashMap[qpn];

        if (pElem->has_dma) {
            /* pop the dmaPkt */
            rnic->qpcDmaRdCplFifo.pop();

            /* update isReturned && rtnCnt */
            qInfo->firstReqReturned();
            ++rtnCnt;
            qInfo->reqCnt -= 1;

            /* write loaded qpc entry to qpc cache */
            writeOne(pElem->reqPkt);

            /* return rsp to qpcRspFifo */
            hitProc(chnlNum, pElem->reqPkt);

            HANGU_PRINT(CxtResc, " QpcModule.qpcRspProc: rtnCnt %d get_size() %d, qpnHashMap.size() %d, qInfo->reqCnt %d\n", 
                    rtnCnt, pendStruct.get_size(), qpnHashMap.size(), qInfo->reqCnt);

            /* delete invalid elem in qpnHashMap */
            if (qInfo->reqCnt == 0) {
                --rtnCnt;
                qpnHashMap.erase(qpn);
            }

            /* remove elem in pendingFifo */
            PendingElemPtr tmp = pendStruct.pop_elem();
        } else {
            /* remove the elem in pendingFifo. No matter if we process it, it cannot be placed 
             * to the head of the pendingFifo again. */
            PendingElemPtr tmp = pendStruct.pop_elem();

            uint8_t rtn = checkNoDmaElem(pElem, chnlNum, qpn);
            if (rtn != 0) {
                panic("[QpcModule.qpcRspProc] Error!");
            }
        }
    } else if (isRspValidRun()) {

        if (pendStruct.get_pending_size()) { /* there's elem in pending fifo */
            HANGU_PRINT(CxtResc, " QpcModule.qpcRspProc: processing check pendingElem!\n");

            PendingElemPtr cpElem = pendStruct.get_elem_check();
            uint32_t qpn = cpElem->reqPkt->num;
            uint8_t chnlNum = cpElem->chnl;
            if (cpElem->has_dma) {
                pendStruct.ignore_elem_check(cpElem);
            } else {
                uint8_t rtn = checkNoDmaElem(cpElem, chnlNum, qpn);
                if (rtn != 0) { /* no elem in cache && qpn hasn't returned, ignore it */
                    pendStruct.ignore_elem_check(cpElem);
                    HANGU_PRINT(CxtResc, " QpcModule.qpcRspProc: do not has dma, and check is ignored!\n");
                }
            }
        } else { /* pendingfifo has not been parpared for processing */
            HANGU_PRINT(CxtResc, " QpcModule.qpcRspProc: pendingfifo has not been parpared for processing, maybe next cycle!\n");
        }
    }

    /* if there's under checked elem in pending fifo, 
     * schedule myself again */
    if (isRspValidRun()) {
        if (!qpcRspProcEvent.scheduled()) { /* schedule myself */
            rnic->schedule(qpcRspProcEvent, curTick() + rnic->clockPeriod());
        }
    }

    HANGU_PRINT(CxtResc, " QpcModule.qpcRspProc: out! rtnCnt %d qpnMap.size %d get_size() %d\n", 
            rtnCnt, qpnHashMap.size(), pendStruct.get_size());
    assert(!(rtnCnt && (pendStruct.get_size() == 0)));
    assert(rtnCnt <= qpnHashMap.size());
    assert(rtnCnt <= pendStruct.get_size());
}

bool 
HanGuRnic::QpcModule::isReqValidRun() {
    return (ccuQpcWreqFifo.size()   || 
            txQpAddrRreqFifo.size() || 
            txQpcRreqFifo.size()    || 
            rxQpcRreqFifo.size()      );
}

void
HanGuRnic::QpcModule::qpcReqProc() {

    HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc!\n");

    if (ccuQpcWreqFifo.size()) {
        qpcCreate(); /* execute qpc entry create */
    } else {
        qpcAccess(); /* execute qpc entry read || write */
    }

    HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc: out!\n");

    /* Schedule myself again if there still has elem in fifo */
    if (isReqValidRun()) {
        if (!qpcReqProcEvent.scheduled()) { /* schedule myself */
            rnic->schedule(qpcReqProcEvent, curTick() + rnic->clockPeriod());
        }
    }
}
///////////////////////////// HanGuRnic::QpcModule {end}//////////////////////////////

///////////////////////////// HanGuRnic::DMA Engine {begin}//////////////////////////////
void 
HanGuRnic::DmaEngine::dmaWriteCplProcessing() {

    HANGU_PRINT(DmaEngine, " DMAEngine.dmaWriteCplProcessing! size %d\n", 
            dmaWrReq2RspFifo.front()->size);
    
    /* Pop related write request */
    DmaReqPtr dmaReq = dmaWrReq2RspFifo.front();
    dmaWrReq2RspFifo.pop();

    /* Schedule myself if there's item in fifo */
    if (dmaWrReq2RspFifo.size()) {
        rnic->schedule(dmaWriteCplEvent, dmaWrReq2RspFifo.front()->schd);
    }
}


void 
HanGuRnic::DmaEngine::dmaWriteProcessing () {

    uint8_t CHNL_NUM = 3;
    bool isEmpty[CHNL_NUM];
    isEmpty[0] = rnic->cacheDmaAccessFifo.empty();
    isEmpty[1] = rnic->dataDmaWriteFifo.empty() ;
    isEmpty[2] = rnic->cqDmaWriteFifo.empty()   ;

    if (rnic->cacheDmaAccessFifo.size() && rnic->cacheDmaAccessFifo.front()->reqType == 0) { /* read request */
        /* shchedule dma read processing if this is a read request */
        if (!dmaReadEvent.scheduled()) {
            rnic->schedule(dmaReadEvent, curTick() + rnic->clockPeriod());
        }

        isEmpty[0] = true; /* Write Request. This also means empty */
    }

    if (isEmpty[0] & isEmpty[1] & isEmpty[2]) {
        return;
    }

    HANGU_PRINT(DmaEngine, " DMAEngine.dmaWrite! size0 %d, size1 %d, size2 %d\n", 
            rnic->cacheDmaAccessFifo.size(), rnic->dataDmaWriteFifo.size(), rnic->cqDmaWriteFifo.size());

    uint8_t cnt = 0;
    while (cnt < CHNL_NUM) {
        if (isEmpty[writeIdx] == false) {
            DmaReqPtr dmaReq;
            switch (writeIdx) {
              case 0 :
                dmaReq = rnic->cacheDmaAccessFifo.front();
                rnic->cacheDmaAccessFifo.pop();
                HANGU_PRINT(DmaEngine, " DMAEngine.dmaWrite: Is cacheDmaAccessFifo! addr 0x%lx\n", (uint64_t)(dmaReq->data));
                break;
              case 1 :
                dmaReq = rnic->dataDmaWriteFifo.front();
                rnic->dataDmaWriteFifo.pop();
                HANGU_PRINT(DmaEngine, " DMAEngine.dmaWrite: Is dataDmaWriteFifo!\n");
                break;
              case 2 :
                dmaReq = rnic->cqDmaWriteFifo.front();
                rnic->cqDmaWriteFifo.pop();
                
                HANGU_PRINT(DmaEngine, " DMAEngine.dmaWrite: Is cqDmaWriteFifo!\n");
                
                break;
            }

            // if (dmaReq->size == 40) {
            //     for (int i = 0; i < dmaReq->size; ++i) {
            //         HANGU_PRINT(DmaEngine, " DMAEngine.dmaWrite: data[%d] is 0x%x\n", i, (dmaReq->data)[i]);
            //     }
            // }
            
            // unit: ps
            Tick bwDelay = (dmaReq->size + 32) * rnic->pciBandwidth;
            Tick delay = rnic->dmaWriteDelay + bwDelay;
            
            HANGU_PRINT(DmaEngine, " DMAEngine.dmaWrite: dmaReq->addr 0x%x, dmaReq->size %d, delay %d, bwDelay %d!\n", 
            dmaReq->addr, dmaReq->size, delay, bwDelay);

            /* Send dma req to dma channel
             * this event is used to call rnic->dmaWrite() */
            dmaWReqFifo.push(dmaReq);
            if (!dmaChnlProcEvent.scheduled()) {
                rnic->schedule(dmaChnlProcEvent, curTick() + rnic->clockPeriod());
            }
            
            /* Schedule DMA Write completion event */
            dmaReq->schd = curTick() + delay;
            dmaWrReq2RspFifo.push(dmaReq);
            if (!dmaWriteCplEvent.scheduled()) {
                rnic->schedule(dmaWriteCplEvent, dmaReq->schd);
            }
            
            /* Point to next chnl */
            ++writeIdx;
            writeIdx = writeIdx % CHNL_NUM;
            
            // bwDelay = (bwDelay > rnic->clockPeriod()) ? bwDelay : rnic->clockPeriod();
            if (dmaWriteEvent.scheduled()) {
                rnic->reschedule(dmaWriteEvent, curTick() + bwDelay);
            } else { // still schedule incase in time interval
                     // [curTick(), curTick() + rnic->dmaWriteDelay + bwDelay] , 
                     // one or more channel(s) schedule dmaWriteEvent
                rnic->schedule(dmaWriteEvent, curTick() + bwDelay);
            }
            HANGU_PRINT(DmaEngine, " DMAEngine.dmaWrite: out!\n");
            return;
        } else {
            ++cnt;

            ++writeIdx;
            writeIdx = writeIdx % CHNL_NUM;
        }
    }
}


void 
HanGuRnic::DmaEngine::dmaReadCplProcessing() {

    HANGU_PRINT(DmaEngine, " DMAEngine.dmaReadCplProcessing! cplSize %d\n", 
            dmaRdReq2RspFifo.front()->size);

    /* post related cpl pkt to related fifo */
    DmaReqPtr dmaReq = dmaRdReq2RspFifo.front();
    dmaRdReq2RspFifo.pop();
    dmaReq->rdVld = 1;
    Event *e = &rnic->qpcModule.qpcRspProcEvent;
    if (dmaReq->event == e) { /* qpc dma read cpl pkt */
        HANGU_PRINT(DmaEngine, " DMAEngine.dmaReadCplProcessing: push to rnic->qpcModule.qpcRspProcEvent!\n");
        rnic->qpcDmaRdCplFifo.push(dmaReq);

        if (dmaReq->size == 256) {
            HANGU_PRINT(DmaEngine, " DMAEngine.dmaReadCplProcessing: 0x%lx!\n", uintptr_t(dmaReq->data));
            HANGU_PRINT(DmaEngine, " DMAEngine.dmaReadCplProcessing: qpn %d dqpn %d!\n", ((QpcResc *)dmaReq->data)->srcQpn, ((QpcResc *)dmaReq->data)->destQpn);
        }
    }

    /* Schedule related completion event */
    if (!(dmaReq->event)->scheduled()) {
        rnic->schedule(*(dmaReq->event), curTick() + rnic->clockPeriod());
    }

    /* Schedule myself if there's item in fifo */
    if (dmaRdReq2RspFifo.size()) {
        rnic->schedule(dmaReadCplEvent, dmaRdReq2RspFifo.front()->schd);
    }

    HANGU_PRINT(DmaEngine, " DMAEngine.dmaReadCplProcessing: out!\n");
}

void 
HanGuRnic::DmaEngine::dmaReadProcessing () {

    HANGU_PRINT(DmaEngine, " DMAEngine.dmaRead! \n");
    
    uint8_t CHNL_NUM = 4;
    bool isEmpty[CHNL_NUM];
    isEmpty[0] = rnic->cacheDmaAccessFifo.empty();
    isEmpty[1] = rnic->descDmaReadFifo.empty() ;
    isEmpty[2] = rnic->dataDmaReadFifo.empty() ;
    isEmpty[3] = rnic->ccuDmaReadFifo.empty()  ;

    /* If there has write request, schedule dma Write Proc */
    if (rnic->cacheDmaAccessFifo.size() && rnic->cacheDmaAccessFifo.front()->reqType == 1) { /* write request */
        /* shchedule dma write processing if this is a write request */
        if (!dmaWriteEvent.scheduled()) {
            rnic->schedule(dmaWriteEvent, curTick() + rnic->clockPeriod());
        }

        isEmpty[0] = true; /* Write Request. This also means empty */
    }
    
    if (isEmpty[0] & isEmpty[1] & isEmpty[2] & isEmpty[3]) {
        return;
    }

    HANGU_PRINT(DmaEngine, " DMAEngine.dmaRead: in! \n");

    uint8_t cnt = 0;
    while (cnt < CHNL_NUM) {
        if (isEmpty[readIdx] == false) {
            DmaReqPtr dmaReq;
            switch (readIdx) {
              case 0 :
                dmaReq = rnic->cacheDmaAccessFifo.front();
                rnic->cacheDmaAccessFifo.pop();
                HANGU_PRINT(DmaEngine, " DMAEngine.dmaRead: Is cacheDmaAccessFifo!\n");
                break;
              case 1 :
                dmaReq = rnic->descDmaReadFifo.front();
                rnic->descDmaReadFifo.pop();
                HANGU_PRINT(DmaEngine, " DMAEngine.dmaRead: Is descDmaReadFifo!\n");
                break;
              case 2 :
                dmaReq = rnic->dataDmaReadFifo.front();
                rnic->dataDmaReadFifo.pop();
                HANGU_PRINT(DmaEngine, " DMAEngine.dmaRead: Is dataDmaReadFifo!\n");
                break;
              case 3 :
                dmaReq = rnic->ccuDmaReadFifo.front();
                rnic->ccuDmaReadFifo.pop();
                HANGU_PRINT(DmaEngine, " DMAEngine.dmaRead: Is ccuDmaReadFifo!\n");
                break;
            }
            
            // unit: ps
            Tick bwDelay = (dmaReq->size + 32) * rnic->pciBandwidth;
            Tick delay = rnic->dmaReadDelay + bwDelay;
            
            HANGU_PRINT(DmaEngine, " DMAEngine.dmaRead: dmaReq->addr 0x%x, dmaReq->size %d, delay %d, bwDelay %d!\n", 
            dmaReq->addr, dmaReq->size, delay, bwDelay);

            /* Send dma req to dma channel, 
             * this event is used to call rnic->dmaRead() */
            dmaRReqFifo.push(dmaReq);
            if (!dmaChnlProcEvent.scheduled()) {
                rnic->schedule(dmaChnlProcEvent, curTick() + rnic->clockPeriod());
            }
            
            /* Schedule DMA read completion event */
            dmaReq->schd = curTick() + delay;
            dmaRdReq2RspFifo.push(dmaReq);
            if (!dmaReadCplEvent.scheduled()) {
                rnic->schedule(dmaReadCplEvent, dmaReq->schd);
            }


            /* Point to next chnl */
            ++readIdx;
            readIdx = readIdx % CHNL_NUM;

            /* Reschedule the dma read event. delay is (byte count * bandwidth) */
            if (dmaReadEvent.scheduled()) {
                rnic->reschedule(dmaReadEvent, curTick() + bwDelay);
            } else { // still schedule incase in time interval
                     // [curTick(), curTick() + rnic->dmaReadDelay * dmaReq->size] , 
                     // one or more channel(s) schedule dmaReadEvent
                rnic->schedule(dmaReadEvent, curTick() + bwDelay);
            }
            
            HANGU_PRINT(DmaEngine, " DMAEngine.dmaRead: out! \n");
            return;
        } else {
            ++cnt;
            ++readIdx;
            readIdx = readIdx % CHNL_NUM;
        }
    }
}

void 
HanGuRnic::DmaEngine::dmaChnlProc () {
    if (dmaWReqFifo.empty() && dmaRReqFifo.empty()) {
        return ;
    }

    /* dma write has the higher priority, cause it is the duty of 
     * app logic to handle the write-after-read error. DMA channel 
     * only needs to avoid read-after-write error (when accessing 
     * the same address) */
    DmaReqPtr dmaReq;
    if (dmaWReqFifo.size()) { 
        
        dmaReq = dmaWReqFifo.front();
        dmaWReqFifo.pop();
        rnic->dmaWrite(dmaReq->addr, dmaReq->size, nullptr, dmaReq->data);
    } else if (dmaRReqFifo.size()) {

        dmaReq = dmaRReqFifo.front();
        dmaRReqFifo.pop();
        rnic->dmaRead(dmaReq->addr, dmaReq->size, nullptr, dmaReq->data);
    }
    
    /* schedule myself to post the dma req to the channel */
    if (dmaWReqFifo.size() || dmaRReqFifo.size()) {
        if (!dmaChnlProcEvent.scheduled()) {
            rnic->schedule(dmaChnlProcEvent, curTick() + rnic->clockPeriod());
        }
    }
}
///////////////////////////// HanGuRnic::DMA Engine {end}//////////////////////////////


///////////////////////////// Ethernet Link Interaction {begin}//////////////////////////////

void
HanGuRnic::ethTxDone() {

    DPRINTF(HanGuRnic, "Enter ethTxDone!\n");
}

bool
HanGuRnic::isMacEqual(uint8_t *devSrcMac, uint8_t *pktDstMac) {
    for (int i = 0; i < ETH_ADDR_LEN; ++i) {
        if (devSrcMac[i] != pktDstMac[i]) {
            return false;
        }
    }
    return true;
}

bool
HanGuRnic::ethRxDelay(EthPacketPtr pkt) {

    HANGU_PRINT(HanGuRnic, " ethRxDelay!\n");
    
    /* dest addr is not local, then abandon it */
    if (isMacEqual(macAddr, pkt->data) == false) {
        return true;
    }

    /* Update statistic */
    rxBytes += pkt->length;
    rxPackets++;

    /* post rx pkt to ethRxPktProc */
    Tick sched = curTick() + LinkDelay;
    ethRxDelayFifo.emplace(pkt, sched);
    if (!ethRxPktProcEvent.scheduled()) {
        schedule(ethRxPktProcEvent, sched);
    }

    HANGU_PRINT(HanGuRnic, " ethRxDelay: out!\n");

    return true;
}

void
HanGuRnic::ethRxPktProc() {

    HANGU_PRINT(HanGuRnic, " ethRxPktProc!\n");
    
    /* get pkt from ethRxDelay */
    EthPacketPtr pkt = ethRxDelayFifo.front().first;
    Tick sched = ethRxDelayFifo.front().second;
    ethRxDelayFifo.pop();

    /* Only used for debugging */
    BTH *bth = (BTH *)(pkt->data + ETH_ADDR_LEN * 2);
    uint8_t type = (bth->op_destQpn >> 24) & 0x1f;
    uint8_t srv  = bth->op_destQpn >> 29;
    if (srv == QP_TYPE_RC) {
        if (type == PKT_TRANS_SEND_ONLY) {
            HANGU_PRINT(HanGuRnic, " ethRxPktProc: Receiving packet from wire, SEND_ONLY RC, data: %s.\n", 
                    (char *)(pkt->data + 8));
        } else if (type == PKT_TRANS_RWRITE_ONLY) {
            RETH *reth = (RETH *)(pkt->data + PKT_BTH_SZ + ETH_ADDR_LEN * 2);
            HANGU_PRINT(HanGuRnic, " ethRxPktProc:"
                    " Receiving packet from wire, RDMA Write data: %s, len %d, raddr 0x%x, rkey 0x%x op_destQpn %d\n", 
                    (char *)(pkt->data + sizeof(BTH) + sizeof(RETH) + ETH_ADDR_LEN * 2), reth->len, reth->rVaddr_l, reth->rKey, ((BTH *)pkt->data)->op_destQpn);
            // for (int i = 0; i < reth->len; ++i) {
            //     HANGU_PRINT(HanGuRnic, " ethRxPkt: data[%d] 0x%x\n", i, (pkt->data)[sizeof(BTH) + sizeof(RETH) + ETH_ADDR_LEN * 2 + i]);
            // }

        } else if (type == PKT_TRANS_RREAD_ONLY) {
            
        } else if (type == PKT_TRANS_ACK) {
            HANGU_PRINT(HanGuRnic, " ethRxPktProc: Receiving packet from wire, Trans ACK needAck_psn: 0x%x\n", ((BTH *)pkt->data)->needAck_psn);
        }
    } else if (srv == QP_TYPE_UD) {
        if (type == PKT_TRANS_SEND_ONLY) {
            
            uint8_t *u8_tmp = (pkt->data + 16 + ETH_ADDR_LEN * 2);
            HANGU_PRINT(HanGuRnic, " ethRxPktProc: Receiving packet from wire, SEND UD data\n");
            HANGU_PRINT(HanGuRnic, " ethRxPktProc: data: 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x.\n", 
                    u8_tmp[0], u8_tmp[1], u8_tmp[2], u8_tmp[3], u8_tmp[4], u8_tmp[5], u8_tmp[6], u8_tmp[7]);
            HANGU_PRINT(HanGuRnic, " ethRxPktProc: data: 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x.\n", 
                    u8_tmp[8], u8_tmp[9], u8_tmp[10], u8_tmp[11], u8_tmp[12], u8_tmp[13], u8_tmp[14], u8_tmp[15]);
        }
    }
    HANGU_PRINT(HanGuRnic, " ethRxPktProc: Receiving packet from wire, trans_type: 0x%x, srv: 0x%x.\n", type, srv);
    
    /* Schedule RAU for pkt receiving */
    rxFifo.push(pkt);
    if (!rdmaArray.rxPktEvent.scheduled()) {
        schedule(rdmaArray.rxPktEvent, curTick() + clockPeriod());
    }

    /* Schedule myself if there is element in ethRxDelayFifo */
    if (ethRxDelayFifo.size()) {
        sched = ethRxDelayFifo.front().second;
        if (!ethRxPktProcEvent.scheduled()) {
            schedule(ethRxPktProcEvent, sched);
        }
    }

    HANGU_PRINT(HanGuRnic, " ethRxPktProc: out!\n");
}

///////////////////////////// Ethernet Link Interaction {end}//////////////////////////////


DrainState
HanGuRnic::drain() {
    
    DPRINTF(HanGuRnic, "HanGuRnic not drained\n");
    return DrainState::Draining;
}

void
HanGuRnic::drainResume() {
    Drainable::drainResume();

    DPRINTF(HanGuRnic, "resuming from drain");
}

void
HanGuRnic::serialize(CheckpointOut &cp) const {
    PciDevice::serialize(cp);

    regs.serialize(cp);

    DPRINTF(HanGuRnic, "Get into HanGuRnic serialize.\n");
}

void
HanGuRnic::unserialize(CheckpointIn &cp) {
    PciDevice::unserialize(cp);

    regs.unserialize(cp);

    DPRINTF(HanGuRnic, "Get into HanGuRnic unserialize.\n");
}

HanGuRnic *
HanGuRnicParams::create() {
    return new HanGuRnic(this);
}