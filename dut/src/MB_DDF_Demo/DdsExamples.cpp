#include "MB_DDF_Demo/DdsExamples.h"

#include "MB_DDF/DDS/DDSCore.h"
#include "MB_DDF/DDS/Publisher.h"
#include "MB_DDF/DDS/Subscriber.h"
#include "MB_DDF/Debug/Logger.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unistd.h>

namespace MB_DDF::Demo {
namespace {

// 每个示例使用固定 Topic，便于用户在其他进程中按名称订阅和观察。
constexpr std::string_view kSynchronousTopic = "demo://dds/synchronous";
constexpr std::string_view kBlockingTopic = "demo://dds/blocking";
constexpr std::string_view kCallbackTopic = "demo://dds/callback";
constexpr std::string_view kZeroCopyTopic = "demo://dds/zero-copy";
constexpr std::string_view kObserverTopic = "demo://dds/observer";

/**
 * @brief 示例消息采用固定布局，便于直接通过 DDS 发送二进制结构体。
 *
 * 生产代码中如果不同程序由不同编译器或不同版本构建，建议进一步使用明确的
 * 序列化协议，避免结构体填充、大小端和字段演进造成兼容问题。
 */
struct DemoSample {
    uint64_t run_id{0};
    uint32_t sequence{0};
    double value{0.0};
    std::array<char, 32> label{};
};

// 零拷贝接口按字节写入对象，因此示例消息必须可以安全地按字节复制。
static_assert(std::is_trivially_copyable_v<DemoSample>);

/**
 * @brief 为本次进程运行生成消息标识。
 *
 * DDS 共享内存在进程退出后仍可能保留旧消息。回调和阻塞读取通过 run_id
 * 忽略历史消息，从而让示例可以在同一块板卡上重复执行。
 */
uint64_t make_run_id() {
    // 计数器保证同一纳秒内连续创建的多个示例仍具有不同标识。
    static std::atomic<uint64_t> counter{0};

    // steady_clock 适合生成进程内唯一值，不依赖系统时间是否被校时。
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();

    // 将进程号放入高位，降低多个演示进程同时运行时发生碰撞的概率。
    const uint64_t process_bits = static_cast<uint64_t>(::getpid()) << 32u;

    // 将进程号、时钟计数和本地计数混合成一个轻量的运行标识。
    return process_bits ^ static_cast<uint64_t>(ticks) ^
           counter.fetch_add(1, std::memory_order_relaxed);
}

/**
 * @brief 构造一条完全初始化的示例消息。
 */
DemoSample make_sample(uint64_t run_id,
                       uint32_t sequence,
                       double value,
                       std::string_view label) {
    // 使用值初始化确保包括字符数组尾部在内的全部字节都可预测。
    DemoSample sample{};

    // run_id 用于区分本次执行产生的消息和共享内存中的历史消息。
    sample.run_id = run_id;

    // sequence 是用户负载中的业务序号，与 DDS 内部序列号是两个概念。
    sample.sequence = sequence;

    // value 模拟温度、压力或控制量等常见浮点遥测值。
    sample.value = value;

    // 保留一个字节放置字符串结束符，确保日志输出不会越界。
    const size_t copy_size = std::min(label.size(), sample.label.size() - 1);

    // 只复制有效字符，剩余字节已经由值初始化清零。
    std::memcpy(sample.label.data(), label.data(), copy_size);

    // 返回按值构造的消息，编译器可以直接消除这次拷贝。
    return sample;
}

/**
 * @brief 判断接收消息是否与预期消息完全一致。
 */
bool samples_equal(const DemoSample& actual, const DemoSample& expected) {
    // 固定布局且全部字节均已初始化，所以可以直接比较完整对象表示。
    return std::memcmp(&actual, &expected, sizeof(DemoSample)) == 0;
}

/**
 * @brief 安全取得消息标签，避免异常数据缺少字符串结束符时日志越界。
 */
std::string_view sample_label(const DemoSample& sample) {
    const auto end = std::find(sample.label.begin(), sample.label.end(), '\0');
    return std::string_view(
        sample.label.data(),
        static_cast<size_t>(end - sample.label.begin()));
}

/**
 * @brief 统一输出 DDS 示例负载的全部业务字段。
 */
void log_sample(std::string_view action, const DemoSample& sample) {
    LOG_INFO << "[DEMO] " << action
             << "：字节数=" << sizeof(sample)
             << "，run_id=" << sample.run_id
             << "，业务序号 sequence=" << sample.sequence
             << "，数值 value=" << sample.value
             << "，标签 label=" << sample_label(sample);
}

/**
 * @brief 输出统一的失败日志并返回 Failed。
 */
DemoResult fail_example(std::string_view example, std::string_view reason) {
    // 统一前缀便于在板卡日志中快速定位失败的示例。
    LOG_ERROR << "[DEMO] " << example << "失败：" << reason;

    // 调用方将 Failed 纳入最终退出码。
    return DemoResult::Failed;
}

/**
 * @brief 在截止时间之前持续读取，直到收到本次运行期望的消息。
 */
bool read_matching_sample(const std::shared_ptr<DDS::Subscriber>& reader,
                          const DemoSample& expected,
                          std::chrono::milliseconds timeout) {
    // 使用绝对截止时间，避免循环忽略旧消息时不断重置总超时时间。
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    // 只要尚未到达截止时间，就继续等待下一次可读通知。
    while (std::chrono::steady_clock::now() < deadline) {
        // 计算本轮剩余时间，并转换为 DDS 阻塞接口使用的微秒。
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - std::chrono::steady_clock::now());

        // 防止极端时钟边界下把负数转换成无符号整数。
        if (remaining.count() <= 0) {
            break;
        }

        // 单次等待最多 100 ms，使循环可以及时检查截止时间并过滤历史消息。
        const auto wait_slice =
            std::min<int64_t>(remaining.count(), std::chrono::milliseconds(100).count() * 1000);

        // latest=true 适合实时状态类数据，可直接跳到当前最新消息。
        DemoSample actual{};
        const int32_t received = reader->read(
            &actual,
            sizeof(actual),
            DDS::ReadStrategy::Blocking,
            static_cast<uint32_t>(wait_slice),
            true);

        // 返回 0 表示本轮等待超时，此时继续检查总截止时间。
        if (received == 0) {
            continue;
        }

        // 长度不匹配说明同一 Topic 上出现了其他协议的数据，示例选择忽略并记录。
        if (received != static_cast<int32_t>(sizeof(actual))) {
            LOG_WARN << "[DEMO] 忽略长度异常的消息：实际字节数=" << received
                     << "，期望字节数=" << sizeof(actual);
            continue;
        }

        // run_id 不匹配时通常是上次运行留下的共享内存消息。
        if (actual.run_id != expected.run_id) {
            LOG_INFO << "[DEMO] 忽略共享内存中的历史消息：实际 run_id=" << actual.run_id
                     << "，本次 run_id=" << expected.run_id;
            continue;
        }

        // 找到本次运行的消息后，再校验完整负载内容。
        log_sample("阻塞读取收到本次运行的消息", actual);
        return samples_equal(actual, expected);
    }

