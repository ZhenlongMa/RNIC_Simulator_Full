#include "dev/rdma/hangu_rnic_defs.hh"
#include <algorithm>
#include <memory>
#include <queue>

// #include "base/inet.hh"
// #include "base/trace.hh"
// #include "base/random.hh"
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

/* -----------------------QPC Cache {begin}---------------------- */
template <class T>
class Cache {
    private:
        /* Name of myself */
        std::string _name;

        /* Cache for resource T */
        std::unordered_map<uint32_t, std::pair<T*, uint64_t> > cache; /* <entryNum, <entry, lru>> */
        uint32_t capacity; /* number of cache entries this cache owns */
        uint32_t cacheSz;

        uint64_t seq_end;

    public:
        Cache (const std::string n, uint32_t qpcCacheNum)
        : _name(n),
            capacity(qpcCacheNum),
            seq_end(0) { cacheSz = sizeof(T); }

        /* Cache replace scheme, return key in cache */
        uint32_t replaceEntry();

        /* lookup entry in cache */
        bool lookupHit(uint32_t entryNum); /* return true if really hit */
        bool lookupFull(uint32_t entryNum); /* return true if cache is full */

        /* read entry from cache */
        bool readEntry(uint32_t entryNum, T* entry); /* use memcpy to get entry */

        bool updateEntry(uint32_t entryNum, const std::function<bool(T&)> &update=nullptr);

        /* write entry to cache */
        bool writeEntry(uint32_t entryNum, T* entry); /* use memcpy to write entry */

        /* delete entry in cache */
        T* deleteEntry(uint32_t entryNum);

        std::string name() { return _name; }
};
/* -----------------------QPC Cache {end}---------------------- */


///////////////////////////// HanGuRnic::Cache {begin}//////////////////////////////
template<class T>
uint32_t Cache<T>::replaceEntry() {

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
bool Cache<T>::lookupHit(uint32_t entryNum) {
    bool res = (cache.find(entryNum) != cache.end());
    if (res) { /* if hit update the state of the entry */
        cache[entryNum].second = seq_end++;
    }
    return res;
}

template<class T>
bool Cache<T>::lookupFull(uint32_t entryNum) {
    return cache.size() == capacity;
}

template<class T>
bool Cache<T>::readEntry(uint32_t entryNum, T* entry) {
    assert(cache.find(entryNum) != cache.end());

    memcpy(entry, cache[entryNum].first, sizeof(T));
    return true;
}

template<class T>
bool Cache<T>::updateEntry(uint32_t entryNum, const std::function<bool(T&)> &update) {
    assert(cache.find(entryNum) != cache.end());
    assert(update != nullptr);

    return update(*cache[entryNum].first);
}

template<class T>
bool Cache<T>::writeEntry(uint32_t entryNum, T* entry) {
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
T* Cache<T>::deleteEntry(uint32_t entryNum) {
    assert(cache.find(entryNum) != cache.end());
    
    T *rtnResc = cache[entryNum].first;
    cache.erase(entryNum);
    assert(cache.size() == capacity - 1);
    return rtnResc;
}
///////////////////////////// HanGuRnic::Cache {end}//////////////////////////////
