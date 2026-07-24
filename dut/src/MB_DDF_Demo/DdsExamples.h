#pragma once

#include "MB_DDF_Demo/DemoResult.h"

namespace MB_DDF::DDS {
class DDSCore;
}

namespace MB_DDF::Demo {

/**
 * @brief 演示最常用的同步发布和非阻塞读取流程。
 */
DemoResult run_dds_synchronous_example(DDS::DDSCore& dds);

/**
 * @brief 演示带超时的阻塞读取，以及生产者和消费者线程配合。
 */
DemoResult run_dds_blocking_example(DDS::DDSCore& dds);

/**
 * @brief 演示由 Subscriber 工作线程触发的异步回调。
 */
DemoResult run_dds_callback_example(DDS::DDSCore& dds);

/**
 * @brief 演示 begin_message/commit 和 publish_fill 两种零拷贝发布方式。
 */
DemoResult run_dds_zero_copy_example(DDS::DDSCore& dds);

/**
 * @brief 演示观察者回调和本地消息序列号。
 */
DemoResult run_dds_observer_example(DDS::DDSCore& dds);

/**
 * @brief 演示枚举当前 DDS 本地域已经注册的 Topic。
 */
DemoResult run_dds_topic_discovery_example(DDS::DDSCore& dds);

} // namespace MB_DDF::Demo
