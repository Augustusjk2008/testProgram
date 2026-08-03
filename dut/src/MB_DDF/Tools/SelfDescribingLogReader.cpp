#include "SelfDescribingLogReader.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <iomanip>

namespace MB_DDF {
namespace Tools {

namespace {

// 文件格式常量（与 SelfDescribingLog.cpp 保持一致）
constexpr char kMagic[4] = {'S', 'L', 'O', 'G'};
constexpr uint16_t kVersion = 1;
constexpr uint8_t kEndianLittle = 1;
constexpr uint32_t kHeaderSize = 16;

// 以小端序读取16位值
uint16_t readLe16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8);
}

// 以小端序读取32位值
uint32_t readLe32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

// 以小端序读取64位值
uint64_t readLe64(const uint8_t* data) {
    return static_cast<uint64_t>(data[0]) |
           (static_cast<uint64_t>(data[1]) << 8) |
           (static_cast<uint64_t>(data[2]) << 16) |
           (static_cast<uint64_t>(data[3]) << 24) |
           (static_cast<uint64_t>(data[4]) << 32) |
           (static_cast<uint64_t>(data[5]) << 40) |
           (static_cast<uint64_t>(data[6]) << 48) |
           (static_cast<uint64_t>(data[7]) << 56);
}

// 以小端序读取双精度浮点数
double readLeDouble(const uint8_t* data) {
    uint64_t bits = readLe64(data);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// JSON转义字符串
std::string jsonEscape(const std::string& str) {
    std::string result;
    result.reserve(str.size());
    for (char c : str) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    result += buf;
                } else {
                    result += c;
                }
        }
    }
    return result;
}

} // namespace

SelfDescribingLogReader::SelfDescribingLogReader()
    : record_size_(0)
    , data_offset_(0)
    , current_position_(0)
    , total_records_(0) {}

SelfDescribingLogReader::~SelfDescribingLogReader() {
    close();
}

bool SelfDescribingLogReader::open(const std::string& path) {
    close();

    // 检查文件是否存在
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return false;
    }

    // 打开文件
    in_.open(path, std::ios::binary);
    if (!in_.is_open()) {
        return false;
    }

    // 解析头部
    if (!parseHeader()) {
        close();
        return false;
    }

    // 计算总记录数
    in_.seekg(0, std::ios::end);
    const auto file_size = in_.tellg();
    if (file_size < static_cast<std::streamoff>(data_offset_)) {
        close();
        return false;
    }

    const uint64_t data_size = static_cast<uint64_t>(file_size) - data_offset_;
    if (record_size_ > 0) {
        total_records_ = data_size / record_size_;
    }

    // 重置到第一条记录
    reset();

    return true;
}

void SelfDescribingLogReader::close() {
    if (in_.is_open()) {
        in_.close();
    }
    record_size_ = 0;
    data_offset_ = 0;
    current_position_ = 0;
    total_records_ = 0;
    schema_ = LogSchema();
}

bool SelfDescribingLogReader::isOpen() const {
    return in_.is_open();
}

const LogSchema& SelfDescribingLogReader::schema() const {
    return schema_;
}

const std::vector<LogField>& SelfDescribingLogReader::fields() const {
    return schema_.fields();
}

uint32_t SelfDescribingLogReader::recordSize() const {
    return record_size_;
}

uint64_t SelfDescribingLogReader::recordCount() const {
    return total_records_;
}

bool SelfDescribingLogReader::readNextRecord(std::vector<uint8_t>& record) {
    if (!in_.is_open() || current_position_ >= total_records_) {
        return false;
    }

    record.resize(record_size_);
    in_.read(reinterpret_cast<char*>(record.data()), record_size_);

    if (!in_.good()) {
        return false;
    }

    ++current_position_;
    return true;
}

bool SelfDescribingLogReader::readRecord(uint64_t index, std::vector<uint8_t>& record) {
    if (!in_.is_open() || index >= total_records_) {
        return false;
    }

    const uint64_t offset = data_offset_ + index * record_size_;
    in_.seekg(offset, std::ios::beg);
    if (!in_.good()) {
        return false;
    }

    record.resize(record_size_);
    in_.read(reinterpret_cast<char*>(record.data()), record_size_);

    return in_.good();
}

void SelfDescribingLogReader::reset() {
    if (in_.is_open()) {
        in_.seekg(data_offset_, std::ios::beg);
        current_position_ = 0;
    }
}

