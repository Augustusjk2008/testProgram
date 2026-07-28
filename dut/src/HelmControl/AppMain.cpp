#include "MB_DDF/Debug/Logger.h"
#include "MB_DDF/DDS/DDSCore.h"
#include "MB_DDF/Timer/SystemTimer.h"
#include "MB_DDF/Timer/ChronoHelper.h"

// ─── 新版硬件层 ───
#include "MB_DDF_HW/Transport/XdmaTransport.h"
#include "MB_DDF_HW/Device/PwmDevice.h"
#include "MB_DDF_HW/Device/Ad7606Device.h"

// ─── 舵机控制 ───
#include "HelmControl/Servo/ServoController.h"
#include "HelmControl/ProtocolModel/helm_ins_frame_protocol.h"
#include "HelmControl/ProtocolModel/helm_fdb_frame_protocol.h"

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstdlib>  // std::abs
#include <mutex>

// 版本号
const char sast_app_version[] __attribute__((section(".myversion"), used)) = "1.0.0";

// 舵机程序分配的核心号
#define SERVO_CORE 7

/*
 * 程序入口
 */
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    LOG_INFO << "HelmControl version: " << sast_app_version;

    int16_t ad_data[4] = {0, 0, 0, 0};
    int32_t pwm_data[4] = {0, 0, 0, 0};
    ProtocolModel::Helm_ins_frame ins_frame = {};
    ProtocolModel::Helm_fdb_frame fdb_frame = {};
    bool ins_pending = false;
    std::mutex ins_mutex;

    // ─── 初始化硬件层 ──────────────────────────────────────

    // 连 FPGA：PWM 在 0x00000，AD7606 在 0x10000
    using namespace MB_DDF::HW;

    XdmaTransport pwm_transport({"/dev/xdma0", 0x00000, 0x10000});
    XdmaTransport adc_transport({"/dev/xdma0", 0x10000, 0x10000});

    {
        auto pwm_open_result = pwm_transport.open();
        if (!pwm_open_result) {
            LOG_ERROR << "PWM transport open failed: " << pwm_open_result.status().message;
            return 1;
        }
    }
    {
        auto adc_open_result = adc_transport.open();
        if (!adc_open_result) {
            LOG_ERROR << "ADC transport open failed: " << adc_open_result.status().message;
            return 1;
        }
    }

    PwmDevice pwm(pwm_transport);
    Ad7606Device adc(adc_transport);

    // 验证硬件通信
    {
        auto pwm_comm_result = pwm.check_communication();
        if (!pwm_comm_result) {
            LOG_ERROR << "PWM communication check failed";
            return 1;
        }
    }
    {
        auto adc_comm_result = adc.check_communication();
        if (!adc_comm_result) {
            LOG_ERROR << "ADC communication check failed";
            return 1;
        }
    }

    // ─── PWM 寄存器默认值（硬件自带，无需手动配）───
    //   载波频率 = 4000Hz        （寄存器 0xB*4）
    //   波峰值   = 100000000     （寄存器 0xC*4）
    //   波形模式 = 锯齿波         （寄存器 0xD*4 / 16'hA001）
    //   占空比模式 = 有符号模式   （寄存器 0xE*4 / 16'hAAAA）
    //
    // 修改配置：
    // PwmConfig pwm_cfg = {
    //     .carrier_frequency_value = 8000,       // 载波频率
    //     .peak_value = 100000000,               // 波峰值
    //     .waveform = PwmWaveform::Sawtooth      // 波形模式
    // };
    // pwm.configure(pwm_cfg);

    // 切换到无符号模式（apply_normalized_outputs / apply_outputs 使用方向寄存器时需要）
    {
        auto mode_result = pwm.set_duty_mode_unsigned();
        if (!mode_result) {
            LOG_ERROR << "PWM set_duty_mode_unsigned failed";
            return 1;
        }
    }

    // 开启更新使能（默认关闭，必须打开）
    {
        auto update_result = pwm.set_update_enabled(true);
        if (!update_result) {
            LOG_ERROR << "PWM set_update_enabled failed";
            return 1;
        }
    }

    // 配置 AD7606：使能采集 + 开滤波
    {
        Ad7606Config adc_cfg = {
            .acquisition_enabled = true,
            .filter_enabled = true,
        };
        auto adc_cfg_result = adc.configure(adc_cfg);
        if (!adc_cfg_result) {
            LOG_ERROR << "ADC configure failed";
            return 1;
        }
    }

    LOG_INFO << "Hardware initialized: PWM@4000Hz, ADC filter=on";

    // ─── 初始化 DDS ──────────────────────────────────────────────
    auto& dds = MB_DDF::DDS::DDSCore::instance();
    if (!dds.initialize()) {
        LOG_ERROR << "DDS initialization failed";
        return 1;
    }

    auto ins_reader = dds.create_reader("local:://helm_command", false,
    [&](const void* data, size_t size, uint64_t timestamp){
        (void)timestamp;
        ProtocolModel::Helm_ins_frame candidate{};
        if (!ProtocolModel::Helm_ins_frameProtocol::unpackFrame(
                static_cast<const char*>(data), size, candidate)) {
            LOG_WARN << "Ignored malformed helm command payload: " << size;
            return;
        }
        std::lock_guard<std::mutex> lock(ins_mutex);
        ins_frame = candidate;
        ins_pending = true;
    });
    auto feedback_writer = dds.create_writer("local:://helm_feedback", false);
    if (!ins_reader || !feedback_writer) {
        LOG_ERROR << "Cannot create helm DDS endpoints";
        return 1;
    }

    HelmControl controller[4];

    // ─── 定时器 ──────────────────────────────────────────────────
    sem_t helm_sem;
    sem_init(&helm_sem, 0, 0);

    MB_DDF::Timer::SystemTimerOptions opt_helm;
    opt_helm.sched_policy = SCHED_FIFO;
    opt_helm.priority = sched_get_priority_max(SCHED_FIFO);
    opt_helm.cpu = SERVO_CORE;
    opt_helm.signal_no = SIGRTMIN;
    opt_helm.user_data = &helm_sem;

    auto helm_timer
    = MB_DDF::Timer::SystemTimer::start("250us"
        , [](void* para) {
            sem_post((sem_t*)para);
            MB_DDF::Timer::ChronoHelper::record(0, 250);  // 期望周期 250μs
        }
        , opt_helm);
    if (!helm_timer) {
        LOG_ERROR << "Cannot start 250 us helm control timer";
        return 1;
    }

    MB_DDF::Timer::SystemTimer::configureThread(
        pthread_self(), SCHED_RR, 98, SERVO_CORE);

    // ─── 控制循环 ────────────────────────────────────────────────
    fdb_frame.bitGroup1.self_check = 0x3;
    while (1) {
        // 0. 等待下一个控制周期
        sem_wait(&helm_sem);

        ProtocolModel::Helm_ins_frame cycle_ins{};
        bool feedback_due = false;
        {
            std::lock_guard<std::mutex> lock(ins_mutex);
            cycle_ins = ins_frame;
            feedback_due = ins_pending;
            ins_pending = false;
        }

        // 1. 读取 AD 反馈（4 路舵机）
        auto snapshot = adc.read_snapshot();
        if (snapshot) {
            for (size_t i = 0; i < 4; ++i) {
                // AD7606 通道 0~3 = 舵 1~4 反馈，低 16 位有符号
                ad_data[i] = snapshot.value().raw[i];
            }
        } else {
            LOG_WARN << "ADC read_snapshot failed";
        }

        // 2. 控制计算
        for (size_t i = 0; i < 4; ++i) {
            controller[i].update(ad_data[i], cycle_ins.ins[i]);
            pwm_data[i] = controller[i].pwm_out;
        }

        // 3. 输出 PWM 占空比
        {
            using namespace MB_DDF::HW;
            PwmRawOutputs raw_outputs = {};
            raw_outputs.enable_mask = 0x0F;
            for (size_t i = 0; i < 4; ++i) {
                // 有符号 → 方向 + 绝对值（无符号模式）
                raw_outputs.direction_mask |= (pwm_data[i] >= 0 ? 1u : 0u) << i;
                raw_outputs.duty[i] = static_cast<uint32_t>(
                    pwm_data[i] >= 0 ? pwm_data[i] : -pwm_data[i]);
            }
            auto pwm_output_result = pwm.apply_outputs(raw_outputs);
            if (!pwm_output_result) {
                LOG_WARN << "PWM apply_outputs failed";
            }
        }

        // 4. 返回反馈信号
        if (!feedback_due) {
            continue;
        }

        static int dd = 0;
        if ((dd == 0) && (cycle_ins.plug_detach == 0xFF)) {
            fdb_frame.serial_b = 0;
            dd = 1;
        } else {
            fdb_frame.serial_b += 1;
        }
        fdb_frame.serial_a = cycle_ins.serial_a;
        uint16_t bit_set_ok = 3;
        for (uint8_t i = 0; i < 4; ++i) {
            fdb_frame.ins[i] = cycle_ins.ins[i];
            fdb_frame.fdb[i] = controller[i].fdb_out;
            if (fabs(fdb_frame.ins[i] - fdb_frame.fdb[i]) > 1.5) {
                if (i == 0)      fdb_frame.bitGroup1.bit1 = 0;
                else if (i == 1) fdb_frame.bitGroup1.bit2 = 0;
                else if (i == 2) fdb_frame.bitGroup1.bit3 = 0;
                else if (i == 3) fdb_frame.bitGroup1.bit4 = 0;
                bit_set_ok = 0;
            } else {
                if (i == 0)      fdb_frame.bitGroup1.bit1 = 3;
                else if (i == 1) fdb_frame.bitGroup1.bit2 = 3;
                else if (i == 2) fdb_frame.bitGroup1.bit3 = 3;
                else if (i == 3) fdb_frame.bitGroup1.bit4 = 3;
            }
        }
        fdb_frame.bitGroup1.bit = bit_set_ok;
        auto fdb_frame_buffer = ProtocolModel::Helm_fdb_frameProtocol::packFrame(fdb_frame);
        feedback_writer->write(fdb_frame_buffer.data(), fdb_frame_buffer.size());
        fdb_frame.timeout = 0;
    }

    return 0;
}
