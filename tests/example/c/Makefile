# 测试程序名称
TEST_NAME = test_adlist test_sds


# Redis 源码路径（假设当前目录是 tests/example/c）
REDIS_SRC = ../../..

CMOCKA_AVAILABLE := $(shell pkg-config --exists cmocka && echo YES)


# 编译器和参数
CC = gcc
CFLAGS = -Wall -Wextra -g \
         -I$(REDIS_SRC)/src \
         -I$(REDIS_SRC)/deps/jemalloc/include \
         -DUSE_JEMALLOC

 # 如果CMocka可用，添加CMocka的包含路径
ifeq ($(CMOCKA_AVAILABLE),YES)
    CMOCKA_CFLAGS := $(shell pkg-config --cflags cmocka)
    CMOCKA_LIBS := $(shell pkg-config --libs cmocka)
    CFLAGS += $(CMOCKA_CFLAGS)
endif        

# 链接参数
LDFLAGS = -g
LDLIBS = $(REDIS_SRC)/src/adlist.o \
         $(REDIS_SRC)/src/zmalloc.o \
         $(REDIS_SRC)/src/sds.o \
         $(REDIS_SRC)/src/util.o \
         $(REDIS_SRC)/deps/jemalloc/lib/libjemalloc.a \
         -lm -lpthread

# 如果CMocka可用，添加CMocka库
ifeq ($(CMOCKA_AVAILABLE),YES)
    LDLIBS += $(CMOCKA_LIBS)
endif

.PHONY: all test clean
# 构建目标
all: $(TEST_NAME)

# 编译规则
test_adlist: test_adlist.c
	$(CC) $(CFLAGS) -DCMOCKA_TEST $< $(LDLIBS) -o $@

test_sds: test_sds.c
	$(CC) $(CFLAGS) $< $(LDLIBS) -o $@

# 清理
clean:
	rm -f $(TEST_NAME)