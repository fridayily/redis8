# 测试程序名称
TEST_NAME = test_adlist test_sds


# Redis 源码路径（假设当前目录是 tests/example/c）
REDIS_SRC = ../../..

# 编译器和参数
CC = gcc
CFLAGS = -Wall -Wextra -g \
         -I$(REDIS_SRC)/src \
         -I$(REDIS_SRC)/deps/jemalloc/include \
         -DUSE_JEMALLOC

# 链接参数
LDFLAGS = -g
LDLIBS = $(REDIS_SRC)/src/adlist.o \
         $(REDIS_SRC)/src/zmalloc.o \
         $(REDIS_SRC)/src/sds.o \
         $(REDIS_SRC)/src/util.o \
         $(REDIS_SRC)/deps/jemalloc/lib/libjemalloc.a \
         -lm -lpthread

# 构建目标
all: $(TEST_NAME)

# 编译规则
test_adlist: test_adlist.c
	$(CC) $(CFLAGS) $< $(LDLIBS) -o $@

test_sds: test_sds.c
	$(CC) $(CFLAGS) $< $(LDLIBS) -o $@

# 清理
clean:
	rm -f $(TEST_NAME)