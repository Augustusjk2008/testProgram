# DDS 详细设计

## 1. 设计目标与边界

`src/MB_DDF/DDS` 提供面向 AArch64 Linux 的本机进程间发布/订阅能力。
核心目标是：

- 通过 POSIX 共享内存在多个进程间共享 Topic 和消息。
- 支持一个 Topic 上的多发布者、多订阅者。
- 支持轮询、阻塞等待和异步回调三种消费方式。
- 支持普通拷贝发布和写槽式零拷贝发布。
- 对消息提供序列号、单调时钟时间戳和可选 CRC32。
- 通过 `ExternalEndpoint` 接入非共享内存链路。
- 通过 `DomainGateway` 在多个 DDS 域之间转发 Topic。

DDS Core 不直接依赖 `MB_DDF_HW`。硬件设备通过
`ExternalEndpoint`/Adapter 在应用层组合，避免核心模块绑定特定驱动。

本实现不是持久化消息队列，也不保证慢订阅者收到每一条历史消息。RingBuffer
回绕时会覆盖旧数据。

## 2. 模块组成

```text
应用 / Demo
    |
    v
DDSCore
    ├─ SharedMemoryManager
    ├─ TopicRegistry
    ├─ RingBuffer (每个本进程已访问 Topic 一个对象)
    ├─ Publisher
    ├─ Subscriber
    ├─ ExternalEndpoint / ExternalPort
    └─ Gateway
       ├─ DdsGatewayLocalBus
       ├─ DomainGateway
       └─ GatewayEnvelope
```

| 模块 | 主要职责 |
|---|---|
| `DDSCore` | 单例入口、生命周期、Topic 与实体创建 |
| `SharedMemoryManager` | 共享内存、映射、named semaphore、初始化锁 |
| `TopicRegistry` | Topic 名称校验、注册、发现和共享内存分配 |
| `RingBuffer` | 消息存储、序列号、订阅进度、写锁和通知 |
| `Publisher` | 普通发布、零拷贝发布、外部端点发送 |
| `Subscriber` | 轮询、阻塞读、回调线程、外部端点接收 |
| `Message` | 消息头、时间戳、CRC32 和有效性检查 |
| `ProcessSharedSync` | 进程共享 robust mutex 和 condition variable |
| `ExternalEndpoint` | 外部字节链路的最小收发接口 |
| `DomainGateway` | Topic 扫描、跨域封装、TTL、去重与回灌抑制 |

## 3. 共享内存对象

### 3.1 固定名称与版本

`DDSCore::initialize()` 固定使用：

```text
共享内存：/MB_DDF_V2_SHM
信号量：  /MB_DDF_V2_SHM_sem
版本：    0x00005001
默认大小：128 MiB
最小大小：1 MiB
```

第一个成功创建共享内存的进程决定其大小。后续进程必须使用相同大小，否则
`SharedMemoryManager` 初始化失败。

`shutdown()` 只执行 `munmap`、`close` 和 `sem_close`，不会执行
`shm_unlink` 或 `sem_unlink`。因此 Topic、序列号和旧消息可在所有进程退出后继续
保留，直到系统重启或显式删除 `/dev/shm` 中的对象。

### 3.2 创建与打开流程

```text
DDSCore::initialize
  -> SharedMemoryManager("/MB_DDF_V2_SHM", size)
     -> 对 /tmp/_MB_DDF_V2_SHM.init.lock 加 flock
     -> shm_open(O_CREAT | O_EXCL)
        ├─ 新对象：ftruncate + 映射后清零
        └─ 已存在：检查现有大小必须一致
     -> mmap(MAP_SHARED | MAP_POPULATE)
     -> 释放初始化文件锁
     -> sem_open("/MB_DDF_V2_SHM_sem")
  -> TopicRegistry
  -> 读取 /proc/self/comm 作为实体名称
```

初始化文件锁防止多个进程同时创建、扩容和清零同一共享内存。

named semaphore 主要保护：

