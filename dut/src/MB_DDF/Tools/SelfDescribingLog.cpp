#include "SelfDescribingLog.h"

#include <array>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>

namespace MB_DDF {
namespace Tools {

// 匿名命名空间 - 内部实现细节和常量定义
namespace {

// 文件格式（小端字节序）：
// 头部: magic(4) + 版本(u16) + 字节序(u8) + 保留(u8) + 模式大小(u32) + 记录大小(u32)
// 模式: 字段数量(u32) + 重复的字段条目
// 字段条目: 名称长度(u16) + 名称(字节) + 类型(u8) + 保留(u8) + 数量(u32) + 比例因子(f64) + 偏移量(f64)
// 记录: 固定大小的二进制记录，大小为record_size字节，按顺序追加

// 文件签名常量 - 用于标识文件类型
constexpr char kMagic[4] = {'S', 'L', 'O', 'G'};
// 文件格式版本号 - 用于向后兼容
constexpr uint16_t kVersion = 1;
// 小端字节序标识符 - 确保跨平台一致性
constexpr uint8_t kEndianLittle = 1;
// 头部固定大小 - 16字节
constexpr uint32_t kHeaderSize = 16;

// 以小端序写入16位值到输出向量
void writeLe16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

// 以小端序写入32位值到输出向量
void writeLe32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

// 以小端序写入64位值到输出向量
void writeLe64(std::vector<uint8_t>& out, uint64_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 32) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 40) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 48) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 56) & 0xFF));
}

// 以小端序写入双精度浮点数到输出向量
// 通过内存复制将double转换为64位整数，然后按小端序写入
void writeLeDouble(std::vector<uint8_t>& out, double value) {
    uint64_t bits = 0;
    static_assert(sizeof(double) == sizeof(uint64_t), "double size must be 8 bytes");
    std::memcpy(&bits, &value, sizeof(bits));
    writeLe64(out, bits);
}

// 从输入数据中以小端序读取16位值
uint16_t readLe16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8);
}

// 从输入数据中以小端序读取32位值
uint32_t readLe32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

// 构建文件头部数组
// 将模式大小和记录大小按小端序打包到头部数组中
std::array<uint8_t, kHeaderSize> buildHeader(uint32_t schema_size, uint32_t record_size) {
    std::array<uint8_t, kHeaderSize> header{};
    header[0] = static_cast<uint8_t>(kMagic[0]);
    header[1] = static_cast<uint8_t>(kMagic[1]);
    header[2] = static_cast<uint8_t>(kMagic[2]);
    header[3] = static_cast<uint8_t>(kMagic[3]);
    header[4] = static_cast<uint8_t>(kVersion & 0xFF);
    header[5] = static_cast<uint8_t>((kVersion >> 8) & 0xFF);
    header[6] = kEndianLittle;
    header[7] = 0;
    header[8] = static_cast<uint8_t>(schema_size & 0xFF);
    header[9] = static_cast<uint8_t>((schema_size >> 8) & 0xFF);
    header[10] = static_cast<uint8_t>((schema_size >> 16) & 0xFF);
    header[11] = static_cast<uint8_t>((schema_size >> 24) & 0xFF);
    header[12] = static_cast<uint8_t>(record_size & 0xFF);
    header[13] = static_cast<uint8_t>((record_size >> 8) & 0xFF);
    header[14] = static_cast<uint8_t>((record_size >> 16) & 0xFF);
    header[15] = static_cast<uint8_t>((record_size >> 24) & 0xFF);
    return header;
}

} // namespace 匿名命名空间结束

// 获取日志字段类型的大小（以字节为单位）
// 对于不支持的类型返回0
uint32_t logFieldTypeSize(LogFieldType type) {
    switch (type) {
    case LogFieldType::UInt8:
    case LogFieldType::Int8:
        return 1;
    case LogFieldType::UInt16:
    case LogFieldType::Int16:
        return 2;
    case LogFieldType::UInt32:
    case LogFieldType::Int32:
    case LogFieldType::Float32:
        return 4;
    case LogFieldType::UInt64:
    case LogFieldType::Int64:
    case LogFieldType::Float64:
        return 8;
    default:
        return 0;
    }
}

