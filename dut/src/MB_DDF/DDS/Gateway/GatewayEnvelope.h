#pragma once

/**
 * @file GatewayEnvelope.h
 * @brief DDS跨域网关信封格式定义
 *
 * GatewayEnvelope是在不同DDS域之间传输用户Topic数据的统一封装。
 * 外部链路只需要传输该二进制信封，接收端再根据Topic名称和payload重新发布到本地域。
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace MB_DDF {
namespace DDS {

/// 网关信封魔数，ASCII为"GDW2"，用于快速识别跨域网关报文。
inline constexpr uint32_t GATEWAY_ENVELOPE_MAGIC = 0x47445732;

/// 当前信封格式版本号。反序列化时会严格匹配，避免不同布局的报文被误解析。
inline constexpr uint16_t GATEWAY_ENVELOPE_VERSION = 1;

/**
 * @brief 固定长度的网关信封头。
 *
 * 该结构需要以紧凑布局写入外部端点，因此使用pack(1)禁止编译器插入填充字节。
 * 字段均使用固定宽度整数，确保AArch64目标板和Windows主机交叉编译时布局一致。
 */
#pragma pack(push, 1)
struct GatewayEnvelopeHeader {
    uint32_t magic{GATEWAY_ENVELOPE_MAGIC};        ///< 信封魔数，用于判断数据是否为网关报文。
    uint16_t version{GATEWAY_ENVELOPE_VERSION};    ///< 信封格式版本，接收端只接受当前版本。
    uint16_t header_size{0};                       ///< 头部字节数，防止结构体布局变化后误解码。
    uint32_t flags{0};                             ///< 预留标志位，当前未定义具体语义。
    uint32_t origin_domain_id{0};                  ///< 原始发布者所在DDS域ID，转发时保持不变。
    uint32_t sender_domain_id{0};                  ///< 当前发送该信封的DDS域ID，每次转发都会更新。
    uint64_t origin_gateway_id{0};                 ///< 首次封装该消息的网关ID，用于跨域去重。
    uint64_t message_id{0};                        ///< 原始网关内递增消息ID，和origin_gateway_id组成全局键。
    uint8_t ttl{0};                                ///< 剩余转发跳数，防止多网关拓扑中无限转发。
    uint8_t reserved[7]{};                         ///< 保留字节，序列化时清零，便于后续协议扩展。
    uint16_t topic_name_size{0};                   ///< Topic名称长度，后续紧跟该长度的Topic字节。
    uint32_t payload_size{0};                      ///< 用户负载长度，Topic名称之后紧跟该长度的payload。
    uint32_t payload_crc32{0};                     ///< payload的CRC32校验，用于发现链路数据损坏。
};
#pragma pack(pop)

/// 编译期记录当前固定头长度，序列化和反序列化都使用该值校验协议布局。
inline constexpr uint16_t GATEWAY_ENVELOPE_HEADER_SIZE =
    static_cast<uint16_t>(sizeof(GatewayEnvelopeHeader));

/// 如果新增字段或编译器布局变化导致头大小改变，需要同步升级协议版本和解析逻辑。
static_assert(sizeof(GatewayEnvelopeHeader) == 54, "GatewayEnvelopeHeader layout changed");

/**
 * @brief 网关传输信封的内存表示。
 *
 * 二进制布局为：
 * GatewayEnvelopeHeader + topic_name原始字节 + payload原始字节。
 */
struct GatewayEnvelope {
    GatewayEnvelopeHeader header{};    ///< 固定头部，保存跨域路由、去重、TTL和校验信息。
    std::string topic_name;            ///< 目标Topic名称，接收端按该名称发布到本地域。
    std::vector<uint8_t> payload;      ///< 用户消息体，不包含DDS本地MessageHeader。
};

/**
 * @brief 将网关信封编码为可发送的连续字节缓冲区。
 * @param envelope 待编码信封。
 * @return 成功返回二进制缓冲区；Topic为空、长度溢出或payload过大时返回空vector。
 */
std::vector<uint8_t> serialize_gateway_envelope(const GatewayEnvelope& envelope);

/**
 * @brief 从外部端点收到的字节缓冲区解析网关信封。
 * @param data 输入缓冲区起始地址。
 * @param size 输入缓冲区字节数。
 * @param envelope 解析成功时写入的输出信封。
 * @return 校验通过并完整解析返回true，否则返回false。
 */
bool deserialize_gateway_envelope(const uint8_t* data, size_t size, GatewayEnvelope& envelope);

} // namespace DDS
} // namespace MB_DDF
