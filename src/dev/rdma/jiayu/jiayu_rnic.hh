#ifndef __JIAYU_RNIC_HH__
#define __JIAYU_RNIC_HH__

#include <deque>
#include <queue>
#include <unordered_map>

#include "dev/rdma/jiayu/jiayu_rnic_defs.hh"
#include "dev/rdma/jiayu/rdma_engine.hh"
#include "params/HanGuRnic.hh"

class JiaYuRnic : public RdmaNic
{
    private:
        JiaYuRnicInterface *ethIf;

        // hardware registers
        Regs regs;

        // interact with Ethernet link
        std::queue<EthPacketPtr> txFifo;
        std::queue<EthPacketPtr> rxFifo;

        /* --------------------TPT <-> DMA Engine {begin}-------------------- */
        std::queue<DmaReqPtr> descDmaReadFifo;
        std::queue<DmaReqPtr> dataDmaReadFifo;
        std::queue<DmaReqPtr> dataDmaWriteFifo;
        std::queue<DmaReqPtr> cqDmaWriteFifo;
        std::queue<DmaReqPtr> icmDmaReadFifo;
        std::queue<DmaReqPtr> icmDmaWriteFifo;
        /* --------------------TPT <-> DMA Engine {end}-------------------- */

        std::queue<DmaReqPtr> mbxDmaReadFifo;

        uint8_t* mailbox;

        class RdmaEngine
        {
            protected:
                JiaYuRnic *rnic;
                std::string _name;

                // desc fetch unit to desc decode unit
                std::queue<DoorbellPtr> df2ddFifo;

                // desc decode unit
                DoorbellPtr dduDoorbell;

                // desc decode unit -> desc process unit
                std::vector<TxDescPtr> dd2dpVector;

                // desc process unit -> desc decode unit
                std::queue<uint8_t> dp2ddIdxFifo;

            public:
                RdmaEngine(const std::string n)
                    : rnic(rnic),
                    _name(n),
                    dfuEvent ([this]{ dfuProcessing(); }, n),
                    dduEvent ([this]{ dduProcessing(); }, n),
                    dpuEvent ([this]{ dpuProcessing(); }, n),
                    rgrrEvent([this]{ rgrrProcessing();}, n),
                    scuEvent ([this]{ scuProcessing(); }, n),
                    sauEvent ([this]{ sauProcessing(); }, n),
                    rauEvent ([this]{ rauProcessing(); }, n),
                    rpuEvent ([this]{ rpuProcessing(); }, n),
                    rcvRpuEvent  ([this]{rcvRpuProcessing();  }, n),
                    rdCplRpuEvent([this]{rdCplRpuProcessing();}, n),
                    rcuEvent([this]{ rcuProcessing();}, n) 
                {

                }

                std::string name() 
                { 
                    return _name; 
                }

                // event for tx packet
                void dfuProcessing(); // Descriptor Fetching Unit
                EventFunctionWrapper dfuEvent;
                
                void dduProcessing(); // Descriptor decode Unit
                EventFunctionWrapper dduEvent;
                
                void dpuProcessing(); // Descriptor processing unit
                EventFunctionWrapper dpuEvent;
                
                void rgrrProcessing(); // Request Generation & Response Receiving Unit
                EventFunctionWrapper rgrrEvent;
                
                void scuProcessing(); // Send Completion Unit
                EventFunctionWrapper scuEvent;
                
                void sauProcessing(); // Send Arbiter Unit, directly post data to link layer
                EventFunctionWrapper sauEvent;

                
                // event for rx packet
                void rauProcessing(); // Receive Arbiter Unit
                EventFunctionWrapper rauEvent;
                
                void rpuProcessing(); // Receive Processing Unit
                EventFunctionWrapper rpuEvent;

                void rcvRpuProcessing ();
                EventFunctionWrapper rcvRpuEvent;

                void rdCplRpuProcessing(); // RDMA read Receive Processing Completion Unit
                EventFunctionWrapper rdCplRpuEvent;