    // 在截止时间内没有读到匹配消息，向调用方报告失败。
    return false;
}

} // namespace

DemoResult run_dds_synchronous_example(DDS::DDSCore& dds) {
    // 本示例演示最短的“创建实体、发布、轮询、校验、释放”路径。
    LOG_INFO << "[DEMO] DDS 同步发布/轮询示例开始：Topic=" << kSynchronousTopic
             << "，发布方式=Publisher::write，读取方式=DDSCore::data_poll"
             << "（非阻塞，latest=true）";

    // 每次运行生成不同标识，避免误把历史消息当成本次结果。
    const DemoSample expected = make_sample(make_run_id(), 1, 23.75, "synchronous");
    log_sample("准备发布同步消息", expected);

    // create_writer 是 create_publisher 的语义别名，二者返回相同类型。
    auto writer = dds.create_writer(std::string(kSynchronousTopic), true);
    if (!writer) {
        return fail_example("DDS 同步发布/轮询示例", "create_writer 返回空指针");
    }
    LOG_INFO << "[DEMO] 已为 Topic=" << kSynchronousTopic
             << " 创建 Writer（create_writer，create_if_missing=true）";

    // create_reader 会自动完成无回调订阅，因此返回后可以直接调用 poll/read。
    auto reader = dds.create_reader(std::string(kSynchronousTopic), true);
    if (!reader) {
        return fail_example("DDS 同步发布/轮询示例", "create_reader 返回空指针");
    }
    LOG_INFO << "[DEMO] 已为同一 Topic 创建 Reader 并完成订阅，准备先发布再立即轮询";

    // write 接受任意连续内存及其字节数，成功时返回 true。
    LOG_INFO << "[DEMO] 调用 Publisher::write 发布 " << sizeof(expected)
             << " 字节连续内存";
    if (!writer->write(&expected, sizeof(expected))) {
        reader->unsubscribe();
        return fail_example("DDS 同步发布/轮询示例", "Publisher::write 返回 false");
    }

    // data_poll 是 DDSCore 提供的非阻塞便捷接口，当前无数据时立即返回 0。
    LOG_INFO << "[DEMO] 发布成功，调用 DDSCore::data_poll 非阻塞读取最新消息："
             << "缓冲区=" << sizeof(DemoSample) << " 字节，latest=true";
    DemoSample actual{};
    const size_t received = dds.data_poll(reader, &actual, sizeof(actual), true);

    // 教学示例显式取消订阅；实际代码也可以依赖 shared_ptr 析构自动清理。
    reader->unsubscribe();

    // 读取字节数必须与发送结构体大小完全一致。
    if (received != sizeof(actual)) {
        LOG_ERROR << "[DEMO] 同步轮询长度不一致：实际字节数=" << received
                  << "，期望字节数=" << sizeof(actual);
        return fail_example("DDS 同步发布/轮询示例", "接收字节数不一致");
    }

    log_sample("非阻塞轮询收到消息", actual);

    // 完整比较负载，确认发布和读取路径没有改变任何字段。
    if (!samples_equal(actual, expected)) {
        return fail_example("DDS 同步发布/轮询示例", "接收负载与发布负载不一致");
    }

    // 输出关键值，让用户在板卡控制台上能够直观看到结果。
    LOG_INFO << "[DEMO] 同步发布/轮询校验通过：Topic=" << kSynchronousTopic
             << "，业务序号=" << actual.sequence << "，数值=" << actual.value
             << "，标签=" << sample_label(actual);

    // 所有调用和校验均成功。
    return DemoResult::Passed;
}

