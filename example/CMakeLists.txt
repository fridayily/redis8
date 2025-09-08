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

set(TEST_ZSET_SOURCES
        test_t_zset.c
        ${REDIS_SRC}/src/sds.c
        ${REDIS_SRC}/src/zmalloc.c
        ${REDIS_SRC}/src/util.c
        ${REDIS_SRC}/src/monotonic.c
        ${REDIS_SRC}/src/mt19937-64.c
        ${REDIS_SRC}/deps/fpconv/fpconv_dtoa.c
        ${REDIS_SRC}/deps/fast_float/fast_float_strtod.cpp
        ${REDIS_SRC}/src/sha256.c
        ${REDIS_SRC}/src/intset.c
)

set(TEST_LISTPACK_SOURCES
        ${REDIS_SRC}/src/listpack.c
        ${REDIS_SRC}/src/util.c
        ${REDIS_SRC}/src/zmalloc.c
        ${REDIS_SRC}/deps/fpconv/fpconv_dtoa.c
        ${REDIS_SRC}/src/sds.c
        ${REDIS_SRC}/src/sha256.c
        ${REDIS_SRC}/deps/fast_float/fast_float_strtod.cpp
)

set(TEST_QUICKLIST_SOURCES
        ${REDIS_SRC}/src/quicklist.c
        ${REDIS_SRC}/src/listpack.c
        ${REDIS_SRC}/src/zmalloc.c
        ${REDIS_SRC}/src/util.c
        ${REDIS_SRC}/src/lzf_c.c
        ${REDIS_SRC}/src/lzf_d.c
        ${REDIS_SRC}/src/sds.c
        ${REDIS_SRC}/src/sha256.c
        ${REDIS_SRC}/deps/fpconv/fpconv_dtoa.c
        ${REDIS_SRC}/deps/fast_float/fast_float_strtod.cpp

)

add_executable(test_quicklist test_quicklist.c ${TEST_QUICKLIST_SOURCES})


set(TEST_INTERNAL_SOURCES
        ${REDIS_SRC}/src/adlist.c
        ${REDIS_SRC}/src/listpack.c
        ${REDIS_SRC}/src/util.c
        ${REDIS_SRC}/src/sds.c
        ${REDIS_SRC}/src/zmalloc.c
        ${REDIS_SRC}/src/sha256.c
        ${REDIS_SRC}/src/monotonic.c
        ${REDIS_SRC}/src/siphash.c
        ${REDIS_SRC}/src/mt19937-64.c
        ${REDIS_SRC}/src/lzf_c.c
        ${REDIS_SRC}/src/lzf_d.c
        ${REDIS_SRC}/deps/fast_float/fast_float_strtod.cpp
        ${REDIS_SRC}/deps/fpconv/fpconv_dtoa.c
        ${REDIS_SRC}/src/redisassert.c
)

set(TEST_HIREDIS_SOURCES
        ${REDIS_SRC}/src/ae.c
        ${REDIS_SRC}/src/anet.c
        ${REDIS_SRC}/src/monotonic.c
        ${REDIS_SRC}/src/zmalloc.c
        ${REDIS_SRC}/src/redisassert.c
        ${REDIS_SRC}/src/strl.c


)


#add_definitions(-DQL_TEST_VERBOSE=1)
# 创建测试可执行文件，将源文件直接包含在内
add_executable(test_adlist test_adlist.c ${REDIS_LIST_SOURCES})
add_executable(test_sds test_sds.c ${REDIS_SDS_SOURCES})

add_executable(test_dict test_dict.c ${REDIS_DICT_SOURCES})
add_executable(test_listpack test_listpack.c ${TEST_LISTPACK_SOURCES})
add_executable(test_internal test_internal.c ${TEST_INTERNAL_SOURCES})
add_executable(test_t_zset test_t_zset.c)  # 添加 zset 测试目标
add_executable(hiredis_connection hiredis_connection.c)  # 添加 zset 测试目标

add_executable(hiredis_example ${REDIS_SRC}/deps/hiredis/examples/example.c)
add_executable(hiredis_example-ae ${REDIS_SRC}/deps/hiredis/examples/example-ae.c ${TEST_HIREDIS_SOURCES})

add_executable(getaddrinfo getaddrinfo.c)

add_executable(socket_server socket_server.c)
add_executable(socket_client socket_client.c)
add_executable(test_base test_base.c)
add_executable(select_server select_server.c)
add_executable(select_client select_client.c)
add_executable(poll_server poll_server.c)
add_executable(poll_client poll_client.c)
add_executable(kqueue_server kqueue_server.c)
add_executable(kqueue_client kqueue_client.c)
add_executable(kqueue_filesystem kqueue_filesystem.c)

# 链接系统库
target_link_libraries(test_adlist m pthread)
target_link_libraries(test_sds m pthread)
target_link_libraries(test_dict m pthread)
target_link_libraries(test_internal m pthread)
target_link_libraries(hiredis_connection hiredis m pthread)
target_link_libraries(hiredis_example hiredis)
target_link_libraries(hiredis_example-ae hiredis)
