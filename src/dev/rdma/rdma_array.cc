
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


///////////////////////////// HanGuRnic::RDMA Array relevant {begin}////////////////////////////

HanGuRnic::RdmaArray::RdmaArray(HanGuRnic *rNic, uint8_t corenum, uint32_t reorderCap, const std::string n) :
    // coreQpn(corenum),
    rNic(rNic),
    _name(n),

    txQpAddrRspEvent([this]{txQpAddrRspSch();}, n),
    txQpcRspEvent([this]{txQpCtxRspSch();}, n),
    txDescRdReqEvent([this]{txDescReqProc();}, n),
    txdduDescRspEvent([this]{txDescRspSch();}, n),
    txDataRdReqEvent([this]{txDataRdReqProc();}, n),
    txDataRdRspEvent([this]{txDataRdRspProc();}, n),
    txSendToLinkLayerEvent([this]{txSendToLinkLayerProc();}, n),
    sendCqcRspEvent([this]{sendCqcRspSch();}, n),
    rxPktEvent([this]{recvPktSch();}, n),
    recvQpcRspEvent([this]{recvQpcRspSch();}, n),
    rxDescRdReqEvent([this]{rxDescRdReqSch();}, n),
    rxDescRdRspEvent([this]{rxDescRdRspAlloc();}, n),
    rcvDataRdReqEvent([this]{rcvDataRdReqSch();}, n),
    wrRpuDataWrReqEvent([this]{wrRpuDataWrReqSch();}, n),
    rdRpuDataRdReqEvent([this]{rdRpuDataRdReqSch();}, n),
    rdRpuDataRdRspEvent([this]{rdRpuDataRdRspAlloc();}, n),
    rcvCqcRdRspEvent([this]{rcvCqcRdRspAlloc();}, n),
    rcvCqDescWrReqEvent([this]{rcvCqDescWrReqSch();}, n),

    // doorbellVectorVec(corenum),
    // df2ccuIdxFifoVec(corenum),
    // rdmaEngineVec(corenum),
    // txQpAddrRspCoreQueVec(corenum),
    // descReqFifoVec(corenum),
    // txdescRspFifoVec(corenum),
    // txQpcRspQueVec(corenum),
    // txDataRdReqQueVec(corenum),
    // txDataRdRspQueVec(corenum),
    // txPktArbQueVec(corenum),
    // sqCqcRspQueVec(corenum),
    // rxPktQueVec(corenum),
    // rxQpcRspQueVec(corenum),
    // rxDescRdReqQueVec(corenum),
    // rxdescRdRspQueVec(corenum),
    // rcvDataRdReqQueVec(corenum),
    // wrRpuDataWrReqQueVec(corenum),
    // rdRpuDataRdReqQueVec(corenum),
    // rdRpuDataRdRspQueVec(corenum),
    // rcvCqcRdRspQueVec(corenum),
    // rcvCqDescWrReqQueVec(corenum),
    coreNum(corenum)
{
    for (uint8_t coreID = 0; coreID < corenum; coreID++)
    {
        std::shared_ptr<RdmaEngine> rdmaEngine = make_shared<RdmaEngine>(rNic, 
            name() + ".RdmaEngine[" + std::to_string(coreID) + "]", 
            reorderCap, coreID);
        rdmaEngineVec.push_back(rdmaEngine);

        // coreQpn.push_back(INVALID_QPN);

        std::queue<uint8_t> IdxQue;
        for(int i = 0; i < reorderCap; i++)
        {
            IdxQue.push(i);
        }
        df2ccuIdxFifoVec.push_back(IdxQue);
        // HANGU_PRINT(RdmaArray, "IdxQue.size: %d\n", IdxQue.size());
        // HANGU_PRINT(RdmaArray, "df2ccuIdxFifoVec[%d].size: %d\n", coreID, df2ccuIdxFifoVec[coreID].size());

        std::vector<DoorbellPtr> dbVec(reorderCap);
        doorbellVectorVec.push_back(dbVec);

        std::queue<CxtReqRspPtr> txQpAddrRspQue;
        txQpAddrRspCoreQueVec.push_back(txQpAddrRspQue);

        std::queue<MrReqRspPtr> MrReqQue;
        descReqFifoVec.push_back(MrReqQue);

        std::queue<TxDescPtr> txdescRspQue;
        txdescRspFifoVec.push_back(txdescRspQue);

        std::queue<CxtReqRspPtr> txQpcRspQue;
        txQpcRspQueVec.push_back(txQpcRspQue);

        std::queue<MrReqRspPtr> txDataRdReqQue;
        txDataRdReqQueVec.push_back(txDataRdReqQue);

        std::queue<MrReqRspPtr> txDataRdRspQue;
        txDataRdRspQueVec.push_back(txDataRdReqQue);
        
        std::queue<EthPacketPtr> txPktArbQue;
        txPktArbQueVec.push_back(txPktArbQue);

        std::queue<CxtReqRspPtr> sqCqcRspQue;
        sqCqcRspQueVec.push_back(sqCqcRspQue);

        std::queue<EthPacketPtr> rxPktQue;
        rxPktQueVec.push_back(rxPktQue);

        std::queue<CxtReqRspPtr> rxQpcRspQue;
        rxQpcRspQueVec.push_back(rxQpcRspQue);

        std::queue<MrReqRspPtr> rxDescRdReqQue;
        rxDescRdReqQueVec.push_back(rxDescRdReqQue);

        std::queue<RxDescPtr> rxdescRdRspQue;
        rxdescRdRspQueVec.push_back(rxdescRdRspQue);
        
        std::queue<MrReqRspPtr> rcvDataRdReqQue;
        rcvDataRdReqQueVec.push_back(rcvDataRdReqQue);

        std::queue<MrReqRspPtr> wrRpuDataWrReqQue;
        wrRpuDataWrReqQueVec.push_back(wrRpuDataWrReqQue);

        std::queue<MrReqRspPtr> rdRpuDataRdReqQue;
        rdRpuDataRdReqQueVec.push_back(rdRpuDataRdReqQue);

        std::queue<MrReqRspPtr> rdRpuDataRdRspQue;
        rdRpuDataRdRspQueVec.push_back(rdRpuDataRdRspQue);

        std::queue<CxtReqRspPtr> rcvCqcRdRspQue;
        rcvCqcRdRspQueVec.push_back(rcvCqcRdRspQue);

        std::queue<MrReqRspPtr> rcvCqDescWrReqQue;
        rcvCqDescWrReqQueVec.push_back(rcvCqDescWrReqQue);
    }
    // HANGU_PRINT(RdmaArray, "RDMA Array initialization finished!\n");
}

