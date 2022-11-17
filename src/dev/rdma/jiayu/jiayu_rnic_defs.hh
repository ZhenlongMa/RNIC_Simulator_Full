#include <memory>
#include "base/bitfield.hh"
#include "sim/eventq.hh"

#define ADD_FIELD32(NAME, OFFSET, BITS) \
    inline uint32_t NAME() { return bits(_data, OFFSET+BITS-1, OFFSET); } \
    inline void NAME(uint32_t d) { replaceBits(_data, OFFSET+BITS-1, OFFSET,d); }

#define ADD_FIELD64(NAME, OFFSET, BITS) \
    inline uint64_t NAME() { return bits(_data, OFFSET+BITS-1, OFFSET); } \
    inline void NAME(uint64_t d) { replaceBits(_data, OFFSET+BITS-1, OFFSET,d); }

struct Regs : public Serializable {
    template<class T>
    struct Reg {
        T _data;
        T operator()() { return _data; }
        const Reg<T> &operator=(T d) { _data = d; return *this;}
        bool operator==(T d) { return d == _data; }
        void operator()(T d) { _data = d; }
        Reg() { _data = 0; }
        // void serialize(CheckpointOut &cp) const
        // {
        //     SERIALIZE_SCALAR(_data);
        // }
        // void unserialize(CheckpointIn &cp)
        // {
        //     UNSERIALIZE_SCALAR(_data);
        // }
    };
    
    struct INPARAM : public Reg<uint64_t> {
        using Reg<uint64_t>::operator=;
        ADD_FIELD64(iparaml,0,32);
        ADD_FIELD64(iparamh,32,32);
    };
    INPARAM inParam;

    uint32_t modifier;
    
    struct OUTPARAM : public Reg<uint64_t> {
        using Reg<uint64_t>::operator=;
        ADD_FIELD64(oparaml,0,32);
        ADD_FIELD64(oparamh,32,32);
    };
    OUTPARAM outParam;

    struct CMDCTRL : public Reg<uint32_t> {
        using Reg<uint32_t>::operator=;
        ADD_FIELD32(op,0,8);
        ADD_FIELD32(go,31,1);
    };
    CMDCTRL cmdCtrl;

    struct DOORBELL : public Reg<uint64_t> {
        using Reg<uint64_t>::operator=;
        ADD_FIELD64(dbl,0,32);
        ADD_FIELD64(dbh,32,32);
        ADD_FIELD64(opcode,0,4);
        ADD_FIELD64(offset,4,28);
        ADD_FIELD64(num,32,8);
        ADD_FIELD64(qpn,40,24);
    };
    DOORBELL db;


    uint64_t mptBase;
    uint64_t mttBase;
    uint64_t qpcBase;
    uint64_t cqcBase;

    uint8_t  mptNumLog;
    uint8_t  mttNumLog;
    uint8_t  qpcNumLog;
    uint8_t  cqcNumLog;

    
    // void serialize(CheckpointOut &cp) const override {
    //     paramOut(cp, "inParam", inParam._data);
    //     paramOut(cp, "modifier", modifier);
    //     paramOut(cp, "outParam", outParam._data);
    //     paramOut(cp, "cmdCtrl", cmdCtrl._data);
    //     paramOut(cp, "db", db._data);
    //     paramOut(cp, "mptBase", mptBase);
    //     paramOut(cp, "mttBase", mttBase);
    //     paramOut(cp, "qpcBase", qpcBase);
    //     paramOut(cp, "cqcBase", cqcBase);
    //     paramOut(cp, "mptNumLog", mptNumLog);
    //     paramOut(cp, "mttNumLog", mttNumLog);
    //     paramOut(cp, "qpcNumLog", qpcNumLog);
    //     paramOut(cp, "cqcNumLog", cqcNumLog);
    // }

    // void unserialize(CheckpointIn &cp) override {

    //     paramIn(cp, "inParam", inParam._data);
    //     paramIn(cp, "modifier", modifier);
    //     paramIn(cp, "outParam", outParam._data);
    //     paramIn(cp, "cmdCtrl", cmdCtrl._data);
    //     paramIn(cp, "db", db._data);
    //     paramIn(cp, "mptBase", mptBase);
    //     paramIn(cp, "mttBase", mttBase);
    //     paramIn(cp, "qpcBase", qpcBase);
    //     paramIn(cp, "cqcBase", cqcBase);
    //     paramIn(cp, "mptNumLog", mptNumLog);
    //     paramIn(cp, "mttNumLog", mttNumLog);
    //     paramIn(cp, "qpcNumLog", qpcNumLog);
    //     paramIn(cp, "cqcNumLog", cqcNumLog);
    // }
};