std::string SelfDescribingLogReader::recordToJson(const std::vector<uint8_t>& record, bool pretty) const {
    if (record.size() != record_size_) {
        return "{}";
    }

    std::ostringstream oss;
    const auto& fields = schema_.fields();
    const std::string indent = pretty ? "  " : "";
    const std::string newline = pretty ? "\n" : "";

    oss << "{" << newline;

    for (size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) {
            oss << "," << newline;
        }

        if (pretty) {
            oss << indent;
        }

        oss << "\"" << jsonEscape(fields[i].name) << "\": ";

        if (fields[i].count == 1) {
            // 标量值
            oss << extractFieldJsonValue(record.data(), fields[i]);
        } else {
            // 数组值
            oss << "[";
            const uint32_t field_size = logFieldTypeSize(fields[i].type);
            const uint8_t* field_data = record.data();
            uint32_t offset = 0;

            // 计算当前字段在记录中的起始偏移
            for (size_t j = 0; j < i; ++j) {
                offset += logFieldTypeSize(fields[j].type) * fields[j].count;
            }

            for (uint32_t elem = 0; elem < fields[i].count; ++elem) {
                if (elem > 0) {
                    oss << ", ";
                }
                LogField single_field = fields[i];
                single_field.count = 1;
                oss << extractFieldJsonValue(field_data + offset + elem * field_size, single_field);
            }
            oss << "]";
        }
    }

    if (pretty) {
        oss << newline;
    }
    oss << "}";

    return oss.str();
}

std::string SelfDescribingLogReader::exportToJson(bool pretty) const {
    return exportRangeToJson(0, total_records_, pretty);
}

std::string SelfDescribingLogReader::exportRangeToJson(uint64_t start_index, uint64_t count, bool pretty) const {
    if (!in_.is_open()) {
        return "[]";
    }

    const uint64_t end_index = std::min(start_index + count, total_records_);
    if (start_index >= end_index) {
        return "[]";
    }

    std::ostringstream oss;
    const std::string indent = pretty ? "  " : "";
    const std::string newline = pretty ? "\n" : "";

    oss << "[" << newline;

    std::vector<uint8_t> record;
    for (uint64_t i = start_index; i < end_index; ++i) {
        if (!const_cast<SelfDescribingLogReader*>(this)->readRecord(i, record)) {
            break;
        }

        if (i > start_index) {
            oss << "," << newline;
        }

        if (pretty) {
            oss << indent;
        }

        oss << recordToJson(record, false);
    }

    if (pretty) {
        oss << newline;
    }
    oss << "]";

    return oss.str();
}

void SelfDescribingLogReader::forEach(std::function<bool(uint64_t, const std::vector<uint8_t>&)> callback) {
    if (!in_.is_open() || !callback) {
        return;
    }

    reset();
    std::vector<uint8_t> record;

    for (uint64_t i = 0; i < total_records_; ++i) {
        if (!readNextRecord(record)) {
            break;
        }

        if (!callback(i, record)) {
            break;
        }
    }
}

bool SelfDescribingLogReader::parseHeader() {
    std::array<uint8_t, kHeaderSize> header{};
    in_.read(reinterpret_cast<char*>(header.data()), kHeaderSize);

    if (in_.gcount() != kHeaderSize) {
        return false;
    }

    // 验证魔数
    if (!std::equal(std::begin(kMagic), std::end(kMagic), header.begin())) {
        return false;
    }

    // 验证版本和字节序
    const uint16_t version = readLe16(header.data() + 4);
    const uint8_t endian = header[6];

    if (version != kVersion || endian != kEndianLittle) {
        return false;
    }

    // 读取模式大小和记录大小
    const uint32_t schema_size = readLe32(header.data() + 8);
    record_size_ = readLe32(header.data() + 12);

    if (record_size_ == 0) {
        return false;
    }

    // 解析模式
    if (!parseSchema(schema_size)) {
        return false;
    }

    // 计算数据区偏移
    data_offset_ = kHeaderSize + schema_size;

    return true;
}