// Allocate an RDMA core for each doorbell
uint8_t HanGuRnic::RdmaArray::AllocCore(uint32_t qpn)
{
    uint8_t coreID;

    for (int i = 0; i < coreNum; i++)
    {
        if (rdmaEngineVec[i]->curQpn == qpn || rdmaEngineVec[i]->curQpn == INVALID_QPN)
        {
            coreID = i;
            rdmaEngineVec[coreID]->curQpn = qpn;
            break;
        }
    }
    
    HANGU_PRINT(RdmaArray, "Allocating RDMA Core: %d, QPN: 'h%x, curQpn: 'h%x\n", 
        coreID, qpn, rdmaEngineVec[coreID]->curQpn);
    assert(rdmaEngineVec[coreID]->curQpn == qpn || rdmaEngineVec[coreID]->curQpn == INVALID_QPN);
    return coreID;
}

// Schedule QP address to corresponding core
void HanGuRnic::RdmaArray::txQpAddrRspSch()
{
    CxtReqRspPtr txQpAddrRsp = rNic->qpcModule.txQpAddrRspFifo.front();
    HANGU_PRINT(RdmaArray, " RDMA Array receives a QP Addr Response, QPN: %d\n", txQpAddrRsp->txQpcRsp->srcQpn);
    if (txQpAddrRspCoreQueVec[txQpAddrRsp->coreID].size() < 10) // how to implement fix-capacity queue?
    {
        rNic->qpcModule.txQpAddrRspFifo.pop();
        txQpAddrRspCoreQueVec[txQpAddrRsp->coreID].push(txQpAddrRsp);
        assert(rdmaEngineVec[txQpAddrRsp->coreID]->curQpn == txQpAddrRsp->txQpcRsp->srcQpn);
        rNic->schedule(rdmaEngineVec[txQpAddrRsp->coreID]->dfuEvent, curTick() + rNic->clockPeriod());
    }
    else
    {
        HANGU_PRINT(RdmaArray, "txQpAddrRspCoreQue is oversize! core ID: %d\n", txQpAddrRsp->coreID);
    }

    if (rNic->qpcModule.txQpAddrRspFifo.size())
    {
        if (!txQpAddrRspEvent.scheduled())
        {
            rNic->schedule(txQpAddrRspEvent, curTick() + rNic->clockPeriod());
        }
    }
}