DemoResult run_dds_blocking_example(DDS::DDSCore& dds) {
    // 本示例演示消费者先等待、生产者稍后发布的典型线程模型。
    LOG_INFO << "[DEMO] DDS 阻塞读取示例开始：Topic=" << kBlockingTopic
             << "，读取策略=Blocking，单次等待片段最大 100 ms，总超时 500 ms，latest=true";

    // 构造只属于本次运行的期望消息。
    const DemoSample expected = make_sample(make_run_id(), 2, 41.5, "blocking");
    log_sample("生产线程稍后将发布的目标消息", expected);

    // 先创建 reader，使消费者在生产者发布之前已经完成订阅。
    auto reader = dds.create_reader(std::string(kBlockingTopic), true);
    if (!reader) {
        return fail_example("DDS 阻塞读取示例", "create_reader 返回空指针");
    }
    LOG_INFO << "[DEMO] Reader 已先完成订阅，确保不会错过随后发布的数据";

    // 再创建 writer；同一 Topic 可以存在多个发布者和订阅者。
    auto writer = dds.create_writer(std::string(kBlockingTopic), true);
    if (!writer) {
        reader->unsubscribe();
        return fail_example("DDS 阻塞读取示例", "create_writer 返回空指针");
    }
    LOG_INFO << "[DEMO] Writer 创建完成；生产线程将在 50 ms 后调用 Publisher::publish，"
             << "主线程立即进入阻塞读取";

    // 原子变量把生产线程中的发布结果安全地传回主线程。
    std::atomic<bool> publish_ok{false};

    // 生产线程模拟采集或控制任务稍后才准备好数据。
    std::thread producer([writer, expected, &publish_ok]() {
        // 短暂延时确保主线程已经进入阻塞读取路径。
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // release 存储保证主线程读取 publish_ok 时能看到发布动作之前的结果。
        publish_ok.store(writer->publish(&expected, sizeof(expected)), std::memory_order_release);
    });

    // 主线程最多等待 500 ms，并自动忽略共享内存中不属于本次运行的消息。
    LOG_INFO << "[DEMO] 主线程开始等待 run_id=" << expected.run_id
             << " 的目标消息：总超时=500 ms，历史消息按 run_id 过滤";
    const bool received_expected =
        read_matching_sample(reader, expected, std::chrono::milliseconds(500));

    // join 保证生产线程结束后再销毁它捕获的对象。
    producer.join();

    // 不再需要 reader 后立即注销，释放共享订阅者槽位。
    reader->unsubscribe();

    // acquire 读取与生产线程的 release 存储配对。
    if (!publish_ok.load(std::memory_order_acquire)) {
        return fail_example("DDS 阻塞读取示例", "生产线程调用 Publisher::publish 失败");
    }
    LOG_INFO << "[DEMO] 生产线程发布成功：延时=50 ms，发布字节数=" << sizeof(expected);

    // 未在期限内收到期望消息属于测试失败。
    if (!received_expected) {
        return fail_example("DDS 阻塞读取示例", "500 ms 内未读到与目标负载完全一致的消息");
    }

    // 阻塞读写流程通过。
    LOG_INFO << "[DEMO] 阻塞读取已被生产线程发布动作唤醒并完成负载校验：Topic="
             << kBlockingTopic << "，sequence=" << expected.sequence
             << "，value=" << expected.value;
    return DemoResult::Passed;
}

