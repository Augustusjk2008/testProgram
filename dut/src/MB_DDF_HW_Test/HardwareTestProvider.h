#pragma once

#include "MB_DDF_HW_Test/HardwareTestService.h"
#include "MB_DDF_HW_Test/SystemTestProvider.h"

#include <memory>

namespace MB_DDF::HWTest {

/// 真实板级 HW_TEST Provider；只在 HW_TEST 画像中编译。
class HardwareTestProvider final : public IHardwareTestProvider {
public:
    HardwareTestProvider();
    ~HardwareTestProvider() override;

    HardwareTestProvider(const HardwareTestProvider&) = delete;
    HardwareTestProvider& operator=(const HardwareTestProvider&) = delete;

    /// 启动 HW_TEST 画像并执行一次板级临时初始化。
    ProductErrorCode initialize();

    ProductErrorCode handle(const ProductMessage& request,
                            ProductMessage& response) override;
    ProductErrorCode begin_dh(const ProductMessage& request) override;
    ProductErrorCode handle_dh_control_report(const ProductMessage& request,
                                              ProductMessage& response,
                                              size_t report_index) override;
    bool helm_feedback_active() const override;
    ProductErrorCode build_helm_feedback(ProductMessage& response) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    SystemTestProvider system_;
};

} // namespace MB_DDF::HWTest
