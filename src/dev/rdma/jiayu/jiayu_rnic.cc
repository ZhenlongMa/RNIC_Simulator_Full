#include "mem/packet.hh"
#include "dev/rdma/jiayu/jiayu_rnic.hh"

JiaYuRnic::JiaYuRnic(const Params *p)
    : RdmaNic(p),
    etherInt(NULL),

    // performance parameter
    dmaReadDelay(p->dma_read_delay), dmaWriteDelay(p->dma_write_delay),
    pciBandwidth(p->pci_speed),
    etherBandwidth(p->ether_speed),
    LinkDelay     (p->link_delay),
    ethRxPktProcEvent([this]{ ethRxPktProc(); }, name()) 
{
    mailbox = new uint8_t[4096];
}

JiaYuRnic::~JiaYuRnic()
{
    delete 
}

// void JiaYuRnic::init()
// {
//     PciDevice::init();
// }

Port &JiaYuRnic::getPort(const std::string &if_name, PortID idx) 
{
    if (if_name == "interface")
        return *etherInt;
    return RdmaNic::getPort(if_name, idx);
}

Tick JiaYuRnic::writeConfig(PacketPtr pkt)
{
    int offset = pkt->getAddr() & PCI_CONFIG_SIZE;
    if (offset < PCI_DEVICE_SPECIFIC) 
    { 
        PciDevice::writeConfig(pkt);
    }
    else // accesses to unimplemented PCI configspace areas
    { 
        panic("Device specific PCI config space not implemented.\n");
    }
    return configDelay;
}

Tick JiaYuRnic::read(PacketPtr pkt) 
{
    int bar;
    Addr daddr;

    if (!getBAR(pkt->getAddr(), bar, daddr)) {
        panic("Invalid PCI memory access to unmapped memory.\n");
    }

    /* Only BAR0-1 is allowed */
    assert(bar == 0);

    /* Only 32bit accesses allowed */
    assert(pkt->getSize() == 4);

    /* Handle read of register here.
     * Here we only implement reading go bit */
    if (daddr == (Addr)&(((JiaYuRnicDef::Hcr*)0)->goOpcode)) // ??
    {
        pkt->setLE<uint32_t>(regs.cmdCtrl.go()<<31 | regs.cmdCtrl.op());
    } 
    else 
    {
        pkt->setLE<uint32_t>(0);
    }

    pkt->makeAtomicResponse();
    return pioDelay;
}

Tick JiaYuRnic::write(PacketPtr pkt)
{
    int bar;
    Addr daddr;

    if (!getBAR(pkt->getAddr(), bar, daddr)) {
        panic("Invalid PCI memory access to unmapped memory.\n");
    }

    assert(bar == 0);

    if (daddr == 0 && pkt->getSize() == sizeof(Hcr)) // write HCR
    {
        HANGU_PRINT(PioEngine, " PioEngine.write: HCR, inparam: 0x%x\n", pkt->getLE<Hcr>().inParam_l);

        regs.inParam.iparaml(pkt->getLE<Hcr>().inParam_l);
        regs.inParam.iparamh(pkt->getLE<Hcr>().inParam_h);
        regs.modifier = pkt->getLE<Hcr>().inMod;
        regs.outParam.oparaml(pkt->getLE<Hcr>().outParam_l);
        regs.outParam.oparamh(pkt->getLE<Hcr>().outParam_h);
        regs.cmdCtrl = pkt->getLE<Hcr>().goOpcode;

        /* Schedule CEU */
        if (!ceuProcEvent.scheduled()) { 
            schedule(ceuProcEvent, curTick() + clockPeriod()); // should not be next tick
        }
    }
    else if (daddr == 0x18 && pkt->getSize() == sizeof(uint64_t))
    {
        /*  Used to Record start of time */
        HANGU_PRINT(HanGuRnic, " PioEngine.write: Doorbell, value %#X pio interval %ld\n", pkt->getLE<uint64_t>(), curTick() - this->tick); 
        
        regs.db._data = pkt->getLE<uint64_t>();
        
        DoorbellPtr dbell = make_shared<DoorbellFifo>(
            regs.db.opcode(), 
            regs.db.num(), 
            regs.db.qpn(), 
            regs.db.offset()
        );
        pio2ccuDbFifo.push(dbell);

        /* Record last tick */
        this->tick = curTick();

        /* Schedule doorbellProc */
        if (!doorbellProcEvent.scheduled()) { 
            schedule(doorbellProcEvent, curTick() + clockPeriod());
        }

        HANGU_PRINT(HanGuRnic, " PioEngine.write: qpn %d, opcode %x, num %d\n", 
                regs.db.qpn(), regs.db.opcode(), regs.db.num());
    }
    else {
        panic("Write request to unknown address : %#x && size 0x%x\n", daddr, pkt->getSize());
    }
    pkt->makeAtomicResponse();
    return pioDelay;
}