// void HanGuRnic::RdmaArray::postQpcReq(CxtReqRspPtr Req)
// {
//     rNic->qpcModule.postQpcReq(Req);
// }

// Process tx WQE read request
void HanGuRnic::RdmaArray::txDescReqProc()
{
    static int TransReqSchPin = 0;
    MrReqRspPtr MrReq;
    for (int i = 0; i < coreNum; i++)
    {
        // round robin
        if (descReqFifoVec[(i + TransReqSchPin) % coreNum].size() != 0)
        {
            MrReq = descReqFifoVec[(i + TransReqSchPin) % coreNum].front();
            // MrReq->coreID = i;
            descReqFifoVec[(i + TransReqSchPin) % coreNum].pop();
            TransReqSchPin = i + 1;
            break;
        }
    }

    rNic->descReqFifo.push(MrReq);
    if (!rNic->mrRescModule.transReqEvent.scheduled()) { /* Schedule MrRescModule.transReqProcessing */
        rNic->schedule(rNic->mrRescModule.transReqEvent, curTick() + rNic->clockPeriod());
    }

    for (int i = 0; i < coreNum; i++)
    {
        if(descReqFifoVec[i].size() != 0)
        {
            if (!txDescRdReqEvent.scheduled())
            {
                rNic->schedule(txDescRdReqEvent, curTick() + rNic->clockPeriod());
                break;
            }
        }
    }
}

void HanGuRnic::RdmaArray::txDescRspSch()
{
    assert(rNic->txdescRspFifo.size());
    MrReqRspPtr MrRsp = rNic->txdescRspFifo.front();
    rNic->txdescRspFifo.pop();
    assert(MrRsp->coreID != INVALID_CORE);
    HANGU_PRINT(RdmaArray, "tx desc response received in RDMA Array! core ID: %d, length: %d\n", MrRsp->coreID, MrRsp->length);

    TxDescPtr txDesc;

    for (uint32_t i = 0; (i * sizeof(TxDesc)) < MrRsp->length; ++i) {
        txDesc = make_shared<TxDesc>(MrRsp->txDescRsp + i);
        assert((txDesc->len != 0) && (txDesc->lVaddr != 0) && (txDesc->opcode != 0));
        txdescRspFifoVec[MrRsp->coreID].push(txDesc); // clock
    }

    HANGU_PRINT(RdmaArray, "tx desc response sent to txdescRspFifoVec! core ID: %d, length: %d\n", MrRsp->coreID, MrRsp->length);

    if (!rdmaEngineVec[MrRsp->coreID]->dduEvent.scheduled())
    {
        rNic->schedule(rdmaEngineVec[MrRsp->coreID]->dduEvent, curTick() + rNic->clockPeriod());
    }

    if (!rNic->txdescRspFifo.empty())
    {
        if (!txdduDescRspEvent.scheduled())
        {
            rNic->schedule(txdduDescRspEvent, curTick() + rNic->clockPeriod());
        }
    }
}

void HanGuRnic::RdmaArray::txQpCtxRspSch()
{
    HANGU_PRINT(RdmaArray, "tx QPC response received in RDMA Array!\n");
    assert(rNic->qpcModule.txQpcRspFifo.size());
    CxtReqRspPtr qpc = rNic->qpcModule.txQpcRspFifo.front();
    if (txQpcRspQueVec[qpc->coreID].size() < 10)
    {
        rNic->qpcModule.txQpcRspFifo.pop();
        txQpcRspQueVec[qpc->coreID].push(qpc);
        assert(qpc->coreID != INVALID_CORE);
        if (rdmaEngineVec[qpc->coreID]->dpuEvent.scheduled() == 0)
        {
            rNic->schedule(rdmaEngineVec[qpc->coreID]->dpuEvent, curTick() + rNic->clockPeriod());
        }
    }
    else
    {
        HANGU_PRINT(RdmaArray, "txQpcRspQueVec[%d] oversized!\n", qpc->coreID);
    }
    if (rNic->qpcModule.txQpcRspFifo.size() != 0)
    {
        if (!txQpcRspEvent.scheduled())
        {
            rNic->schedule(txQpcRspEvent, curTick() + rNic->clockPeriod());
        }
    }
}

