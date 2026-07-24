#pragma once

#include "MB_DDF_Demo/DemoResult.h"

#include <cstdint>

#ifdef MB_DDF_TEST_BUILD
namespace MB_DDF::HW {
class ISpiTransport;
}
#endif

namespace MB_DDF::Demo {

/**
 * @brief 直接调用各具体 Device，对目标板执行只读通信检查和状态读取。
 *
 * 设备路径默认是 /dev/xdma0，可通过环境变量 MB_DDF_XDMA_DEVICE 覆盖。
 * 本示例不调用配置、复位、清错、输出更新或点火接口。
 */
DemoResult run_hw_direct_device_example();

/**
 * @brief 执行硬件写入、回环、DMA 和 DDS Adapter 的全能力示例。
 *
 * 仅当 MB_DDF_HW_FULL_DEMO=1 时执行。可恢复配置会在示例结束前恢复；
 * ADS1258 错误计数清零和 DH 点火属于不可恢复动作。CPU SPI Flash 测试会备份、
 * 临时改写并恢复一个 4 KiB 子扇区，但进程终止或掉电仍可能中断恢复。
 */
DemoResult run_hw_full_capability_example();

#ifdef MB_DDF_TEST_BUILD
namespace TestHooks {
/// 仅供目标板单元测试注入内存 SPI Transport，覆盖完整备份/擦写/恢复状态机。
bool run_spi_flash_workflow(HW::ISpiTransport& transport, uint32_t address);
}
#endif

} // namespace MB_DDF::Demo
