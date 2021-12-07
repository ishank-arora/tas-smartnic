include mk/subdir_pre.mk

HOST_NIC_OBJS := $(addprefix $(d)/, \
	rdma_common.o host_nic.o)
HOST_NIC_SOBJS := $(HOST_NIC_OBJS:.o=.shared.o)
HOST_NIC_CPPFLAGS := -I$(d)/include/

$(HOST_NIC_OBJS): CFLAGS += $(HOST_NIC_CFLAGS)
$(HOST_NIC_OBJS): CPPFLAGS += $(HOST_NIC_CPPFLAGS)
$(HOST_NIC_SOBJS): CPPFLAGS += $(HOST_NIC_CPPFLAGS)

DEPS += $(HOST_NIC_OBJS:.o=.d) $(HOST_NIC_SOBJS:.o=.d)
CLEAN += $(HOST_NIC_OBJS) $(HOST_NIC_SOBJS)

include mk/subdir_post.mk