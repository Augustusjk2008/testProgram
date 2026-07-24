#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "MB_DDF/DDS/Gateway/GatewayEnvelope.h"

using namespace MB_DDF::DDS;

namespace {

GatewayEnvelope make_envelope() {
    GatewayEnvelope envelope;
    envelope.header.flags = 7;
    envelope.header.origin_domain_id = 1;
    envelope.header.sender_domain_id = 2;
    envelope.header.origin_gateway_id = 0x12345678ULL;
    envelope.header.message_id = 42;
    envelope.header.ttl = 5;
    envelope.topic_name = "rt://sensor/temp";
    envelope.payload = {'h', 'e', 'l', 'l', 'o'};
    return envelope;
}

GatewayEnvelopeHeader read_header(const std::vector<uint8_t>& buffer) {
    GatewayEnvelopeHeader header{};
    std::memcpy(&header, buffer.data(), sizeof(header));
    return header;
}

void write_header(std::vector<uint8_t>& buffer, const GatewayEnvelopeHeader& header) {
    std::memcpy(buffer.data(), &header, sizeof(header));
}

} // namespace

TEST(GatewayEnvelopeTest, RoundTrip) {
    const auto input = make_envelope();
    const auto buffer = serialize_gateway_envelope(input);
    ASSERT_FALSE(buffer.empty());

    GatewayEnvelope output;
    ASSERT_TRUE(deserialize_gateway_envelope(buffer.data(), buffer.size(), output));

    EXPECT_EQ(output.header.magic, GATEWAY_ENVELOPE_MAGIC);
    EXPECT_EQ(output.header.version, GATEWAY_ENVELOPE_VERSION);
    EXPECT_EQ(output.header.header_size, GATEWAY_ENVELOPE_HEADER_SIZE);
    EXPECT_EQ(output.header.flags, input.header.flags);
    EXPECT_EQ(output.header.origin_domain_id, input.header.origin_domain_id);
    EXPECT_EQ(output.header.sender_domain_id, input.header.sender_domain_id);
    EXPECT_EQ(output.header.origin_gateway_id, input.header.origin_gateway_id);
    EXPECT_EQ(output.header.message_id, input.header.message_id);
    EXPECT_EQ(output.header.ttl, input.header.ttl);
    EXPECT_EQ(output.topic_name, input.topic_name);
    EXPECT_EQ(output.payload, input.payload);
}

TEST(GatewayEnvelopeTest, BadMagicFails) {
    auto buffer = serialize_gateway_envelope(make_envelope());
    ASSERT_FALSE(buffer.empty());

    auto header = read_header(buffer);
    header.magic = 0;
    write_header(buffer, header);

    GatewayEnvelope output;
    EXPECT_FALSE(deserialize_gateway_envelope(buffer.data(), buffer.size(), output));
}

TEST(GatewayEnvelopeTest, BadVersionFails) {
    auto buffer = serialize_gateway_envelope(make_envelope());
    ASSERT_FALSE(buffer.empty());

    auto header = read_header(buffer);
    header.version = static_cast<uint16_t>(GATEWAY_ENVELOPE_VERSION + 1);
    write_header(buffer, header);

    GatewayEnvelope output;
    EXPECT_FALSE(deserialize_gateway_envelope(buffer.data(), buffer.size(), output));
}

TEST(GatewayEnvelopeTest, TruncatedHeaderFails) {
    auto buffer = serialize_gateway_envelope(make_envelope());
    ASSERT_GT(buffer.size(), sizeof(GatewayEnvelopeHeader));
    buffer.resize(sizeof(GatewayEnvelopeHeader) - 1);

    GatewayEnvelope output;
    EXPECT_FALSE(deserialize_gateway_envelope(buffer.data(), buffer.size(), output));
}

TEST(GatewayEnvelopeTest, TruncatedPayloadFails) {
    auto buffer = serialize_gateway_envelope(make_envelope());
    ASSERT_FALSE(buffer.empty());
    buffer.pop_back();

    GatewayEnvelope output;
    EXPECT_FALSE(deserialize_gateway_envelope(buffer.data(), buffer.size(), output));
}

TEST(GatewayEnvelopeTest, CrcErrorFails) {
    auto buffer = serialize_gateway_envelope(make_envelope());
    ASSERT_FALSE(buffer.empty());
    buffer.back() ^= 0x01;

    GatewayEnvelope output;
    EXPECT_FALSE(deserialize_gateway_envelope(buffer.data(), buffer.size(), output));
}