DemoResult run_dds_callback_example(DDS::DDSCore& dds) {
    // 本示例演示 Subscriber 自带工作线程的异步回调模式。
    LOG_INFO << "[DEMO] DDS 异步回调订阅示例开始：Topic=" << kCallbackTopic
             << "，发布方式=Publisher::publish，回调等待超时=500 ms";

    // 回调只接受 run_id 匹配的消息，历史消息会被安静地忽略。
    const DemoSample expected = make_sample(make_run_id(), 3, 88.0, "callback");
    log_sample("回调函数要接收的目标消息", expected);

    // mutex 保护回调线程和主线程共享的状态。
    std::mutex mutex;

    // condition_variable 让主线程高效等待回调，不需要固定 sleep。
    std::condition_variable condition;

    // received 标记回调是否已经收到并复制了期望消息。
    bool received = false;

    // actual 保存回调期间复制出的负载，回调返回后不再依赖 DDS 内部指针。
    DemoSample actual{};

    // timestamp 保存 DDS 消息头产生的单调时钟纳秒时间戳。
    uint64_t timestamp = 0;

    // 把 callback 直接传给 create_reader 后，Subscriber 会自动启动工作线程。
    auto reader = dds.create_reader(
        std::string(kCallbackTopic),
        true,
        [&](const void* data, size_t size, uint64_t message_timestamp) {
            // 回调首先验证指针和长度，再按字节复制固定布局负载。
            if (data == nullptr || size != sizeof(DemoSample)) {
                return;
            }

            // candidate 是回调栈上的副本，不会在 DDS 复用环形缓冲区后失效。
            DemoSample candidate{};
            std::memcpy(&candidate, data, sizeof(candidate));

            // 只处理本次运行的消息，避免共享内存历史数据影响示例。
            if (candidate.run_id != expected.run_id) {
                return;
            }

            log_sample("Subscriber 工作线程回调收到目标消息", candidate);
            LOG_INFO << "[DEMO] 本次回调携带的 DDS 消息头时间戳="
                     << message_timestamp << " ns";

            // 加锁后一次性更新主线程需要观察的全部状态。
            {
                std::lock_guard<std::mutex> lock(mutex);
                actual = candidate;
                timestamp = message_timestamp;
                received = true;
            }

            // 状态更新完成后唤醒等待线程。
            condition.notify_one();
        });

    // reader 创建失败时不会启动回调线程。
    if (!reader) {
        return fail_example("DDS 异步回调订阅示例", "create_reader 返回空指针，回调线程未启动");
    }
    LOG_INFO << "[DEMO] 已为 Topic=" << kCallbackTopic
             << " 创建带回调的 Reader；目标是仅处理 run_id=" << expected.run_id
             << " 的本次消息";

    // 创建同 Topic 的 writer，用于触发刚才注册的回调。
    auto writer = dds.create_writer(std::string(kCallbackTopic), true);
    if (!writer) {
        reader->unsubscribe();
        return fail_example("DDS 异步回调订阅示例", "create_writer 返回空指针");
    }

    // 发布失败时先停止回调线程，再返回错误。
    LOG_INFO << "[DEMO] 调用 Publisher::publish 发布 " << sizeof(expected)
             << " 字节，发布完成后等待 Subscriber 工作线程执行回调";
    if (!writer->publish(&expected, sizeof(expected))) {
        reader->unsubscribe();
        return fail_example("DDS 异步回调订阅示例", "Publisher::publish 返回 false");
    }

    // wait_for 使用谓词抵御虚假唤醒，并限制板卡测试的最长等待时间。
    LOG_INFO << "[DEMO] 发布成功，主线程通过 condition_variable 最多等待回调 500 ms";
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait_for(lock, std::chrono::milliseconds(500), [&received]() {
            return received;
        });
    }

    // unsubscribe 会停止工作线程、唤醒潜在等待并完成 join。
    reader->unsubscribe();

    // 超时未收到回调时返回失败。
    if (!received) {
        return fail_example("DDS 异步回调订阅示例", "等待回调超过 500 ms");
    }

    // 对回调复制出的负载进行完整校验。
    if (!samples_equal(actual, expected)) {
        return fail_example("DDS 异步回调订阅示例", "回调收到的负载与发布负载不一致");
    }

    // 本地 DDS 消息应带有非零时间戳。
    if (timestamp == 0) {
        return fail_example("DDS 异步回调订阅示例", "回调中的 DDS 消息头时间戳为 0");
    }

    // 输出时间戳，说明该参数来自 DDS 消息头而不是用户结构体。
    LOG_INFO << "[DEMO] 异步回调校验通过：Topic=" << kCallbackTopic
             << "，sequence=" << actual.sequence << "，value=" << actual.value
             << "，label=" << sample_label(actual)
             << "，DDS 消息头时间戳=" << timestamp << " ns";
    return DemoResult::Passed;
}