// LogSchema::addField - 添加具有隐式布局的字段
// 字段不包含字节偏移信息，主要用于元数据
bool LogSchema::addField(const std::string& name,
                         LogFieldType type,
                         uint32_t count,
                         double scale,
                         double offset) {
    if (name.empty() || name.size() > std::numeric_limits<uint16_t>::max()) {
        return false;
    }
    if (count == 0 || logFieldTypeSize(type) == 0) {
        return false;
    }
    fields_.push_back({name,
                       type,
                       count,
                       scale,
                       offset,
                       LogSchema::kInvalidOffset,
                       0,
                       type,
                       0,
                       0});
    return true;
}

// LogSchema::addFieldAt - 在源结构体中添加具有显式布局的字段
// 允许指定字段在结构体中的位置和间距
bool LogSchema::addFieldAt(const std::string& name,
                           LogFieldType type,
                           uint32_t count,
                           uint32_t byte_offset,
                           uint32_t byte_stride,
                           double scale,
                           double offset) {
    if (!addField(name, type, count, scale, offset)) {
        return false;
    }
    fields_.back().byte_offset = byte_offset;
    fields_.back().byte_stride = byte_stride;
    return true;
}

// LogSchema::addBitFieldAt - 添加从无符号存储类型中提取的位字段
// 允许从更大的无符号类型中提取指定位宽度的字段
bool LogSchema::addBitFieldAt(const std::string& name,
                              LogFieldType storage_type,
                              uint8_t bit_offset,
                              uint8_t bit_width,
                              uint32_t byte_offset,
                              uint32_t byte_stride,
                              LogFieldType output_type,
                              double scale,
                              double offset) {
    auto is_unsigned = [](LogFieldType type) {
        return type == LogFieldType::UInt8 ||
            type == LogFieldType::UInt16 ||
            type == LogFieldType::UInt32 ||
            type == LogFieldType::UInt64;
    };
    if (bit_width == 0) {
        return false;
    }
    if (!is_unsigned(storage_type) || !is_unsigned(output_type)) {
        return false;
    }
    const uint32_t storage_size = logFieldTypeSize(storage_type);
    const uint32_t output_size = logFieldTypeSize(output_type);
    if (storage_size == 0 || output_size == 0) {
        return false;
    }
    const uint32_t storage_bits = storage_size * 8;
    if (static_cast<uint32_t>(bit_offset) + static_cast<uint32_t>(bit_width) > storage_bits) {
        return false;
    }
    if (!addField(name, output_type, 1, scale, offset)) {
        return false;
    }
    fields_.back().byte_offset = byte_offset;
    fields_.back().byte_stride = byte_stride;
    fields_.back().storage_type = storage_type;
    fields_.back().bit_offset = bit_offset;
    fields_.back().bit_width = bit_width;
    return true;
}

// LogSchema::fields - 获取所有字段定义的常量引用
const std::vector<LogField>& LogSchema::fields() const {
    return fields_;
}

// LogSchema::recordSize - 计算记录的总大小
// 计算所有字段在内存中的总占用空间
uint32_t LogSchema::recordSize() const {
    uint64_t total = 0;
    for (const auto& field : fields_) {
        total += static_cast<uint64_t>(logFieldTypeSize(field.type)) * field.count;
        if (total > std::numeric_limits<uint32_t>::max()) {
            return 0;
        }
    }
    return static_cast<uint32_t>(total);
}

// LogSchema::serializedSize - 计算序列化后的模式大小
// 计算模式数据写入文件时占用的字节数
uint32_t LogSchema::serializedSize() const {
    uint64_t total = sizeof(uint32_t);
    for (const auto& field : fields_) {
        total += sizeof(uint16_t);
        total += field.name.size();
        total += sizeof(uint8_t);
        total += sizeof(uint8_t);
        total += sizeof(uint32_t);
        total += sizeof(double);
        total += sizeof(double);
        if (total > std::numeric_limits<uint32_t>::max()) {
            return 0;
        }
    }
    return static_cast<uint32_t>(total);
}