bool SelfDescribingLogReader::parseSchema(uint32_t schema_size) {
    if (schema_size == 0) {
        return false;
    }

    std::vector<uint8_t> schema_data(schema_size);
    in_.read(reinterpret_cast<char*>(schema_data.data()), schema_size);

    if (static_cast<uint32_t>(in_.gcount()) != schema_size) {
        return false;
    }

    const uint8_t* ptr = schema_data.data();
    const uint8_t* end = ptr + schema_size;

    // 读取字段数量
    if (ptr + 4 > end) {
        return false;
    }
    const uint32_t field_count = readLe32(ptr);
    ptr += 4;

    // 读取每个字段
    for (uint32_t i = 0; i < field_count; ++i) {
        // 读取名称长度
        if (ptr + 2 > end) {
            return false;
        }
        const uint16_t name_len = readLe16(ptr);
        ptr += 2;

        // 读取名称
        if (ptr + name_len > end) {
            return false;
        }
        std::string name(reinterpret_cast<const char*>(ptr), name_len);
        ptr += name_len;

        // 读取类型、保留字节、数量
        if (ptr + 6 > end) {
            return false;
        }
        const auto type = static_cast<LogFieldType>(ptr[0]);
        ptr += 2; // type + reserved

        const uint32_t count = readLe32(ptr);
        ptr += 4;

        // 读取比例因子和偏移量
        if (ptr + 16 > end) {
            return false;
        }
        const double scale = readLeDouble(ptr);
        ptr += 8;
        const double offset = readLeDouble(ptr);
        ptr += 8;

        // 添加字段到模式
        if (!schema_.addField(name, type, count, scale, offset)) {
            return false;
        }
    }

    return schema_.isValid();
}

std::string SelfDescribingLogReader::extractFieldValue(const uint8_t* data, const LogField& field) const {
    char buf[64];

    switch (field.type) {
        case LogFieldType::UInt8:
            std::snprintf(buf, sizeof(buf), "%u", data[0]);
            return buf;

        case LogFieldType::Int8: {
            int8_t value;
            std::memcpy(&value, data, sizeof(value));
            std::snprintf(buf, sizeof(buf), "%d", value);
            return buf;
        }

        case LogFieldType::UInt16: {
            const uint16_t value = readLe16(data);
            std::snprintf(buf, sizeof(buf), "%u", value);
            return buf;
        }

        case LogFieldType::Int16: {
            const uint16_t raw = readLe16(data);
            int16_t value;
            std::memcpy(&value, &raw, sizeof(value));
            std::snprintf(buf, sizeof(buf), "%d", value);
            return buf;
        }

        case LogFieldType::UInt32: {
            const uint32_t value = readLe32(data);
            std::snprintf(buf, sizeof(buf), "%u", value);
            return buf;
        }

        case LogFieldType::Int32: {
            const uint32_t raw = readLe32(data);
            int32_t value;
            std::memcpy(&value, &raw, sizeof(value));
            std::snprintf(buf, sizeof(buf), "%d", value);
            return buf;
        }

        case LogFieldType::UInt64: {
            const uint64_t value = readLe64(data);
            std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(value));
            return buf;
        }

        case LogFieldType::Int64: {
            const uint64_t raw = readLe64(data);
            int64_t value;
            std::memcpy(&value, &raw, sizeof(value));
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
            return buf;
        }

        case LogFieldType::Float32: {
            const uint32_t raw = readLe32(data);
            float value;
            std::memcpy(&value, &raw, sizeof(value));
            std::snprintf(buf, sizeof(buf), "%.6g", value);
            return buf;
        }

        case LogFieldType::Float64: {
            const double value = readLeDouble(data);
            std::snprintf(buf, sizeof(buf), "%.15g", value);
            return buf;
        }

        default:
            return "null";
    }
}

std::string SelfDescribingLogReader::extractFieldJsonValue(const uint8_t* data, const LogField& field) const {
    // 对于数值类型，直接返回数值（不带引号）
    // 对于其他类型，返回字符串（带引号）
    switch (field.type) {
        case LogFieldType::UInt8:
        case LogFieldType::Int8:
        case LogFieldType::UInt16:
        case LogFieldType::Int16:
        case LogFieldType::UInt32:
        case LogFieldType::Int32:
        case LogFieldType::UInt64:
        case LogFieldType::Int64:
        case LogFieldType::Float32:
        case LogFieldType::Float64:
            return extractFieldValue(data, field);

        default:
            return "\"" + jsonEscape(extractFieldValue(data, field)) + "\"";
    }
}

} // namespace Tools
} // namespace MB_DDF
