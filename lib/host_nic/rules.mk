include mk/subdir_pre.mk

HOST_NIC_OBJS := $(addprefix $(d)/, \
	rdma_common.o host_nic.o)

$(HOST_NIC_OBJS): CFLAGS += $(HOST_NIC_CFLAGS)
$(HOST_NIC_OBJS): CPPFLAGS += $(HOST_NIC_CPPFLAGS)

DEPS += $(HOST_NIC_OBJS:.o=.d)
CLEAN += $(HOST_NIC_OBJS)


include mk/subdir_post.mk