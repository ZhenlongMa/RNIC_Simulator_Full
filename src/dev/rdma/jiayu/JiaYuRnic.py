from m5.objects.PciDevice import PciDevice
from m5.params import *

class JiaYuRnic(PciDevice):
    type = "JiaYuRnic"
    cxx_header = "dev/rdma/jiayu/jiayu_rnic.hh"

class JiaYuDriver(EmulatedDriver):
    type = "JiaYuDriver"
    cxx_header = "dev/rdma/jiayu/jiayu_driver.hh"
    device = Param.JiaYuRnic("JiaYu Rnic")