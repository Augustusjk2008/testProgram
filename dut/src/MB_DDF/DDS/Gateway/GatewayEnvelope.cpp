#include "MB_DDF/DDS/Gateway/GatewayEnvelope.h"

#include "MB_DDF/DDS/Message.h"

#include <cstring>
#include <limits>

namespace MB_DDF {
namespace DDS {

namespace {

/**
 * @brief 复用DDS消息头中的CRC32实现计算payload校验值。
 *
 * 空payload在MessageHeader::calculate_checksum中返回0，因此发送端和接收端
 * 对空数据会得到一致结果。
 */
uint32_t calculate_payload_crc(const uint8_t* data, size_t size) {
    return MessageHeader::calculate_checksum(data, size);
}

} // namespace

std::vector<uint8_t> serialize_gateway_envelope(const GatewayEnvelope& envelope) {
    // Topic名称必须存在，因为远端需要用它决定重新发布到哪个本地Topic。
    // 同时限制字段长度，确保后续static_cast不会截断。
    if (envelope.topic_name.empty() ||
        envelope.topic_name.size() > std::numeric_limits<uint16_t>::max() ||
        envelope.payload.size() > std::numeric_limits<uint32_t>::max()) {
        return {};
    }

    // 复制调用方提供的路由字段，再由序列化函数统一补齐协议固定字段和长度字段。
    GatewayEnvelopeHeader header = envelope.header;
    header.magic = GATEWAY_ENVELOPE_MAGIC;
    header.version = GATEWAY_ENVELOPE_VERSION;
    header.header_size = GATEWAY_ENVELOPE_HEADER_SIZE;
    header.topic_name_size = static_cast<uint16_t>(envelope.topic_name.size());
    header.payload_size = static_cast<uint32_t>(envelope.payload.size());

    // 保留区不承载语义，发送前固定清零，避免栈上旧值进入链路。
    std::memset(header.reserved, 0, sizeof(header.reserved));
    header.payload_crc32 = calculate_payload_crc(
        envelope.payload.empty() ? nullptr : envelope.payload.data(),
        envelope.payload.size());

    // 外部端点发送的是单块连续内存：固定头 + Topic名称 + payload。
    const size_t total_size = sizeof(GatewayEnvelopeHeader) +
                              envelope.topic_name.size() +
                              envelope.payload.size();
    std::vector<uint8_t> buffer(total_size);

    size_t offset = 0;
    std::memcpy(buffer.data() + offset, &header, sizeof(header));
    offset += sizeof(header);

    std::memcpy(buffer.data() + offset, envelope.topic_name.data(), envelope.topic_name.size());
    offset += envelope.topic_name.size();

    if (!envelope.payload.empty()) {
        std::memcpy(buffer.data() + offset, envelope.payload.data(), envelope.payload.size());
    }

    return buffer;
}

bool deserialize_gateway_envelope(const uint8_t* data, size_t size, GatewayEnvelope& envelope) {
    // 至少需要完整固定头；空指针或短报文都不能继续读取字段。
    if (data == nullptr || size < sizeof(GatewayEnvelopeHeader)) {
        return false;
    }

    GatewayEnvelopeHeader header{};
    std::memcpy(&header, data, sizeof(header));

    // 先校验协议身份和布局，避免把其他链路数据当作GatewayEnvelope解析。
    if (header.magic != GATEWAY_ENVELOPE_MAGIC ||
        header.version != GATEWAY_ENVELOPE_VERSION ||
        header.header_size != GATEWAY_ENVELOPE_HEADER_SIZE) {
        return false;
    }

    const size_t header_size = sizeof(GatewayEnvelopeHeader);
    // Topic为空没有路由意义；同时防止topic_name_size越过输入缓冲区尾部。
    if (header.topic_name_size == 0 || header.topic_name_size > size - header_size) {
        return false;
    }

    const size_t topic_offset = header_size;
    const size_t payload_offset = topic_offset + header.topic_name_size;
    // payload_size必须完整落在缓冲区内，并且报文不能带未声明的尾部字节。
    if (header.payload_size > size - payload_offset) {
        return false;
    }
    if (payload_offset + header.payload_size != size) {
        return false;
    }

    const uint8_t* payload_data = data + payload_offset;
    // 只有payload参与CRC，Topic和头部字段依靠长度、魔数、版本等结构校验。
    const uint32_t crc = calculate_payload_crc(
        header.payload_size == 0 ? nullptr : payload_data,
        header.payload_size);
    if (crc != header.payload_crc32) {
        return false;
    }

    // 全部校验通过后再构造输出对象，避免失败路径污染调用方传入的envelope。
    GatewayEnvelope parsed{};
    parsed.header = header;
    parsed.topic_name.assign(
        reinterpret_cast<const char*>(data + topic_offset),
        header.topic_name_size);
    parsed.payload.assign(payload_data, payload_data + header.payload_size);

    envelope = std::move(parsed);
    return true;
}

} // namespace DDS
} // namespace MB_DDF
