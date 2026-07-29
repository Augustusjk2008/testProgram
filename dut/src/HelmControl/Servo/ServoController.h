#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// NOLINT
class HelmControl {
public:
    HelmControl() = default;
    ~HelmControl() = default;

    // 反馈输出
    double fdb_out = 0.0;
    // 实际输出占空比
    int32_t pwm_out = 0;

    void update(int32_t fdb_data_in, double ins_in) {
        _assignInputs(fdb_data_in, ins_in);
        _fn_getFdb();
        _fn_generatePara();
        _fn_dataFilter();
        _fn_step();
        _fn_limitControlOnce();
        _iterateSequences();
    }

private:
    template <typename T>
    struct DynamicSequence {
        std::vector<T> data_;
        DynamicSequence() : data_(1, T{}) {}

        T& operator[](size_t n) {
            if (n >= data_.size()) {
                data_.resize(n + 1, T{});
            }
            return data_[n];
        }

        void shift() {
            for (size_t i = data_.size() - 1; i > 0; --i) {
                data_[i] = data_[i - 1];
            }
        }

        void zero() noexcept {
            std::fill(data_.begin(), data_.end(), T{});
        }
    };

    enum class FlightState {
        Step1
    };
    FlightState STATE = FlightState::Step1;

    // 占空比放大比例（1e8/22）
    const double Pwm_gain = 4545454.545454546;
    // 控制解算周期
    const double Ts = 0.00025;
    // 偏差绝对值
    double abs_err = 0.0;
    // 偏差微分
    DynamicSequence<double> derr;
    // 偏差微分滤波参数
    const std::vector<double> derrA = {0.1111, 0.1111, 0.7778};
    // 偏差微分滤波
    DynamicSequence<double> derrf;
    // PWM输出占空比限幅
    const double duty_max = 0.95;
    // 偏差
    DynamicSequence<double> err;
    // 小偏差范围设置
    const double err_small = 0.04;
    // 反馈
    DynamicSequence<double> fdb;
    // 反馈数字量
    int32_t fdb_data = 0;
    // 分段长度
    std::vector<double> gap = {0.0, 0.0};
    // 积分
    double ierr = 0.0;
    // 误差积分量上限
    const double ierr_max = 1.0;
    // 指令
    DynamicSequence<double> ins;
    // 指令滤波参数
    const std::vector<double> insA = {1.0, 0.0, 0.0, 0.0, 0.0};
    // 指令限幅
    const double ins_max = 20.5;
    // 指令滤波
    DynamicSequence<double> insf;
    // 参数分段设置
    const std::vector<double> inter = {0.2, 0.3, 0.9, 1.0};
    // 微分控制参数
    const std::vector<double> kd = {0.025, 0.045, 0.042};
    // 实际微分增益
    double kd_real = 0.0;
    // 积分控制参数
    const std::vector<double> ki = {0.0, 0.0, 0.0};
    // 实际积分增益
    double ki_real = 0.0;
    // 比例控制参数
    const std::vector<double> kp = {9.0, 12.0, 10.0};
    // 实际比例增益
    double kp_real = 0.0;
    // 小偏差下额外比例参数
    const double kp_small = 0.0;
    // 大电源电压
    const double power = 22.0;
    // PWM载波周期
    const uint32_t pwm_freq = 8000;
    // 微分增益斜率
    std::vector<double> sloped = {0.0, 0.0};
    // 积分增益斜率
    std::vector<double> slopei = {0.0, 0.0};
    // 比例增益斜率
    std::vector<double> slopep = {0.0, 0.0};
    // 比例、微分、积分输出
    std::vector<double> u = {0.0, 0.0, 0.0, 0.0};
    // 最终控制量
    double u_out = 0.0;
    // 小偏差下额外输出限幅
    const double u_small_max = 0.32;

    void _assignInputs(int32_t fdb_data_in, double ins_in) {
        // 反馈数字量
        fdb_data = fdb_data_in;
        // 指令
        ins[0] = ins_in;
    }

    // 反馈换算
    void _fn_getFdb() {
        // AD7606 原始反馈码 → 舵机实际角度的现行标定公式
        fdb[0] =
            (static_cast<double>(fdb_data) * 10.0 / 65535.0 - 2.048)
            * 3.0 * 115.0 / 20.0;
    }