                void rcuProcessing(); // Receive Completion Unit
                EventFunctionWrapper rcuEvent;

        };

        class MrModule
        {
            protected:
                std::string n;
                JiaYuRnic *rNic;

                EventFunctionWrapper DmaRspEvent;
                void dmaRspProc();
                EventFunctionWrapper MptRspEvent;
                void MptRspProc();
                EventFunctionWrapper MttRspEvent;
                void MttRspProc();

                std::queue<std::pair<MrReqRspPtr, DmaReqPtr> dmaReq2RspFifo;
                void dmaReqProc(uint64_t pAddr, MrReqRspPtr tptReq);

                void mptReqProc(MrReqRspPtr tptReq);
                void mttReqProc();
                bool isValid(MrReqRspPtr tptReq);

                uint64_t icmMptBase;
                uint64_t icmMttBase;
            
            public:
                MrModule(std::string name, uint32_t mptCacheNum, uint32_t mttCacheNum);
                EventFunctionWrapper transReqEvent;
                void transReqProc();
                CommRescCache <MptItem> MptCache;
                CommRescCache <MttItem> MttCache;
        };

        class CqcModule
        {

        };

        class IcmManage
        {

        };

        template <class T>
        class CommRescCache
        {
            private:
                std::unordered_map<uint32_t, T*> cache;
                uint32_t capacity;

            public:
                CommRescCache(int entryNum)
                : capacity(entryNum)
                {

                }
                bool isHit(uint32_t entryId);
                bool isFull();
                bool update(uint32_t entryId, const std::function<bool(T&)> &updateFunc = nullptr);
                bool readEntry(uint32_t entryId, T *outputEntry);
                bool writeEntry(uint32_t entryId, T* inputEntry);
                bool deleteEntry(uint32_t entryId);
        };

        class QpcModule
        {

        };

        class DmaEngine
        {
            protected:
                JiaYuRnic *rnic;

                uint8_t readIdx;
                uint8_t writeIdx;

            public:
                DmaEngine(JiaYuRnic *dev):
                    rnic(dev),
                    readIdx(0),
                    writeIdx(0)
                {

                }
                // std::queue<DmaReqPtr> dmaQrReq2RspFifo;
                std::queue<DmaReqPtr> dmaRdReq2RspFifo;
                std::queue<DmaReqPtr> dmaRdReqFifo;
                std::queue<DmaReqPtr> dmaWrReqFifo;

                EventFunctionWrapper dmaWriteCplEvent;
                EventFunctionWrapper dmaReadCplEvent;
                EventFunctionWrapper dmaChnlProcEvent; // ?
                EventFunctionWrapper dmaWriteEvent;
                EventFunctionWrapper dmaReadEvent;
                void dmaWriteProc();
                void dmaReadProc();
                void dmaChnlProcEvent();
        };

        class PacketScheduler
        {

        };

        RdmaEngine RdmaCore[];
        DmaEngine dmaEngine(this);
        MrModule mrModule(this);
    
    public:
        JiaYuRnic(const Params *params);
        ~JiaYuRnic();
        void init() override;
        Port &getPort(const std::string &if_name, PortID idx=InvalidPortID) override;
        
        // PIO
        Tick writeConfig(PacketPtr pkt) override;
        Tick read(PacketPtr pkt) override;
        Tick write(PacketPtr pkt) override;

        Tick dmaReadDelay; // ps
        Tick dmaWriteDelay; //ps
        uint32_t pciSpeed; // ps/Byte


};

class JiaYuRnicInterface: public EtherInt
{
    private:
        JiaYuRnic *dev;

    public:
        JiaYuRnicInterface(const std::string &name, JiaYuRnic *d)
            : EgherInt(name, dev(d))
            {

            }
        virtual bool recvPacket(EthPacketPtr pkt)
        {
            return dev->ethRxDelay(pkt);
        }
        virtual void sendDone()
        {
            dev->ethTxDone();
        }
}

#endif