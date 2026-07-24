# DDS 使用说明

内部模块、共享内存布局、同步机制和网关设计见
[DDS 详细设计](../design/dds-detailed-design.md)。

## 1. 是什么

这是个基于 POSIX 共享内存的进程间消息总线，核心能力：

- Topic 注册和发现
- 同机多进程发布/订阅
- 同 Topic 多发布者
- 同步读取
- 异步回调订阅
- 零拷贝写入
- 可选 CRC32 校验

核心代码只在：

- `src/MB_DDF/DDS`
- `src/MB_DDF/Debug/Logger.h`

## 2. 使用前先知道的约束

### 2.1 运行环境

- 只支持 AArch64 目标环境
- Windows 主机只负责交叉编译
- 多进程共享同一块 POSIX 共享内存

### 2.2 共享内存约束

`DDSCore::initialize()` 默认使用：

- 共享内存名：`/MB_DDF_V2_SHM`
- 信号量名：`/MB_DDF_V2_SHM_sem`
- 默认大小：`128MB`

注意：

- 第一个初始化进程决定共享内存大小
- 后续进程必须用同样大小
- 若旧共享内存版本或大小不匹配，初始化会失败
- 升级版本或改共享内存大小前，先停掉旧进程并清理旧共享内存对象

### 2.3 Topic 约束

Topic 名必须是：

```text
domain://address
```

例如：

- `rt://nav/pose`
- `shm://camera/frame`
- `demo://hello`

注意：

- `://` 两边都不能为空
- Topic 名实际存储上限是 63 字符，建议自己控制在 63 以内
- 新 Topic 默认分配 `1MB` RingBuffer
- 全局最多 `128` 个 Topic
- 单 Topic 最多 `64` 个订阅者

## 3. 先怎么构建

```powershell
.\build.ps1 debug
.\build.ps1 lib_release
.\tests\test-dds-only.ps1 -BuildOnly
```

## 4. 最常用 API

### 4.1 头文件

常用只要这几个：

```cpp
#include "MB_DDF/DDS/DDSCore.h"
#include "MB_DDF/DDS/Publisher.h"
#include "MB_DDF/DDS/Subscriber.h"
```

### 4.2 DDSCore

`DDSCore` 是总入口：

```cpp
auto& dds = MB_DDF::DDS::DDSCore::instance();
```

常用接口：

- `initialize(size_t shm_size = 128 * 1024 * 1024)`
- `shutdown()`
- `create_publisher(topic, enable_checksum)`
- `create_writer(topic, enable_checksum)`，同上，别名
- `create_subscriber(topic, enable_checksum, callback)`
- `create_reader(topic, enable_checksum, callback)`，同上，别名
- `data_write(pub, data, size)`
- `data_read(sub, data, size)`

建议：

- 不要依赖“自动初始化”，显式先调 `initialize()`
- 同个进程结束前调 `shutdown()`

## 5. 最小发布/订阅示例

### 5.1 同进程最小示例

```cpp
#include "MB_DDF/DDS/DDSCore.h"
#include <cstring>
#include <iostream>

int main() {
    auto& dds = MB_DDF::DDS::DDSCore::instance();
    if (!dds.initialize(128 * 1024 * 1024)) {
        return 1;
    }

    auto pub = dds.create_publisher("demo://hello");
    auto sub = dds.create_subscriber("demo://hello");
    if (!pub || !sub) {
        dds.shutdown();
        return 2;
    }

    const char text[] = "hello dds";
    if (!pub->publish(text, sizeof(text))) {
        dds.shutdown();
        return 3;
    }

    char buffer[64] = {};
    size_t n = sub->read(buffer, sizeof(buffer));
    if (n > 0) {
        std::cout << buffer << std::endl;
    }

    dds.shutdown();
    return 0;
}
```

### 5.2 两进程场景怎么理解

进程 A：

- `initialize()`
- `create_subscriber("rt://nav/pose")`
- `read()` 或回调订阅

进程 B：

- `initialize()`
- `create_publisher("rt://nav/pose")`
- `publish()`

只要：

- Topic 名一样
- 共享内存大小一样
- 版本一致

它们就会连到同一条 Topic。

## 6. 发布者怎么用

### 6.1 普通发布

```cpp
auto pub = dds.create_publisher("rt://demo/raw");

uint32_t value = 42;
pub->publish(&value, sizeof(value));

struct Pose {
    double x;
    double y;
    double z;
};

Pose pose{1.0, 2.0, 3.0};
pub->write(&pose, sizeof(pose));  // write 是 publish 别名
```

