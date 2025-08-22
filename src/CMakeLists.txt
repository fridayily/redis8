# Redis CMakeLists.txt

# Redis server executable
add_executable(redis-server
        threads_mngr.c adlist.c quicklist.c ae.c anet.c dict.c ebuckets.c eventnotifier.c
        iothread.c mstr.c kvstore.c server.c sds.c zmalloc.c lzf_c.c lzf_d.c pqsort.c
        zipmap.c sha1.c ziplist.c release.c networking.c util.c object.c db.c replication.c
        rdb.c t_string.c t_list.c t_set.c t_zset.c t_hash.c config.c aof.c pubsub.c multi.c
        debug.c sort.c intset.c syncio.c cluster.c cluster_legacy.c crc16.c endianconv.c
        slowlog.c eval.c bio.c rio.c rand.c memtest.c syscheck.c crcspeed.c crccombine.c
        crc64.c bitops.c sentinel.c notify.c setproctitle.c blocked.c hyperloglog.c latency.c
        sparkline.c redis-check-rdb.c redis-check-aof.c geo.c lazyfree.c module.c evict.c
        expire.c geohash.c geohash_helper.c childinfo.c defrag.c siphash.c rax.c t_stream.c
        listpack.c localtime.c lolwut.c lolwut5.c lolwut6.c lolwut8.c acl.c tracking.c
        socket.c tls.c sha256.c timeout.c setcpuaffinity.c monotonic.c mt19937-64.c
        resp_parser.c call_reply.c script_lua.c script.c functions.c function_lua.c
        commands.c strl.c connection.c unix.c logreqres.c)

target_link_libraries(redis-server
        hiredis lua hdr_histogram fpconv fast_float linenoise
        ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT})

# Redis CLI executable
add_executable(redis-cli
        anet.c adlist.c dict.c redis-cli.c zmalloc.c release.c ae.c redisassert.c
        crcspeed.c crccombine.c crc64.c siphash.c crc16.c monotonic.c cli_common.c
        mt19937-64.c strl.c cli_commands.c)

target_link_libraries(redis-cli
        hiredis linenoise hdr_histogram
        ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT})

# Redis benchmark executable
add_executable(redis-benchmark
        ae.c anet.c redis-benchmark.c adlist.c dict.c zmalloc.c redisassert.c release.c
        crcspeed.c crccombine.c crc64.c siphash.c crc16.c monotonic.c cli_common.c
        mt19937-64.c strl.c)

target_link_libraries(redis-benchmark
        hiredis hdr_histogram
        ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT})

# Redis sentinel (symlink to redis-server)
add_custom_target(redis-sentinel ALL
        COMMAND ${CMAKE_COMMAND} -E create_symlink redis-server redis-sentinel
        DEPENDS redis-server)

# Redis check RDB (symlink to redis-server)
add_custom_target(redis-check-rdb ALL
        COMMAND ${CMAKE_COMMAND} -E create_symlink redis-server redis-check-rdb
        DEPENDS redis-server)

# Redis check AOF (symlink to redis-server)
add_custom_target(redis-check-aof ALL
        COMMAND ${CMAKE_COMMAND} -E create_symlink redis-server redis-check-aof
        DEPENDS redis-server)