// LogSchema::isValid - 检查模式是否有效
// 模式必须包含至少一个有效字段，且记录大小和序列化大小必须大于0
bool LogSchema::isValid() const {
    return !fields_.empty() && recordSize() > 0 && serializedSize() > 0;
}

// LogSchema::serialize - 将模式序列化为二进制数据
// 只序列化元数据；字节偏移/位字段信息不写入文件
std::vector<uint8_t> LogSchema::serialize() const {
    if (!isValid()) {
        return {};
    }
    // Serialize only metadata; byte offsets/bitfields are not written to file.
    const uint32_t size = serializedSize();
    std::vector<uint8_t> out;
    out.reserve(size);
    writeLe32(out, static_cast<uint32_t>(fields_.size()));
    for (const auto& field : fields_) {
        const uint16_t name_len = static_cast<uint16_t>(field.name.size());
        writeLe16(out, name_len);
        out.insert(out.end(), field.name.begin(), field.name.end());
        out.push_back(static_cast<uint8_t>(field.type));
        out.push_back(0);
        writeLe32(out, field.count);
        writeLeDouble(out, field.scale);
        writeLeDouble(out, field.offset);
    }
    return out;
}

// LogRecordBuilder::LogRecordBuilder - 构造函数
// 初始化指定大小的记录缓冲区，并将写入位置设为0
LogRecordBuilder::LogRecordBuilder(uint32_t record_size)
    : buffer_(record_size, 0), pos_(0) {}

// LogRecordBuilder::reset - 重置记录构建器
// 将写入位置重置到开头，并将缓冲区清零
void LogRecordBuilder::reset() {
    pos_ = 0;
    std::fill(buffer_.begin(), buffer_.end(), 0);
}

// LogRecordBuilder::writeUInt8 - 写入8位无符号整数
// 向记录缓冲区写入一个uint8_t值
bool LogRecordBuilder::writeUInt8(uint8_t value) {
    return writeBytes(&value, sizeof(value));
}

// LogRecordBuilder::writeUInt16 - 写入16位无符号整数（小端序）
// 向记录缓冲区写入一个uint16_t值，以小端序存储
bool LogRecordBuilder::writeUInt16(uint16_t value) {
    return writeLe16(value);
}

// LogRecordBuilder::writeUInt32 - 写入32位无符号整数（小端序）
// 向记录缓冲区写入一个uint32_t值，以小端序存储
bool LogRecordBuilder::writeUInt32(uint32_t value) {
    return writeLe32(value);
}

// LogRecordBuilder::writeUInt64 - 写入64位无符号整数（小端序）
// 向记录缓冲区写入一个uint64_t值，以小端序存储
bool LogRecordBuilder::writeUInt64(uint64_t value) {
    return writeLe64(value);
}

// LogRecordBuilder::writeInt8 - 写入8位有符号整数
// 向记录缓冲区写入一个int8_t值
bool LogRecordBuilder::writeInt8(int8_t value) {
    return writeBytes(&value, sizeof(value));
}

// LogRecordBuilder::writeInt16 - 写入16位有符号整数（小端序）
// 向记录缓冲区写入一个int16_t值，以小端序存储
bool LogRecordBuilder::writeInt16(int16_t value) {
    return writeLe16(static_cast<uint16_t>(value));
}

// LogRecordBuilder::writeInt32 - 写入32位有符号整数（小端序）
// 向记录缓冲区写入一个int32_t值，以小端序存储
bool LogRecordBuilder::writeInt32(int32_t value) {
    return writeLe32(static_cast<uint32_t>(value));
}

// LogRecordBuilder::writeInt64 - 写入64位有符号整数（小端序）
// 向记录缓冲区写入一个int64_t值，以小端序存储
bool LogRecordBuilder::writeInt64(int64_t value) {
    return writeLe64(static_cast<uint64_t>(value));
}

