#include "MB_DDF/Debug/Logger.h"

#if defined(MB_DDF_SOURCE_DEBUG)
#include "MB_DDF_DEBUG/DytDebug.h"
#elif defined(MB_DDF_APP_HW_TEST)
#include "MB_DDF_HW_Test/HardwareTestService.h"
#elif defined(MB_DDF_APP_DEMO)
#include "MB_DDF_Demo/DemoRunner.h"
#endif

#include <exception>

int main() {
    LOG_SET_LEVEL_INFO();
    LOG_DISABLE_TIMESTAMP();
    LOG_DISABLE_FUNCTION_LINE();

    try {
#if defined(MB_DDF_SOURCE_DEBUG)
        // 导引头调试：COM1 持续接收 B 帧、记录日志并按 4:1 回发 A 帧。
        LOG_INFO << "DYT 调试";
        return MB_DDF::DytDebug::run_dyt_debug();
#elif defined(MB_DDF_APP_HW_TEST)
        return MB_DDF::HWTest::run_hardware_test_service();
#elif defined(MB_DDF_APP_DEMO)
        return MB_DDF::Demo::run_demo();
#endif
    } catch (const std::exception& exception) {
        LOG_ERROR << "[APP] 主入口发生异常：" << exception.what();
        return 2;
    } catch (...) {
        LOG_ERROR << "[APP] 主入口发生未知异常";
        return 3;
    }
}