void HanGuRnic::RdmaArray::txDataRdReqProc()
{
    static int SchPin = 0;
    MrReqRspPtr MrReq;
    for (int i = 0; i < coreNum; i++)
    {
        // round robin
        if (txDataRdReqQueVec[(i + SchPin) % coreNum].size() != 0)
        {
            MrReq = txDataRdReqQueVec[(i + SchPin) % coreNum].front();
            // MrReq->coreID = i;
            txDataRdReqQueVec[(i + SchPin) % coreNum].pop();
            SchPin = i + 1;
            break;
        }
    }

    rNic->dataReqFifo.push(MrReq);
    if (!rNic->mrRescModule.transReqEvent.scheduled()) { /* Schedule MrRescModule.transReqProcessing */
        rNic->schedule(rNic->mrRescModule.transReqEvent, curTick() + rNic->clockPeriod());
    }

    for (int i = 0; i < coreNum; i++)
    {
        if (txDataRdReqQueVec[i].size() != 0)
        {
            if (!txDataRdReqEvent.scheduled())
            {
                rNic->schedule(txDataRdReqEvent, curTick() + rNic->clockPeriod());
                break;
            }
        }
    }
}

void HanGuRnic::RdmaArray::txDataRdRspProc()
{
    HANGU_PRINT(RdmaArray, "tx data response received in RDMA Array!\n");
    MrReqRspPtr dataRsp;
    dataRsp = rNic->txdataRspFifo.front();
    assert(dataRsp->coreID != INVALID_CORE);
    if (txDataRdRspQueVec[dataRsp->coreID].size() < 10)
    {
        rNic->txdataRspFifo.pop();
        txDataRdRspQueVec[dataRsp->coreID].push(dataRsp);
        if (!rdmaEngineVec[dataRsp->coreID]->rgrrEvent.scheduled())
        {
            rNic->schedule(rdmaEngineVec[dataRsp->coreID]->rgrrEvent, curTick() + rNic->clockPeriod());
        }
    }
    else
    {
        HANGU_PRINT(RdmaArray, "txdataRspFifo oversize!\n");
    }

    if (rNic->txdataRspFifo.size() != 0)
    {
        if (!txDataRdRspEvent.scheduled())
        {
            rNic->schedule(txDataRdRspEvent, curTick() + rNic->clockPeriod());
        }
    }
}

void HanGuRnic::RdmaArray::txSendToLinkLayerProc()
{
    static int schedulePin = 0;
    EthPacketPtr dataPkt;
    for (int i = 0; i < coreNum; i++)
    {
        // round robin
        if (txPktArbQueVec[(i + schedulePin) % coreNum].size() != 0)
        {
            dataPkt = txPktArbQueVec[(i + schedulePin) % coreNum].front();
            if (rNic->etherInt->sendPacket(dataPkt))
            {
                HANGU_PRINT(RdmaArray, " RdmaEngine.sauProcessing: TxFIFO: Successful transmit!\n");

                rNic->txBytes += dataPkt->length;
                rNic->txPackets++;

                txPktArbQueVec[(i + schedulePin) % coreNum].pop();
            }
            schedulePin = i + 1;
            break;
        }
    }

    for (int i = 0; i < coreNum; i++)
    {
        if (txPktArbQueVec[i].size() != 0)
        {
            if (!txSendToLinkLayerEvent.scheduled())
            {
                rNic->schedule(txSendToLinkLayerEvent, curTick() + rNic->clockPeriod());
                break;
            }
        }
    }
}

void HanGuRnic::RdmaArray::postCqcReq(CxtReqRspPtr req)
{
    rNic->cqcModule.postCqcReq(req);
}