// LogRecordBuilder::writeFloat - 写入单精度浮点数（小端序）
// 通过内存复制将float转换为32位整数，然后以小端序写入
bool LogRecordBuilder::writeFloat(float value) {
    uint32_t bits = 0;
    static_assert(sizeof(float) == sizeof(uint32_t), "float size must be 4 bytes");
    std::memcpy(&bits, &value, sizeof(bits));
    return writeLe32(bits);
}

// LogRecordBuilder::writeDouble - 写入双精度浮点数（小端序）
// 通过内存复制将double转换为64位整数，然后以小端序写入
bool LogRecordBuilder::writeDouble(double value) {
    uint64_t bits = 0;
    static_assert(sizeof(double) == sizeof(uint64_t), "double size must be 8 bytes");
    std::memcpy(&bits, &value, sizeof(bits));
    return writeLe64(bits);
}

// LogRecordBuilder::writeBytes - 写入原始字节数据
// 向记录缓冲区写入指定大小的数据块
bool LogRecordBuilder::writeBytes(const void* data, uint32_t size) {
    if (!reserve(size)) {
        return false;
    }
    if (size > 0) {
        std::memcpy(buffer_.data() + pos_, data, size);
        pos_ += size;
    }
    return true;
}

// LogRecordBuilder::bytesWritten - 获取已写入的字节数
// 返回当前写入位置，即已写入的字节数
uint32_t LogRecordBuilder::bytesWritten() const {
    return pos_;
}

// LogRecordBuilder::capacity - 获取缓冲区容量
// 返回记录缓冲区的总大小（字节）
uint32_t LogRecordBuilder::capacity() const {
    return static_cast<uint32_t>(buffer_.size());
}

// LogRecordBuilder::isComplete - 检查记录是否完整
// 检查是否已写入所有数据（达到缓冲区容量）
bool LogRecordBuilder::isComplete() const {
    return pos_ == buffer_.size();
}

// LogRecordBuilder::data - 获取记录数据
// 返回记录缓冲区的常量引用
const std::vector<uint8_t>& LogRecordBuilder::data() const {
    return buffer_;
}

// LogRecordBuilder::writeLe16 - 内部：写入16位值（小端序）
// 私有方法，直接操作缓冲区写入小端序的16位值
bool LogRecordBuilder::writeLe16(uint16_t value) {
    if (!reserve(sizeof(uint16_t))) {
        return false;
    }
    buffer_[pos_++] = static_cast<uint8_t>(value & 0xFF);
    buffer_[pos_++] = static_cast<uint8_t>((value >> 8) & 0xFF);
    return true;
}

// LogRecordBuilder::writeLe32 - 内部：写入32位值（小端序）
// 私有方法，直接操作缓冲区写入小端序的32位值
bool LogRecordBuilder::writeLe32(uint32_t value) {
    if (!reserve(sizeof(uint32_t))) {
        return false;
    }
    buffer_[pos_++] = static_cast<uint8_t>(value & 0xFF);
    buffer_[pos_++] = static_cast<uint8_t>((value >> 8) & 0xFF);
    buffer_[pos_++] = static_cast<uint8_t>((value >> 16) & 0xFF);
    buffer_[pos_++] = static_cast<uint8_t>((value >> 24) & 0xFF);
    return true;
}

// LogRecordBuilder::writeLe64 - 内部：写入64位值（小端序）
// 私有方法，直接操作缓冲区写入小端序的64位值
bool LogRecordBuilder::writeLe64(uint64_t value) {
    if (!reserve(sizeof(uint64_t))) {
        return false;
    }
    buffer_[pos_++] = static_cast<uint8_t>(value & 0xFF);
    buffer_[pos_++] = static_cast<uint8_t>((value >> 8) & 0xFF);
    buffer_[pos_++] = static_cast<uint8_t>((value >> 16) & 0xFF);
    buffer_[pos_++] = static_cast<uint8_t>((value >> 24) & 0xFF);
    buffer_[pos_++] = static_cast<uint8_t>((value >> 32) & 0xFF);
    buffer_[pos_++] = static_cast<uint8_t>((value >> 40) & 0xFF);
    buffer_[pos_++] = static_cast<uint8_t>((value >> 48) & 0xFF);
    buffer_[pos_++] = static_cast<uint8_t>((value >> 56) & 0xFF);
    return true;
}