- TopicRegistry 首次初始化和 Topic 注册。
- RingBuffer 首次初始化。
- 订阅者槽位注册/注销。
- 进程共享同步对象异常后的重建。

### 3.3 总体内存布局

```text
共享内存起始地址
┌─────────────────────────────────────────┐
│ TopicRegistryHeader                    │
├─────────────────────────────────────────┤
│ TopicMetadata[128]                     │
├─────────────────────────────────────────┤
│ Topic 1 RingBuffer                     │
├─────────────────────────────────────────┤
│ Topic 2 RingBuffer                     │
├─────────────────────────────────────────┤
│ ...                                     │
└─────────────────────────────────────────┘
```

每个 Topic 默认分配 1 MiB，按 64 字节对齐顺序放置。当前实现不回收 Topic
区域，也不支持运行时改变单个 Topic 的 RingBuffer 大小。

### 3.4 共享内存版本与共享库 ABI

共享内存版本和动态库 ABI 是两套独立的兼容门禁：

- `DDSCore::VERSION` 标识共享内存布局及字段语义。`SubscriberState` 启用
  `owner_pid` 后版本已经升级，旧共享内存必须在所有相关进程退出后清理，不能由新旧
  程序混用。
- `libMB_DDF_v2` 当前项目版本为 2.0.0、公共 ABI 主版本为 2，构建产物使用
  `SOVERSION 2`。ABI v1 客户端必须重新编译、重新链接，不能继续装载 ABI v2
  动态库。

动态库 ABI 升级不能替代共享内存版本检查；即使客户端已经重新链接，打开旧版本共享
内存时仍会因 `DDSCore::VERSION` 不匹配而初始化失败。

## 4. TopicRegistry 设计

### 4.1 注册表头

`TopicRegistryHeader` 包含：

- `magic_number`：`"LDDS"` 魔数。
- `version`：必须等于 `DDSCore::VERSION`。
- `next_topic_id`：原子递增 Topic ID，初始为 1。
- `topic_count`：当前 Topic 数量。

版本不一致时注册表被标记为无效，`DDSCore::initialize()` 返回失败，避免旧布局被
新程序错误解释。

### 4.2 Topic 元数据

`TopicMetadata` 保存：

```text
topic_id
topic_name[64]
ring_buffer_offset
ring_buffer_size
```

限制：

- 最多 128 个 Topic。
- 名称必须符合 `domain://address`。
- 域和地址都不能为空。
- 元数据只能保存 63 个字符和结尾 `\0`。

名称校验当前没有显式拒绝超过 63 个字符的输入，注册时会截断。因此业务层应把
Topic 名限制在 63 字符以内，避免名称截断和潜在冲突。

### 4.3 注册流程

```text
create_publisher/create_subscriber
  -> create_or_get_topic_buffer
     -> TopicRegistry::get_topic_metadata(name)
        ├─ 已存在：映射该 Topic 的 RingBuffer
        └─ 不存在：
           -> register_topic(name, 1 MiB)
           -> 顺序计算 ring_buffer_offset
           -> 检查共享内存剩余容量
           -> 写入元数据并递增 topic_count
```

同名 Topic 重复注册会返回已有元数据，不会再次分配空间。

## 5. RingBuffer 设计

### 5.1 单 Topic 布局

```text
RingBuffer 起始地址
┌─────────────────────────────────────────┐
│ RingHeader                              │
├─────────────────────────────────────────┤
│ SubscriberRegistry                     │
│   SubscriberState[64]                  │
├─────────────────────────────────────────┤
│ RingSyncState                          │
│   write_mutex                          │
│   notify_mutex                         │
│   notify_cond                          │
│   generation / waiter_count            │
├─────────────────────────────────────────┤
│ Message 数据区                         │
└─────────────────────────────────────────┘
```

`RingHeader` 中最重要的字段：

