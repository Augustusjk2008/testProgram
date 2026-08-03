#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <functional>

#include "SelfDescribingLog.h"

namespace MB_DDF {
namespace Tools {

// 自描述日志读取器 - 用于读取和解析 SelfDescribingLog 生成的二进制文件
// 支持顺序读取、随机访问、JSON导出等功能
class SelfDescribingLogReader {
public:
    SelfDescribingLogReader();
    ~SelfDescribingLogReader();

    // 打开日志文件并解析头部和模式
    bool open(const std::string& path);

    // 关闭文件并释放资源
    void close();

    // 检查文件是否已打开
    bool isOpen() const;

    // 获取模式信息
    const LogSchema& schema() const;
    const std::vector<LogField>& fields() const;

    // 获取记录信息
    uint32_t recordSize() const;
    uint64_t recordCount() const;

    // 顺序读取下一条记录（返回原始二进制数据）
    bool readNextRecord(std::vector<uint8_t>& record);

    // 随机访问指定索引的记录
    bool readRecord(uint64_t index, std::vector<uint8_t>& record);

    // 重置读取位置到第一条记录
    void reset();

    // 将单条记录转换为JSON字符串
    std::string recordToJson(const std::vector<uint8_t>& record, bool pretty = false) const;

    // 将所有记录导出为JSON数组
    std::string exportToJson(bool pretty = false) const;

    // 将指定范围的记录导出为JSON数组
    std::string exportRangeToJson(uint64_t start_index, uint64_t count, bool pretty = false) const;

    // 遍历所有记录（回调函数返回false时停止）
    void forEach(std::function<bool(uint64_t index, const std::vector<uint8_t>& record)> callback);

private:
    // 解析文件头部
    bool parseHeader();

    // 解析模式数据
    bool parseSchema(uint32_t schema_size);

    // 从二进制数据中提取字段值并转换为字符串
    std::string extractFieldValue(const uint8_t* data, const LogField& field) const;

    // 从二进制数据中提取字段值并转换为JSON值（带引号或不带引号）
    std::string extractFieldJsonValue(const uint8_t* data, const LogField& field) const;

    std::ifstream in_;
    LogSchema schema_;
    uint32_t record_size_;
    uint64_t data_offset_;      // 数据区起始位置
    uint64_t current_position_; // 当前读取位置（记录索引）
    uint64_t total_records_;    // 总记录数
};

} // namespace Tools
} // namespace MB_DDF