// LogRecordBuilder::reserve - 检查缓冲区是否有足够空间
// 私有方法，检查当前位置后是否有足够的字节空间
bool LogRecordBuilder::reserve(uint32_t size) {
    return pos_ + size <= buffer_.size();
}

// 内部辅助函数 - 用于追加字段值到记录构建器
namespace {

// appendFieldValue - 根据字段类型追加值到记录构建器
// 从原始数据中读取指定类型的值，并追加到构建器中
bool appendFieldValue(LogRecordBuilder& builder, LogFieldType type, const uint8_t* data) {
    switch (type) {
    case LogFieldType::UInt8:
        return builder.writeUInt8(*data);
    case LogFieldType::Int8: {
        int8_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return builder.writeInt8(value);
    }
    case LogFieldType::UInt16: {
        uint16_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return builder.writeUInt16(value);
    }
    case LogFieldType::Int16: {
        int16_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return builder.writeInt16(value);
    }
    case LogFieldType::UInt32: {
        uint32_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return builder.writeUInt32(value);
    }
    case LogFieldType::Int32: {
        int32_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return builder.writeInt32(value);
    }
    case LogFieldType::UInt64: {
        uint64_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return builder.writeUInt64(value);
    }
    case LogFieldType::Int64: {
        int64_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return builder.writeInt64(value);
    }
    case LogFieldType::Float32: {
        float value = 0.0f;
        std::memcpy(&value, data, sizeof(value));
        return builder.writeFloat(value);
    }
    case LogFieldType::Float64: {
        double value = 0.0;
        std::memcpy(&value, data, sizeof(value));
        return builder.writeDouble(value);
    }
    default:
        return false;
    }
}

// isUnsignedType - 检查字段类型是否为无符号整数类型
// 辅助函数，用于判断给定的类型是否为无符号整数
bool isUnsignedType(LogFieldType type) {
    return type == LogFieldType::UInt8 ||
        type == LogFieldType::UInt16 ||
        type == LogFieldType::UInt32 ||
        type == LogFieldType::UInt64;
}

// readUnsignedValue - 从无符号存储类型中读取值
// 从给定数据中读取无符号整数类型的值，支持8/16/32/64位
bool readUnsignedValue(LogFieldType type, const uint8_t* data, uint64_t& value) {
    if (!isUnsignedType(type)) {
        return false;
    }
    switch (type) {
    case LogFieldType::UInt8:
        value = *data;
        return true;
    case LogFieldType::UInt16: {
        uint16_t tmp = 0;
        std::memcpy(&tmp, data, sizeof(tmp));
        value = tmp;
        return true;
    }
    case LogFieldType::UInt32: {
        uint32_t tmp = 0;
        std::memcpy(&tmp, data, sizeof(tmp));
        value = tmp;
        return true;
    }
    case LogFieldType::UInt64: {
        uint64_t tmp = 0;
        std::memcpy(&tmp, data, sizeof(tmp));
        value = tmp;
        return true;
    }
    default:
        return false;
    }
}

// appendUnsignedValue - 向记录构建器追加无符号值
// 将64位值转换为适当的无符号类型后追加到记录构建器中
bool appendUnsignedValue(LogRecordBuilder& builder, LogFieldType type, uint64_t value) {
    if (!isUnsignedType(type)) {
        return false;
    }
    switch (type) {
    case LogFieldType::UInt8:
        return builder.writeUInt8(static_cast<uint8_t>(value));
    case LogFieldType::UInt16:
        return builder.writeUInt16(static_cast<uint16_t>(value));
    case LogFieldType::UInt32:
        return builder.writeUInt32(static_cast<uint32_t>(value));
    case LogFieldType::UInt64:
        return builder.writeUInt64(value);
    default:
        return false;
    }
}

} // namespace 第二个匿名命名空间结束

