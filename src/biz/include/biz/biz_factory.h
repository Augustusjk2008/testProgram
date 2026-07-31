#pragma once

// 包含业务层的全局定义头文件
// 该文件中通常包含 HWTEST_BIZ_EXPORT 等导出宏的定义
// HWTEST_BIZ_EXPORT 用于控制符号的可见性（在 Windows 上是 __declspec(dllexport/dllimport)）
#include "biz_global.h"

// 包含 Qt 框架的基础对象类
// QObject 是 Qt 对象模型的核心，提供了信号槽机制、对象树管理等功能
#include <QObject>

// 业务逻辑命名空间
// 使用嵌套命名空间 hwtest::biz 来组织代码，避免命名冲突
namespace hwtest::biz {

// 前置声明三个纯虚接口类（接口的具体定义在其他头文件中）
// 这种做法可以减少头文件依赖，提高编译速度

// 算法执行器接口
// 负责执行具体的测试算法逻辑
class IAlgorithmExecutor;

// 报告生成器接口
// 负责根据测试结果生成测试报告
class IReportGenerator;

// 测试运行服务接口
// 作为门面/协调者，管理整个测试流程的执行
class ITestRunService;

// ============================================================================
// 工厂函数声明 - 用于创建和销毁业务对象
// 采用工厂模式，将对象的创建与实现细节解耦
// ============================================================================

// ----------------------------------------------------------------------------
// TestRunService 相关工厂函数
// ----------------------------------------------------------------------------

// 创建 TestRunService 实例
// 参数：
//   executor    - 指向 IAlgorithmExecutor 实例的指针（非持有所有权）
//                 调用者负责保证 executor 的生命周期长于返回的 service 对象
//                 即：service 不会接管 executor 的所有权，也不会负责释放它
//   parent      - Qt 对象树中的父对象指针，默认为 nullptr
//                 如果提供了 parent，则 service 的生命周期将由 Qt 的对象树机制管理
//                 当 parent 被销毁时，service 也会被自动销毁
// 返回值：
//   指向新创建的 ITestRunService 接口的指针
//   调用者通过该指针对测试运行服务进行操作
HWTEST_BIZ_EXPORT ITestRunService* createTestRunService(IAlgorithmExecutor* executor,
                                                         QObject* parent = nullptr);

// 销毁 TestRunService 实例
// 参数：
//   service - 之前通过 createTestRunService 创建的实例指针
// 注意：
//   - 如果创建时传入了 parent，通常不需要手动调用此函数
//     （Qt 对象树会自动管理生命周期）
//   - 如果创建时未传入 parent，调用者必须调用此函数来释放资源
//   - 调用此函数后 service 指针将变为悬空指针，不应再使用
HWTEST_BIZ_EXPORT void destroyTestRunService(ITestRunService* service);

// ----------------------------------------------------------------------------
// ReportGenerator 相关工厂函数
// ----------------------------------------------------------------------------

// 创建 ReportGenerator 实例
// 参数：无
// 返回值：
//   指向新创建的 IReportGenerator 接口的指针
//   调用者通过该指针对报告生成器进行操作
// 注意：
//   返回的对象没有 parent，调用者需要在合适的时候调用 destroyReportGenerator
//   来手动释放资源
HWTEST_BIZ_EXPORT IReportGenerator* createReportGenerator();

// 销毁 ReportGenerator 实例
// 参数：
//   generator - 之前通过 createReportGenerator 创建的实例指针
// 注意：
//   调用此函数后 generator 指针将变为悬空指针，不应再使用
HWTEST_BIZ_EXPORT void destroyReportGenerator(IReportGenerator* generator);

} // namespace hwtest::biz