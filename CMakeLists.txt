cmake_minimum_required(VERSION 3.15)
project(redis
        VERSION 8.0
        DESCRIPTION "Redis is an in-memory database that persists on disk."
        LANGUAGES C CXX)

# 设置C标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 设置C++标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 检查编译器是否支持C11 _Atomic
include(CheckCSourceCompiles)
check_c_source_compiles("
#include <stdatomic.h>
int main() {
    _Atomic int i;
    return 0;
}" HAS_C11_ATOMIC)

if(NOT HAS_C11_ATOMIC)
    message(STATUS "C11 _Atomic not supported, disabling vector sets")
    set(SKIP_VEC_SETS yes)
else()
    set(SKIP_VEC_SETS no)
endif()

# 检查系统类型
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(REDIS_OS_LINUX 1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(REDIS_OS_DARWIN 1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(REDIS_OS_FREEBSD 1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "OpenBSD")
    set(REDIS_OS_OPENBSD 1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
    set(REDIS_OS_NETBSD 1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "SunOS")
    set(REDIS_OS_SOLARIS 1)
endif()

# 默认设置
set(REDIS_MALLOC "libc")
if(REDIS_OS_LINUX)
    set(REDIS_MALLOC "jemalloc")
endif()

# 编译选项
set(REDIS_CFLAGS "-pedantic -DREDIS_STATIC='' -Wall -W -Wno-missing-field-initializers -Werror=deprecated-declarations -Wstrict-prototypes")
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(REDIS_CFLAGS "${REDIS_CFLAGS} -Wno-c11-extensions")
elseif(REDIS_OS_FREEBSD)
    set(REDIS_CFLAGS "${REDIS_CFLAGS} -Wno-c11-extensions")
endif()

if(NOT SKIP_VEC_SETS)
    set(REDIS_CFLAGS "${REDIS_CFLAGS} -DINCLUDE_VEC_SETS=1")
endif()

# 优化选项
set(REDIS_OPTIMIZATION "-O3")
if(REDIS_OPTIMIZATION STREQUAL "-O3")
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        set(REDIS_OPTIMIZATION "${REDIS_OPTIMIZATION} -flto")
    else()
        set(REDIS_OPTIMIZATION "${REDIS_OPTIMIZATION} -flto=auto")
    endif()
endif()

if(NOT REDIS_OPTIMIZATION STREQUAL "-O0")
    set(REDIS_OPTIMIZATION "${REDIS_OPTIMIZATION} -fno-omit-frame-pointer")
endif()

# 调试选项
set(REDIS_DEBUG "-g -ggdb")

# 包含目录
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/deps/hiredis)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/deps/linenoise)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/deps/lua/src)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/deps/hdr_histogram)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/deps/fpconv)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/deps/fast_float)



# 添加子目录
add_subdirectory(deps)
add_subdirectory(src)
add_subdirectory(example)




# 安装规则
install(TARGETS redis-server redis-cli redis-benchmark
        RUNTIME DESTINATION bin)

install(FILES redis.conf
        DESTINATION etc
        RENAME redis.conf.default)
