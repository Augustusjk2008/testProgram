#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <memory>

namespace MB_DDF {
namespace Tools {

// 自描述日志系统 - 用于高效序列化和存储结构化数据的二进制日志
// 支持任意结构体、数组、位字段，并包含模式信息实现自描述
// 主要特点：
// 1. 支持多种数据类型（8/16/32/64位有符号/无符号整数、32/64位浮点数）
// 2. 支持数组和位字段
// 3. 自动包含模式信息，无需单独的模式文件
// 4. 使用小端字节序，便于跨平台使用
// 5. 支持写入时的数值缩放和偏移

// 日志字段类型枚举 - 定义支持的所有数据类型
// 每个枚举值对应一个特定的数据类型
enum class LogFieldType : uint8_t {
    UInt8,
    UInt16,
    UInt32,
    UInt64,
    Int8,
    Int16,
    Int32,
    Int64,
    Float32,
    Float64
};

// 模式中的一个字段定义
// count: 数组的元素数量；标量则为1
// scale/offset: 读取器的元数据（值 * 比例因子 + 偏移量），写入器不应用它们
// byte_offset/byte_stride: 在appendRecord()中源结构体内的位置
// storage_type/bit_offset/bit_width: 从无符号存储类型中可选提取位字段
struct LogField {
    std::string name;
    LogFieldType type;
    uint32_t count;
    double scale;
    double offset;
    uint32_t byte_offset;
    uint32_t byte_stride;
    LogFieldType storage_type;
    uint8_t bit_offset;
    uint8_t bit_width;
};

// 日志模式类 - 定义日志数据的结构和布局
// 管理所有字段的定义，包括类型、位置、数量等元数据
class LogSchema {
public:
    // 无效偏移量常量 - 用于表示未定义的字节偏移
    static constexpr uint32_t kInvalidOffset = 0xFFFFFFFFu;

    // 添加具有隐式布局的字段：appendRecord()无法使用它
    // 这主要用于仅包含模式元数据
    bool addField(const std::string& name,
                  LogFieldType type,
                  uint32_t count = 1,
                  double scale = 1.0,
                  double offset = 0.0);
    // 在源结构体中添加具有显式布局的字段
    // byte_offset: 第一个元素的偏移量
    // byte_stride: 元素之间的字节数；0表示按类型大小连续
    bool addFieldAt(const std::string& name,
                    LogFieldType type,
                    uint32_t count,
                    uint32_t byte_offset,
                    uint32_t byte_stride = 0,
                    double scale = 1.0,
                    double offset = 0.0);
    // 添加从无符号存储类型中提取的位字段
    // bit_offset/bit_width在存储类型中从最低有效位(LSB)开始
    // output_type必须是适合位宽度的无符号整数类型
    bool addBitFieldAt(const std::string& name,
                       LogFieldType storage_type,
                       uint8_t bit_offset,
                       uint8_t bit_width,
                       uint32_t byte_offset,
                       uint32_t byte_stride = 0,
                       LogFieldType output_type = LogFieldType::UInt8,
                       double scale = 1.0,
                       double offset = 0.0);

      // 获取所有字段定义的常量引用
    const std::vector<LogField>& fields() const;
    // 计算记录的总大小（所有字段大小之和）
    uint32_t recordSize() const;
    // 计算序列化后的模式大小（写入文件的大小）
    uint32_t serializedSize() const;
    // 检查模式是否有效（至少有一个有效字段）
    bool isValid() const;
    // 将模式序列化为二进制数据（用于写入文件）
    std::vector<uint8_t> serialize() const;

private:
    std::vector<LogField> fields_;
};

// 记录构建器类 - 用于构建固定大小的二进制记录
// 提供各种数据类型的写入方法，并确保数据按预期布局
class LogRecordBuilder {
public:
    // 构造函数 - 创建指定大小的记录缓冲区
    explicit LogRecordBuilder(uint32_t record_size);

    // 将写入位置重置到记录缓冲区的开头
    void reset();
    bool writeUInt8(uint8_t value);
    bool writeUInt16(uint16_t value);
    bool writeUInt32(uint32_t value);
    bool writeUInt64(uint64_t value);
    bool writeInt8(int8_t value);
    bool writeInt16(int16_t value);
    bool writeInt32(int32_t value);
    bool writeInt64(int64_t value);
    bool writeFloat(float value);
    bool writeDouble(double value);
    bool writeBytes(const void* data, uint32_t size);

    // 已写入的字节数；容量固定为record_size
    uint32_t bytesWritten() const;
    uint32_t capacity() const;
    bool isComplete() const;
    // 获取记录数据的常量引用
    const std::vector<uint8_t>& data() const;

private:
    bool writeLe16(uint16_t value);
    bool writeLe32(uint32_t value);
    bool writeLe64(uint64_t value);
    bool reserve(uint32_t size);

    std::vector<uint8_t> buffer_;
    uint32_t pos_;
};

// 自描述日志写入器类 - 负责将结构化数据写入自描述日志文件
// 处理文件的创建、验证、模式检查和记录追加
class SelfDescribingLogWriter {
public:
    // 构造函数 - 初始化写入器状态
    SelfDescribingLogWriter();
    // 析构函数 - 确保文件被正确关闭
    ~SelfDescribingLogWriter();

    // 打开或创建一个包含模式头和模式字节的日志文件
    // 如果文件存在，追加前会验证头部和模式
    bool open(const std::string& path, const LogSchema& schema);
    // 关闭文件并释放资源
    void close();
    // 检查文件是否已打开
    bool isOpen() const;
    // 获取单条记录的大小（字节）
    uint32_t recordSize() const;

    // 根据模式序列化结构体实例并追加一条记录
    // struct_size必须是sizeof(T)用于边界检查
    bool appendRecord(const void* struct_data, uint32_t struct_size);
    // 追加一条预构建的、大小正好为recordSize()字节的记录缓冲区
    bool appendRawRecord(const uint8_t* data, uint32_t size);
    bool appendRawRecord(const std::vector<uint8_t>& data);
    // 将缓冲区数据写入磁盘，确保数据持久化
    void flush();

private:
    // 创建新文件并写入模式头和模式数据
    bool writeNewFile(const std::string& path,
                      const std::vector<uint8_t>& schema_bytes,
                      uint32_t record_size);
    // 验证现有文件的头部和模式是否匹配期望值
    bool validateExistingFile(const std::string& path,
                              const std::vector<uint8_t>& expected_schema,
                              uint32_t record_size);

    std::ofstream out_;
    uint32_t record_size_;
    std::vector<LogField> fields_;
    std::unique_ptr<LogRecordBuilder> record_builder_;
};

// 获取日志字段类型的大小（以字节为单位）
// 对于不支持的类型返回0
uint32_t logFieldTypeSize(LogFieldType type);

} // namespace Tools
} // namespace MB_DDF