    // 自动生成可变参数
    void _fn_generatePara() {
        gap[0] = inter[1]-inter[0];
        gap[1] = inter[3]-inter[2];
        slopep[0] = (kp[1] - kp[0]) / gap[0];
        slopep[1] = (kp[2] - kp[1]) / gap[1];
        sloped[0] = (kd[1] - kd[0]) / gap[0];
        sloped[1] = (kd[2] - kd[1]) / gap[1];
        slopei[0] = (ki[1] - ki[0]) / gap[0];
        slopei[1] = (ki[2] - ki[1]) / gap[1];
    }

    // 数据滤波
    void _fn_dataFilter() {
        // 指令限幅
        ins[0] = std::clamp(ins[0], -ins_max, ins_max);
        // 指令滤波
        insf[0] = insA[0]*ins[0] + insA[1]*ins[1] + insA[2]*ins[2] + insA[3]*insf[0] + insA[4]*insf[1];
    }

    // 单步控制函数
    void _fn_step() {
        // 计算偏差
        err[0] = ins[0] - fdb[0];
        // 计算微分
        derr[0] = err[0] - err[1];
        // 偏差微分滤波
        derrf[0] = derrA[0]*derr[0] + derrA[1]*derr[1] + derrA[2]*derrf[0];
        // 计算积分并限幅
        ierr = std::clamp(ierr+err[0], -ierr_max, ierr_max);
        abs_err = std::abs(err[0]);
        // 计算分段 kp 参数
        if (abs_err < inter[0]) {
            kp_real = kp[0];
        }
        else if (abs_err < inter[1]) {
            kp_real = kp[0] + (abs_err - inter[0]) * slopep[0];
        }
        else if (abs_err < inter[2]) {
            kp_real = kp[1];
        }
        else if (abs_err < inter[3]) {
            kp_real = kp[1] + (abs_err - inter[2]) * slopep[1];
        }
        else {
            kp_real = kp[2];
        }
        // 计算分段 kd 参数
        if (abs_err < inter[0]) {
            kd_real = kd[0];
        }
        else if (abs_err < inter[1]) {
            kd_real = kd[0] + (abs_err - inter[0]) * sloped[0];
        }
        else if (abs_err < inter[2]) {
            kd_real = kd[1];
        }
        else if (abs_err < inter[3]) {
            kd_real = kd[1] + (abs_err - inter[2]) * sloped[1];
        }
        else {
            kd_real = kd[2];
        }
        // 计算分段 ki 参数
        if (abs_err < inter[0]) {
            ki_real = ki[0];
        }
        else if (abs_err < inter[1]) {
            ki_real = ki[0] + (abs_err - inter[0]) * slopei[0];
        }
        else if (abs_err < inter[2]) {
            ki_real = ki[1];
        }
        else if (abs_err < inter[3]) {
            ki_real = ki[1] + (abs_err - inter[2]) * slopei[1];
        }
        else {
            ki_real = ki[2];
        }
        // 比例输出
        u[0] = kp_real * err[0];
        // 微分输出
        u[1] = kd_real * derrf[0] / Ts;
        // 积分输出
        u[2] = ki_real * ierr * Ts;
        // 小偏差额外输出
        if (abs_err < err_small) {
            u[3] = std::clamp(kp_small * err[0], -u_small_max, u_small_max);
        } else {
            u[3] = 0;
        }
        // 总输出
        u_out = u[0] + u[1] +u[2] +u[3];
    }

    // 输出及限幅
    void _fn_limitControlOnce() {
        u_out = std::clamp(u_out, -power * duty_max, power * duty_max);
        // 指令转换后输出
        pwm_out = u_out * Pwm_gain;
        // 反馈输出
        fdb_out = fdb[0];
    }

    void _iterateSequences() {
        // 偏差微分
        derr.shift();
        // 偏差微分滤波
        derrf.shift();
        // 偏差
        err.shift();
        // 反馈
        fdb.shift();
        // 指令
        ins.shift();
        // 指令滤波
        insf.shift();
    }
};