说明：

- `publish()` 和 `write()` 等价
- 返回 `true` 表示提交成功
- 数据按原始字节写入，不做序列化
- 跨进程双方必须自己保证结构体布局一致

### 6.2 字符串/二进制建议

如果传字符串：

```cpp
const char msg[] = "hello";
pub->publish(msg, sizeof(msg));
```

若传裸二进制：

```cpp
std::vector<uint8_t> payload = ...;
pub->publish(payload.data(), payload.size());
```

不要依赖对方“猜大小”，长度要明确传。

## 7. 订阅者怎么用

`create_subscriber()` 返回的对象已经自动 `subscribe()` 了。

也就是说，用 `DDSCore` 创建时：

- 不用再手动调 `subscribe()`
- 直接 `read()` 或等回调

### 7.1 同步读取

```cpp
auto sub = dds.create_subscriber("rt://demo/raw");

char buffer[256] = {};
size_t n = sub->read(buffer, sizeof(buffer));   // 默认读最新消息
```

`read(void*, size_t, bool latest = true)` 规则：

- `latest = true`：直接跳到当前最新消息
- `latest = false`：按顺序读下一条
- 没消息返回 `0`
- 若这个订阅者是回调模式，`read()` 也会返回 `0`

### 7.2 按顺序消费

```cpp
char buffer[256] = {};
size_t n = sub->read(buffer, sizeof(buffer), false);
```

适合：

- 想按消息顺序拉取
- 不想只看最新状态

### 7.3 带超时读取

```cpp
char buffer[256] = {};
int32_t n = sub->read(buffer, sizeof(buffer), 500000); // 500ms
```

这里超时单位是微秒。

规则：

- `0`：超时或无消息
- `>0`：读到数据
- `timeout_us = 0`：无限等待

注意：

- 这个带超时重载读到后取的是“最新消息”
- 若你要严格逐条消费，自己用 `read(..., false)` 配合轮询

### 7.4 异步回调订阅

```cpp
auto sub = dds.create_subscriber(
    "rt://imu/data",
    true,
    [](const void* data, size_t size, uint64_t ts_ns) {
        (void)ts_ns;
        if (size == sizeof(float) * 3) {
            auto* v = static_cast<const float*>(data);
            // v[0], v[1], v[2]
        }
    }
);
```

规则：

- 传回调后，内部会起工作线程
- 工作线程收到消息后调用回调
- 回调模式下不要再自己 `read()`

很重要：

- 回调线程优先按序读取下一条消息
- 目标序列已被 RingBuffer 覆盖时会回退到最新消息
- 如果生产速度长期高于消费速度，中间消息仍可能被跳过

所以回调模式更适合：

- 状态量
- 最新值
- 高频传感器当前帧

不适合：

- 每条都必须处理的业务流水

### 7.5 回调线程绑核

```cpp
bool ok = sub->bind_to_cpu(0);
```

注意：

- 只有回调模式有工作线程
- 没有回调线程时，`bind_to_cpu()` 会失败
- 可选设置优先级和调度策略

## 8. 零拷贝发布怎么用

### 8.1 `begin_message()` / `commit()`

```cpp
auto pub = dds.create_publisher("rt://camera/meta");

auto msg = pub->begin_message(256);
if (!msg.valid()) {
    return;
}

struct Meta {
    uint32_t frame_id;
    uint64_t ts;
};

auto* meta = static_cast<Meta*>(msg.data());
meta->frame_id = 100;
meta->ts = 123456789;

if (!msg.commit(sizeof(Meta))) {
    return;
}
```

规则：

- `begin_message(max_size)` 先预留写槽
- `data()` 返回可直接写的内存
- `commit(used)` 提交实际写入字节数
- 若不 `commit()`，析构时会自动 `cancel()`

### 8.2 `publish_fill()`

```cpp
bool ok = pub->publish_fill(256, [](void* buffer, size_t capacity) -> size_t {
    if (capacity < sizeof(uint32_t) * 4) {
        return 0;
    }
    auto* p = static_cast<uint32_t*>(buffer);
    p[0] = 1;
    p[1] = 2;
    p[2] = 3;
    p[3] = 4;
    return sizeof(uint32_t) * 4;
});
```

规则：

- 回调返回 `0` 表示取消
- 回调返回值不能大于 `capacity`

### 8.3 零拷贝和多发布者

同 Topic 多发布者已支持。

实现上：

- 同 Topic 写入会串行化
- `begin_message()` 到 `commit()` 期间持有跨进程写锁
- 提交顺序就是序列号顺序

