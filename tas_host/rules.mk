include mk/subdir_pre.mk

objs_top := tas_host.o config.o nicif.o shm.o packetmem.o

TAS_HOST_OBJS := $(addprefix $(d)/, \
	$(objs_top))

exec := $(d)/tas_host

TAS_HOST_CPPFLAGS := -Iinclude/ -I$(d)/include/ $(DPDK_CPPFLAGS) $(RDMA_CPPFLAGS)
TAS_HOST_CFLAGS := $(DPDK_CFLAGS) $(RDMA_CFLAGS)

$(TAS_HOST_OBJS): CPPFLAGS += $(TAS_HOST_CPPFLAGS)
$(TAS_HOST_OBJS): CFLAGS += $(TAS_HOST_CFLAGS)

$(exec): LDFLAGS += $(RDMA_LDFLAGS)
$(exec): LDLIBS += $(RDMA_LDLIBS)
$(exec): $(TAS_HOST_OBJS) $(LIB_UTILS_OBJS) $(HOST_NIC_OBJS)

DEPS += $(TAS_HOST_OBJS:.o=.d)
CLEAN += $(TAS_HOST_OBJS) $(exec)
TARGETS += $(exec)

include mk/subdir_post.mk