DemoResult run_dds_zero_copy_example(DDS::DDSCore& dds) {
    // 本示例演示两种在 DDS 环形缓冲区内直接填充负载的方式。
    LOG_INFO << "[DEMO] DDS 零拷贝发布示例开始：Topic=" << kZeroCopyTopic
             << "，依次演示 begin_message/commit 和 publish_fill，"
             << "每条消息接收超时=500 ms";

    // 两条消息共用 run_id，并使用不同业务序号区分先后。
    const uint64_t run_id = make_run_id();
    const DemoSample first = make_sample(run_id, 4, 12.25, "begin-message");
    const DemoSample second = make_sample(run_id, 5, 13.5, "publish-fill");
    log_sample("第一条零拷贝消息（begin_message/commit）", first);
    log_sample("第二条零拷贝消息（publish_fill）", second);

    // reader 必须先注册，才能以订阅者身份消费随后发布的消息。
    auto reader = dds.create_reader(std::string(kZeroCopyTopic), true);
    if (!reader) {
        return fail_example("DDS 零拷贝发布示例", "create_reader 返回空指针");
    }
    LOG_INFO << "[DEMO] Reader 已先订阅 Topic=" << kZeroCopyTopic
             << "，随后创建 Writer，避免错过零拷贝消息";

    // writer 提供 begin_message 和 publish_fill 两组零拷贝 API。
    auto writer = dds.create_writer(std::string(kZeroCopyTopic), true);
    if (!writer) {
        reader->unsubscribe();
        return fail_example("DDS 零拷贝发布示例", "create_writer 返回空指针");
    }

    // begin_message 在环形缓冲区中预留一块至少为指定大小的可写区域。
    LOG_INFO << "[DEMO] 第一步：调用 begin_message 在 DDS 环形缓冲区直接预留 "
             << sizeof(first) << " 字节可写空间";
    auto writable = writer->begin_message(sizeof(first));

    // valid=false 表示当前无法安全预留写槽。
    if (!writable.valid()) {
        reader->unsubscribe();
        return fail_example("DDS 零拷贝发布示例", "begin_message 返回无效写槽");
    }

    LOG_INFO << "[DEMO] begin_message 预留成功：请求容量=" << sizeof(first)
             << " 字节，实际可用容量=" << writable.capacity() << " 字节";

    // capacity 可能大于请求大小，但不能小于要写入的负载。
    if (writable.capacity() < sizeof(first)) {
        reader->unsubscribe();
        return fail_example("DDS 零拷贝发布示例", "预留写槽容量小于消息字节数");
    }

    // data 指向 DDS 已经预留的最终负载位置，因此这里不需要中间发送缓冲区。
    std::memcpy(writable.data(), &first, sizeof(first));
    LOG_INFO << "[DEMO] 已将第一条负载直接写入 DDS 最终存储位置，"
             << "准备 commit 实际字节数=" << sizeof(first);

    // commit 发布实际使用的字节数；未 commit 的 WritableMessage 会在析构时自动取消。
    if (!writable.commit(sizeof(first))) {
        reader->unsubscribe();
        return fail_example("DDS 零拷贝发布示例", "WritableMessage::commit 返回 false");
    }

    // 立即读取并校验第一种零拷贝接口产生的消息。
    LOG_INFO << "[DEMO] 第一条消息 commit 成功，使用 Blocking/latest=true 读取，"
             << "超时=500 ms";
    if (!read_matching_sample(reader, first, std::chrono::milliseconds(500))) {
        reader->unsubscribe();
        return fail_example("DDS 零拷贝发布示例", "500 ms 内未收到 begin_message/commit 发布的负载");
    }
    LOG_INFO << "[DEMO] 第一种零拷贝方式校验通过：begin_message + memcpy + commit";

    // publish_fill 把预留、回调填充和提交包装为一次调用。
    LOG_INFO << "[DEMO] 第二步：调用 publish_fill 一次完成预留、回调填充和提交，"
             << "请求负载字节数=" << sizeof(second);
    const bool filled = writer->publish_fill(
        sizeof(second),
        [&second](void* buffer, size_t capacity) -> size_t {
            // 回调仍需检查容量，不能假设底层一定接受任意大小。
            if (buffer == nullptr || capacity < sizeof(second)) {
                return 0;
            }

            // 直接写入 DDS 提供的最终负载区域。
            std::memcpy(buffer, &second, sizeof(second));

            // 返回实际写入字节数；返回 0 会取消本次发布。
            return sizeof(second);
        });

    // publish_fill 返回 false 表示填充回调取消或提交失败。
    if (!filled) {
        reader->unsubscribe();
        return fail_example("DDS 零拷贝发布示例", "publish_fill 返回 false，填充回调取消或提交失败");
    }

    // 读取并校验第二种零拷贝接口产生的消息。
    LOG_INFO << "[DEMO] 第二条消息 publish_fill 成功，使用 Blocking/latest=true 读取，"
             << "超时=500 ms";
    const bool received_second =
        read_matching_sample(reader, second, std::chrono::milliseconds(500));

    // 两条消息都处理完毕后释放订阅者资源。
    reader->unsubscribe();

    // 第二条消息未收到时报告失败。
    if (!received_second) {
        return fail_example("DDS 零拷贝发布示例", "500 ms 内未收到 publish_fill 发布的负载");
    }

    // 两种零拷贝路径均已通过。
    LOG_INFO << "[DEMO] 两种零拷贝发布方式均校验通过：Topic=" << kZeroCopyTopic
             << "，共同 run_id=" << run_id << "，业务序号依次为 "
             << first.sequence << "、" << second.sequence;
    return DemoResult::Passed;
}

