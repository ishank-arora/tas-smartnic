include mk/subdir_pre.mk

LIB_UTILS_SHARED := $(addprefix $(d)/, \
  rng.o timeout.o utils.o rdma.o rdma_queue.o)
LIB_UTILS_NONSHARED := $(addprefix $(d)/, ) 
LIB_UTILS_OBJS := $(LIB_UTILS_NONSHARED) $(LIB_UTILS_SHARED)
LIB_UTILS_SOBJS := $(LIB_UTILS_SHARED:.o=.shared.o)

DEPS += $(LIB_UTILS_OBJS:.o=.d) $(LIB_UTILS_SOBJS:.o=.d)
CLEAN += $(LIB_UTILS_OBJS) $(LIB_UTILS_SOBJS)

include mk/subdir_post.mk
