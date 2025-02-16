#include "dev/rdma/hangu_rnic.hh"
#include <algorithm>
#include <memory>
#include <queue>
// #include "dev/rdma/hangu_rnic_defs.hh"

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

///////////////////////////// HanGuRnic::Resource Cache {begin}//////////////////////////////
template <class T, class S>
uint32_t HanGuRnic::RescCache<T, S>::replaceScheme() {
    uint32_t cnt = random_mt.random(0, (int)cache.size() - 1);
    uint32_t rescNum = cache.begin()->first;
    for (auto iter = cache.begin(); iter != cache.end(); ++iter, --cnt) {
        if (cnt == 0) {
            rescNum = iter->first;
        }
    }
    return rescNum;
}

template <class T, class S>
uint32_t HanGuRnic::RescCache<T, S>::lruReplaceScheme() {
    uint64_t temp = maxParam;
    int replaceIdx = -1;
    for (auto iter = cache.begin(); iter != cache.end(); iter++) {
        assert(replaceParam.find(iter->first) != replaceParam.end());
        if (replaceParam[iter->first] < temp) {
            temp = replaceParam[iter->first];
            replaceIdx = iter->first;
        }
    }
    HANGU_PRINT(RescCache, "replace index: %d\n", replaceIdx);
    assert(replaceIdx >= 0);
    return replaceIdx;
}

template <class T, class S>
void HanGuRnic::RescCache<T, S>::storeReq(uint64_t addr, T *resc) {
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
void HanGuRnic::RescCache<T, S>::fetchReq(uint64_t addr, Event *cplEvent, 
        uint32_t rescIdx, S reqPkt, T *rspResc, const std::function<bool(T&)> &rescUpdate) {
    HANGU_PRINT(RescCache, "fetchReq: enter\n");
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
    HANGU_PRINT(RescCache, "fetchReq: fifo size %d\n", rreq2rrspFifo.size());
}

template <class T, class S>
void HanGuRnic::RescCache<T, S>::fetchRsp() {
    HANGU_PRINT(RescCache, "fetchRsp: capacity: %d, size %d, rescSz %d\n", capacity, cache.size(), sizeof(T));
    if (rreq2rrspFifo.empty()) {
        return ;
    }
    CacheRdPkt rrsp = rreq2rrspFifo.front();
    if (rrsp.dmaReq->rdVld == 0) {
        return;
    }
    if (std::is_same<T, MptResc>::value) {
        HANGU_PRINT(RescCache, "fetchRsp: MPT[%d] request time until fetchRsp: %ld\n", rrsp.reqPkt->chnl, curTick() - rrsp.reqPkt->reqTick);
    }
    rreq2rrspFifo.pop();
    HANGU_PRINT(RescCache, "fetchRsp: rescNum %d, dma_addr 0x%lx, rsp_addr 0x%lx, fifo size %d\n", 
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
            HANGU_PRINT(RescCache, "fetchRsp: capacity %d size %d\n", capacity, cache.size());
        } else { /* Cache is full */
            HANGU_PRINT(RescCache, "fetchRsp: Cache is full!\n");
            // uint32_t wbRescNum = replaceScheme();
            uint32_t wbRescNum = lruReplaceScheme();
            uint64_t pAddr = rescNum2phyAddr(wbRescNum);
            T *wbReq = new T;
            memcpy(wbReq, &(cache[wbRescNum]), sizeof(T));
            storeReq(pAddr, wbReq);
            // Output printing
            if (sizeof(T) == sizeof(struct QpcResc)) {
                struct QpcResc *val = (struct QpcResc *)(rrsp.rescDma);
                struct QpcResc *rep = (struct QpcResc *)(wbReq);
                HANGU_PRINT(RescCache, "fetchRsp: qpn 0x%x, sndlkey 0x%x \n\n", 
                        val->srcQpn, val->sndWqeBaseLkey);
                HANGU_PRINT(RescCache, "fetchRsp: replaced qpn 0x%x, sndlkey 0x%x \n\n", 
                        rep->srcQpn, rep->sndWqeBaseLkey);
            }
            cache.erase(wbRescNum);
            cache.emplace(rrsp.rescIdx, *(rrsp.rescDma));
            HANGU_PRINT(RescCache, "fetchRsp: capacity %d size %d, replaced idx %d pAddr 0x%lx\n", 
                    capacity, cache.size(), wbRescNum, pAddr);
        }
        /* Push fetched resource to FIFO */ 
        if (rrsp.rspResc) {
            memcpy(rrsp.rspResc, rrsp.rescDma, sizeof(T));
        }
    }
    /* Schedule read response cplEvent */
    if (rrsp.cplEvent == nullptr) { // this is a write request
        HANGU_PRINT(RescCache, "fetchRsp: this is a write request!\n");
    } else { // this is a read request
        if (!rrsp.cplEvent->scheduled()) {
            rnic->schedule(rrsp.cplEvent, curTick() + rnic->clockPeriod());
        }
        rrspFifo.emplace(rrsp.rescDma, rrsp.reqPkt);
    }
    HANGU_PRINT(RescCache, "fetchRsp: Push fetched resource to FIFO!\n");
    /* Update content in cache entry
    * Note that this should be called last because 
    * we hope get older resource, not updated resource */
    if (rrsp.rescUpdate == nullptr) {
        HANGU_PRINT(RescCache, "fetchRsp: rescUpdate is null!\n");
    } else {
        HANGU_PRINT(RescCache, "fetchRsp: rescUpdate is not null\n");
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
    } else { /* schedule readProc if it does not have pending read req **/
        // if (!readProcEvent.scheduled()) {
        //     rnic->schedule(readProcEvent, curTick() + rnic->clockPeriod());
        // }
    }
    HANGU_PRINT(RescCache, "fetchRsp: out\n");
}