DemoResult run_dds_observer_example(DDS::DDSCore& dds) {
    // 本示例面向网关或诊断程序，展示如何取得 DDS 内部本地序列号。
    LOG_INFO << "[DEMO] DDS Observer/本地序列号示例开始：Topic=" << kObserverTopic
             << "，发布方式=DDSCore::publish_and_get_sequence，回调等待超时=500 ms";

    // 观察者只认本次运行的负载。
    const DemoSample expected = make_sample(make_run_id(), 6, 64.0, "observer");
    log_sample("Observer 要观察的目标消息", expected);

    // 观察者回调和主线程通过互斥量与条件变量同步。
    std::mutex mutex;
    std::condition_variable condition;
    bool observed = false;
    uint64_t observed_sequence = 0;

    // create_observer 的回调参数包含 Topic、DDS 本地序列号、时间戳和负载视图。
    auto observer = dds.create_observer(
        std::string(kObserverTopic),
        [&](const DDS::LocalMessageView& view) {
            // 负载指针只在回调期间有效，因此必须立即复制。
            if (view.data == nullptr || view.size != sizeof(DemoSample)) {
                return;
            }

            // 将负载复制到局部对象后再检查 run_id。
            DemoSample candidate{};
            std::memcpy(&candidate, view.data, sizeof(candidate));
            if (candidate.run_id != expected.run_id) {
                return;
            }

            log_sample("Observer 回调收到目标负载", candidate);
            LOG_INFO << "[DEMO] Observer 回调取得 DDS 本地序列号=" << view.sequence
                     << "；该序列号用于排序、去重或网关回灌检测";

            // 记录 DDS 环形缓冲区分配的序列号。
            {
                std::lock_guard<std::mutex> lock(mutex);
                observed_sequence = view.sequence;
                observed = true;
            }

            // 唤醒等待发布结果的主线程。
            condition.notify_one();
        });

    // 观察者创建失败通常表示 Topic 或订阅者注册失败。
    if (!observer) {
        return fail_example("DDS Observer/本地序列号示例", "create_observer 返回空指针");
    }
    LOG_INFO << "[DEMO] Observer 已注册：目标 Topic=" << kObserverTopic
             << "，目标 run_id=" << expected.run_id
             << "，回调将同时检查负载并读取 DDS 本地序列号";

    // 该便捷接口发布消息并返回同一条消息的本地 DDS 序列号。
    LOG_INFO << "[DEMO] 调用 publish_and_get_sequence 发布 " << sizeof(expected)
             << " 字节，并请求返回本次发布分配的 DDS 本地序列号";
    const uint64_t published_sequence =
        dds.publish_and_get_sequence(std::string(kObserverTopic), &expected, sizeof(expected));

    // 序列号 0 被接口保留为发布失败标志。
    if (published_sequence == 0) {
        observer->unsubscribe();
        return fail_example("DDS Observer/本地序列号示例", "publish_and_get_sequence 返回 0，表示发布失败");
    }
    LOG_INFO << "[DEMO] 发布成功：接口返回 DDS 本地序列号=" << published_sequence
             << "，开始等待 Observer 回调，超时=500 ms";

    // 等待观察者线程看到刚才发布的消息。
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait_for(lock, std::chrono::milliseconds(500), [&observed]() {
            return observed;
        });
    }

    // 停止观察线程并释放共享订阅者槽位。
    observer->unsubscribe();

    // 未收到观察回调属于失败。
    if (!observed) {
        return fail_example("DDS Observer/本地序列号示例", "等待 Observer 回调超过 500 ms");
    }

    // 发布接口和观察回调应报告同一个本地序列号。
    if (observed_sequence != published_sequence) {
        LOG_ERROR << "[DEMO] DDS 本地序列号不一致：发布接口返回=" << published_sequence
                  << "，Observer 回调收到=" << observed_sequence;
        return fail_example("DDS Observer/本地序列号示例", "发布接口与 Observer 回调报告的序列号不同");
    }

    // 输出序列号，便于用户理解它可用于去重、排序或网关回灌检测。
    LOG_INFO << "[DEMO] Observer 校验通过：Topic=" << kObserverTopic
             << "，业务 sequence=" << expected.sequence
             << "，value=" << expected.value
             << "，发布端和观察端的 DDS 本地序列号均为 " << observed_sequence;
    return DemoResult::Passed;
}

