// #include "dev/rdma/hangu_rnic.hh"
#include <algorithm>
#include <memory>
#include <queue>
#include "dev/rdma/hangu_rnic_defs.hh"

#include "base/inet.hh"
// #include "base/trace.hh"
#include "base/random.hh"
// #include "debug/Drain.hh"
// #include "dev/net/etherpkt.hh"
// #include "debug/HanGu.hh"
// #include "mem/packet.hh"
// #include "mem/packet_access.hh"
// #include "params/HanGuRnic.hh"
// #include "sim/stats.hh"
// #include "sim/system.hh"

using namespace HanGuRnicDef;
using namespace Net;
using namespace std;

/* -----------------------Cache {begin}------------------------ */
template <class T, class S>
class RescCache {
    private:

        struct CacheRdPkt {
            CacheRdPkt(Event *cplEvent, uint32_t rescIdx, 
                    T *rescDma, S reqPkt, DmaReqPtr dmaReq, T *rspResc, const std::function<bool(T&)> &rescUpdate) 
            : cplEvent(cplEvent), rescIdx(rescIdx), rescDma(rescDma), reqPkt(reqPkt), 
                rspResc(rspResc), 
                rescUpdate(rescUpdate) { this->dmaReq = dmaReq; }
            Event   *cplEvent; /* event to be scheduled when resource fetched */
            uint32_t rescIdx; /* resource index */
            T       *rescDma; /* addr used to get resc through DMA read */
            S        reqPkt ; /* temp store the request pkt */
            DmaReqPtr dmaReq; /* DMA read request pkt, to fetch missed resource (cache), 
                                we only use its isValid to fetch the rsp */
            T       *rspResc; /* addr used to rsp the requester !TODO: delete it later */
            const std::function<bool(T&)> rescUpdate;
        };

        /* Pointer to the device I am in. */
        HanGuRnic *rnic;

        /* Stores my name in string */
        std::string _name;

        /* Cache for resource T */
        std::unordered_map<uint32_t, T> cache;
        uint32_t capacity; /* number of cache entries this Resource cache owns */

        /* used to process cache read */
        void readProc();
        EventFunctionWrapper readProcEvent;
    
        /* Request FIFO, Only used in Cache Read.
        * Used to temp store request pkt in order */
        std::queue<CacheRdPkt> reqFifo;
        

        // Base ICM address of resources in ICM space.
        uint64_t baseAddr;
        
        // Storage for ICM
        uint64_t *icmPage;
        uint32_t rescSz; // size of one entry of resource

        // Convert resource number into physical address.
        uint64_t rescNum2phyAddr(uint32_t num);

        /* Cache replace scheme, return key in cache */
        uint32_t replaceScheme();

        // Write evited elem back to memory
        void storeReq(uint64_t addr, T *resc);

        // Read wanted elem from memory
        void fetchReq(uint64_t addr, Event *cplEvent, uint32_t rescNum, S reqPkt, T *resc, const std::function<bool(T&)> &rescUpdate=nullptr);

        /* get fetched data from memory */
        void fetchRsp();
        EventFunctionWrapper fetchCplEvent;
        
        /* read req -> read rsp Fifo
        * Used only in Read Cache miss. */
        std::queue<CacheRdPkt> rreq2rrspFifo;

    public:

        RescCache (HanGuRnic *i, uint32_t cacheSize, const std::string n) 
        : rnic(i),
            _name(n),
            capacity(cacheSize),
            readProcEvent([this]{ readProc(); }, n),
            fetchCplEvent([this]{ fetchRsp(); }, n) { icmPage = new uint64_t [ICM_MAX_PAGE_NUM]; rescSz = sizeof(T); }

        /* Set base address of ICM space */
        void setBase(uint64_t base);

        /* ICM Write Request */
        void icmStore(IcmResc *icmResc, uint32_t chunkNum);

        /* Write resource back to Cache */
        void rescWrite(uint32_t rescIdx, T *resc, const std::function<bool(T&, T&)> &rescUpdate=nullptr);

        /* Read resource from Cache */
        void rescRead(uint32_t rescIdx, Event *cplEvent, S reqPkt, T *rspResc=nullptr, const std::function<bool(T&)> &rescUpdate=nullptr);

        /* Outer module uses to get cache entry (so don't delete the element) */
        std::queue<std::pair<T *, S> > rrspFifo;

        std::string name() { return _name; }
};
/* -----------------------Cache {end}------------------------ */


///////////////////////////// HanGuRnic::Resource Cache {begin}//////////////////////////////
template <class T, class S>
uint32_t RescCache<T, S>::replaceScheme() {
    
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
void RescCache<T, S>::storeReq(uint64_t addr, T *resc) {

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
void RescCache<T, S>::fetchReq(uint64_t addr, Event *cplEvent, 
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
void RescCache<T, S>::fetchRsp() {

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
void RescCache<T, S>::setBase(uint64_t base) {
    baseAddr = base;
}

template <class T, class S>
void RescCache<T, S>::icmStore(IcmResc *icmResc, uint32_t chunkNum) {
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
uint64_t RescCache<T, S>::rescNum2phyAddr(uint32_t num) {
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
void RescCache<T, S>::rescWrite(uint32_t rescIdx, T *resc, const std::function<bool(T&, T&)> &rescUpdate) {

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
void RescCache<T, S>::rescRead(uint32_t rescIdx, Event *cplEvent, S reqPkt, T *rspResc, const std::function<bool(T&)> &rescUpdate) {

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
void RescCache<T, S>::readProc() {

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