void HanGuRnic::RdmaArray::sendCqcRspSch()
{
    CxtReqRspPtr cqc;
    cqc = rNic->txCqcRspFifo.front();
    assert(cqc->coreID != INVALID_CORE);

    if (sqCqcRspQueVec[cqc->coreID].size() < 10)
    {
        rNic->txCqcRspFifo.pop();
        HANGU_PRINT(RdmaArray, "send CQC to sqCqcRspQueVec[%d]!\n", cqc->coreID);
        sqCqcRspQueVec[cqc->coreID].push(cqc);
        if (!rdmaEngineVec[cqc->coreID]->scuEvent.scheduled())
        {
            rNic->schedule(rdmaEngineVec[cqc->coreID]->scuEvent, curTick() + rNic->clockPeriod());
        }
    }
    else
    {
        HANGU_PRINT(RdmaArray, "sqCqcRspQueVec[%d] oversize!\n", cqc->coreID);
    }
    if (rNic->txCqcRspFifo.size() != 0)
    {
        if (!sendCqcRspEvent.scheduled())
        {
            rNic->schedule(sendCqcRspEvent, curTick() + rNic->clockPeriod());
        }
    }
}

void HanGuRnic::RdmaArray::recvPktSch()
{
    EthPacketPtr pkt;
    pkt = rNic->rxFifo.front();
    BTH *bth = (BTH *)(pkt->data + ETH_ADDR_LEN * 2);
    // uint8_t type = (bth->op_destQpn >> 24) & 0x1f;
    // uint8_t srv  = bth->op_destQpn >> 29;
    uint32_t dstQpn = bth->op_destQpn & 0xffffff;
    uint8_t coreID = AllocCore(dstQpn);
    
    assert(rdmaEngineVec[coreID]->curQpn == INVALID_QPN || rdmaEngineVec[coreID]->curQpn == dstQpn);
    rxPktQueVec[coreID].push(pkt);
    rNic->rxFifo.pop();
    if (!rdmaEngineVec[coreID]->rauEvent.scheduled())
    {
        rNic->schedule(rdmaEngineVec[coreID]->rauEvent, curTick() + rNic->clockPeriod());
    }
    if (rNic->rxFifo.size() != 0)
    {
        rNic->schedule(rxPktEvent, curTick() + rNic->clockPeriod());
    }
}

void HanGuRnic::RdmaArray::recvQpcRspSch()
{
    assert(rNic->qpcModule.rxQpcRspFifo.size());
    CxtReqRspPtr ctx = rNic->qpcModule.rxQpcRspFifo.front();
    rNic->qpcModule.rxQpcRspFifo.pop();
    assert(ctx->coreID != INVALID_CORE);
    assert(ctx->rxQpcRsp->srcQpn == rdmaEngineVec[ctx->coreID]->curQpn);
    
    rxQpcRspQueVec[ctx->coreID].push(ctx);
    HANGU_PRINT(RdmaArray, "QPC response pushed to rxQpcRspQueVec[%d]!\n", ctx->coreID);
    if (!rdmaEngineVec[ctx->coreID]->rpuEvent.scheduled())
    {
        rNic->schedule(rdmaEngineVec[ctx->coreID]->rpuEvent, curTick() + rNic->clockPeriod());
    }
    if (rNic->qpcModule.rxQpcRspFifo.size() != 0)
    {
        if (!recvQpcRspEvent.scheduled())
        {
            rNic->schedule(recvQpcRspEvent, curTick() + rNic->clockPeriod());
        }
    }
}

void HanGuRnic::RdmaArray::rxDescRdReqSch()
{
    static int schedulePin = 0;
    MrReqRspPtr descReq;
    for (int i = 0; i < coreNum; i++)
    {
        if (rxDescRdReqQueVec[(schedulePin + i) % coreNum].size() != 0)
        {
            descReq = rxDescRdReqQueVec[(schedulePin + i) % coreNum].front();
            rxDescRdReqQueVec[(schedulePin + i) % coreNum].pop();
            schedulePin = i + 1;
            break;
        }
    }
    assert(descReq != nullptr);
    rNic->descReqFifo.push(descReq);
    if (!rNic->mrRescModule.transReqEvent.scheduled())
    {
        rNic->schedule(rNic->mrRescModule.transReqEvent, curTick() + rNic->clockPeriod());
    }

    for (int i = 0; i < coreNum; i++)
    {
        if (rxDescRdReqQueVec[i].size() != 0)
        {
            rNic->schedule(rxDescRdReqEvent, curTick() + rNic->clockPeriod());
            break;
        }
    }
}