void JiaYuRnic::mbxFetchCpl()
{
    switch (regs.cmdCtrl.op())
    {
        case INIT_ICM:
            regs.mptBase   = ((InitResc *)mailbox)->mptBase;
            regs.mttBase   = ((InitResc *)mailbox)->mttBase;
            regs.qpcBase   = ((InitResc *)mailbox)->qpcBase;
            regs.cqcBase   = ((InitResc *)mailbox)->cqcBase;
            regs.mptNumLog = ((InitResc *)mailbox)->mptNumLog;
            regs.qpcNumLog = ((InitResc *)mailbox)->qpsNumLog;
            regs.cqcNumLog = ((InitResc *)mailbox)->cqsNumLog;
            
    }
}

bool JiaYuRnic::CommRescCache::isHit(uint32_t entryId)
{
    return (cache.find(entryId) != cache.end());
}

bool JiaYuRnic::CommRescCache::isFull()
{
    return (cache.size() == capacity);
}

template<class T> 
bool JiaYuRnic::CommRescCache::update(
    uint32_t entryId, 
    const std::function<bool(T&)> &updateFunc
)
{
    assert(cache.find(entryId) != cache.end());
    assert(updateFunc != nullptr);
    return(updateFunc(cache[entryId]));
}

template<class T>
bool JiaYuRnic::CommRescCache::readEntry(uint32_t entryId, T* outputEntry)
{
    assert(cache.find(entryId) != cache.end());
    memcpy(outputEntry, cache[entryId].first, sizeof(T));
    return true;
}

template<class T>
bool JiaYuRnic::CommRescCache::writeEntry(uint32_t entryId, T* inputEntry)
{
    assert(cache.find(entryId) == cache.end());
    T *inputValue = new T;
    memcpy(inputVaue, inputEntry, sizeof(T));
}

template <class T>
bool JiaYuRnic::CommRescCache::deleteEntry(uint32_t entryId)
{
    assert(cache.find(entryId) != cache.end());
    cache.erase(entryId);
    assert(cache.size() == capacity - 1);
    return true;
}

void JiaYuRnic::MrModule::MrModule(JiaYuRnic *rnic, std::string name, uint32_t mptCacheNum, uint32_t mttCacheNum):
    rNic(rnic),
    n(name),
    DmaRspEvent(dmaRspProc),
    MptRspEvent(MptRspProc),
    MttRspEvent(MttRspProc),
    transReqEvent(transReqProc),
    MptCache(mptCacheNum),
    mttCache(mttCacheNum)
{

}

void JiaYuRnic::MrModule::dmaReqProc(uint64_t pAddr, MrReqRspPtr tptReq)
{
    JIAYU_PRINT(MrResc, " MrRescModule.dmaReqProcess!\n");

    // write request
    if (tptReq->type == DMA_TYPE_WREQ)
    {
        DmaReqPtr dmaReq = make_shared<DmaReq>(rNic->pciToDma(pAddr), tptReq->length, nullptr, tptReq->data, 0);
        if (tptReq->channel == TPT_WCHNL_TX_CQ || tptReq->channel == TPT_WCHNL_RX_CQ)
        {
            rNic->cqDmaWriteFifo.push(dmaReq);
        }
        else if (tptReq->channel == TPT_WCHNL_TX_DATA || tptReq->channel == TPT_WCHNL_RX_DATA)
        {
            rNic->dataDmaWriteFifo.push(dmaReq);
        }
        else
        {
            panic("Illegal DMA write request channel!\n");
        }
        if (!rNic->dmaEngine.dmaWriteEvent.scheduled())
        {
            schedule(rNic->dmaEngine.dmaWriteEvent, curTick() + rNic->clockPeriod()); // todo: change time gap
        }
    }
    else if (tptReq->type == DMA_TYPE_RREQ)
    {
        DmaReqPtr dmaReq = make_shared<DmaReq>(rNic-pciToDma(pAddr), tptReq->length, &DmaRspEvent, tptReq->data, 0);
        if (tptReq->channel == TPT_RCHNL_TX_DESC || tptReq->channel == TPT_RCHNL_RX_DESC)
        {
            rNic->descDmaReadFifo.push(dmaReq);
        }
        else if (tptReq->channel == TPT_RCHNL_TX_DATA || tptReq->channel == TPT_RCHNL_RX_DATA)
        {
            rNic->dataDmaReadFifo.push(dmaReq);
        }
        else 
        {
            panic("Illegal DMA read request channel!\n");
        }
        dmaReq2RspFifo.emplace(tptReq, dmaReq);
        if (!rNic->dmaEngine.dmaReadEvent.scheduled())
        {
            schedule(rNic->dmaEngine.dmaReadEvent, curTick() + rNic->clockPeriod());
        }
    }
    else
    {
        panic("Illegal DMA request type!\n");
    }
}