struct Hcr {
    uint32_t inParam_l;
    uint32_t inParam_h;
    uint32_t inMod;
    uint32_t outParam_l;
    uint32_t outParam_h;
    uint32_t goOpcode;
};
// Command Opcode for CEU command
const uint8_t INIT_ICM  = 0x01;
const uint8_t WRITE_ICM = 0x02;
const uint8_t WRITE_MPT = 0x03;
const uint8_t WRITE_MTT = 0x04;
const uint8_t WRITE_QPC = 0x05;
const uint8_t WRITE_CQC = 0x06;

struct Doorbell {
    uint8_t  opcode;
    uint8_t  num;
    uint32_t qpn;
    uint32_t offset;
};
// Command Opcode for Doorbell command
const uint8_t OPCODE_NULL       = 0x00;
const uint8_t OPCODE_SEND       = 0x01;
const uint8_t OPCODE_RECV       = 0x02;
const uint8_t OPCODE_RDMA_WRITE = 0x03;
const uint8_t OPCODE_RDMA_READ  = 0x04;

struct DoorbellFifo {
    DoorbellFifo (uint8_t  opcode, uint8_t  num, 
            uint32_t qpn, uint32_t offset) {
        this->opcode = opcode;
        this->num = num;
        this->qpn = qpn;
        this->offset = offset;
    }
    uint8_t  opcode;
    uint8_t  num;
    uint32_t qpn;
    uint32_t offset;
    // Addr     qpAddr;
};
typedef std::shared_ptr<DoorbellFifo> DoorbellPtr;

struct InitResc {
    uint8_t qpsNumLog;
    uint8_t cqsNumLog;
    uint8_t mptNumLog;
    uint64_t qpcBase;
    uint64_t cqcBase;
    uint64_t mptBase;
    uint64_t mttBase;
};

struct MptItem
{
    uint32_t pageSize;
    uint32_t key;
    uint32_t pd;
    uint64_t startVA;
    uint64_t length;
    uint64_t mttSeg;
};

struct MttItem
{
    uint64_t pA;
};

struct MrReqRsp
{
    uint8_t type; // 1: write request; 2: read request; 3: read response
    uint8_t channel;
    uint32_t key;
    uint32_t length;
    uint64_t offset;
    union
    {
        uint8_t *data;
    };
};
typedef std::shared_ptr<MrReqRsp> MrReqRspPtr;

const uint8_t TPT_WCHNL_TX_CQ = 0x01;
const uint8_t TPT_WCHNL_RX_CQ = 0x02;
const uint8_t TPT_WCHNL_TX_DATA = 0x03;
const uint8_t TPT_WCHNL_RX_DATA = 0x04;
const uint8_t TPT_RCHNL_TX_DESC = 0x05;
const uint8_t TPT_RCHNL_RX_DESC = 0x06;
const uint8_t TPT_RCHNL_TX_DATA = 0x07;
const uint8_t TPT_RCHNL_RX_DATA = 0x08;

struct DmaReq 
{
    DmaReq (Addr addr, int size, Event *event, uint8_t *data, uint32_t chnl=0) 
    {
        this->addr  = addr;
        this->size  = size;
        this->event = event;
        this->data  = data;
        this->channel  = chnl;
        this->rdVld = 0;
        this->schd  = 0;
        this->reqType = 0;
    }
    Addr         addr  ; 
    int          size  ; 
    Event       *event ;
    uint8_t      rdVld ; /* the dma req's return data is valid */
    uint8_t     *data  ; 
    uint32_t     channel  ; /* channel number the request belongs to, see below DMA_REQ_* for details */
    Tick         schd  ; /* when to schedule the event */
    uint8_t      reqType; /* type of request: 0 for read request, 1 for write request */
};
typedef std::shared_ptr<DmaReq> DmaReqPtr;
const uint8_t DMA_TYPE_WREQ = 0x01;
const uint8_t DMA_TYPE_RREQ = 0x02;
const uint8_t DMA_TYPE_RRSP = 0x03;