SelfDescribingLogWriter::SelfDescribingLogWriter() : record_size_(0) {}

// SelfDescribingLogWriter::~SelfDescribingLogWriter - 析构函数
// 确保文件在对象销毁时被正确关闭
SelfDescribingLogWriter::~SelfDescribingLogWriter() {
    close();
}

// SelfDescribingLogWriter::open - 打开或创建日志文件
// 根据提供的模式打开或创建日志文件，如果文件存在则验证其兼容性
bool SelfDescribingLogWriter::open(const std::string& path, const LogSchema& schema) {
    close();
    if (!schema.isValid()) {
        return false;
    }

    const uint32_t record_size = schema.recordSize();
    const std::vector<uint8_t> schema_bytes = schema.serialize();
    if (record_size == 0 || schema_bytes.empty()) {
        return false;
    }

    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        return false;
    }

    if (exists) {
        const auto file_size = std::filesystem::file_size(path, error);
        if (error) {
            return false;
        }
        if (file_size == 0) {
            if (!writeNewFile(path, schema_bytes, record_size)) {
                return false;
            }
        } else {
            if (!validateExistingFile(path, schema_bytes, record_size)) {
                return false;
            }
            out_.open(path, std::ios::binary | std::ios::out | std::ios::app);
        }
    } else {
        if (!writeNewFile(path, schema_bytes, record_size)) {
            return false;
        }
    }

    record_size_ = record_size;
    fields_ = schema.fields();
    record_builder_ = std::make_unique<LogRecordBuilder>(record_size_);
    return out_.is_open();
}

// SelfDescribingLogWriter::close - 关闭文件并释放资源
// 关闭文件流，重置所有内部状态
void SelfDescribingLogWriter::close() {
    if (out_.is_open()) {
        out_.close();
    }
    record_size_ = 0;
    fields_.clear();
    record_builder_.reset();
}

// SelfDescribingLogWriter::isOpen - 检查文件是否已打开
bool SelfDescribingLogWriter::isOpen() const {
    return out_.is_open();
}

// SelfDescribingLogWriter::recordSize - 获取单条记录的大小
uint32_t SelfDescribingLogWriter::recordSize() const {
    return record_size_;
}

// SelfDescribingLogWriter::appendRecord - 从结构体追加记录
// 根据模式布局从结构体实例中提取数据并追加一条记录
bool SelfDescribingLogWriter::appendRecord(const void* struct_data, uint32_t struct_size) {
    if (!out_.is_open() || struct_data == nullptr || struct_size == 0) {
        return false;
    }
    if (!record_builder_ || fields_.empty() || record_size_ == 0) {
        return false;
    }

    // Build a record by walking schema fields and extracting values from the struct.
    record_builder_->reset();
    const auto* base = reinterpret_cast<const uint8_t*>(struct_data);
    for (const auto& field : fields_) {
        if (field.count == 0 || field.byte_offset == LogSchema::kInvalidOffset) {
            return false;
        }
        const LogFieldType storage_type = (field.bit_width > 0) ? field.storage_type : field.type;
        const uint32_t storage_size = logFieldTypeSize(storage_type);
        if (storage_size == 0) {
            return false;
        }
        // For arrays, stride is either explicit or the size of the storage type.
        const uint32_t stride = (field.byte_stride == 0) ? storage_size : field.byte_stride;
        const uint64_t end = static_cast<uint64_t>(field.byte_offset) +
            static_cast<uint64_t>(field.count - 1) * stride + storage_size;
        if (end > struct_size) {
            return false;
        }

        const uint8_t* data = base + field.byte_offset;
        for (uint32_t i = 0; i < field.count; ++i) {
            const uint8_t* element = data + i * stride;
            if (field.bit_width > 0) {
                // Bitfield extraction from an unsigned storage type.
                uint64_t raw = 0;
                if (!readUnsignedValue(storage_type, element, raw)) {
                    return false;
                }
                if (field.bit_width >= 64) {
                    if (!appendUnsignedValue(*record_builder_, field.type, raw)) {
                        return false;
                    }
                    continue;
                }
                const uint64_t mask = (static_cast<uint64_t>(1) << field.bit_width) - 1;
                const uint64_t value = (raw >> field.bit_offset) & mask;
                if (!appendUnsignedValue(*record_builder_, field.type, value)) {
                    return false;
                }
            } else {
                if (!appendFieldValue(*record_builder_, field.type, element)) {
                    return false;
                }
            }
        }
    }

    if (!record_builder_->isComplete()) {
        return false;
    }
    return appendRawRecord(record_builder_->data());
}