void HanGuRnic::RdmaArray::rxDescRdRspAlloc()
{
    assert(rNic->rxdescRspFifo.size());
    MrReqRspPtr rxDescRsp;
    rxDescRsp = rNic->rxdescRspFifo.front();
    rNic->rxdescRspFifo.pop();
    assert(rxDescRsp->coreID != INVALID_CORE);
    for (uint32_t i = 0; (i * sizeof(RxDesc)) < rxDescRsp->length; ++i) 
    {
        RxDescPtr rxDesc = make_shared<RxDesc>(rxDescRsp->rxDescRsp + i);
        assert((rxDesc->len != 0) && (rxDesc->lVaddr != 0));
        rxdescRdRspQueVec[rxDescRsp->coreID].push(rxDesc);
    }
    if (!rdmaEngineVec[rxDescRsp->coreID]->rcvRpuEvent.scheduled())
    {
        rNic->schedule(rdmaEngineVec[rxDescRsp->coreID]->rcvRpuEvent, curTick() + rNic->clockPeriod());
    }
    if (rNic->rxdescRspFifo.size() != 0)
    {
        if (!rxDescRdRspEvent.scheduled())
        {
            rNic->schedule(rxDescRdRspEvent, curTick() + rNic->clockPeriod());
        }
    }
}

void HanGuRnic::RdmaArray::rcvDataRdReqSch()
{
    static int schPin = 0;
    MrReqRspPtr wrReq;
    for (int i = 0; i < coreNum; i++)
    {
        if (rcvDataRdReqQueVec[(schPin + i) % coreNum].size() != 0)
        {
            wrReq = rcvDataRdReqQueVec[(schPin + i) % coreNum].front();
            rcvDataRdReqQueVec[(schPin + i) % coreNum].pop();
            schPin = i + 1;
            break;
        }
    }
    assert(wrReq != nullptr);
    rNic->dataReqFifo.push(wrReq);
    if (!rNic->mrRescModule.transReqEvent.scheduled()) {
        rNic->schedule(rNic->mrRescModule.transReqEvent, curTick() + rNic->clockPeriod());
    }
    for (int i = 0; i < coreNum; i++)
    {
        if (rcvDataRdReqQueVec[i].size() != 0)
        {
            if (!rcvDataRdReqEvent.scheduled())
            {
                rNic->schedule(rcvDataRdReqEvent, curTick() + rNic->clockPeriod());
            }
        }
    }
}

void HanGuRnic::RdmaArray::wrRpuDataWrReqSch()
{
    MrReqRspPtr dataWrReq;
    static int schPin = 0;
    for (int i = 0; i < coreNum; i++)
    {
        if (wrRpuDataWrReqQueVec[(i + schPin) % coreNum].size() != 0)
        {
            dataWrReq = wrRpuDataWrReqQueVec[(i + schPin) % coreNum].front();
            wrRpuDataWrReqQueVec[(i + schPin) % coreNum].pop();
            schPin = i + 1;
            break;
        }
    }
    assert(dataWrReq != nullptr);
    rNic->dataReqFifo.push(dataWrReq);
    if (!rNic->mrRescModule.transReqEvent.scheduled()) {
        rNic->schedule(rNic->mrRescModule.transReqEvent, curTick() + rNic->clockPeriod());
    }
    for (int i = 0; i < coreNum; i++)
    {
        if (wrRpuDataWrReqQueVec[i].size() != 0)
        {
            if (!wrRpuDataWrReqEvent.scheduled())
            {
                rNic->schedule(wrRpuDataWrReqEvent, curTick() + rNic->clockPeriod());
            }
        }
    }
}