所以要点只有一个：

- 持锁区尽量短，拿到 `WritableMessage` 后尽快填完并 `commit()`

同理：

- `publish_fill()` 的回调也在锁内执行
- 不要在回调里做慢操作

## 9. 外部端点抽象

DDS 还提供一个轻量外部端点抽象，用于把已有收发对象接到 `Publisher` / `Subscriber`。

头文件：

```cpp
#include "MB_DDF/DDS/ExternalEndpoint.h"
#include "MB_DDF/DDS/ExternalPort.h"
```

外部端点只需要实现：

- `send(const uint8_t*, size_t)`
- `receive(uint8_t*, size_t)`
- `receive(uint8_t*, size_t, uint32_t timeout_us)`
- `mtu()`

示例：

```cpp
auto external = std::make_shared<MyExternalEndpoint>();

auto& dds = MB_DDF::DDS::DDSCore::instance();
auto writer = dds.create_publisher("external://tx", external);
auto reader = dds.create_subscriber("external://rx", external);

writer->write(data, size);
auto n = reader->read(buffer, sizeof(buffer), 500000);
```

说明：

- 外部端点模式不创建共享内存 Topic。
- `begin_message()` / `publish_fill()` 仍只用于 DDS 共享内存 RingBuffer。
- 回调模式下不要再手动 `read()`。

## 10. 校验和怎么理解

创建发布者/订阅者时可传 `enable_checksum`：

```cpp
auto pub = dds.create_publisher("rt://demo/checksum", true);
auto sub = dds.create_subscriber("rt://demo/checksum", true);
```

建议：

- 默认保持 `true`
- 同一 Topic 的读写双方保持一致

关闭后：

- 少一点校验开销
- 但不会做数据完整性校验

## 11. 典型使用模式

### 11.1 状态广播

适合：

- 姿态
- 导航状态
- 心跳
- UI 状态

建议：

- 1 个 Topic 只放 1 类结构
- 订阅端用回调或 `read(..., true)`

### 11.2 命令/事件流

适合：

- 控制命令
- 任务事件
- 业务流水

建议：

- 订阅端用 `read(..., false)` 顺序消费
- 不用回调 latest 模式

### 11.3 高频大数据

适合：

- 图像元数据
- 大块二进制
- 高频采样块

建议：

- 发布端用 `begin_message()` 或 `publish_fill()`
- 尽量避免重复拷贝

## 12. 常见坑

### 12.1 不显式初始化

虽然 `create_publisher()` / `create_subscriber()` 内部会尝试自动初始化，但不建议依赖。

建议固定写法：

```cpp
auto& dds = MB_DDF::DDS::DDSCore::instance();
if (!dds.initialize(128 * 1024 * 1024)) {
    return 1;
}
```

### 12.2 不同进程给了不同共享内存大小

后启动进程会失败。

做法：

- 约定一个固定大小
- 所有进程统一用这一个值

### 12.3 Topic 名过长

虽然代码没显式拒绝超长字符串，但元数据里只有 64 字节存储区。

建议：

- 自己限制在 63 字符内

### 12.4 回调模式还想自己 `read()`

这会读不到。

因为：

- 回调模式内部已有工作线程消费消息
- `Subscriber::read()` 对回调模式直接返回 `0`

### 12.5 想保证“每条都收到”

这份 DDS 不是带持久化的队列。

尤其以下情况会丢中间消息：

- 用 `read_latest()`
- 用回调模式
- RingBuffer 回绕覆盖旧消息
- 消费速度跟不上发布速度

若业务要求“每条都处理”，要：

- 用 `read(..., false)` 顺序消费
- 适当放大共享内存和 Topic RingBuffer
- 降低发布速率或拆 Topic

## 13. 一个推荐模板

```cpp
#include "MB_DDF/DDS/DDSCore.h"

int main() {
    auto& dds = MB_DDF::DDS::DDSCore::instance();
    if (!dds.initialize(128 * 1024 * 1024)) {
        return 1;
    }

    auto pub = dds.create_publisher("app://example/topic");
    auto sub = dds.create_subscriber("app://example/topic");
    if (!pub || !sub) {
        dds.shutdown();
        return 2;
    }

    struct Packet {
        uint32_t seq;
        float value;
    } tx{1, 3.14f};

    if (!pub->publish(&tx, sizeof(tx))) {
        dds.shutdown();
        return 3;
    }

    Packet rx{};
    if (sub->read(&rx, sizeof(rx), false) == sizeof(rx)) {
        // use rx
    }

    dds.shutdown();
    return 0;
}
```
