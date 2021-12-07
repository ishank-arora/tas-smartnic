include mk/subdir_pre.mk

objs_top := tas_nic.o config.o shm.o blocking.o
objs_sp := kernel.o packetmem.o appif.o appif_ctx.o nicif.o cc.o tcp.o arp.o \
  routing.o kni.o rdmaif.o
objs_fp := fastemu.o network.o qman.o trace.o fast_kernel.o fast_appctx.o \
  fast_flows.o

TAS_NIC_OBJS := $(addprefix $(d)/, \
  $(objs_top) \
  $(addprefix slow/, $(objs_sp)) \
  $(addprefix fast/, $(objs_fp)))

exec := $(d)/tas_nic

TAS_NIC_CPPFLAGS := -Iinclude/ -I$(d)/include/ $(DPDK_CPPFLAGS) $(RDMA_CPPFLAGS) 
TAS_NIC_CFLAGS := $(DPDK_CFLAGS) $(RDMA_CFLAGS)

$(TAS_NIC_OBJS): CPPFLAGS += $(TAS_NIC_CPPFLAGS)
$(TAS_NIC_OBJS): CFLAGS += $(TAS_NIC_CFLAGS)

$(exec): LDFLAGS += $(DPDK_LDFLAGS) $(RDMA_LDFLAGS)
$(exec): LDLIBS += $(DPDK_LDLIBS) $(RDMA_LDLIBS)
$(exec): $(TAS_NIC_OBJS) $(LIB_UTILS_OBJS) $(HOST_NIC_OBJS)

DEPS += $(TAS_NIC_OBJS:.o=.d)
CLEAN += $(TAS_NIC_OBJS) $(exec)
TARGETS += $(exec)

include mk/subdir_post.mk