template <class T, class S>
void HanGuRnic::RescCache<T, S>::setBase(uint64_t base) {
    baseAddr = base;
}

template <class T, class S>
void HanGuRnic::RescCache<T, S>::icmStore(IcmResc *icmResc, uint32_t chunkNum) {
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
uint64_t HanGuRnic::RescCache<T, S>::rescNum2phyAddr(uint32_t num) {
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
void HanGuRnic::RescCache<T, S>::rescWrite(uint32_t rescIdx, T *resc, const std::function<bool(T&, T&)> &rescUpdate) {
    HANGU_PRINT(RescCache, "rescWrite! capacity: %d, size: %d rescSz %d, rescIndex %d\n", 
            capacity, cache.size(), sizeof(T), rescIdx);
    // if (replaceParam.find(rescIdx) == replaceParam.end()) {
    //     replaceParam[rescIdx] = 0;
    // }
    if (cache.find(rescIdx) != cache.end()) { /* Cache hit */
        HANGU_PRINT(RescCache, "rescWrite: Cache hit\n");
        /* If there's specified update function */
        if (rescUpdate == nullptr) {
            T tmp = cache[rescIdx];
            cache.erase(rescIdx);
            delete &tmp;
            cache.emplace(rescIdx, *resc);
            HANGU_PRINT(RescCache, "rescWrite: Resc is written\n");
        } else {
            rescUpdate(cache[rescIdx], *resc);
            HANGU_PRINT(RescCache, "rescWrite: Desc updated\n");
        }
        HANGU_PRINT(RescCache, " RescCache: capacity %d size %d\n", capacity, cache.size());
    } else if (cache.size() < capacity) { /* Cache miss & insert elem directly */
        HANGU_PRINT(RescCache, "rescWrite: Cache miss\n");
        cache.emplace(rescIdx, *resc);
        HANGU_PRINT(RescCache, " RescCache: capacity %d size %d\n", capacity, cache.size());
    } else if (cache.size() == capacity) { /* Cache miss & replace */
        HANGU_PRINT(RescCache, "rescWrite: Cache miss & replace\n");
        /* Select one elem in cache to evict */
        // uint32_t wbRescNum = replaceScheme();
        uint32_t wbRescNum = lruReplaceScheme();
        uint64_t pAddr = rescNum2phyAddr(wbRescNum);
        T *writeReq = new T;
        memcpy(writeReq, &(cache[wbRescNum]), sizeof(T));
        storeReq(pAddr, writeReq);
        cache.erase(wbRescNum);
        cache.emplace(rescIdx, *resc);
        HANGU_PRINT(RescCache, "rescWrite: wbRescNum %d, ICM_paddr_base 0x%x, new_index %d\n", wbRescNum, pAddr, rescIdx);
        HANGU_PRINT(RescCache, " RescCache: capacity %d size %d\n", capacity, cache.size());
    } else {
        panic("rescWrite: mismatch! capacity %d size %d\n", capacity, cache.size());
    }
    replaceParam[rescIdx] = maxParam;
    maxParam++;
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
void HanGuRnic::RescCache<T, S>::rescRead(uint32_t rescIdx, Event *cplEvent, S reqPkt, T *rspResc, const std::function<bool(T&)> &rescUpdate) {
    HANGU_PRINT(RescCache, "rescRead! cap: %d, rescIdx %d, is_write %d, rescSz: %d, size: %d\n", 
            capacity, rescIdx, (cplEvent == nullptr), sizeof(T), cache.size());
    /* push event to fetchRsp */
    reqFifo.emplace(cplEvent, rescIdx, nullptr, reqPkt, nullptr, rspResc, rescUpdate);
    if (!readProcEvent.scheduled()) {
        rnic->schedule(readProcEvent, curTick() + rnic->clockPeriod());
    }
    replaceParam[rescIdx] = maxParam;
    maxParam++;
    if (std::is_same<T, MptResc>::value) {
        HANGU_PRINT(RescCache, "rescRead: MPT[%d] launch time: %ld, request time until rescRead: %ld\n", 
            reqPkt->chnl, reqPkt->reqTick, curTick() - reqPkt->reqTick);
    }
    HANGU_PRINT(RescCache, "rescRead: out! reqFifo size: %d\n", reqFifo.size());
}

/**
 * @note This Func get resource req and put it back to rrspFifo.
 *      Note that this function returns resc in two data struct:
 *      1. rrspFifo, this Fifo stores reference to the cache.
 *      2. T *rspResc, this input is an optional, which may be "nullptr"
 */
template <class T, class S>
void HanGuRnic::RescCache<T, S>::readProc() {
    /* If there's pending read req or there's no req in reqFifo, 
    * do not process next rquest */
    // if (rreq2rrspFifo.size() || reqFifo.empty()) {
    //     return;
    // }
    /* Get cache rd req pkt from reqFifo */
    assert(reqFifo.size() > 0);
    CacheRdPkt rreq = reqFifo.front();
    uint32_t rescIdx = rreq.rescIdx;
    reqFifo.pop();
    /* only used to dump information */
    HANGU_PRINT(RescCache, "readProc: capacity: %d, rescIdx %d, is_write %d, rescSz: %d, size: %d\n", 
            capacity, rescIdx, (rreq.cplEvent == nullptr), sizeof(T), cache.size());
    if (cache.find(rescIdx) != cache.end()) { /* Cache hit */
        HANGU_PRINT(RescCache, "readProc: Cache hit!\n");
        recordMissHit(rreq, true);
        /** 
         * If rspResc is not nullptr, which means 
         * it need to put resc to rspResc, copy 
         * data in cache entry.
         */
        if (rreq.rspResc) {
            memcpy(rreq.rspResc, &cache[rescIdx], sizeof(T));
        }
        if (rreq.cplEvent == nullptr) { // This is write request
            HANGU_PRINT(RescCache, "readProc: This is write request\n");
        } else { // This is read request
            HANGU_PRINT(RescCache, "readProc: This is read request\n");
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
    } else if (cache.size() <= capacity) { /* Cache miss & read elem */
        recordMissHit(rreq, false);
        /* Fetch required data */
        uint64_t pAddr = rescNum2phyAddr(rescIdx);
        fetchReq(pAddr, rreq.cplEvent, rescIdx, rreq.reqPkt, rreq.rspResc, rreq.rescUpdate);
        HANGU_PRINT(RescCache, "readProc resc_index %d, ICM paddr 0x%lx\n", rescIdx, pAddr);
        if (reqFifo.size()) {
            if (!readProcEvent.scheduled()) {
                rnic->schedule(readProcEvent, curTick() + rnic->clockPeriod());
            }
        }
    } else {
        panic("readProc: mismatch! capacity %d size %d\n", capacity, cache.size());
    }
    if (std::is_same<T, MptResc>::value) {
        HANGU_PRINT(RescCache, "readProc: MPT[%d] request time until readProc: %ld\n", rreq.reqPkt->chnl, curTick() - rreq.reqPkt->reqTick);
    }
    HANGU_PRINT(RescCache, "readProc: out! capacity: %d, size: %d\n", capacity, cache.size());
}

template <class T, class S>
void HanGuRnic::RescCache<T, S>::recordMissHit(CacheRdPkt &pkt, bool hit) {
    if (hit == true) {
        if (std::is_same<T, MptResc>::value) {
            if (pkt.reqPkt->chnl != MR_RCHNL_TX_DESC_PREFETCH && pkt.reqPkt->chnl != MR_RCHNL_TX_MPT_PREFETCH) {
                hitNum++;
                HANGU_PRINT(RescCache, "readProc: MPT fetch hit! hitNum: %d, missNum: %d, key: 0x%x, chnl: %d, type: %d\n", 
                    hitNum, missNum, pkt.rescIdx, pkt.reqPkt->chnl, pkt.reqPkt->type);
            }
            else if (pkt.reqPkt->chnl == MR_RCHNL_TX_DESC_PREFETCH) {
                HANGU_PRINT(RescCache, "readProc: desc MPT prefetch hit! hitNum: %d, missNum: %d, key: 0x%x, chnl: %d, type: %d\n", 
                    hitNum, missNum, pkt.rescIdx, pkt.reqPkt->chnl, pkt.reqPkt->type);
            }
            else if (pkt.reqPkt->chnl == MR_RCHNL_TX_MPT_PREFETCH) {
                HANGU_PRINT(RescCache, "readProc: data MPT prefetch hit! hitNum: %d, missNum: %d, key: 0x%x, chnl: %d, type: %d\n", 
                    hitNum, missNum, pkt.rescIdx, pkt.reqPkt->chnl, pkt.reqPkt->type);
            }
        }
        else if (std::is_same<T, MttResc>::value) {
            if (pkt.reqPkt->chnl != MR_RCHNL_TX_DESC_PREFETCH && pkt.reqPkt->chnl != MR_RCHNL_TX_MPT_PREFETCH) {
                hitNum++;
                HANGU_PRINT(RescCache, "readProc: MTT hit! hitNum: %d, missNum: %d\n", hitNum, missNum);
            }
        }
        else if (std::is_same<T, CqcResc>::value) {
            hitNum++;
            HANGU_PRINT(RescCache, "readProc: CQC hit! hitNum: %d, missNum: %d\n", hitNum, missNum);
        }
        else {
            panic("invalid type!\n");
        }
    }
    else {
        if (std::is_same<T, MptResc>::value) {
            if (pkt.reqPkt->chnl != MR_RCHNL_TX_DESC_PREFETCH && pkt.reqPkt->chnl != MR_RCHNL_TX_MPT_PREFETCH) {
                missNum++;
                HANGU_PRINT(RescCache, "readProc: MPT fetch miss! hitNum: %d, missNum: %d, key: 0x%x, chnl: %d, type: %d\n", 
                    hitNum, missNum, pkt.rescIdx, pkt.reqPkt->chnl, pkt.reqPkt->type);
            }
            else if (pkt.reqPkt->chnl == MR_RCHNL_TX_DESC_PREFETCH) {
                HANGU_PRINT(RescCache, "readProc: desc MPT prefetch miss! hitNum: %d, missNum: %d, key: 0x%x, chnl: %d, type: %d\n", 
                    hitNum, missNum, pkt.rescIdx, pkt.reqPkt->chnl, pkt.reqPkt->type);
            }
            else if (pkt.reqPkt->chnl == MR_RCHNL_TX_MPT_PREFETCH) {
                HANGU_PRINT(RescCache, "readProc: data MPT prefetch miss! hitNum: %d, missNum: %d, key: 0x%x, chnl: %d, type: %d\n", 
                    hitNum, missNum, pkt.rescIdx, pkt.reqPkt->chnl, pkt.reqPkt->type);
            }
        }
        else if (std::is_same<T, MttResc>::value) {
            if (pkt.reqPkt->chnl != MR_RCHNL_TX_DESC_PREFETCH && pkt.reqPkt->chnl != MR_RCHNL_TX_MPT_PREFETCH) {
                missNum++;
                HANGU_PRINT(RescCache, "readProc: MTT miss! hitNum: %d, missNum: %d\n", hitNum, missNum);
            }
        }
        else if (std::is_same<T, CqcResc>::value) {
            missNum++;
            HANGU_PRINT(RescCache, "readProc: CQC miss! hitNum: %d, missNum: %d\n", hitNum, missNum);
        }
        else {
            panic("invalid type!\n");
        }
    }
}

///////////////////////////// HanGuRnic::Resource Cache {end}//////////////////////////////

template class HanGuRnic::RescCache<CqcResc, CxtReqRspPtr>;
template class HanGuRnic::RescCache<MptResc, MrReqRspPtr>;
template class HanGuRnic::RescCache<MttResc, MrReqRspPtr>;