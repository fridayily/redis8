Redis 源码的推荐顺序：

## 1. 核心数据结构
- [sds.h](/src/sds.h)/[sds.c](/src/sds.c) - 简单动态字符串实现
- [adlist.h](/src/adlist.h)/[adlist.c](/src/adlist.c) - 链表
- [dict.h](/src/dict.h)/[dict.c](/src/dict.c) - 哈希表（字典实现）
- [t_zet.c](/src/t_zset.c) 跳跃表
- [insert.h](/src/intset.h)/[insert.c](/src/intset.c) 整数集合
- [ziplist.h](/src/ziplist.h)/[ziplist.c](/src/ziplist.c) 压缩列表
- [quicklist.h](/src/quicklist.h)/[quicklist.c](/src/quicklist.c) 快速列表
- [listpack.h](/src/listpack.h)/[listpack.c](/src/listpack.c) 列表包
- [rax.h](/src/rax.h)/[rax.c](/src/rax.c) 基数树
- [object.c](/src/object.c) 对象系统
- [zmalloc.h](/src/zmalloc.h)/[zmalloc.c](/src/zmalloc.c) - 内存分配封装

## 2. 基础工具
- [util.h](/src/util.h)/[util.c](/src/util.c) - 工具函数
- [anet.h](/src/anet.h)/[anet.c](/src/anet.c) - 网络工具
- [ae.h](/src/ae.h)/[ae.c](/src/ae.c) - 事件循环实现（Redis 异步 I/O 的核心）

## 3. 主服务器组件
- [server.h](/src/server.h) - 主服务器头文件，包含关键定义
- [server.c](/src/server.c) - 主服务器实现
- [networking.c](/src/networking.c) - 客户端网络处理
- [db.c](/src/db.c) - 数据库操作（键值存储逻辑）

## 4. 命令系统
- `command.h`/`command.c` - 命令处理和注册
- 各个命令实现文件，如 [t_string.c](/src/t_string.c)、[t_list.c](/src/t_list.c)、[t_hash.c](/src/t_hash.c) 等

## 5. 持久化
- [rdb.h](/src/rdb.h)/[rdb.c](/src/rdb.c) - RDB 持久化
- `aof.h`/[aof.c](/src/aof.c) - AOF（追加只写文件）持久化

## 6. 高级功能
- [replication.c](/src/replication.c) - 主从复制
- [cluster.h](/src/cluster.h)/[cluster.c](/src/cluster.c) - Redis 集群实现
- `sentinel.h`/[sentinel.c](/src/sentinel.c) - Redis 哨兵
- [pubsub.c](/src/pubsub.c) - 发布/订阅消息

这个顺序遵循了 Redis 的架构，从基础数据结构到高级特性，让你在学习复杂组件之前先理解基础原理。