// 包含本模块对应的工厂函数头文件
// 该头文件中声明了 createTestRunService、destroyTestRunService 等工厂函数接口
#include <biz/biz_factory.h>

// 包含业务层接口的头文件
// 这些头文件中定义了 IReportGenerator 和 ITestRunService 的纯虚接口
#include <biz/i_report_generator.h>
#include <biz/i_test_run_service.h>

// 业务逻辑命名空间
namespace hwtest::biz {

// ============================================================================
// 前置声明：具体实现类的创建函数（定义在其他 .cpp 文件中）
// 此处只是声明，目的是将接口工厂与具体实现解耦
// ============================================================================

// 创建 ReportGenerator 具体实现类的实例
// 该函数在另一个编译单元（.cpp 文件）中定义
// 返回值：
//   指向 IReportGenerator 接口的指针，实际指向的是具体的实现类对象
IReportGenerator* createReportGeneratorImplementation();

// 创建 TestRunService 具体实现类的实例
// 该函数在另一个编译单元（.cpp 文件）中定义
// 参数：
//   executor - 算法执行器指针（非持有所有权）
//   parent   - Qt 对象树的父对象指针
// 返回值：
//   指向 ITestRunService 接口的指针，实际指向的是具体的实现类对象
ITestRunService* createTestRunServiceImplementation(IAlgorithmExecutor* executor, QObject* parent);

// ============================================================================
// 对外暴露的工厂函数实现
// 这些函数作为库的公共 API，内部委托给具体实现类的创建函数
// ============================================================================

// ----------------------------------------------------------------------------
// TestRunService 工厂函数实现
// ----------------------------------------------------------------------------

// 创建测试运行服务（对外 API）
// 该函数只是一个薄薄的委托层，将调用转发给 createTestRunServiceImplementation
// 设计意图：
//   - 头文件 biz_factory.h 对外只暴露工厂函数，不暴露实现细节
//   - 具体实现类的创建逻辑封装在 createTestRunServiceImplementation 中
//   - 未来如需更改创建逻辑（如添加缓存、单例等），只需修改实现函数即可
ITestRunService* createTestRunService(IAlgorithmExecutor* executor, QObject* parent)
{
    // 调用具体实现类的创建函数，并直接返回结果
    return createTestRunServiceImplementation(executor, parent);
}

// 销毁测试运行服务（对外 API）
// 使用 C++ 标准的 delete 操作符销毁对象
// 注意事项：
//   - 如果 service 在创建时有 Qt 的 parent，Qt 对象树会自动管理其生命周期
//     此时手动调用本函数可能导致 double free（两次释放同一块内存）
//     调用者需要确保了解对象的生命周期管理方式
//   - delete 会自动调用对象的析构函数，完成资源清理
//   - 执行后 service 指针将变为悬空指针，调用者不应再使用它
void destroyTestRunService(ITestRunService* service)
{
    delete service;
}

// ----------------------------------------------------------------------------
// ReportGenerator 工厂函数实现
// ----------------------------------------------------------------------------

// 创建报告生成器（对外 API）
// 该函数只是一个薄薄的委托层，将调用转发给 createReportGeneratorImplementation
// 设计意图：
//   - 隔离接口与实现，外部代码只知道 IReportGenerator 接口
//   - 具体由哪个类实现 IReportGenerator 完全由内部决定
//   - 方便后续扩展（例如根据配置选择不同的实现类）
IReportGenerator* createReportGenerator()
{
    // 调用具体实现类的创建函数，并直接返回结果
    return createReportGeneratorImplementation();
}

// 销毁报告生成器（对外 API）
// 使用 C++ 标准的 delete 操作符销毁对象
// 注意事项：
//   - 与 destroyTestRunService 不同，createReportGenerator 不接受 parent 参数
//     因此报告生成器不会纳入 Qt 对象树管理，调用者必须手动调用本函数来释放
//   - delete 会自动调用对象的析构函数，完成资源清理
//   - 执行后 generator 指针将变为悬空指针，调用者不应再使用它
void destroyReportGenerator(IReportGenerator* generator)
{
    delete generator;
}

} // namespace hwtest::biz