- `write_pos`：下一次写入位置。
- `current_sequence`：该 Topic 最新消息序列号。
- `timestamp`：最新消息时间戳。
- `publisher_id/name`：最近一次登记的发布者诊断信息。
- `capacity/data_offset`：数据区布局。

### 5.2 消息格式

每条消息是：

```text
MessageHeader + payload + 8 字节对齐填充
```

`MessageHeader` 包含：

| 字段 | 含义 |
|---|---|
| `magic` | 固定为 `0xDEADBEEF` |
| `topic_id` | 所属 Topic |
| `sequence` | Topic 内单调递增序列号 |
| `timestamp` | `steady_clock` 纳秒值 |
| `data_size` | payload 字节数 |
| `checksum` | payload CRC32，可关闭 |

时间戳只适合比较同一启动环境中的相对时序，不是 UTC 时间。

### 5.3 写入流程

普通发布：

```text
Publisher::publish
  -> RingBuffer::publish_message
     -> 获取 write_mutex
     -> reserve
     -> memcpy payload
     -> commit
        -> 填写消息头、时间戳和 CRC
        -> current_sequence + 1
        -> 更新 write_pos
        -> notify_subscribers
```

`write_mutex` 是 `PTHREAD_PROCESS_SHARED` 的 robust mutex。因此同一 Topic 的
多个进程和多个发布者会被串行化，提交顺序就是序列号顺序。

写入点到达尾部且剩余连续空间不足时，从数据区起点重新写入。RingBuffer
始终允许覆盖：

- `full()` 固定返回 `false`。
- `available_space()` 表达的是可覆盖容量，不是无损剩余容量。
- 慢订阅者可能发现目标序列已被覆盖。

### 5.4 零拷贝写槽

`Publisher::begin_message(max_size)`：

1. 获取 Topic 写锁。
2. 在 RingBuffer 中预留连续区域。
3. 把消息魔数清零，使未完成写槽对读者不可见。
4. 返回 `WritableMessage`。
5. 用户直接写入 `data()`。
6. `commit(used)` 填写消息头并发布序列号。

`WritableMessage` 在析构前未提交时会自动 `abort`。从 `begin_message()` 到
`commit()`/`cancel()` 的整个期间都持有跨进程写锁，所以用户填充逻辑必须尽量短。

`publish_fill()` 也在写锁内执行用户回调。回调抛异常、返回 0 或返回值大于容量时，
写槽被取消。

`publish_and_get_sequence()` 的 `before_visible` 回调同样在当前 Topic 的写锁内执行。
该回调只能进行同步、短时的本地记账；若在其中重入订阅者注册、Topic 创建或任意需要
RingBuffer 写锁的发布 API，入口会立即返回失败，不会再次等待同一写锁或全局注册
信号量。调用方必须处理该失败结果，不得把这些 API 用作 `before_visible` 内的正常
工作流。

### 5.5 订阅者状态

每个 Topic 最多 64 个订阅者。`SubscriberState` 存在共享内存中，保存：

- `read_pos`
- `last_read_sequence`
- `timestamp`
- `subscriber_id`
- `subscriber_name`
- `owner_pid`

每个订阅者独立推进自己的序列号和读取位置。订阅和注销在全局 named semaphore
保护下更新槽位。

`owner_pid` 放在 `SubscriberState` 原有的尾部 padding 中。AArch64 Linux 与 SylixOS
上的 `sizeof(SubscriberState)` 仍为 128 字节，原有字段的偏移也保持不变；但 padding
从“未定义内容”变为进程所有权元数据，属于共享内存字段语义变化，因此仍需升级
`DDSCore::VERSION`。

注册时以调用进程的 `getpid()` 写入 `owner_pid`。只有同一 owner 的相同实体 ID 才复用
原槽；名称不再作为活槽复用键，因此同进程、同 Topic 的同名 Subscriber 仍拥有独立
读取槽。注销按共享槽地址精确定位并再次校验 owner，避免不同进程碰巧使用相同实体 ID
时互相清槽。扫描已占用槽位时，仅当 `kill(owner_pid, 0)` 返回失败且
`errno == ESRCH`，才把该 owner 判定为已经退出并回收槽位；探测成功、`EPERM` 或其他
错误都按“仍存活或无法确认”处理，不得回收。PID 被其他进程复用时也只会保守地暂不
回收，不会覆盖活跃订阅者。

