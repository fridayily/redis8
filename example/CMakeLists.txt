# CMakeLists.txt for Redis C examples
cmake_minimum_required(VERSION 3.15)

# 设置包含目录
set(REDIS_SRC ..)
include_directories(${REDIS_SRC}/src)
include_directories(${REDIS_SRC}/deps/hiredis)
include_directories(${REDIS_SRC}/deps/linenoise)
include_directories(${REDIS_SRC}/deps/lua/src)
include_directories(${REDIS_SRC}/deps/hdr_histogram)
include_directories(${REDIS_SRC}/deps/fpconv)
include_directories(${REDIS_SRC}/deps/fast_float)

# 定义所需的源文件
set(REDIS_SDS_SOURCES
        ${REDIS_SRC}/src/sds.c
        ${REDIS_SRC}/src/zmalloc.c
        ${REDIS_SRC}/src/util.c
        ${REDIS_SRC}/src/monotonic.c
        ${REDIS_SRC}/src/mt19937-64.c
        ${REDIS_SRC}/deps/fpconv/fpconv_dtoa.c
        ${REDIS_SRC}/deps/fast_float/fast_float_strtod.cpp
        ${REDIS_SRC}/src/sha256.c
)

set(REDIS_LIST_SOURCES
        ${REDIS_SRC}/src/adlist.c
        ${REDIS_SRC}/src/zmalloc.c
)

set(REDIS_DICT_SOURCES
        ${REDIS_SRC}/src/dict.c
        ${REDIS_SRC}/src/siphash.c
        ${REDIS_SRC}/src/zmalloc.c
        ${REDIS_SRC}/src/util.c
        ${REDIS_SRC}/src/monotonic.c
        ${REDIS_SRC}/src/mt19937-64.c
        ${REDIS_SRC}/src/sds.c
        ${REDIS_SRC}/deps/fpconv/fpconv_dtoa.c
        ${REDIS_SRC}/deps/fast_float/fast_float_strtod.cpp
        ${REDIS_SRC}/src/sha256.c
)

# 创建测试可执行文件，将源文件直接包含在内
add_executable(test_adlist test_adlist.c ${REDIS_LIST_SOURCES})
add_executable(test_sds test_sds.c ${REDIS_SDS_SOURCES})
add_executable(test_dict test_dict.c ${REDIS_DICT_SOURCES})

# 链接系统库
target_link_libraries(test_adlist m pthread)
target_link_libraries(test_sds m pthread)
target_link_libraries(test_dict m pthread)