void HanGuRnic::RdmaArray::rdRpuDataRdReqSch()
{
    static int schPin = 0;
    MrReqRspPtr dataRdReq;
    for (int i = 0; i < coreNum; i++)
    {
        if (rdRpuDataRdReqQueVec[(schPin + i) % coreNum].size() != 0)
        {
            dataRdReq = rdRpuDataRdReqQueVec[(schPin + i) % coreNum].front();
            rdRpuDataRdReqQueVec[(schPin + i) % coreNum].pop();
            schPin = i + 1;
            break;
        }
    }
    assert(dataRdReq != nullptr);
    rNic->dataReqFifo.push(dataRdReq);
    if (!rNic->mrRescModule.transReqEvent.scheduled())
    {
        rNic->schedule(rNic->mrRescModule.transReqEvent, curTick() + rNic->clockPeriod());
    }
    for (int i = 0; i < coreNum; i++)
    {
        if (rdRpuDataRdReqQueVec[i].size() != 0)
        {
            if (!rdRpuDataRdReqEvent.scheduled())
            {
                rNic->schedule(rdRpuDataRdReqEvent, curTick() + rNic->clockPeriod());
            }
        }
    }
}

void HanGuRnic::RdmaArray::rdRpuDataRdRspAlloc()
{
    assert(rNic->rxdataRspFifo.size() != 0);
    MrReqRspPtr dataRsp = rNic->rxdataRspFifo.front();
    assert(dataRsp->coreID != INVALID_CORE);
    if (rdRpuDataRdRspQueVec[dataRsp->coreID].size() < 10)
    {
        rdRpuDataRdRspQueVec[dataRsp->coreID].push(dataRsp);
    }
    if (!rdmaEngineVec[dataRsp->coreID]->rdCplRpuEvent.scheduled())
    {
        rNic->schedule(rdmaEngineVec[dataRsp->coreID]->rdCplRpuEvent, curTick() + rNic->clockPeriod());
    }
    if (rNic->rxdataRspFifo.size() != 0)
    {
        if (!rdRpuDataRdRspEvent.scheduled()){
            rNic->schedule(rdRpuDataRdRspEvent, curTick() + rNic->clockPeriod());
        }
    }
}

void HanGuRnic::RdmaArray::rcvCqcRdRspAlloc()
{
    CxtReqRspPtr cqcRsp;
    cqcRsp = rNic->rxCqcRspFifo.front();
    rNic->rxCqcRspFifo.pop();
    assert(cqcRsp->coreID != INVALID_CORE);
    if (rcvCqcRdRspQueVec[cqcRsp->coreID].size() < 10)
    {
        rcvCqcRdRspQueVec[cqcRsp->coreID].push(cqcRsp);
    }
    if (!rdmaEngineVec[cqcRsp->coreID]->rcuEvent.scheduled())
    {
        rNic->schedule(rdmaEngineVec[cqcRsp->coreID]->rcuEvent, curTick() + rNic->clockPeriod());
    }
    if (!rNic->rxCqcRspFifo.empty())
    {
        if (!rcvCqcRdRspEvent.scheduled())
        {
            rNic->schedule(rcvCqcRdRspEvent, curTick() + rNic->clockPeriod());
        }
    }
}

void HanGuRnic::RdmaArray::rcvCqDescWrReqSch()
{
    static int schPin = 0;
    MrReqRspPtr cqDescWrReq;
    for (int i = 0; i < coreNum; i++)
    {
        if (!rcvCqDescWrReqQueVec[(i + schPin) % coreNum].empty())
        {
            cqDescWrReq = rcvCqDescWrReqQueVec[(i + schPin) % coreNum].front();
            assert(cqDescWrReq->coreID != INVALID_CORE);
            rcvCqDescWrReqQueVec[(i + schPin) % coreNum].pop();
            schPin = i + 1;
            break;
        }
    }
    assert(cqDescWrReq != nullptr);
    rNic->cqWreqFifo.push(cqDescWrReq);
    if (!rNic->mrRescModule.transReqEvent.scheduled()) { // If not scheduled yet, schedule the event.
        rNic->schedule(rNic->mrRescModule.transReqEvent, curTick() + rNic->clockPeriod());
    }
    for (int i = 0; i < coreNum; i++)
    {
        if (!rcvCqDescWrReqQueVec[i].empty())
        {
            if (!rcvCqDescWrReqEvent.scheduled())
            {
                rNic->schedule(rcvCqDescWrReqEvent, curTick() + rNic->clockPeriod());
            }
        }
    }
}
///////////////////////////// HanGuRnic::RDMA Array relevant {end}//////////////////////////////
