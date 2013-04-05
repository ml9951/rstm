PLATFORM = aou
# COMPILER = GCC
CC = gcc
CXX = g++
CXXFLAGS += -Wall -Wextra # -Werror
CXXFLAGS += -ggdb
LDFLAGS  += -lrt -lpthread
CXXFLAGS += -DSTM_CC_GCC
CXXFLAGS += -DSTM_API_LIB
CXXFLAGS += -DSTM_WS_WORDLOG
# OS = LINUX
ASFLAGS  += -DSTM_OS_LINUX
CXXFLAGS += -DSTM_OS_LINUX
CXXFLAGS += -DSTM_TLS_GCC
# CPU = AMD64
ASFLAGS += -DSTM_CPU_X86
CXXFLAGS += -DSTM_CPU_X86
ASFLAGS += -m64 -DSTM_BITS_64
CFLAGS += -m64
LDFLAGS += -m64
CXXFLAGS += -m64 -DSTM_BITS_64
CXXFLAGS += -march=native -mtune=native -msse2 -mfpmath=sse -DSTM_USE_SSE
# OPTIMIZATION = O3
CXXFLAGS += -O3
# ADAPT = NONE
CXXFLAGS += -DSTM_PROFILETMTRIGGER_NONE
# CHECKPOINT = SJLJ
CXXFLAGS += -DSTM_CHECKPOINT_SJLJ
# DESCRIPTOR = TLS
CXXFLAGS += -DSTM_API_NOTLSPARAM
# PMU = OFF
CXXFLAGS += -DSTM_NO_PMU
# TOXIC = OFF
CXXFLAGS += -DSTM_COUNTCONSEC_NO
# INSTRUMENTATION = FINE GRAINED ADAPTIVITY
CXXFLAGS += -DSTM_INST_FINEGRAINADAPT

# AOU OPTION 1
CXXFLAGS += -DASF_STACK -fomit-frame-pointer
# AOU OPTION 2
# CXXFLAGS += -DASF_PUSH -fno-omit-frame-pointer

# More custom AOU stuff:
CXXFLAGS += -DSTM_HAS_AOU
USE_AOU = true