`timestamp` 仍表示最后成功读取消息的时间戳，不是心跳或租约。活跃但暂无消息的
订阅者可以长期保持 `timestamp == 0`，所以槽位回收不能依据时间戳、读取序列或空闲
时长。

### 5.6 读取策略

| 接口 | 是否等待 | 消费方式 |
|---|---|---|
| `poll(..., latest=true)` | 否 | 跳到最新消息 |
| `poll(..., latest=false)` | 否 | 尝试读取下一序列 |
| `read(...)` 兼容重载 | 否 | 等价于 `poll` |
| `read_blocking(..., latest)` | 是 | 先立即读，再等待通知后读取 |
| 回调订阅 | 后台线程等待 | 顺序读取，失败时回退到最新消息 |

`read_next()` 按 `last_read_sequence + 1` 搜索。若消息已被覆盖，严格的下一序列可能
找不到；回调线程会回退到 `read_latest()`，因此继续工作但可能跳过中间消息。

回调模式和手工读取不能同时使用。同一个 `Subscriber` 一旦配置 callback 或
observer callback，`poll/read` 会返回 0。

### 5.7 通知机制

每个 RingBuffer 有独立的：

- robust `write_mutex`
- robust `notify_mutex`
- process-shared `notify_cond`
- 通知代数 `generation`
- 等待者计数 `waiter_count`

提交消息后：

1. 增加 `notification_count` 和 `generation`。
2. 没有等待者时直接返回。
3. 有等待者时持有 `notify_mutex` 并 `pthread_cond_broadcast`。

阻塞读的微秒超时会向上取整为毫秒。条件变量优先使用
`CLOCK_MONOTONIC`，平台不支持时退回 `CLOCK_REALTIME`。

robust mutex 所有者异常退出时，后续进程会尝试 `pthread_mutex_consistent` 恢复。
同步状态魔数或 mutex 探测失败时，RingBuffer 会在 named semaphore 保护下重建
同步对象。

## 6. DDSCore 生命周期与对象所有权

### 6.1 初始化

`DDSCore` 是进程内单例。推荐显式调用：

```cpp
auto& dds = MB_DDF::DDS::DDSCore::instance();
if (!dds.initialize()) {
    return 1;
}
```

重复调用 `initialize()` 会直接返回成功。创建 Publisher/Subscriber 前如果尚未
初始化，`create_or_get_topic_buffer()` 也会尝试使用默认大小自动初始化。

### 6.2 进程内对象关系

```text
DDSCore
  owns SharedMemoryManager
  owns TopicRegistry
  owns unordered_map<TopicMetadata*, unique_ptr<RingBuffer>>

Publisher/Subscriber
  non-owning -> TopicMetadata
  non-owning -> RingBuffer
```

因此必须保证 Publisher/Subscriber 在 `DDSCore::shutdown()` 前停止使用。Demo
通过 `DDSShutdownGuard` 确保退出 `main` 时统一关闭。

### 6.3 关闭

`shutdown()` 顺序：

1. 清空本进程的 RingBuffer 包装对象。
2. 销毁 TopicRegistry。
3. 解除共享内存映射并关闭信号量。
4. 清空进程名。
5. 重置初始化状态。

`Subscriber` 析构时会自动 `unsubscribe()`：停止回调线程、通知唤醒、`join`，
再释放共享订阅者槽位。

`DDSCore` 的 `topic_buffers_` 当前没有进程内 mutex。应用应在初始化和首次创建
实体阶段串行调用 `create_publisher/create_subscriber`；不要让多个本地线程并发
修改该映射。

## 7. Publisher 与 Subscriber

