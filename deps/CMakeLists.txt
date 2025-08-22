# Dependencies CMakeLists.txt

# hiredis
add_subdirectory(hiredis)

# linenoise
add_library(linenoise STATIC linenoise/linenoise.c)
target_include_directories(linenoise PUBLIC linenoise)

# lua
file(GLOB LUA_SOURCES lua/src/*.c)
list(REMOVE_ITEM LUA_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/lua/src/lua.c)
list(REMOVE_ITEM LUA_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/lua/src/luac.c)
add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC lua/src)
target_compile_definitions(lua PRIVATE LUA_ANSI)

# hdr_histogram
file(GLOB HDR_HISTOGRAM_SOURCES hdr_histogram/*.c)
add_library(hdr_histogram STATIC ${HDR_HISTOGRAM_SOURCES})
target_include_directories(hdr_histogram PUBLIC hdr_histogram)
target_compile_definitions(hdr_histogram PRIVATE HDR_MALLOC_INCLUDE=\"hdr_redis_malloc.h\")

# fpconv
add_library(fpconv STATIC fpconv/fpconv_dtoa.c)
target_include_directories(fpconv PUBLIC fpconv)

# fast_float
add_library(fast_float STATIC fast_float/fast_float_strtod.cpp)
target_include_directories(fast_float PUBLIC fast_float)

# jemalloc (only on Linux)
if(REDIS_OS_LINUX AND REDIS_MALLOC STREQUAL "jemalloc")
    # Note: jemalloc requires special handling, for simplicity we're using system malloc
    # In a full implementation, we would build jemalloc from source
    message(STATUS "Using system malloc instead of jemalloc for CMake build")
endif()
