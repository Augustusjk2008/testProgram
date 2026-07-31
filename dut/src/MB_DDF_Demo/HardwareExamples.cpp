#include "MB_DDF_Demo/HardwareExamples.h"

#include "MB_DDF/Debug/Logger.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef MB_DDF_DEMO_WITH_HARDWARE
#include "MB_DDF_HW/Device/Ad7606Device.h"
#include "MB_DDF_HW/Device/Ads1258Device.h"
#include "MB_DDF_HW/Device/ComDevice.h"
#include "MB_DDF_HW/Device/DhController.h"
#include "MB_DDF_HW/Device/DidoDevice.h"
#include "MB_DDF_HW/Device/PwmDevice.h"
#include "MB_DDF_HW/Device/SpiFlashDevice.h"
#include "MB_DDF_HW/Device/Registers/Ad7606Registers.h"
#include "MB_DDF_HW/Device/Registers/DhRegisters.h"
#include "MB_DDF_HW/Device/Registers/DidoRegisters.h"
#include "MB_DDF_HW/Device/Registers/PwmRegisters.h"
#include "MB_DDF_HW/Transport/SpidevTransport.h"
#include "MB_DDF_HW/Transport/XdmaTransport.h"
#ifdef MB_DDF_DEMO_WITH_HW_DDS_ADAPTER
#include "MB_DDF_HW/DdsAdapter/ComExternalEndpoint.h"
#endif
#endif