### 7.1 Publisher

两种工作模式：

1. 共享内存模式：持有 Topic 元数据和 RingBuffer 指针。
2. 外部端点模式：持有 `ExternalEndpointRef`，`publish()` 直接调用 `send()`。

外部端点模式没有 TopicRegistry/RingBuffer，因此：

- `get_topic_id()` 返回 0。
- `begin_message()` 无效。
- `publish_and_get_sequence()` 返回 0。

### 7.2 Subscriber

共享内存同步模式只注册订阅槽，不创建线程。回调/观察者模式会创建
`worker_thread_`。

外部端点回调模式按端点 `mtu()` 分配接收缓冲区，以 10 ms 超时循环调用
`receive()`。外部链路没有 DDS 消息头，所以回调时间戳为 0。

`bind_to_cpu(cpu_id, priority, policy)` 只适用于已经创建的回调线程。默认策略为
`SCHED_FIFO`，默认优先级取该策略最大值；目标板用户必须有设置实时调度的权限。

## 8. ExternalEndpoint 与硬件适配

`ExternalEndpoint` 定义最小字节端点：

```cpp
send(data, size)
receive(data, capacity)
receive(data, capacity, timeout_us)
mtu()
control(command, argument, argument_size)
```

DDSCore 可直接基于外部端点创建 Publisher/Subscriber：

```cpp
auto writer = dds.create_writer("external://tx", endpoint);
auto reader = dds.create_reader("external://rx", endpoint);
```

`MB_DDF_HW_DDS_Adapter` 提供：

- `ExternalEndpointAdapter`：把 `IByteEndpoint` 适配成 DDS
  `ExternalEndpoint`。
- `ComExternalEndpoint`：上述 Adapter 的 COM 语义别名。
- `CallbackExternalEndpoint`：通过三个回调桥接任意硬件或链路。

Adapter 将硬件层 `Timeout` 映射到 DDS 超时接口；硬件超时返回 0，其他错误返回
`-1`。

## 9. 跨域网关

### 9.1 组成

`DomainGateway` 不直接绑定 `DDSCore`，而依赖 `GatewayLocalBus`。生产实现
`DdsGatewayLocalBus` 把接口映射到：

- `DDSCore::list_topics()`
- `DDSCore::create_observer(topic, callback, start_after_sequence)`
- `DDSCore::publish_and_get_sequence()`，并支持在序列号分配后、消息对 observer
  可见前执行同步回调。

这种设计允许使用 fake local bus 做网关单元测试。

### 9.2 本地消息外发

```text
周期扫描 Topic
  -> 首次扫描已有 Topic：以 current_sequence 作为 observer 边界
  -> 后续发现新 Topic：以 0 作为 observer 边界
  -> 为非 gateway:// Topic 创建 observer(start_after_sequence)
  -> 收到 LocalMessageView
  -> 检查回灌抑制窗口
  -> 构造 GatewayEnvelope
  -> 向所有启用 ExternalEndpoint 发送
```

网关启动时首先为当时已经存在的 Topic 保存 `current_sequence` 快照，并把它作为
`start_after_sequence`，从而跳过启动前已经存在的历史消息。启动完成后的周期扫描若
发现新建 Topic，则使用边界 0；这样 Topic 创建后、下一次扫描前已经发布的首批消息
仍会被 observer 补读，不会落入 100 ms 扫描间隙。边界表达的是“跳过不大于该序列的
消息”，不是断线存储转发保证；已被 RingBuffer 覆盖的消息仍无法恢复。

启动扫描、后台扫描和手工扫描由同一 mutex 串行化。首次订阅暂态失败时保留该 Topic
的启动快照，后续重试仍使用原边界；`stop()` 后再次 `start()` 会开启新的启动会话，
对停机期间新建但尚未监控的 Topic 重新取快照，不回放停机期历史。

