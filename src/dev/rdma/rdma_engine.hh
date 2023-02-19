
/**
 * @file
 * Device model for Han Gu RNIC.
 */

#ifndef __RDMA_ENGINE_HH__
#define __RDMA_ENGINE_HH__

#include <deque>
#include <queue>
#include <string>
#include <list>
#include <unordered_map>

#include "dev/rdma/hangu_rnic_defs.hh"

#include "base/inet.hh"
#include "debug/EthernetDesc.hh"
#include "debug/EthernetIntr.hh"
#include "dev/rdma/rdma_nic.hh"
#include "dev/net/etherdevice.hh"
#include "dev/net/etherint.hh"
#include "dev/net/etherpkt.hh"
#include "dev/net/pktfifo.hh"
#include "dev/pci/device.hh"
#include "params/HanGuRnic.hh"

#endif