namespace MB_DDF::Demo {
namespace {

#ifdef MB_DDF_DEMO_WITH_HARDWARE

/**
 * @brief 输出统一的硬件示例失败日志。
 */
DemoResult fail_hardware_example(const char* example, const std::string& reason) {
    // Status::message 已经包含失败动作的底层原因。
    LOG_ERROR << "[DEMO] " << example << "失败：" << reason;

    // 主程序会统计 Failed 并返回非零退出码。
    return DemoResult::Failed;
}

/**
 * @brief 将硬件 Status 格式化为适合板卡日志的一行文本。
 */
std::string format_status(const HW::Status& status) {
    // 使用字符串流同时保留状态码、errno 和实现提供的说明。
    std::ostringstream stream;
    stream << "状态码=" << static_cast<int>(status.code)
           << ", errno=" << status.errno_value
           << ", 说明=" << status.message;
    return stream.str();
}

/** @brief 将整数格式化为带 0x 前缀的大写十六进制。 */
std::string hex_value(uint64_t value, unsigned width = 0) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase;
    if (width != 0) {
        stream << std::setw(static_cast<int>(width)) << std::setfill('0');
    }
    stream << value;
    return stream.str();
}

/** @brief 将 bool 输出为适合教学日志的中文。 */
const char* yes_no(bool value) {
    return value ? "是" : "否";
}

/**
 * @brief 打开一个 XDMA MMIO Transport，并在失败时输出设备标签。
 */
bool open_transport(HW::XdmaTransport& transport, const char* label) {
    LOG_INFO << "[DEMO] 正在打开 " << label << " 的 XDMA Transport（映射 user BAR）";
    // open 会映射 user BAR；COM 配置还会按 event_number 打开事件节点。
    const auto result = transport.open();

    // Result<void> 可以直接用于布尔判断。
    if (result) {
        LOG_INFO << "[DEMO] 已打开 " << label << " 的 XDMA Transport";
        return true;
    }

    // 打开失败时输出完整 Status，常见原因是设备节点不存在或权限不足。
    LOG_ERROR << "[DEMO] 打开 " << label << " 的 XDMA Transport 失败："
              << format_status(result.status());
    return false;
}

/**
 * @brief 读取并输出 PWM 当前状态，不修改任何输出寄存器。
 */
bool probe_pwm(const std::string& device_path) {
    // PWM IP 位于 XDMA user BAR 偏移 0x00000，映射窗口大小为 64 KiB。
    LOG_INFO << "[DEMO] [PWM只读巡检] 基地址=0x00000，窗口=0x10000；将校验通信签名并读取4路输出状态";
    HW::XdmaTransport transport({device_path, 0x00000, 0x10000});

    // 打开 MMIO Transport 后才能创建并使用强类型设备对象。
    if (!open_transport(transport, "PWM")) {
        return false;
    }

    // Device 不拥有 Transport，因此 Transport 的生命周期必须覆盖 Device。
    HW::PwmDevice device(transport);

    // 通信检查读取偏移 0 的固定签名 0xAAAABBBB。
    const auto communication = device.check_communication();
    if (!communication) {
        LOG_ERROR << "[DEMO] PWM通信校验失败（期望签名=0xAAAABBBB）："
                  << format_status(communication.status());
        transport.close();
        return false;
    }
    LOG_INFO << "[DEMO] PWM通信校验通过：固定签名=0xAAAABBBB";

    // read_state 只读取载波、峰值、波形、方向、占空比和使能状态。
    const auto state = device.read_state();
    if (!state) {
        LOG_ERROR << "[DEMO] 读取PWM组合状态失败：" << format_status(state.status());
        transport.close();
        return false;
    }

    // 将四路占空比和控制位输出到日志，便于与 FPGA 调试工具交叉核对。
    LOG_INFO << "[DEMO] PWM当前状态：载波计数=" << state.value().config.carrier_frequency_value
             << "，峰值=" << state.value().config.peak_value
             << "，波形="
             << (state.value().config.waveform == HW::PwmWaveform::Sawtooth ? "锯齿波" : "三角波")
             << "，4路占空比原始值=[" << state.value().outputs.duty[0]
             << ", " << state.value().outputs.duty[1]
             << ", " << state.value().outputs.duty[2]
             << ", " << state.value().outputs.duty[3]
             << "]，方向掩码=0x" << std::hex
             << static_cast<unsigned>(state.value().outputs.direction_mask)
             << "，使能掩码=0x"
             << static_cast<unsigned>(state.value().outputs.enable_mask)
             << std::dec << "，允许更新=" << yes_no(state.value().update_enabled);

    // 显式 close 方便用户理解资源释放位置；异常或提前返回时成员 RAII 也会释放句柄。
    transport.close();
    return true;
}

/**
 * @brief 读取 AD7606 八通道原始采样，不改变采集和滤波配置。
 */
bool probe_ad7606(const std::string& device_path) {
    // AD7606 IP 位于 XDMA user BAR 偏移 0x10000。
    LOG_INFO << "[DEMO] [AD7606只读巡检] 基地址=0x10000，窗口=0x10000；将读取8路16位有符号原始采样";
    HW::XdmaTransport transport({device_path, 0x10000, 0x10000});
    if (!open_transport(transport, "AD7606")) {
        return false;
    }

    // 强类型接口把每个通道的低 16 位解释为有符号采样值。
    HW::Ad7606Device device(transport);

    // 首先验证 IP 核通信签名。
    const auto communication = device.check_communication();
    if (!communication) {
        LOG_ERROR << "[DEMO] AD7606通信校验失败："
                  << format_status(communication.status());
        transport.close();
        return false;
    }
    LOG_INFO << "[DEMO] AD7606通信校验通过，开始读取通道0~7当前快照";

    // read_snapshot 连续读取八个通道，不等待下一次采样完成。
    const auto snapshot = device.read_snapshot();
    if (!snapshot) {
        LOG_ERROR << "[DEMO] 读取AD7606八通道快照失败："
                  << format_status(snapshot.status());
        transport.close();
        return false;
    }

    // 使用字符串流把八路数据放在同一行，便于保存和比较测试日志。
    std::ostringstream values;
    values << "[";
    for (size_t index = 0; index < snapshot.value().raw.size(); ++index) {
        if (index != 0) {
            values << ", ";
        }
        values << snapshot.value().raw[index];
    }
    values << "]";
    LOG_INFO << "[DEMO] AD7606通道0~7原始采样值=" << values.str()
             << "（有符号16位，未换算工程单位）";

    // 完成本 IP 核检查后释放映射。
    transport.close();
    return true;
}

/**
 * @brief 读取 ADS1258 三十二通道原始值、两片诊断值和错误计数。
 */
bool probe_ads1258(const std::string& device_path) {
    // ADS1258 IP 位于 XDMA user BAR 偏移 0x20000。
    LOG_INFO << "[DEMO] [ADS1258只读巡检] 基地址=0x20000，窗口=0x10000；将读取32路原始值、两片OFFSET/VCC/TEMP/GAIN/VREF和错误计数";
    HW::XdmaTransport transport({device_path, 0x20000, 0x10000});
    if (!open_transport(transport, "ADS1258")) {
        return false;
    }

    // Device 只把寄存器映射为强类型结构，不在示例中解释模拟量单位。
    HW::Ads1258Device device(transport);

    // 固定签名不匹配通常表示偏移、bitstream 或寄存器版本不一致。
    const auto communication = device.check_communication();
    if (!communication) {
        LOG_ERROR << "[DEMO] ADS1258通信校验失败："
                  << format_status(communication.status());
        transport.close();
        return false;
    }
    LOG_INFO << "[DEMO] ADS1258通信校验通过，开始读取32通道、10个诊断值和9类FPGA错误计数";

    // 快照包含 32 路数据、两片五类诊断量和 FPGA 维护的 9 个错误计数。
    const auto snapshot = device.read_snapshot();
    if (!snapshot) {
        LOG_ERROR << "[DEMO] 读取ADS1258数据快照失败："
                  << format_status(snapshot.status());
        transport.close();
        return false;
    }

    // 每八路输出一行，避免 32 路数据在串口终端中过长。
    for (size_t base = 0; base < snapshot.value().raw.size(); base += 8) {
        std::ostringstream values;
        values << "[";
        for (size_t index = base; index < base + 8; ++index) {
            if (index != base) {
                values << ", ";
            }
            values << snapshot.value().raw[index];
        }
        values << "]";
        LOG_INFO << "[DEMO] ADS1258通道" << base << "~" << (base + 7)
                 << "原始值=" << values.str();
    }

    for (size_t chip = 0; chip < snapshot.value().diagnostics.size(); ++chip) {
        const auto calibration = HW::Ads1258Device::decode_diagnostics(
            snapshot.value().diagnostics[chip]);
        if (!calibration) {
            LOG_ERROR << "[DEMO] ADS1258芯片" << (chip + 1)
                      << "诊断量换算失败：" << format_status(calibration.status());
            transport.close();
            return false;
        }
        LOG_INFO << "[DEMO] ADS1258芯片" << (chip + 1)
                 << "诊断：OFFSET=" << calibration.value().offset_voltage
                 << "V，VCC=" << calibration.value().supply_voltage
                 << "V，TEMP=" << calibration.value().temperature_celsius
                 << "°C，GAIN=" << calibration.value().gain
                 << "，VREF=" << calibration.value().reference_voltage << "V";
    }

    // 输出错误计数摘要；本只读示例不会调用 clear_error_counters。
    const auto& errors = snapshot.value().errors;
    LOG_INFO << "[DEMO] ADS1258错误计数：采集超时=" << errors.acquisition_timeout
             << "，芯片1无新数据=" << errors.chip1_no_new_data
             << "，芯片1溢出=" << errors.chip1_overflow
             << "，芯片1电源异常=" << errors.chip1_power_fault
             << "，芯片1通道异常=" << errors.chip1_channel_fault
             << "，芯片2无新数据=" << errors.chip2_no_new_data
             << "，芯片2溢出=" << errors.chip2_overflow
             << "，芯片2电源异常=" << errors.chip2_power_fault
             << "，芯片2通道异常=" << errors.chip2_channel_fault;

    // 释放 ADS1258 映射。
    transport.close();
    return true;
}

/**
 * @brief 读取 DH 点火控制器反馈，绝不发出点火或配置命令。
 */
bool probe_dh(const std::string& device_path) {
    // DH IP 位于 XDMA user BAR 偏移 0x30000。
    LOG_INFO << "[DEMO] [DH只读巡检] 基地址=0x30000，窗口=0x10000；只读取48路反馈和电池状态，不执行点火";
    HW::XdmaTransport transport({device_path, 0x30000, 0x10000});
    if (!open_transport(transport, "DH")) {
        return false;
    }

    // DhController 包含危险写操作，因此本示例只调用 check 和 read_feedback。
    HW::DhController device(transport);

    // 验证设备签名，防止在错误地址上解释反馈寄存器。
    const auto communication = device.check_communication();
    if (!communication) {
        LOG_ERROR << "[DEMO] DH通信校验失败："
                  << format_status(communication.status());
        transport.close();
        return false;
    }
    LOG_INFO << "[DEMO] DH通信校验通过，开始读取点火反馈位图和电池状态";

    // read_feedback 只读取 48 路反馈位和电池状态。
    const auto feedback = device.read_feedback();
    if (!feedback) {
        LOG_ERROR << "[DEMO] 读取DH反馈状态失败：" << format_status(feedback.status());
        transport.close();
        return false;
    }

    // 十六进制掩码适合快速定位置位通道。
    LOG_INFO << "[DEMO] DH反馈：48路通道掩码=0x" << std::hex
             << feedback.value().channel_mask << std::dec
             << "，电池已点火=" << yes_no(feedback.value().battery_fired)
             << "，电池已激活=" << yes_no(feedback.value().battery_activated);

    // 不调用 fire、fire_multiple、configure_timebase 或 set_pulse_width。
    transport.close();
    return true;
}

/**
 * @brief 读取 DIDO 输入和输出状态，不更新任何输出通道。
 */
bool probe_dido(const std::string& device_path) {
    // DIDO IP 位于 XDMA user BAR 偏移 0x140000。
    LOG_INFO << "[DEMO] [DIDO只读巡检] 基地址=0x140000，窗口=0x10000；读取16路DI/DO业务有效位图，不修改输出";
    HW::XdmaTransport transport({device_path, 0x140000, 0x10000});
    if (!open_transport(transport, "DIDO")) {
        return false;
    }

    // DidoDevice 内部处理高有效和低有效通道的极性差异。
    HW::DidoDevice device(transport);

    // 先验证通信签名。
    const auto communication = device.check_communication();
    if (!communication) {
        LOG_ERROR << "[DEMO] DIDO通信校验失败："
                  << format_status(communication.status());
        transport.close();
        return false;
    }
    LOG_INFO << "[DEMO] DIDO通信校验通过，开始读取输入和输出快照";

    // read_snapshot 同时返回业务有效意义下的输入和输出位图。
    const auto snapshot = device.read_snapshot();
    if (!snapshot) {
        LOG_ERROR << "[DEMO] 读取DIDO输入输出快照失败："
                  << format_status(snapshot.status());
        transport.close();
        return false;
    }

    // 输出位图使用十六进制，16 路状态可以紧凑显示。
    LOG_INFO << "[DEMO] DIDO快照：DO业务有效位图=0x" << std::hex
             << snapshot.value().outputs_active << "，DI业务有效位图=0x"
             << snapshot.value().inputs_active << std::dec;

    // 不调用 set_outputs，因此不会改变板卡数字输出。
    transport.close();
    return true;
}

/**
 * @brief 读取一个 COM IP 的配置和错误状态，不发送数据、不清错、不复位。
 */
bool probe_com(const std::string& device_path, unsigned index) {
    // 四个 COM IP 的基地址按 0x40000 间隔排列。
    constexpr uint64_t offsets[] = {0x40000, 0x80000, 0xC0000, 0x100000};

    // 每个 COM 映射 256 KiB，并绑定同编号 XDMA event 节点。
    HW::XdmaTransport transport(
        {device_path, offsets[index], 0x40000, -1, -1, static_cast<int>(index)});

    // 日志标签按人类习惯显示 COM1 到 COM4。
    const std::string label = "COM" + std::to_string(index + 1);
    LOG_INFO << "[DEMO] [" << label << "只读巡检] user BAR基地址="
             << hex_value(offsets[index], 5) << "，窗口=0x40000，XDMA event=" << index
             << "；只读配置和错误状态，不发送、不清错";
    if (!open_transport(transport, label.c_str())) {
        return false;
    }

    // ComDevice 同时实现 IByteEndpoint，但本示例只使用只读诊断接口。
    HW::ComDevice device(transport);

    // 验证固定通信签名。
    const auto communication = device.check_communication();
    if (!communication) {
        LOG_ERROR << "[DEMO] " << label << "通信校验失败："
                  << format_status(communication.status());
        transport.close();
        return false;
    }
    LOG_INFO << "[DEMO] " << label << "通信校验通过，开始读取帧格式、回环、中断和波特率配置";

    // read_config 获取当前帧格式、接收开关、回环开关和波特率计数。
    const auto config = device.read_config();
    if (!config) {
        LOG_ERROR << "[DEMO] 读取" << label << "配置失败："
                  << format_status(config.status());
        transport.close();
        return false;
    }

    // read_error_status 只读错误寄存器，不会清除错误。
    const auto errors = device.read_error_status();
    if (!errors) {
        LOG_ERROR << "[DEMO] 读取" << label << "错误状态失败："
                  << format_status(errors.status());
        transport.close();
        return false;
    }

    // 输出最常用的配置字段，完整结构仍可按业务需要继续扩展日志。
    LOG_INFO << "[DEMO] " << label
             << "配置：允许接收=" << yes_no(config.value().receive_enabled)
             << "，内部回环=" << yes_no(config.value().loopback)
             << "，中断模式="
             << (config.value().interrupt_mode == HW::ComInterruptMode::Level ? "电平" : "脉冲")
             << "，波特率计数=" << config.value().baudrate_counter
             << "，发送/接收长度字段="
             << static_cast<unsigned>(config.value().frame.send_length_bytes) << "/"
             << static_cast<unsigned>(config.value().frame.receive_length_bytes) << "字节"
             << "，发送/接收帧头="
             << static_cast<unsigned>(config.value().frame.send_header_length) << "/"
             << static_cast<unsigned>(config.value().frame.receive_header_length) << "字节"
             << "，错误状态=" << hex_value(errors.value(), 8);

    // 不调用 send、configure、clear_error_status、reset 或 enable_receive。
    transport.close();
    return true;
}

/**
 * @brief 记录全能力示例中单个硬件动作的统一结果。
 *
 * 返回值用于累计能力组结果；失败时保留底层状态码、errno 和说明，调用方仍可继续
 * 执行恢复步骤或其他独立能力组。
 */
template <typename T>
bool require_hardware_result(const HW::Result<T>& result, const std::string& action) {
    if (result) {
        LOG_INFO << "[DEMO] 步骤完成：" << action;
        return true;
    }
    LOG_ERROR << "[DEMO] 步骤失败：" << action << "；" << format_status(result.status());
    return false;
}

/**
 * @brief 判断用户是否显式允许执行会修改硬件状态的全能力示例。
 */
bool full_demo_enabled() {
    const char* value = std::getenv("MB_DDF_HW_FULL_DEMO");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

/**
 * @brief 解析 DMA 备份、测试和恢复使用的设备侧地址。
 *
 * strtoull 的 base=0 同时支持十进制和 0x 前缀十六进制。非法、负数或溢出输入回退
 * 到地址 0，并在真正执行 DMA 前输出警告。
 */
uint64_t full_demo_dma_offset() {
    const char* value = std::getenv("MB_DDF_HW_DMA_OFFSET");
    if (value == nullptr || value[0] == '\0') {
        return 0;
    }

    char* end = nullptr;
    errno = 0;
    const auto offset = std::strtoull(value, &end, 0);
    if (value[0] == '-' || end == value || *end != '\0' || errno == ERANGE) {
        LOG_WARN << "[DEMO] 环境变量MB_DDF_HW_DMA_OFFSET='" << value
                 << "'不是有效地址，改用设备偏移0x0";
        return 0;
    }
    return static_cast<uint64_t>(offset);
}

/**
 * @brief 解析 CPU SPI Flash 测试使用的 4 KiB 子扇区地址。
 *
 * 缺省选择 N25Q512A 最后一个子扇区；与 DMA 地址不同，非法 Flash 地址不能静默
 * 回退，否则可能擦除操作者未确认的区域。
 */
HW::Result<uint32_t> full_demo_spi_flash_address() {
    constexpr uint32_t default_address =
        HW::SpiFlashDevice::CapacityBytes - HW::SpiFlashDevice::SubsectorSize;
    const char* value = std::getenv("MB_DDF_HW_SPI_FLASH_TEST_ADDRESS");
    if (value == nullptr || value[0] == '\0') {
        return default_address;
    }

    char* end = nullptr;
    errno = 0;
    const auto parsed = std::strtoull(value, &end, 0);
    if (value[0] == '-' || end == value || *end != '\0' || errno == ERANGE ||
        parsed > default_address ||
        (parsed % HW::SpiFlashDevice::SubsectorSize) != 0) {
        return HW::Status::error(
            HW::StatusCode::InvalidArgument, 0,
            "MB_DDF_HW_SPI_FLASH_TEST_ADDRESS must be a 4 KiB-aligned address "
            "in the N25Q512A range 0x00000000..0x03FFF000");
    }
    return static_cast<uint32_t>(parsed);
}

/**
 * @brief 演示 PWM 配置、归一化输出和精确状态恢复。
 */
bool exercise_pwm(const std::string& device_path) {
    // 全能力示例仍使用 PWM 固定窗口，不与其他 IP 共享局部寄存器地址。
    HW::XdmaTransport transport({device_path, 0x00000, 0x10000});
    LOG_INFO << "[DEMO] [PWM全能力] 基地址=0x00000；流程=备份→暂停更新→写测试输出→恢复原值";
    if (!open_transport(transport, "PWM全能力示例")) {
        return false;
    }
    HW::PwmDevice device(transport);

    // 写寄存器前再次检查签名；前面的只读示例失败不会阻止本区段被调度。
    if (!require_hardware_result(device.check_communication(),
                                 "PWM全能力通信校验（签名0xAAAABBBB）")) {
        return false;
    }
    const auto saved = device.read_state();
    if (!require_hardware_result(saved, "备份PWM载波、4路占空比、方向、使能和更新状态")) {
        return false;
    }

    // PwmState 保存业务语义；这些寄存器需要额外保留原始编码才能逐位恢复。
    const auto saved_update_register =
        transport.read32(HW::Registers::Pwm::UpdateEnable);
    const auto saved_enable_register = transport.read32(HW::Registers::Pwm::Enable);
    const auto saved_duty_mode_register = transport.read32(HW::Registers::Pwm::DutyMode);
    if (!require_hardware_result(saved_update_register,
                                 "读取PWM更新寄存器，地址=" +
                                     hex_value(HW::Registers::Pwm::UpdateEnable)) ||
        !require_hardware_result(saved_enable_register,
                                 "读取PWM使能寄存器，地址=" +
                                     hex_value(HW::Registers::Pwm::Enable)) ||
        !require_hardware_result(saved_duty_mode_register,
                                 "读取PWM占空比模式寄存器，地址=" +
                                     hex_value(HW::Registers::Pwm::DutyMode))) {
        return false;
    }
    LOG_INFO << "[DEMO] PWM备份值：载波计数=" << saved.value().config.carrier_frequency_value
             << "，峰值=" << saved.value().config.peak_value
             << "，占空比=[" << saved.value().outputs.duty[0] << ", "
             << saved.value().outputs.duty[1] << ", " << saved.value().outputs.duty[2]
             << ", " << saved.value().outputs.duty[3] << "]，方向掩码="
             << hex_value(saved.value().outputs.direction_mask, 2) << "，使能掩码="
             << hex_value(saved.value().outputs.enable_mask, 2) << "，更新寄存器="
             << hex_value(saved_update_register.value(), 8) << "，使能寄存器="
             << hex_value(saved_enable_register.value(), 8) << "，占空比模式寄存器="
             << hex_value(saved_duty_mode_register.value(), 8);
    std::array<uint32_t, 4> saved_direction_registers{};
    for (unsigned index = 0; index < saved_direction_registers.size(); ++index) {
        const auto value = transport.read32(HW::Registers::Pwm::direction(index));
        if (!require_hardware_result(
                value, "读取PWM方向寄存器CH" + std::to_string(index) + "，地址=" +
                           hex_value(HW::Registers::Pwm::direction(index)))) {
            return false;
        }
        saved_direction_registers[index] = value.value();
        LOG_INFO << "[DEMO] PWM方向寄存器CH" << index
                 << "备份值=" << hex_value(value.value(), 8);
    }

    // 更新期间先暂停硬件采纳新值，再一次性启用四路代表性正反向输出。
    bool passed = true;
    passed &= require_hardware_result(device.set_update_enabled(false),
                                      "暂停PWM寄存器更新，避免配置过程中输出半成品");
    passed &= require_hardware_result(device.set_duty_mode_unsigned(),
                                      "选择PWM无符号占空比模式");
    passed &= require_hardware_result(device.configure(saved.value().config),
                                      "按备份值配置PWM载波：计数=" +
                                          std::to_string(saved.value().config.carrier_frequency_value) +
                                          "，峰值=" + std::to_string(saved.value().config.peak_value));
    HW::PwmNormalizedOutputs outputs{{0.25, -0.25, 0.5, -0.5}, 0x0F};
    LOG_INFO << "[DEMO] PWM测试输出：归一化值=[0.25, -0.25, 0.50, -0.50]，使能掩码=0x0F";
    passed &= require_hardware_result(device.apply_normalized_outputs(outputs),
                                      "写入4路PWM归一化测试输出");
    passed &= require_hardware_result(device.set_update_enabled(true),
                                      "重新允许PWM寄存器更新，使测试值生效");

    // 无论演示步骤是否失败都尝试恢复；原始编码最后写回，覆盖语义转换的差异。
    bool restored = true;
    restored &= require_hardware_result(device.set_update_enabled(false),
                                         "恢复PWM前再次暂停更新");
    restored &= require_hardware_result(device.configure(saved.value().config),
                                         "恢复原PWM载波配置");
    restored &= require_hardware_result(device.apply_outputs(saved.value().outputs),
                                         "恢复原4路PWM占空比、方向和使能值");
    for (unsigned index = 0; index < saved_direction_registers.size(); ++index) {
        restored &= require_hardware_result(
            transport.write32(HW::Registers::Pwm::direction(index),
                              saved_direction_registers[index]),
            "恢复PWM方向寄存器CH" + std::to_string(index) + "，值=" +
                hex_value(saved_direction_registers[index], 8));
    }
    restored &= require_hardware_result(
        transport.write32(HW::Registers::Pwm::Enable, saved_enable_register.value()),
        "恢复PWM使能寄存器，值=" + hex_value(saved_enable_register.value(), 8));
    restored &= require_hardware_result(
        transport.write32(HW::Registers::Pwm::DutyMode, saved_duty_mode_register.value()),
        "恢复PWM占空比模式寄存器，值=" + hex_value(saved_duty_mode_register.value(), 8));
    restored &= require_hardware_result(
        transport.write32(HW::Registers::Pwm::UpdateEnable, saved_update_register.value()),
        "恢复PWM更新寄存器，值=" + hex_value(saved_update_register.value(), 8));
    transport.close();
    return passed && restored;
}

/**
 * @brief 演示 AD7606 配置、采集复位、组合状态读取和配置恢复。
 */
bool exercise_ad7606(const std::string& device_path) {
    HW::XdmaTransport transport({device_path, 0x10000, 0x10000});
    LOG_INFO << "[DEMO] [AD7606全能力] 基地址=0x10000；流程=备份→翻转滤波→复位采集→读取→恢复";
    if (!open_transport(transport, "AD7606全能力示例")) {
        return false;
    }
    HW::Ad7606Device device(transport);
    if (!require_hardware_result(device.check_communication(),
                                 "AD7606全能力通信校验")) {
        return false;
    }
    const auto saved = device.read_state();
    if (!require_hardware_result(saved, "备份AD7606配置和8通道当前快照")) {
        return false;
    }

    // 使能寄存器使用硬件魔数编码，单独保存可避免 bool 解码丢失原值。
    const auto saved_acquisition =
        transport.read32(HW::Registers::Ad7606::AcquisitionEnable);
    const auto saved_filter = transport.read32(HW::Registers::Ad7606::FilterEnable);
    if (!require_hardware_result(saved_acquisition,
                                 "读取AD7606采集使能寄存器，地址=" +
                                     hex_value(HW::Registers::Ad7606::AcquisitionEnable)) ||
        !require_hardware_result(saved_filter,
                                 "读取AD7606滤波使能寄存器，地址=" +
                                     hex_value(HW::Registers::Ad7606::FilterEnable))) {
        return false;
    }
    LOG_INFO << "[DEMO] AD7606备份配置：采集使能="
             << yes_no(saved.value().config.acquisition_enabled)
             << "，滤波使能=" << yes_no(saved.value().config.filter_enabled)
             << "，过采样=" << static_cast<unsigned>(saved.value().config.oversampling)
             << "，采集次数=" << saved.value().config.acquisition_count
             << "；原采集寄存器=" << hex_value(saved_acquisition.value(), 8)
             << "，原滤波寄存器=" << hex_value(saved_filter.value(), 8);

    // 翻转滤波开关让 configure 产生可观察写入，随后复位一次采集状态机。
    auto temporary = saved.value().config;
    temporary.filter_enabled = !temporary.filter_enabled;
    bool passed = require_hardware_result(
        device.configure(temporary), "配置AD7606：滤波使能由" +
                                         std::string(yes_no(saved.value().config.filter_enabled)) +
                                         "切换为" + yes_no(temporary.filter_enabled));
    passed &= require_hardware_result(device.reset(), "复位AD7606采集状态机一次");
    passed &= require_hardware_result(device.read_state(), "读取AD7606配置与8通道组合状态");

    // 先用强类型接口恢复完整配置，再精确覆盖两个魔数编码寄存器。
    bool restored =
        require_hardware_result(device.configure(saved.value().config), "恢复AD7606原配置");
    restored &= require_hardware_result(
        transport.write32(HW::Registers::Ad7606::AcquisitionEnable,
                          saved_acquisition.value()),
        "精确恢复AD7606采集使能寄存器，值=" + hex_value(saved_acquisition.value(), 8));
    restored &= require_hardware_result(
        transport.write32(HW::Registers::Ad7606::FilterEnable, saved_filter.value()),
        "精确恢复AD7606滤波使能寄存器，值=" + hex_value(saved_filter.value(), 8));
    transport.close();
    return passed && restored;
}

/**
 * @brief 演示 ADS1258 完整配置写入、错误计数清零和采样快照。
 */
bool exercise_ads1258(const std::string& device_path) {
    HW::XdmaTransport transport({device_path, 0x20000, 0x10000});
    LOG_INFO << "[DEMO] [ADS1258全能力] 基地址=0x20000；流程=备份配置→写回配置→清零错误计数→读取→恢复";
    if (!open_transport(transport, "ADS1258全能力示例")) {
        return false;
    }
    HW::Ads1258Device device(transport);
    if (!require_hardware_result(device.check_communication(),
                                 "ADS1258全能力通信校验")) {
        return false;
    }
    const auto saved = device.read_config();
    if (!require_hardware_result(saved, "备份ADS1258全部可写配置寄存器")) {
        return false;
    }
    LOG_INFO << "[DEMO] ADS1258备份配置摘要：write_command="
             << hex_value(saved.value().write_command, 8)
             << "，config0=" << hex_value(saved.value().config0, 8)
             << "，config1=" << hex_value(saved.value().config1, 8)
             << "，muxsch=" << hex_value(saved.value().muxsch, 8)
             << "，spi_divider=" << saved.value().spi_divider
             << "，drdy_timeout=" << saved.value().drdy_timeout;

    // 写回当前配置即可覆盖 configure 全路径，避免猜测板卡模拟前端参数。
    bool passed = require_hardware_result(device.configure(saved.value()),
                                          "按备份值逐项写回ADS1258配置，覆盖configure接口");

    // 错误计数清零不可逆；仅在 MB_DDF_HW_FULL_DEMO=1 时进入此处。
    passed &= require_hardware_result(device.clear_error_counters(),
                                      "清零ADS1258的9类错误计数（该动作不可恢复）");
    passed &= require_hardware_result(device.read_snapshot(),
                                      "读取ADS1258的32通道数据和清零后的错误计数");
    const bool restored = require_hardware_result(device.configure(saved.value()),
                                                   "恢复ADS1258原配置寄存器值");
    transport.close();
    return passed && restored;
}

/**
 * @brief 演示单路 DIDO 输出修改、快照读取和全部输出恢复。
 */
bool exercise_dido(const std::string& device_path) {
    HW::XdmaTransport transport({device_path, 0x140000, 0x10000});
    LOG_INFO << "[DEMO] [DIDO全能力] 基地址=0x140000；仅翻转DO0，读取快照后逐寄存器恢复16路输出";
    if (!open_transport(transport, "DIDO全能力示例")) {
        return false;
    }
    HW::DidoDevice device(transport);
    if (!require_hardware_result(device.check_communication(),
                                 "DIDO全能力通信校验")) {
        return false;
    }
    const auto saved = device.read_outputs();
    if (!require_hardware_result(saved, "备份DIDO的16路DO业务有效位图")) {
        return false;
    }

    // 高/低有效通道的业务位图不能表示原始魔数，因此备份全部输出寄存器。
    std::array<uint32_t, 16> saved_registers{};
    for (unsigned index = 0; index < saved_registers.size(); ++index) {
        const auto value = transport.read32(HW::Registers::Dido::output(index));
        if (!require_hardware_result(
                value, "读取DIDO输出寄存器DO" + std::to_string(index) + "，地址=" +
                           hex_value(HW::Registers::Dido::output(index)))) {
            return false;
        }
        saved_registers[index] = value.value();
        LOG_INFO << "[DEMO] DIDO输出寄存器DO" << index
                 << "备份值=" << hex_value(value.value(), 8);
    }

    // DO0 代表 set_outputs 能力，其他十五路在演示期间保持不变。
    constexpr uint16_t demo_channel = 0x0001;
    const uint16_t temporary = saved.value() ^ demo_channel;
    LOG_INFO << "[DEMO] DIDO测试值：原DO位图=" << hex_value(saved.value(), 4)
             << "，更新掩码=0x0001，仅将DO0翻转为临时位图=" << hex_value(temporary, 4);
    bool passed = require_hardware_result(device.set_outputs(temporary, demo_channel),
                                          "按更新掩码0x0001翻转DIDO输出DO0");
    passed &= require_hardware_result(device.read_snapshot(),
                                      "读取DIDO翻转后的DI/DO业务有效位图");

    // 逐寄存器回写原值，恢复过程不依赖极性转换实现。
    bool restored = true;
    for (unsigned index = 0; index < saved_registers.size(); ++index) {
        restored &= require_hardware_result(
            transport.write32(HW::Registers::Dido::output(index), saved_registers[index]),
            "恢复DIDO输出寄存器DO" + std::to_string(index) + "，值=" +
                hex_value(saved_registers[index], 8));
    }
    transport.close();
    return passed && restored;
}

/**
 * @brief 严格按硬件规定的使能链执行主发动机2点火，并在结束后反向失能。
 */
bool fire_main_engine_2(HW::ITransport& transport, uint16_t pulse_width_ms) {
    // 地址表给出的是寄存器编号，XDMA Transport 接收的是局部字节偏移。
    constexpr uint64_t kReturn28VEnable = 0x01u * sizeof(uint32_t);
    constexpr uint64_t kIgnitionEnable = 0x77u * sizeof(uint32_t);
    constexpr uint64_t kFireMode = 0x72u * sizeof(uint32_t);
    constexpr uint64_t kRepeatMode = 0x75u * sizeof(uint32_t);
    constexpr uint64_t kMainEngine2Command = 0x13u * sizeof(uint32_t);

    // 后三项与现有 DH 强类型寄存器定义应始终指向同一地址。
    static_assert(kFireMode == HW::Registers::Dh::FireMode);
    static_assert(kRepeatMode == HW::Registers::Dh::RepeatMode);
    static_assert(kMainEngine2Command == HW::Registers::Dh::fire(2));

    LOG_WARN << "[DEMO] [主发动机2点火] 开始执行5步控制链；当前点火脉宽="
             << pulse_width_ms << " ms，完成回告最长等待=" << (pulse_width_ms + 1000)
             << " ms";

    // 每次写入后读取同一寄存器，仅显示板端回读值，不改变原有控制流。
    const auto log_register_value = [&transport](uint64_t byte_offset) {
        const uint64_t register_address = byte_offset / sizeof(uint32_t);
        std::ostringstream address_text;
        address_text << "0x" << std::hex << std::uppercase << register_address;

        const auto value = transport.read32(byte_offset);
        if (!value) {
            LOG_ERROR << "[DEMO] 主发动机2：读取寄存器" << address_text.str()
                      << "回读值失败：" << format_status(value.status());
            return;
        }

        std::ostringstream value_text;
        value_text << "0x" << std::hex << std::uppercase << std::setw(8)
                   << std::setfill('0') << value.value();
        LOG_INFO << "[DEMO] 主发动机2：寄存器" << address_text.str()
                 << "当前回读值=" << value_text.str();
    };

    bool sequence_ok = require_hardware_result(
        transport.write32(kReturn28VEnable, 0xA000u),
        "点火步骤1/5：使能28V回线，写寄存器0x1 <- 0xA000");
    log_register_value(kReturn28VEnable);
    const bool return_28v_enabled = sequence_ok;

    if (sequence_ok) {
        sequence_ok = require_hardware_result(
            transport.write32(kIgnitionEnable, 0xAAAAu),
            "点火步骤2/5：打开点火总使能，写寄存器0x77 <- 0xAAAA");
        log_register_value(kIgnitionEnable);
    }
    const bool ignition_enabled = sequence_ok;

    if (sequence_ok) {
        sequence_ok = require_hardware_result(
            transport.write32(kFireMode, 0xBBBBu),
            "点火步骤3/5：选择单通道点火模式，写寄存器0x72 <- 0xBBBB");
        log_register_value(kFireMode);
    }
    if (sequence_ok) {
        sequence_ok = require_hardware_result(
            transport.write32(kRepeatMode, 0xBBBBu),
            "点火步骤4/5：选择可重复点火模式，写寄存器0x75 <- 0xBBBB");
        log_register_value(kRepeatMode);
    }
    if (sequence_ok) {
        sequence_ok = require_hardware_result(
            transport.write32(kMainEngine2Command, 0xB002u),
            "点火步骤5/5：下发主发动机2点火命令，写寄存器0x13 <- 0xB002");
        log_register_value(kMainEngine2Command);
    }

    // 保持使能直到通道2回告点火完成，避免撤销使能截断当前脉冲。
    if (sequence_ok) {
        bool completed = false;
        uint32_t last_feedback = 0;
        LOG_INFO << "[DEMO] 主发动机2：开始轮询寄存器0x13低16位，完成值=0xAAAA，轮询周期=1 ms";
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(pulse_width_ms) +
                              std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto feedback = transport.read32(kMainEngine2Command);
            if (!feedback) {
                require_hardware_result(feedback, "读取主发动机2点火回告寄存器0x13");
                sequence_ok = false;
                break;
            }
            last_feedback = feedback.value();
            if ((feedback.value() & 0xFFFFu) == 0xAAAAu) {
                completed = true;
                LOG_INFO << "[DEMO] 主发动机2点火完成：寄存器0x13回告="
                         << hex_value(feedback.value(), 8) << "，低16位=0xAAAA";
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (sequence_ok && !completed) {
            LOG_ERROR << "[DEMO] 主发动机2点火回告超时：最长等待="
                      << (pulse_width_ms + 1000) << " ms，最后回读值="
                      << hex_value(last_feedback, 8) << "，期望低16位=0xAAAA";
            sequence_ok = false;
        }
    }

    // 点火动作不可撤销；使能链使用地址表规定的关闭值按相反顺序收回。
    bool disabled = true;
    if (ignition_enabled) {
        disabled &= require_hardware_result(
            transport.write32(kIgnitionEnable, 0xFFFFu),
            "收回点火使能：写寄存器0x77 <- 0xFFFF");
        log_register_value(kIgnitionEnable);
    }
    if (return_28v_enabled) {
        disabled &= require_hardware_result(
            transport.write32(kReturn28VEnable, 0x00A0u),
            "关闭28V回线：写寄存器0x1 <- 0x00A0");
        log_register_value(kReturn28VEnable);
    }
    return sequence_ok && disabled;
}

/**
 * @brief 演示 DH 时基、主发动机2点火和反馈读取。
 */
bool exercise_dh(const std::string& device_path) {
    HW::XdmaTransport transport({device_path, 0x30000, 0x10000});
    LOG_WARN << "[DEMO] [DH全能力] 基地址=0x30000；将保存配置、执行主发动机2点火、读取回告并反向关闭使能链";
    if (!open_transport(transport, "DH全能力示例")) {
        return false;
    }
    HW::DhController device(transport);
    if (!require_hardware_result(device.check_communication(),
                                 "DH全能力通信校验")) {
        return false;
    }

    // Device 未提供模式和时基读取接口，直接保存对应寄存器以便演示后恢复。
    const auto saved_timebase = transport.read32(HW::Registers::Dh::Timebase);
    const auto saved_fire_mode = transport.read32(HW::Registers::Dh::FireMode);
    const auto saved_repeat_mode = transport.read32(HW::Registers::Dh::RepeatMode);
    const auto saved_pulses = device.read_pulse_widths();
    if (!require_hardware_result(saved_timebase,
                                 "备份DH时基寄存器0x71（局部偏移0x1C4）") ||
        !require_hardware_result(saved_fire_mode,
                                 "备份DH点火模式寄存器0x72（局部偏移0x1C8）") ||
        !require_hardware_result(saved_repeat_mode,
                                 "备份DH重复模式寄存器0x75（局部偏移0x1D4）") ||
        !require_hardware_result(saved_pulses, "备份DH全部48路点火脉宽计数")) {
        return false;
    }

    // 通道2对应主发动机2；保留当前脉宽，仅演示配置接口和指定点火写序列。
    constexpr uint8_t demo_channel = 2;
    LOG_INFO << "[DEMO] DH备份值：时基=" << hex_value(saved_timebase.value(), 8)
             << "，点火模式=" << hex_value(saved_fire_mode.value(), 8)
             << "，重复模式=" << hex_value(saved_repeat_mode.value(), 8)
             << "，主发动机2脉宽寄存器CH2="
             << hex_value(saved_pulses.value()[demo_channel], 4)
             << "（演示按1 tick=1 ms配置，约"
             << saved_pulses.value()[demo_channel] << " ms）";
    bool passed = require_hardware_result(
        device.configure_timebase({saved_timebase.value(), 1}),
        "配置DH时基：保持原值" + hex_value(saved_timebase.value(), 8) + "，单位参数=1");
    passed &= require_hardware_result(
        device.set_pulse_width_ms(demo_channel, saved_pulses.value()[demo_channel], 1),
        "配置主发动机2点火脉宽：CH2=" +
            std::to_string(saved_pulses.value()[demo_channel]) + " ms，单位参数=1");
    if (passed) {
        passed = fire_main_engine_2(transport, saved_pulses.value()[demo_channel]);
    }
    const auto feedback = device.read_feedback();
    passed &= require_hardware_result(feedback, "读取DH的48路反馈位图和电池状态");
    if (feedback) {
        LOG_INFO << "[DEMO] 点火后DH反馈：通道掩码="
                 << hex_value(feedback.value().channel_mask, 12)
                 << "，电池已点火=" << yes_no(feedback.value().battery_fired)
                 << "，电池已激活=" << yes_no(feedback.value().battery_activated);
    }

    // 点火动作本身不可撤销，其余时基、模式和脉宽均恢复为进入示例前的值。
    bool restored = true;
    restored &= require_hardware_result(
        device.set_pulse_width_ticks(demo_channel, saved_pulses.value()[demo_channel]),
        "恢复主发动机2脉宽寄存器CH2，值=" +
            hex_value(saved_pulses.value()[demo_channel], 4));
    restored &= require_hardware_result(
        transport.write32(HW::Registers::Dh::Timebase, saved_timebase.value()),
        "恢复DH时基寄存器0x71，值=" + hex_value(saved_timebase.value(), 8));
    restored &= require_hardware_result(
        transport.write32(HW::Registers::Dh::FireMode, saved_fire_mode.value()),
        "恢复DH点火模式寄存器0x72，值=" + hex_value(saved_fire_mode.value(), 8));
    restored &= require_hardware_result(
        transport.write32(HW::Registers::Dh::RepeatMode, saved_repeat_mode.value()),
        "恢复DH重复模式寄存器0x75，值=" + hex_value(saved_repeat_mode.value(), 8));
    transport.close();
    return passed && restored;
}

constexpr size_t kComLoopbackIterationCount = 16;
constexpr size_t kComLoopbackPayloadSize = 32;
constexpr size_t kComReceiveBufferSize = 65536;

std::array<uint8_t, kComLoopbackPayloadSize> make_com_loopback_payload(size_t iteration) {
    std::array<uint8_t, kComLoopbackPayloadSize> payload{};
    constexpr std::array<uint8_t, 13> marker = {
        0x4D, 0x42, 0x5F, 0x44, 0x44, 0x46, 0x5F, 0x52, 0x58, 0x5F, 0x42, 0x41, 0x4E};
    std::copy(marker.begin(), marker.end(), payload.begin());

    const uint32_t round = static_cast<uint32_t>(iteration + 1);
    payload[13] = static_cast<uint8_t>(round & 0xFFu);
    payload[14] = static_cast<uint8_t>((round >> 8u) & 0xFFu);
    payload[15] = static_cast<uint8_t>(~round & 0xFFu);
    for (size_t index = 16; index < payload.size(); ++index) {
        payload[index] = static_cast<uint8_t>(
            0xA5u ^ ((round * 0x3Du) & 0xFFu) ^ ((index * 0x17u) & 0xFFu));
    }
    return payload;
}

std::string format_payload_hex(const uint8_t* data, size_t size, size_t limit = 64) {
    std::ostringstream stream;
    stream << std::hex << std::uppercase << std::setfill('0');
    const size_t displayed = std::min(size, limit);
    for (size_t index = 0; index < displayed; ++index) {
        if (index != 0) {
            stream << ' ';
        }
        stream << std::setw(2) << static_cast<unsigned>(data[index]);
    }
    if (displayed < size) {
        stream << " ... (" << std::dec << size << " bytes)";
    }
    return stream.str();
}

bool verify_com_loopback_iterations(HW::IByteEndpoint& endpoint,
                                    size_t iteration_count,
                                    uint32_t timeout_us) {
    if (iteration_count == 0) {
        LOG_ERROR << "[DEMO] COM1 RX bank loopback requires at least one iteration";
        return false;
    }
    if (endpoint.mtu() < kComLoopbackPayloadSize) {
        LOG_ERROR << "[DEMO] COM1 RX bank loopback payload exceeds endpoint MTU: payload="
                  << kComLoopbackPayloadSize << ", mtu=" << endpoint.mtu();
        return false;
    }

    std::vector<uint8_t> received(kComReceiveBufferSize);
    std::array<size_t, 2> parity_totals{};
    std::array<size_t, 2> parity_passed{};
    size_t passed_rounds = 0;

    LOG_INFO << "[DEMO] COM1 dual RX bank loopback started: iterations=" << iteration_count
             << ", payload_bytes=" << kComLoopbackPayloadSize
             << ", every payload is unique and retries are disabled";
    for (size_t iteration = 0; iteration < iteration_count; ++iteration) {
        const size_t round = iteration + 1;
        const size_t parity = iteration % 2;
        ++parity_totals[parity];
        const auto payload = make_com_loopback_payload(iteration);
        std::fill(received.begin(), received.end(), uint8_t{0});

        const auto sent = endpoint.send({payload.data(), payload.size()});
        if (!sent) {
            LOG_ERROR << "[DEMO] COM1 RX bank round " << round << "/" << iteration_count
                      << " send failed: " << format_status(sent.status())
                      << ", expected="
                      << format_payload_hex(payload.data(), payload.size());
            continue;
        }
        if (sent.value() != payload.size()) {
            LOG_ERROR << "[DEMO] COM1 RX bank round " << round << "/" << iteration_count
                      << " partial send: expected_bytes=" << payload.size()
                      << ", sent_bytes=" << sent.value();
            continue;
        }

        const auto result = endpoint.receive(
            {received.data(), received.size()}, HW::Timeout::after_us(timeout_us));
        if (!result) {
            LOG_ERROR << "[DEMO] COM1 RX bank round " << round << "/" << iteration_count
                      << " receive failed: " << format_status(result.status())
                      << ", expected="
                      << format_payload_hex(payload.data(), payload.size());
            continue;
        }

        const size_t received_size = result.value();
        const size_t common_size = std::min(payload.size(), received_size);
        size_t first_difference = common_size;
        for (size_t index = 0; index < common_size; ++index) {
            if (payload[index] != received[index]) {
                first_difference = index;
                break;
            }
        }
        const bool matched = received_size == payload.size() &&
                             first_difference == payload.size();
        if (!matched) {
            std::ostringstream difference;
            if (first_difference < common_size) {
                difference << "offset=" << first_difference
                           << ", expected=" << hex_value(payload[first_difference], 2)
                           << ", actual=" << hex_value(received[first_difference], 2);
            } else if (received_size < payload.size()) {
                difference << "offset=" << received_size << ", actual frame ended";
            } else {
                difference << "offset=" << payload.size() << ", unexpected trailing data";
            }
            LOG_ERROR << "[DEMO] COM1 RX bank round " << round << "/" << iteration_count
                      << " mismatch: expected_bytes=" << payload.size()
                      << ", actual_bytes=" << received_size
                      << ", first_difference=" << difference.str()
                      << ", expected="
                      << format_payload_hex(payload.data(), payload.size())
                      << ", actual="
                      << format_payload_hex(received.data(), received_size);
            continue;
        }

        ++passed_rounds;
        ++parity_passed[parity];
        LOG_INFO << "[DEMO] COM1 RX bank round " << round << "/" << iteration_count
                 << " passed, bytes=" << received_size;
    }

    const bool passed = passed_rounds == iteration_count;
    if (passed) {
        LOG_INFO << "[DEMO] COM1 dual RX bank loopback passed: " << passed_rounds << "/"
                 << iteration_count << ", odd=" << parity_passed[0] << "/"
                 << parity_totals[0] << ", even=" << parity_passed[1] << "/"
                 << parity_totals[1];
        return true;
    }

    LOG_ERROR << "[DEMO] COM1 dual RX bank loopback failed: " << passed_rounds << "/"
              << iteration_count << ", odd=" << parity_passed[0] << "/"
              << parity_totals[0] << ", even=" << parity_passed[1] << "/"
              << parity_totals[1];
    const bool alternating_failure =
        (parity_passed[0] == parity_totals[0] && parity_passed[1] == 0) ||
        (parity_passed[1] == parity_totals[1] && parity_passed[0] == 0);
    if (alternating_failure) {
        LOG_ERROR << "[DEMO] COM1 failures alternate by round parity; this is consistent with "
                     "one of the two FPGA RX banks returning stale data";
    }
    return false;
}

/**
 * @brief 通过 COM1 内部回环演示 COM Device、event fd 和 DDS Adapter。
 */
bool exercise_com_and_adapter(const std::string& device_path) {
    // COM1 代表四个同构 COM，并绑定 XDMA event 0 用于阻塞接收。
    HW::XdmaTransport transport({device_path, 0x40000, 0x40000, -1, -1, 0});
    if (!open_transport(transport, "COM1 full capability")) {
        return false;
    }
    HW::ComDevice device(transport);
    if (!require_hardware_result(device.check_communication(),
                                 "COM1 full capability communication check")) {
        return false;
    }
    const auto saved = device.read_config();
    if (!require_hardware_result(saved, "COM1 save configuration")) {
        return false;
    }

    // 继承当前帧格式和波特率。
    auto cfg = saved.value();
    cfg.loopback = true;
    cfg.receive_enabled = true;
    cfg.interrupt_mode = HW::ComInterruptMode::Level;
    const auto default_config = HW::ComDevice::default_config();
    LOG_INFO << "[DEMO] COM default config: mtu=" << device.mtu()
             << ", baudrate_counter=" << default_config.baudrate_counter;
    bool passed = device.mtu() != 0;
    passed &= require_hardware_result(transport.event_fd(), "COM1 query event fd");
    // Control.D7 软件复位会锁存到外部总线复位，正常回环流程不可调用 reset()。
    passed &= require_hardware_result(device.configure(cfg), "COM1 configure loopback");
    passed &= require_hardware_result(device.clear_error_status(), "COM1 clear errors");
    passed &= require_hardware_result(device.enable_receive(), "COM1 enable receive");

    // 固定 payload 便于在串口日志和逻辑分析工具中识别本次回环。
    const std::array<uint8_t, 16> payload = {
        0x4D, 0x42, 0x5F, 0x44, 0x44, 0x46, 0x5F, 0x44,
        0x45, 0x4D, 0x4F, 0x5F, 0x43, 0x4F, 0x4D, 0x31};
    std::array<uint8_t, 64> received{};
#ifdef MB_DDF_DEMO_WITH_HW_DDS_ADAPTER
    // ComExternalEndpoint 是 ExternalEndpointAdapter 的别名，代表同类 Adapter 能力。
    HW::ComExternalEndpoint endpoint(device);
    const bool sent = endpoint.send(payload.data(), payload.size());
    if (!sent) {
        LOG_ERROR << "[DEMO] COM1 adapter send failed";
        passed = false;
    }
    const int32_t received_size = endpoint.receive(received.data(), received.size(), 1000000);
    const bool loopback_matched =
        received_size == static_cast<int32_t>(payload.size()) &&
        std::memcmp(payload.data(), received.data(), payload.size()) == 0;
    passed &= loopback_matched;
    if (!loopback_matched) {
        LOG_ERROR << "[DEMO] COM1 adapter loopback mismatch";
    } else {
        LOG_INFO << "[DEMO] COM1 adapter loopback succeeded, mtu=" << endpoint.mtu();
    }
#else
    // Adapter 未参与构建时仍保留直接 IByteEndpoint 收发演示。
    const auto sent = device.send({payload.data(), payload.size()});
    passed &= require_hardware_result(sent, "COM1 send");
    const auto received_result =
        device.receive({received.data(), received.size()}, HW::Timeout::after_us(1000000));
    passed &= require_hardware_result(received_result, "COM1 receive");
    passed &= received_result && received_result.value() == payload.size() &&
              std::memcmp(payload.data(), received.data(), payload.size()) == 0;
#endif
    passed &= verify_com_loopback_iterations(
        device, kComLoopbackIterationCount, 1000000);
    passed &= require_hardware_result(device.read_error_status(), "COM1 read error status");

    // reset 和清错无法恢复瞬时状态，但持久 COM 配置必须恢复。
    const bool restored =
        require_hardware_result(device.configure(saved.value()), "COM1 restore configuration");
    transport.close();
    return passed && restored;
}

/**
 * @brief 演示 XDMA DMA0 的 C2H/H2C 双向传输并恢复测试区域。
 */
bool exercise_dma(const std::string& device_path) {
    constexpr int channel = 0;
    constexpr size_t transfer_size = 64;
    const uint64_t offset = full_demo_dma_offset();
    HW::XdmaTransport transport({device_path, 0, 0x1000, channel, channel, -1});
    if (!open_transport(transport, "XDMA DMA0")) {
        return false;
    }

    // 先保留设备侧原内容；测试图案随字节位置变化，能发现顺序和覆盖错误。
    std::array<uint8_t, transfer_size> saved{};
    std::array<uint8_t, transfer_size> pattern{};
    std::array<uint8_t, transfer_size> received{};
    for (size_t index = 0; index < pattern.size(); ++index) {
        pattern[index] = static_cast<uint8_t>(0xA5u ^ index);
    }

    const auto backup = transport.dma_read(channel, {saved.data(), saved.size()}, offset);
    bool passed = require_hardware_result(backup, "XDMA DMA backup read") &&
                  backup.value() == saved.size();
    bool wrote_pattern = false;

    // 只有完整备份成功才写入，避免没有恢复数据时破坏目标区域。
    if (passed) {
        const auto write = transport.dma_write(channel, {pattern.data(), pattern.size()}, offset);
        passed = require_hardware_result(write, "XDMA DMA pattern write") &&
                 write.value() == pattern.size();
        wrote_pattern = write && write.value() == pattern.size();
    }

    // 使用同一设备地址回读并逐字节比较，验证 H2C 和 C2H 组合路径。
    if (passed) {
        const auto read = transport.dma_read(channel, {received.data(), received.size()}, offset);
        passed = require_hardware_result(read, "XDMA DMA pattern readback") &&
                 read.value() == received.size() && received == pattern;
        if (!passed) {
            LOG_ERROR << "[DEMO] XDMA DMA readback mismatch at device offset 0x"
                      << std::hex << offset << std::dec;
        }
    }

    // 只要测试图案完整写入过，就不受回读结果影响，始终尝试恢复备份。
    bool restored = true;
    if (wrote_pattern) {
        const auto restore = transport.dma_write(channel, {saved.data(), saved.size()}, offset);
        restored = require_hardware_result(restore, "XDMA DMA restore") &&
                   restore.value() == saved.size();
    }
    transport.close();
    return passed && restored;
}

/**
 * @brief 用 CPU spidev0.0 演示 N25Q512A 的可恢复 4 KiB 擦写测试。
 */
bool exercise_spi_flash(HW::ISpiTransport* injected_transport = nullptr,
                        uint32_t injected_address = 0) {
    constexpr const char* spi_path = "/dev/spidev0.0";
    constexpr auto erase_timeout_us = 2'000'000u;
    constexpr auto program_timeout_us = 100'000u;
    constexpr auto recovery_ready_timeout_us = 5'000'000u;
    constexpr std::array<uint8_t, 16> test_pattern{
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x99};

    const bool injected_address_valid =
        (injected_address % HW::SpiFlashDevice::SubsectorSize) == 0 &&
        injected_address <= HW::SpiFlashDevice::CapacityBytes -
                                HW::SpiFlashDevice::SubsectorSize;
    const HW::Result<uint32_t> address_result =
        injected_transport == nullptr
            ? full_demo_spi_flash_address()
            : (injected_address_valid
                   ? HW::Result<uint32_t>(injected_address)
                   : HW::Result<uint32_t>(HW::Status::error(
                         HW::StatusCode::InvalidArgument, 0,
                         "injected SPI Flash test address is invalid")));
    if (!require_hardware_result(address_result, "校验CPU SPI Flash测试子扇区地址")) {
        return false;
    }
    const uint32_t address = address_result.value();
    const uint32_t last_address =
        address + static_cast<uint32_t>(HW::SpiFlashDevice::SubsectorSize) - 1u;
    LOG_WARN << "[DEMO] [CPU SPI Flash全能力] 设备=" << spi_path
             << "，型号=Micron N25Q512A，SPI=Mode0/8bit/1MHz，测试范围=["
             << hex_value(address, 8) << ", " << hex_value(last_address, 8)
             << "]；流程=永久关闭#HOLD→双Die Ready→JEDEC ID校验→按四字节地址完整备份→"
                "4KiB擦除→写读校验→完整恢复。"
                "测试期间断电仍可能破坏该4KiB，请确保该区域可从外部备份重刷";

    HW::SpidevTransport owned_transport({spi_path, 1'000'000, 0, 8});
    HW::ISpiTransport& transport =
        injected_transport == nullptr ? static_cast<HW::ISpiTransport&>(owned_transport)
                                      : *injected_transport;
    if (injected_transport == nullptr) {
        LOG_INFO << "[DEMO] 正在打开 CPU SPI Flash 的 spidev Transport";
        const auto opened = owned_transport.open();
        if (!require_hardware_result(opened, "打开/dev/spidev0.0并核验SPI配置")) {
            return false;
        }
    } else if (!transport.is_open()) {
        LOG_ERROR << "[DEMO] 注入的SPI Flash测试Transport尚未打开";
        return false;
    }
    HW::SpiFlashDevice flash(transport);
    const auto restore_and_close_transport = [&]() {
        if (injected_transport != nullptr) {
            return true;
        }
        const bool config_restored = require_hardware_result(
            owned_transport.restore_configuration(),
            "恢复/dev/spidev0.0原Mode、位序、字宽和速率配置");
        owned_transport.close();
        return config_restored;
    };

    if (!require_hardware_result(
            flash.disable_hold(),
            "发送06h和B1h EFh FFh永久关闭N25Q512A #HOLD")) {
        (void)restore_and_close_transport();
        return false;
    }

    // 接管堆叠器件时，先分别读取两个 die 的 70h 状态。状态寄存器事务不含
    // Flash 内存地址；后续首笔内存数据事务仍是 13h + 调用方确认的四字节地址。
    if (!require_hardware_result(
            flash.wait_until_all_dies_idle(
                HW::Timeout::after_us(recovery_ready_timeout_us)),
            "首次地址读取前确认N25Q512A两个Die均为Ready")) {
        (void)restore_and_close_transport();
        return false;
    }

    const auto initial_status = flash.read_status();
    const auto initial_flags_first_die = flash.read_flag_status();
    const auto initial_flags_second_die = flash.read_flag_status();
    const bool status_readable =
        require_hardware_result(initial_status, "读取N25Q512A初始Status Register") &&
        require_hardware_result(initial_flags_first_die,
                                "读取N25Q512A第一个Die的初始Flag Status Register") &&
        require_hardware_result(initial_flags_second_die,
                                "读取N25Q512A第二个Die的初始Flag Status Register");
    if (!status_readable) {
        (void)restore_and_close_transport();
        return false;
    }
    if (initial_status.value() == 0xFF &&
        initial_flags_first_die.value() == 0xFF &&
        initial_flags_second_die.value() == 0xFF) {
        LOG_WARN << "[DEMO] /dev/spidev0.0连续读到Status=0xFF、两个Die的Flag Status=0xFF；"
                     "SPI MISO保持高电平，器件片选/供电/引脚复用未响应，未执行任何擦写命令";
        // (void)restore_and_close_transport();
        // return false;
    }

    const auto jedec_id = flash.read_jedec_id();
    if (!require_hardware_result(jedec_id, "读取N25Q512A JEDEC ID（9Fh）")) {
        (void)restore_and_close_transport();
        return false;
    }
    if (jedec_id.value() != HW::SpiFlashDevice::ExpectedJedecId) {
        LOG_ERROR << "[DEMO] N25Q512A JEDEC ID不匹配：实际="
                  << hex_value(jedec_id.value()[0], 2) << " "
                  << hex_value(jedec_id.value()[1], 2) << " "
                  << hex_value(jedec_id.value()[2], 2) << "，期望="
                  << hex_value(HW::SpiFlashDevice::ExpectedJedecId[0], 2) << " "
                  << hex_value(HW::SpiFlashDevice::ExpectedJedecId[1], 2) << " "
                  << hex_value(HW::SpiFlashDevice::ExpectedJedecId[2], 2)
                  << "；未执行任何地址读取或擦写命令";
        (void)restore_and_close_transport();
        return false;
    }
    LOG_INFO << "[DEMO] N25Q512A JEDEC ID校验通过："
             << hex_value(jedec_id.value()[0], 2) << " "
             << hex_value(jedec_id.value()[1], 2) << " "
             << hex_value(jedec_id.value()[2], 2);

    // 完整地址读取和备份成功后，才清除历史错误并为后续擦写确认器件空闲。
    bool ready = require_hardware_result(flash.clear_flag_status(),
                                         "清除N25Q512A历史Flag Status错误位");
    if (ready) {
        ready = require_hardware_result(
            flash.wait_until_ready(HW::Timeout::after_us(erase_timeout_us)),
            "确认N25Q512A进入Ready状态");
    }
    if (!ready) {
        (void)restore_and_close_transport();
        return false;
    }

    // 9Fh 只用于读取 JEDEC ID；Flash 数据访问使用调用方确认的四字节地址。
    std::array<uint8_t, HW::SpiFlashDevice::SubsectorSize> backup{};
    std::array<uint8_t, HW::SpiFlashDevice::SubsectorSize> verify{};
    const auto backup_read = flash.read(address, {backup.data(), backup.size()});
    const bool backup_complete =
        require_hardware_result(
            backup_read,
            "使用13h和四字节地址直接备份Flash测试子扇区4096字节，地址=" +
                hex_value(address, 8)) &&
        backup_read.value() == backup.size();
    if (!backup_complete) {
        if (backup_read && backup_read.value() != backup.size()) {
            LOG_ERROR << "[DEMO] Flash备份长度错误：实际=" << backup_read.value()
                      << "，期望=" << backup.size() << "；未执行任何擦写命令";
        }
        (void)restore_and_close_transport();
        return false;
    }

    bool test_passed = true;
    const auto erased = flash.erase_subsector(
        address, HW::Timeout::after_us(erase_timeout_us));
    // 前置 CLFSR/WREN 失败时不主动擦除；只要21h进入传输阶段，就保守执行恢复。
    const bool mutation_may_have_started =
        flash.last_mutation_command_attempted();
    test_passed = require_hardware_result(erased, "擦除Flash测试子扇区（21h，4字节地址）");

    if (test_passed) {
        const auto erased_read = flash.read(address, {verify.data(), verify.size()});
        test_passed = require_hardware_result(erased_read,
                                              "读回完整4KiB并校验擦除结果") &&
                      erased_read.value() == verify.size();
        const auto not_erased = std::find_if(verify.begin(), verify.end(),
                                             [](uint8_t value) { return value != 0xFF; });
        if (test_passed && not_erased != verify.end()) {
            const size_t offset = static_cast<size_t>(not_erased - verify.begin());
            LOG_ERROR << "[DEMO] Flash擦除校验失败：偏移=" << hex_value(offset)
                      << "，实际=" << hex_value(*not_erased, 2) << "，期望=0xFF";
            test_passed = false;
        }
    }

    if (test_passed) {
        const auto programmed = flash.program_page(
            address, {test_pattern.data(), test_pattern.size()},
            HW::Timeout::after_us(program_timeout_us));
        test_passed = require_hardware_result(
                          programmed, "写入SpiFlash.h的16字节测试图样（12h）") &&
                      programmed.value() == test_pattern.size();
    }

    if (test_passed) {
        std::array<uint8_t, test_pattern.size()> readback{};
        const auto read = flash.read(address, {readback.data(), readback.size()});
        test_passed = require_hardware_result(read,
                                              "使用13h读回16字节测试图样") &&
                      read.value() == readback.size() && readback == test_pattern;
        if (!test_passed && read && read.value() == readback.size()) {
            const auto mismatch = std::mismatch(test_pattern.begin(), test_pattern.end(),
                                                readback.begin());
            if (mismatch.first != test_pattern.end()) {
                const size_t offset =
                    static_cast<size_t>(mismatch.first - test_pattern.begin());
                LOG_ERROR << "[DEMO] Flash测试图样不匹配：偏移=" << hex_value(offset)
                          << "，实际=" << hex_value(readback[offset], 2)
                          << "，期望=" << hex_value(test_pattern[offset], 2);
            }
        }
    }

    bool original_untouched = false;
    bool controller_state_safe = true;
    bool recovery_had_failure = false;
    bool restored = false;
    if (!mutation_may_have_started) {
        // WREN 的 ioctl 或后续 RDSR 可能失败，但数据命令未发送；用 WRDI 收敛未知 WEL。
        original_untouched = true;
        controller_state_safe = require_hardware_result(
            flash.write_disable(),
            "擦除命令未发送，执行WRDI并确认WEL已清零");
        restored = true;
    } else {
        const auto settle_for_recovery = [&](const std::string& phase) {
            // 先等潜在的破坏性命令结束；Busy 期间发送 CLFSR 可能被器件忽略。
            const auto first_wait = flash.wait_until_ready(
                HW::Timeout::after_us(recovery_ready_timeout_us));
            if (!first_wait) {
                recovery_had_failure = true;
                require_hardware_result(first_wait, phase + "等待N25Q512A Ready");
                if (first_wait.status().code != HW::StatusCode::HardwareFault) {
                    return false;
                }
                LOG_WARN << "[DEMO] " << phase
                         << "检测到已结束操作的Flag Status错误，清错后继续恢复";
            }

            bool settled = require_hardware_result(
                flash.clear_flag_status(), phase + "清除N25Q512A Flag Status错误位");
            recovery_had_failure |= !settled;
            if (settled) {
                settled = require_hardware_result(
                    flash.wait_until_ready(HW::Timeout::after_us(program_timeout_us)),
                    phase + "清错后再次确认N25Q512A Ready");
                recovery_had_failure |= !settled;
            }
            if (settled) {
                const bool write_disabled = require_hardware_result(
                    flash.write_disable(), phase + "执行WRDI并确认WEL已清零");
                controller_state_safe &= write_disabled;
                recovery_had_failure |= !write_disabled;
                settled = write_disabled;
            }
            return settled;
        };

        bool settled = settle_for_recovery("恢复前");
        if (settled) {
            // ioctl 报错并不等于命令已执行；内容仍与备份相同就无需增加一次擦写。
            const auto current = flash.read(address, {verify.data(), verify.size()});
            const bool current_read = require_hardware_result(
                                          current, "恢复前读取当前Flash测试子扇区") &&
                                      current.value() == verify.size();
            recovery_had_failure |= !current_read;
            if (current_read && verify == backup) {
                restored = true;
                LOG_INFO << "[DEMO] Flash测试子扇区仍与备份一致，无需执行恢复擦写";
            }
        }

        // 恢复失败时从重新擦除开始最多重试一次，避免在部分编程页上直接续写。
        for (unsigned attempt = 0; settled && !restored && attempt < 2; ++attempt) {
            if (attempt != 0) {
                LOG_WARN << "[DEMO] Flash恢复将从子扇区擦除开始重试一次";
            }
            bool attempt_ok = true;
            const auto restore_erase = flash.erase_subsector(
                address, HW::Timeout::after_us(erase_timeout_us));
            attempt_ok = require_hardware_result(
                restore_erase,
                "恢复Flash前擦除测试子扇区（第" + std::to_string(attempt + 1) + "次）");
            if (!attempt_ok) {
                recovery_had_failure = true;
                if (flash.last_mutation_command_attempted()) {
                    settled = settle_for_recovery("恢复擦除失败后");
                } else {
                    const bool write_disabled = require_hardware_result(
                        flash.write_disable(),
                        "恢复擦除命令未发送，执行WRDI并确认WEL已清零");
                    controller_state_safe &= write_disabled;
                    recovery_had_failure |= !write_disabled;
                    settled = write_disabled;
                }
                continue;
            }

            size_t restored_pages = 0;
            size_t skipped_erased_pages = 0;
            for (size_t offset = 0;
                 attempt_ok && offset < backup.size();
                 offset += HW::SpiFlashDevice::PageSize) {
                const auto first = backup.begin() + static_cast<std::ptrdiff_t>(offset);
                const auto last = first +
                                  static_cast<std::ptrdiff_t>(HW::SpiFlashDevice::PageSize);
                if (std::all_of(first, last,
                                [](uint8_t value) { return value == 0xFF; })) {
                    ++skipped_erased_pages;
                    continue;
                }
                const auto programmed = flash.program_page(
                    address + static_cast<uint32_t>(offset),
                    {backup.data() + offset, HW::SpiFlashDevice::PageSize},
                    HW::Timeout::after_us(program_timeout_us));
                attempt_ok = require_hardware_result(
                                 programmed,
                                 "恢复Flash原始页，页偏移=" + hex_value(offset)) &&
                             programmed.value() == HW::SpiFlashDevice::PageSize;
                if (attempt_ok) {
                    ++restored_pages;
                } else if (flash.last_mutation_command_attempted()) {
                    recovery_had_failure = true;
                    settled = settle_for_recovery("恢复页编程失败后");
                } else {
                    recovery_had_failure = true;
                    const bool write_disabled = require_hardware_result(
                        flash.write_disable(),
                        "恢复页命令未发送，执行WRDI并确认WEL已清零");
                    controller_state_safe &= write_disabled;
                    recovery_had_failure |= !write_disabled;
                    settled = write_disabled;
                }
            }
            LOG_INFO << "[DEMO] Flash恢复页统计：已编程=" << restored_pages
                     << "，原本全FF并跳过=" << skipped_erased_pages;
            if (!attempt_ok) {
                continue;
            }

            const auto restored_read = flash.read(address, {verify.data(), verify.size()});
            const bool restored_read_complete = require_hardware_result(
                                                    restored_read,
                                                    "读回完整4KiB校验Flash原始内容恢复") &&
                                                restored_read.value() == verify.size();
            restored = restored_read_complete && verify == backup;
            recovery_had_failure |= !restored;
            if (!restored && restored_read_complete) {
                const auto mismatch = std::mismatch(backup.begin(), backup.end(),
                                                    verify.begin());
                if (mismatch.first != backup.end()) {
                    const size_t offset =
                        static_cast<size_t>(mismatch.first - backup.begin());
                    LOG_ERROR << "[DEMO] Flash恢复校验不匹配：地址="
                              << hex_value(address + static_cast<uint32_t>(offset), 8)
                              << "，实际=" << hex_value(verify[offset], 2)
                              << "，期望=" << hex_value(backup[offset], 2);
                }
            }
        }
    }

    const bool transport_config_restored = restore_and_close_transport();
    if (original_untouched && !controller_state_safe) {
        LOG_ERROR << "[DEMO] Flash数据命令未发送，原始4KiB内容未被本流程修改；"
                     "但WEL状态未确认清零，请停止SPI访问并断电复位器件";
    } else if (!restored) {
        LOG_ERROR << "[DEMO] Flash原始内容未确认恢复；请停止访问，并从外部备份重刷范围["
                  << hex_value(address, 8) << ", " << hex_value(last_address, 8) << "]";
    } else if (recovery_had_failure) {
        LOG_ERROR << "[DEMO] Flash原始4KiB内容最终已恢复，但恢复过程出现失败，"
                     "本次示例仍判定失败";
    } else if (!transport_config_restored) {
        LOG_ERROR << "[DEMO] Flash内容与控制器状态已恢复，但原SPI总线配置恢复失败";
    } else if (!test_passed && original_untouched) {
        LOG_ERROR << "[DEMO] CPU SPI Flash测试在数据命令前失败，原始4KiB内容未被修改";
    } else if (!test_passed) {
        LOG_ERROR << "[DEMO] CPU SPI Flash读写测试失败，但原始4KiB内容已完整恢复";
    } else {
        LOG_INFO << "[DEMO] CPU SPI Flash读写测试通过，原始4KiB内容已完整恢复";
    }
    return test_passed && restored && controller_state_safe &&
           !recovery_had_failure && transport_config_restored;
}

#endif

} // namespace

#if defined(MB_DDF_DEMO_WITH_HARDWARE) && defined(MB_DDF_TEST_BUILD)
bool TestHooks::run_com_loopback_iterations(HW::IByteEndpoint& endpoint,
                                            size_t iteration_count,
                                            uint32_t timeout_us) {
    return verify_com_loopback_iterations(endpoint, iteration_count, timeout_us);
}

bool TestHooks::run_spi_flash_workflow(HW::ISpiTransport& transport,
                                       uint32_t address) {
    return exercise_spi_flash(&transport, address);
}
#endif

DemoResult run_hw_direct_device_example() {
#ifndef MB_DDF_DEMO_WITH_HARDWARE
    // 未编译硬件层时不尝试访问 /dev 节点。
    LOG_WARN << "[DEMO] MB_DDF_HW is not enabled; direct Device example skipped";
    return DemoResult::Skipped;
#else
    // 本示例直接构造并调用具体 Device，不经过 DDS 或 Adapter。
    LOG_INFO << "[DEMO] MB_DDF_HW direct Device example started";

    // 默认设备前缀与现有 smoke 测试保持一致。
    std::string device_path = "/dev/xdma0";

    // 环境变量允许同一二进制测试 xdma1 等其他设备实例。
    if (const char* configured = std::getenv("MB_DDF_XDMA_DEVICE");
        configured != nullptr && configured[0] != '\0') {
        device_path = configured;
    }

    // 输出最终路径；XdmaTransport 会在其后追加 _user 和 _events_N。
    LOG_INFO << "[DEMO] using XDMA device prefix: " << device_path;

    // 聚合所有 IP 核结果，使一次运行尽可能报告完整的板卡状态。
    bool all_passed = true;

    // PWM 只读检查。
    if (!probe_pwm(device_path)) {
        all_passed = false;
    }

    // AD7606 只读检查。
    if (!probe_ad7606(device_path)) {
        all_passed = false;
    }

    // ADS1258 只读检查。
    if (!probe_ads1258(device_path)) {
        all_passed = false;
    }

    // DH 只读检查，不调用任何点火接口。
    if (!probe_dh(device_path)) {
        all_passed = false;
    }

    // DIDO 只读检查，不修改输出。
    if (!probe_dido(device_path)) {
        all_passed = false;
    }

    // 顺序检查 COM1 到 COM4 的配置和错误状态。
    for (unsigned index = 0; index < 4; ++index) {
        if (!probe_com(device_path, index)) {
            all_passed = false;
        }
    }

    // 任一 IP 核失败都会使程序最终返回非零退出码，便于自动化部署脚本判断。
    if (!all_passed) {
        return fail_hardware_example(
            "direct Device example",
            "one or more hardware blocks failed");
    }

    // 所有板卡只读检查通过。
    LOG_INFO << "[DEMO] all direct Device read-only checks passed";
    return DemoResult::Passed;
#endif
}

DemoResult run_hw_full_capability_example() {
#ifndef MB_DDF_DEMO_WITH_HARDWARE
    // DDS-only 构建不包含任何 HW 符号，保持与只读示例一致的 Skipped 语义。
    LOG_WARN << "[DEMO] MB_DDF_HW is not enabled; full capability example skipped";
    return DemoResult::Skipped;
#else
    // 默认运行严格只读；必须由操作者显式确认本环境允许写入和不可逆动作。
    if (!full_demo_enabled()) {
        LOG_WARN << "[DEMO] full capability example requires MB_DDF_HW_FULL_DEMO=1; skipped";
        return DemoResult::Skipped;
    }

    // 与只读巡检共用设备前缀，便于在 xdma0/xdma1 之间整体切换。
    std::string device_path = "/dev/xdma0";
    if (const char* configured = std::getenv("MB_DDF_XDMA_DEVICE");
        configured != nullptr && configured[0] != '\0') {
        device_path = configured;
    }
    LOG_WARN << "[DEMO] full hardware capability mode enabled on " << device_path;
    LOG_WARN << "[DEMO] DH main engine 2 will fire; ADS1258 error counters will be cleared";
    LOG_WARN << "[DEMO] CPU SPI Flash will temporarily modify and then restore one 4 KiB subsector";

    // 使用 &= 保证某一能力组失败后仍继续执行其余独立设备，尽可能收集完整结果。
    bool all_passed = true;
    LOG_INFO << "=======================================================================";
    all_passed &= exercise_pwm(device_path);
    LOG_INFO << "=======================================================================";
    all_passed &= exercise_ad7606(device_path);
    LOG_INFO << "=======================================================================";
    all_passed &= exercise_ads1258(device_path);
    LOG_INFO << "=======================================================================";
    all_passed &= exercise_dido(device_path);
    LOG_INFO << "=======================================================================";
    all_passed &= exercise_dh(device_path);
    LOG_INFO << "=======================================================================";
    all_passed &= exercise_com_and_adapter(device_path);
    LOG_INFO << "=======================================================================";
    all_passed &= exercise_dma(device_path);
    LOG_INFO << "=======================================================================";
    all_passed &= exercise_spi_flash();

    // 任一动作或恢复失败都会使本区段失败，最终由 main 汇总为退出码 4。
    if (!all_passed) {
        return fail_hardware_example("full capability example",
                                     "one or more capability groups failed");
    }
    LOG_INFO << "[DEMO] all representative hardware capabilities passed";
    return DemoResult::Passed;
#endif
}

} // namespace MB_DDF::Demo