DemoResult run_dds_topic_discovery_example(DDS::DDSCore& dds) {
    // 本示例应在其他 DDS 示例之后执行，此时它们的 Topic 已经注册。
    LOG_INFO << "[DEMO] DDS Topic 发现示例开始：调用 DDSCore::list_topics，"
             << "检查前五个示例注册的 Topic 及其环形缓冲区信息";

    // list_topics 返回当前共享内存域中的全部本地 Topic 元信息。
    const auto topics = dds.list_topics();

    // 空列表通常表示 DDS 尚未初始化或 TopicRegistry 不可用。
    if (topics.empty()) {
        return fail_example("DDS Topic 发现示例", "list_topics 返回空列表，DDS 可能未初始化或 TopicRegistry 不可用");
    }

    LOG_INFO << "[DEMO] list_topics 返回 " << topics.size()
             << " 个本地 Topic，下面逐项输出 Topic ID、名称和环形缓冲区容量";

    // 输出所有 Topic，板卡上可用该日志核对其他进程是否已完成注册。
    for (const auto& topic : topics) {
        LOG_INFO << "[DEMO] Topic 信息：ID=" << topic.topic_id
                 << "，名称=" << topic.topic_name
                 << "，环形缓冲区容量=" << topic.ring_buffer_size << " 字节";
    }

    // 检查本文件前面五个示例使用的 Topic 是否全部可见。
    constexpr std::array<std::string_view, 5> expected_topics{
        kSynchronousTopic,
        kBlockingTopic,
        kCallbackTopic,
        kZeroCopyTopic,
        kObserverTopic,
    };

    LOG_INFO << "[DEMO] 开始核对 5 个必需 Topic："
             << kSynchronousTopic << "，" << kBlockingTopic << "，"
             << kCallbackTopic << "，" << kZeroCopyTopic << "，"
             << kObserverTopic;

    // 逐个查找可以给出明确的缺失 Topic 日志。
    for (const auto expected : expected_topics) {
        const bool found = std::any_of(
            topics.begin(),
            topics.end(),
            [expected](const DDS::LocalTopicInfo& topic) {
                return topic.topic_name == expected;
            });

        // 任一示例 Topic 缺失都说明注册或共享状态存在问题。
        if (!found) {
            LOG_ERROR << "[DEMO] 缺少必需 Topic：" << expected;
            return DemoResult::Failed;
        }

        LOG_INFO << "[DEMO] 已找到必需 Topic：" << expected;
    }

    // Topic 枚举和元信息校验通过。
    LOG_INFO << "[DEMO] Topic 发现校验通过：5 个 DDS 示例 Topic 均已注册且可见";
    return DemoResult::Passed;
}

} // namespace MB_DDF::Demo