void JiaYuRnic::MrModule::dmaRspProc()
{
    JIAYU_PRINT(MrResc, " MrRescModule.dmaRrspProcessing! FIFO size: %d\n", dmaReq2RspFifo.size());
    MrReqRspPtr tptRsp = dmaReq2RspFifo.front().first;
    dmaReq2RspFifo.pop();
}

void JiaYuRnic::MrModule::MptRspProc()
{

}

void JiaYuRnic::MrModule::mptReqProc()
{

}

void JiaYuRnic::MrModule::mttReqProc()
{

}

void JiaYuRnic::MrModule::transReqProc()
{

}

// generate DMA read request and send it to channel proc
void JiaYuRnic::DmaEngine::dmaReadProc()
{
    uint8_t channel_num = 4;
    bool isEmpty[channel_num];
    isEmpty[0] = rNic->icmDmaReadFifo.empty();
    isEmpty[1] = rNic->descDmaReadFifo.empty() ;
    isEmpty[2] = rNic->dataDmaReadFifo.empty() ;
    isEmpty[3] = rNic->mbxDmaReadFifo.empty()  ; // for mailbox
    // to optimize
    for (int i = readIdx; i < channel_num; i++)
    {
        if (isEmpty[i] == true)
        {
            continue;
        }
        DmaReqPtr dmaReq;
        switch(i)
        {
            case 0:
                dmaReq = rNic->icmDmaReadFifo.front();
                rNic->icmDmaReadFifo.pop();
                break;
            case 1:
                dmaReq = rNic->descDmaReadFifo.front();
                rNic->descDmaReadFifo.pop();
                break;
            case 2:
                dmaReq = rNic->dataDmaReadFifo.front();
                rNic->dataDmaReadFifo.pop();
                break;
            case 3:
                dmaReq = rNic->mbxDmaReadFifo.front();
                rNic->mbxDmaReadFifo.pop();
                break;
            default:
                panic("Illegal dma req channel!\n");
        }
        Tick flyDelay = (dmaReq->size + 32) * rNic->pciSpeed;
        Tick readDelay = rNic->dmaReadDelay + flyDelay;
        dmaRdReqFifo.push(dmaReq);
        
        if (!dmaChnlProcEvent.scheduled())
        {
            schedule(dmaChnlProcEvent, curTick() + rnic->clockPeriod());
        }

        dmaReq->schd = curTick() + readDelay;
        dmaRdReq2RspFifo.push(dmaReq);
        if (!dmaReadCplEvent.scheduled())
        {
            schedule(dmaReadCplEvent, dmaReq->schd);
        }
        readIdx++;
        readIdx = readIdx % channel_num;
        // return;
    }
    else
    {
        readIdx++;
        readIdx = readIdx % channel_num;
    }
}

void JiaYuRnic::DMAEngine::dmaChnlProcEvent()
{
    DmaReqPtr dmaReq;
    if (dmaRdReqFifo.size() != 0)
    {
        dmaReq = dmaRdReqFifo.front();
        dmaRdReqFifo.pop();
        rNic->dmaRead(dmaReq->addr, dmaReq->size, nullptr, dmaReq->data);
    }
    else if (dmaWrReqFifo.size() != 0)
    {
        dmaReq = dmaWrReqFifo.front();
        dmaWrReqFifo.pop();
        rNic->dmaWrite(dmaReq->addr, dmaReq->size, nullptr, dmaReq->data);
    }
    
    if (dmaRdReqFifo.size() != 0 || dmaWrReqFifo.size() != 0)
    {
        if (!dmaChnlProcEvent.scheduled())
        {
            rNic->schedule(dmaChnlProcEvent, curTick() + rNic->clockPeriod()); // todo
        }
    }
}

void JiaYuRnic::DmaEngine::dmaWriteProc()
{

}