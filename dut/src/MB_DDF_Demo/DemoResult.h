#pragma once

namespace MB_DDF::Demo {

/**
 * @brief 单个示例的执行结果。
 *
 * Passed 表示示例完成且校验通过。
 * Skipped 表示当前构建未启用示例所需的可选模块。
 * Failed 表示示例已执行，但某个调用或结果校验失败。
 */
enum class DemoResult {
    Passed,
    Skipped,
    Failed,
};

} // namespace MB_DDF::Demo