observer 回调通过共享生命周期门控进入 `DomainGateway`。`stop()` 会先关闭门控并等待
在途回调退出，再停止网关线程；`start()` 可重新打开门控并复用已有 Topic 观察订阅，
不会重复订阅或在停止后继续外发。

`GatewayEnvelope` 固定头长 54 字节，包含：

- 魔数和协议版本。
- 源域、当前发送域。
- 源网关 ID 和消息 ID。
- TTL。
- Topic 名长度、payload 长度。
- payload CRC32。

### 9.3 远端消息接收

每个启用端点有独立接收线程。收到信封后：

1. 校验魔数、版本、长度和 CRC。
2. 使用 `(origin_domain_id, origin_gateway_id, message_id)` 去重。
3. `gateway://`（或配置的内部前缀）Topic 直接丢弃，不能通过远端信封绕过过滤。
4. 源域等于本地域时丢弃，防止环路回灌。
5. 发布到同名本地 Topic；序列号分配后、消息对 observer 可见前登记回灌抑制键。
6. TTL 大于 1 时递减并转发到除入口外的其他端点。

去重窗口和回灌抑制窗口最大各 4096 项，超过后按插入顺序淘汰。
抑制键被 observer 消费时会同时从查找集合和顺序窗口移除，避免已消费记录长期占用窗口存储。

## 10. 关键约束与风险

- 共享内存对象不会自动删除，升级版本、改变大小或测试异常退出后可能需要手工清理。
- Topic 和单 Topic RingBuffer 大小固定，不支持删除、扩容或压缩。
- Topic 名超过 63 字符会被截断，调用方必须主动限制。
- RingBuffer 是覆盖式实时缓冲区，不提供消息持久化和“至少一次”交付。
- 同 Topic 多发布者由写 mutex 串行化，不是完全无锁实现。
- 零拷贝写槽和 `publish_fill` 回调持有跨进程写锁，不得执行阻塞 I/O。
- callback/observer 和手工 read 不能混用。
- `GatewayLocalBus::publish_topic()` 的序列号回调必须同步、短时、不可抛异常，并在消息
  对 observer 可见前完成；回调内重入订阅者注册、Topic 创建或嵌套发布会立即失败。
- `GatewayLocalBus::subscribe_topic()` 具有显式起始序列边界，外部实现必须按
  `start_after_sequence` 跳过边界及更早消息。
- 原始结构体 payload 没有序列化和 schema 演进；跨程序必须保证 ABI、大小端和版本一致。
- 公共共享库 ABI 主版本为 2，ABI v1 客户端必须重新编译、重新链接。
- `DDSCore` 的进程内 Topic/RingBuffer 映射由 mutex 保护，同 Topic 的并发实体创建只会
  构造一个 RingBuffer 实例；`Gateway`、observer 和其他实体仍必须在
  `DDSCore::shutdown()` 前析构。
- 网关不分片，序列化后的信封超过端点 MTU 时直接发送失败。

## 11. 测试对应关系

| 设计区域 | 主要测试 |
|---|---|
| Message/CRC | `tests/unit/test_message.cpp` |
| RingBuffer/同步/覆盖 | `tests/unit/test_ringbuffer.cpp` |
| 共享内存 | `tests/unit/test_shared_memory.cpp` |
| Topic 注册 | `tests/unit/test_topic_registry.cpp` |
| Publisher/Subscriber | `tests/unit/test_publisher_subscriber.cpp` |
| DDSCore 生命周期 | `tests/unit/test_dds_core.cpp` |
| ExternalEndpoint | `tests/unit/test_external_endpoint.cpp` |
| GatewayEnvelope | `tests/unit/test_gateway_envelope.cpp` |
| DomainGateway | `tests/unit/test_domain_gateway.cpp` |
| 目标板 IPC/压力/性能 | `tests/hardware` |

修改共享内存布局、同步原语或消息格式时，至少同步：

1. 提升 `DDSCore::VERSION`。
2. 更新本设计文档。
3. 更新对应单元测试和目标板测试。
4. 验证旧共享内存版本不匹配路径。