// SelfDescribingLogWriter::appendRawRecord - 追加原始记录
// 将预构建的二进制记录直接写入文件
bool SelfDescribingLogWriter::appendRawRecord(const uint8_t* data, uint32_t size) {
    if (!out_.is_open() || data == nullptr || size != record_size_) {
        return false;
    }
    out_.write(reinterpret_cast<const char*>(data), size);
    return out_.good();
}

// SelfDescribingLogWriter::appendRawRecord - 追加vector记录（重载）
// 将vector格式的二进制记录写入文件
bool SelfDescribingLogWriter::appendRawRecord(const std::vector<uint8_t>& data) {
    return appendRawRecord(data.data(), static_cast<uint32_t>(data.size()));
}

// SelfDescribingLogWriter::flush - 刷新输出缓冲区
// 将缓冲区数据强制写入磁盘，确保数据持久化
void SelfDescribingLogWriter::flush() {
    if (out_.is_open()) {
        out_.flush();
    }
}

// SelfDescribingLogWriter::writeNewFile - 创建新文件并写入头部和模式
// 创建新的日志文件，写入文件头部和模式数据
bool SelfDescribingLogWriter::writeNewFile(const std::string& path,
                                           const std::vector<uint8_t>& schema_bytes,
                                           uint32_t record_size) {
    out_.open(path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!out_.is_open()) {
        return false;
    }

    const auto header = buildHeader(static_cast<uint32_t>(schema_bytes.size()), record_size);
    out_.write(reinterpret_cast<const char*>(header.data()), header.size());
    if (!schema_bytes.empty()) {
        out_.write(reinterpret_cast<const char*>(schema_bytes.data()), schema_bytes.size());
    }
    return out_.good();
}

// SelfDescribingLogWriter::validateExistingFile - 验证现有文件
// 检查现有文件的头部和模式是否与期望值匹配
bool SelfDescribingLogWriter::validateExistingFile(const std::string& path,
                                                   const std::vector<uint8_t>& expected_schema,
                                                   uint32_t record_size) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }

    std::array<uint8_t, kHeaderSize> header{};
    in.read(reinterpret_cast<char*>(header.data()), header.size());
    if (static_cast<size_t>(in.gcount()) != header.size()) {
        return false;
    }

    if (!std::equal(std::begin(kMagic), std::end(kMagic), header.begin())) {
        return false;
    }
    const uint16_t version = readLe16(header.data() + 4);
    const uint8_t endian = header[6];
    const uint32_t schema_size = readLe32(header.data() + 8);
    const uint32_t file_record_size = readLe32(header.data() + 12);

    if (version != kVersion || endian != kEndianLittle) {
        return false;
    }
    if (file_record_size != record_size || schema_size != expected_schema.size()) {
        return false;
    }

    std::vector<uint8_t> existing_schema(schema_size);
    if (schema_size > 0) {
        in.read(reinterpret_cast<char*>(existing_schema.data()), schema_size);
        if (static_cast<uint32_t>(in.gcount()) != schema_size) {
            return false;
        }
        if (existing_schema != expected_schema) {
            return false;
        }
    }
    return true;
}

} // namespace Tools 命名空间结束
} // namespace MB_DDF 命名空间结束
