include mk/subdir_pre.mk

LIB_SOCKETS_OBJS = $(addprefix $(d)/, \
  control.o transfer.o context.o manage_fd.o epoll.o poll.o libc.o)
LIB_SOCKETS_SOBJS := $(LIB_SOCKETS_OBJS:.o=.shared.o)

LIB_SINT_OBJS = $(addprefix $(d)/,interpose.o)
LIB_SINT_SOBJS := $(LIB_SINT_OBJS:.o=.shared.o)

allobjs := $(LIB_SOCKETS_OBJS) $(LIB_SOCKETS_SOBJS) $(LIB_SINT_OBJS) \
  $(LIB_SINT_SOBJS)

LIB_SOCKETS_CPPFLAGS := -I$(d)/include/ -Ilib/tas/include

$(LIB_SOCKETS_OBJS): CPPFLAGS += $(LIB_SOCKETS_CPPFLAGS)
$(LIB_SOCKETS_SOBJS): CPPFLAGS += $(LIB_SOCKETS_CPPFLAGS)
$(LIB_SINT_OBJS): CPPFLAGS += $(LIB_SOCKETS_CPPFLAGS)
$(LIB_SINT_SOBJS): CPPFLAGS += $(LIB_SOCKETS_CPPFLAGS)

sockets_lib = lib/libtas_sockets.so
interpose_lib = lib/libtas_interpose.so

$(sockets_lib): LDFLAGS += $(RDMA_LDFLAGS)
$(sockets_lib): LDLIBS += $(RDMA_LDLIBS)
$(sockets_lib): $(LIB_SOCKETS_SOBJS) $(LIB_TAS_SOBJS) \
  $(LIB_UTILS_SOBJS)

$(interpose_lib): LDFLAGS += $(RDMA_LDFLAGS)
$(interpose_lib): LDLIBS += $(RDMA_LDLIBS)
$(interpose_lib): $(LIB_SINT_SOBJS) $(LIB_SOCKETS_SOBJS) \
  $(LIB_TAS_SOBJS) $(LIB_UTILS_SOBJS) $(HOST_NIC_SOBJS)

DEPS += $(allobjs:.o=.d)
CLEAN += $(allobjs) lib/libtas_sockets.so lib/libtas_interpose.so
TARGETS += lib/libtas_sockets.so lib/libtas_interpose.so

include mk/subdir_post.mk
