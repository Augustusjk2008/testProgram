#include <gtest/gtest.h>
#include "MB_DDF_HW/Device/ComDevice.h"
#include "hw_unit/support/RecordingTransport.h"
#include <vector>
using namespace MB_DDF::HW;
using namespace MB_DDF::HW::Test;

TEST(HwComDevice, DefaultConfigMatchesCom1WireProtocol) {
    const auto config = ComDevice::default_config();

    EXPECT_EQ(config.format.byte_format, 0xB0u);
    EXPECT_EQ(config.format.receive_control, 0x21u);
    EXPECT_TRUE(config.receive_enabled);
    EXPECT_TRUE(config.byte_timeout_returns_idle);
    EXPECT_FALSE(config.loopback);
    EXPECT_EQ(config.interrupt_mode, ComInterruptMode::Pulse);
    EXPECT_EQ(config.frame.send_length_bytes, 1u);
    EXPECT_EQ(config.frame.receive_length_bytes, 1u);
    EXPECT_EQ(config.frame.send_header_length, 2u);
    EXPECT_EQ(config.frame.receive_header_length, 2u);
    EXPECT_EQ(config.frame.send_tail_length, 0u);
    EXPECT_EQ(config.frame.receive_tail_length, 0u);
    EXPECT_EQ(config.frame.send_header[0], 0x55u);
    EXPECT_EQ(config.frame.send_header[1], 0xAAu);
    EXPECT_EQ(config.frame.receive_header[0], 0x55u);
    EXPECT_EQ(config.frame.receive_header[1], 0xAAu);
    EXPECT_EQ(config.baudrate_counter, 0x00CAu);
}

TEST(HwComDevice, ConfigureDefaultWritesProtocolRegistersInOrder) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    ComDevice d(t);

    ASSERT_TRUE(d.configure(ComDevice::default_config()));

    const std::vector<Access> expected = {
        {true, 0x20000u, 8u, 0xB0u},
        {true, 0x20001u, 8u, 0x21u},
        {true, 0x20002u, 8u, 0x42u},
        {true, 0x20003u, 8u, 0x42u},
        {true, 0x20004u, 8u, 0x55u},
        {true, 0x20005u, 8u, 0xAAu},
        {true, 0x20006u, 8u, 0x00u},
        {true, 0x20007u, 8u, 0x00u},
        {true, 0x20008u, 8u, 0x00u},
        {true, 0x20009u, 8u, 0x00u},
        {true, 0x2000Au, 8u, 0x00u},
        {true, 0x2000Bu, 8u, 0x00u},
        {true, 0x2000Cu, 8u, 0x55u},
        {true, 0x2000Du, 8u, 0xAAu},
        {true, 0x2000Eu, 8u, 0x00u},
        {true, 0x2000Fu, 8u, 0x00u},
        {true, 0x20010u, 8u, 0x00u},
        {true, 0x20011u, 8u, 0x00u},
        {true, 0x20012u, 8u, 0x00u},
        {true, 0x20013u, 8u, 0x00u},
        {true, 0x20014u, 8u, 0x00u},
        {true, 0x20015u, 8u, 0x00u},
        {true, 0x30008u, 8u, 0x01u},
        {true, 0x20018u, 16u, 0x0005u},
        {true, 0x20020u, 32u, 312499u},
        {true, 0x20028u, 16u, 0x00CAu},
        {true, 0x2002Au, 16u, 0x00CAu},
    };

    ASSERT_EQ(t.accesses().size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(t.accesses()[i].write, expected[i].write) << "access " << i;
        EXPECT_EQ(t.accesses()[i].offset, expected[i].offset) << "access " << i;
        EXPECT_EQ(t.accesses()[i].width, expected[i].width) << "access " << i;
        EXPECT_EQ(t.accesses()[i].value, expected[i].value) << "access " << i;
    }
}

TEST(HwComDevice, PacksSendRamBeforeTriggeringTransmit) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    t.preset(0x30000, 2);
    ComDevice d(t);
    const uint8_t data[] = {1, 2, 3, 4, 5};
    auto r = d.send({data, 5});
    ASSERT_TRUE(r);
    const auto& a = t.accesses();
    ASSERT_EQ(a.size(), 6u);
    EXPECT_FALSE(a[0].write);
    EXPECT_EQ(a[0].offset, 0x30000u);
    EXPECT_FALSE(a[1].write);
    EXPECT_EQ(a[1].offset, 0x20000u);
    EXPECT_TRUE(a[2].write);
    EXPECT_EQ(a[2].offset, 0x10000u);
    EXPECT_EQ(a[2].value, 0x04030201u);
    EXPECT_TRUE(a[3].write);
    EXPECT_EQ(a[3].offset, 0x10004u);
    EXPECT_EQ(a[3].value, 5u);
    EXPECT_TRUE(a[4].write);
    EXPECT_EQ(a[4].offset, 0x2001Cu);
    EXPECT_EQ(a[4].width, 16u);
    EXPECT_EQ(a[4].value, 5u);
    EXPECT_TRUE(a[5].write);
    EXPECT_EQ(a[5].offset, 0x30000u);
    EXPECT_EQ(a[5].value, 0x81u);
}
TEST(HwComDevice, EmptyPayloadDoesNotTouchHardware) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    ComDevice d(t);

    auto r = d.send({nullptr, 0});

    ASSERT_TRUE(r);
    EXPECT_EQ(r.value(), 0u);
    EXPECT_TRUE(t.accesses().empty());
}
TEST(HwComDevice, ConfigureExplicitlyEnablesInterruptOutput) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    ComDevice d(t);

    ASSERT_TRUE(d.configure({}));

    bool enabled = false;
    for (const auto& access : t.accesses()) {
        if (access.write && access.offset == 0x30008u) {
            enabled = true;
            EXPECT_EQ(access.width, 8u);
            EXPECT_EQ(access.value, 1u);
        }
    }
    EXPECT_TRUE(enabled);
}
TEST(HwComDevice, PrefixesPayloadWhenFrameUsesOneByteLength) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    t.preset(0x30000, 2);
    t.preset(0x20000, 0x40400000u);
    ComDevice d(t);
    const uint8_t data[] = {1, 2, 3, 4, 5};

    auto r = d.send({data, sizeof(data)});

    ASSERT_TRUE(r);
    bool found_first_word = false;
    bool found_second_word = false;
    for (const auto& access : t.accesses()) {
        if (access.write && access.offset == 0x10000u) {
            found_first_word = true;
            EXPECT_EQ(access.value, 0x03020105u);
        }
        if (access.write && access.offset == 0x10004u) {
            found_second_word = true;
            EXPECT_EQ(access.value, 0x00000504u);
        }
    }
    EXPECT_TRUE(found_first_word);
    EXPECT_TRUE(found_second_word);
    for (const auto& access : t.accesses()) {
        EXPECT_FALSE(access.write && access.offset == 0x2001Cu);
    }
    ASSERT_FALSE(t.accesses().empty());
    EXPECT_TRUE(t.accesses().back().write);
    EXPECT_EQ(t.accesses().back().offset, 0x30000u);
    EXPECT_EQ(t.accesses().back().value, 0x81u);
}
TEST(HwComDevice, EventTimeoutReturnsZero) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    ComDevice d(t);
    uint8_t data[4]{};
    auto r = d.receive({data, 4}, Timeout::poll());
    ASSERT_TRUE(r);
    EXPECT_EQ(r.value(), 0u);
}
TEST(HwComDevice, ClearErrorStatusOnlyWritesTheErrorRegister) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    ComDevice d(t);

    auto r = d.clear_error_status();

    ASSERT_TRUE(r);
    ASSERT_EQ(t.accesses().size(), 1u);
    EXPECT_TRUE(t.accesses()[0].write);
    EXPECT_EQ(t.accesses()[0].offset, 0x30004u);
    EXPECT_EQ(t.accesses()[0].width, 32u);
    EXPECT_EQ(t.accesses()[0].value, 0xCCCCu);
}
TEST(HwComDevice, EventTimeoutConsumesOnePendingLoopbackFrameWithoutRepeatingIt) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    ComDevice d(t);
    ComConfig config{};
    config.loopback = true;
    ASSERT_TRUE(d.configure(config));
    t.preset(0x30000, 2);
    t.preset(0x20000, 0x40400000u);
    t.preset(0x00000, 0x0000AB01u);
    const uint8_t sent_byte = 0xAB;
    ASSERT_TRUE(d.send({&sent_byte, 1}));
    t.preset(0x30000, 3);
    uint8_t received[4]{};

    auto first = d.receive({received, sizeof(received)}, Timeout::poll());
    auto second = d.receive({received, sizeof(received)}, Timeout::poll());

    ASSERT_TRUE(first);
    EXPECT_EQ(first.value(), 1u);
    EXPECT_EQ(received[0], 0xABu);
    ASSERT_TRUE(second);
    EXPECT_EQ(second.value(), 0u);
}
TEST(HwComDevice, EventTimeoutKeepsPendingLoopbackFrameUntilHardwareIsReady) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    ComDevice d(t);
    ComConfig config{};
    config.loopback = true;
    ASSERT_TRUE(d.configure(config));
    t.preset(0x30000, 2);
    t.preset(0x20000, 0x40400000u);
    t.preset(0x00000, 0x0000AB01u);
    const uint8_t sent_byte = 0xAB;
    ASSERT_TRUE(d.send({&sent_byte, 1}));
    t.preset(0x30000, 2);
    uint8_t received[4]{};

    auto not_ready = d.receive({received, sizeof(received)}, Timeout::poll());
    t.preset(0x30000, 3);
    auto ready = d.receive({received, sizeof(received)}, Timeout::poll());
    auto consumed = d.receive({received, sizeof(received)}, Timeout::poll());

    ASSERT_TRUE(not_ready);
    EXPECT_EQ(not_ready.value(), 0u);
    ASSERT_TRUE(ready);
    EXPECT_EQ(ready.value(), 1u);
    EXPECT_EQ(received[0], 0xABu);
    ASSERT_TRUE(consumed);
    EXPECT_EQ(consumed.value(), 0u);
}
TEST(HwComDevice, DoesNotSwitchOrReadReceiveRamUntilHardwareIsReady) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    t.set_event(1);
    t.preset(0x30000, 2);
    t.preset(0x30004, 0);
    t.preset(0x20000, 0x40400000u);
    t.preset(0x00000, 0x0000AB01u);
    ComDevice d(t);
    uint8_t data[4]{};

    auto r = d.receive({data, sizeof(data)}, Timeout::poll());

    ASSERT_TRUE(r);
    EXPECT_EQ(r.value(), 0u);
    for (const auto& access : t.accesses()) {
        EXPECT_FALSE(access.write && access.offset == 0x30000u && access.value == 0x82u);
        EXPECT_FALSE(!access.write && access.offset == 0x00000u);
    }
}
TEST(HwComDevice, StaleEventDoesNotAbortReceiveBeforeCurrentFrameIsReady) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    t.set_event(1);
    t.queue_reads(0x30000, {2, 3});
    t.queue_reads(0x30004, {0, 0});
    t.preset(0x20000, 0x40400000u);
    t.preset(0x00000, 0x0000AB01u);
    ComDevice d(t);
    uint8_t data[4]{};

    auto result = d.receive({data, sizeof(data)}, Timeout::after_us(1000000));

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), 1u);
    EXPECT_EQ(data[0], 0xABu);
    size_t control_reads = 0;
    size_t receive_rearms = 0;
    for (const auto& access : t.accesses()) {
        if (!access.write && access.offset == 0x30000u) {
            ++control_reads;
        }
        if (access.write && access.offset == 0x30000u && access.value == 0x82u) {
            ++receive_rearms;
        }
    }
    EXPECT_EQ(control_reads, 2u);
    EXPECT_EQ(receive_rearms, 1u);
    ASSERT_EQ(t.waited_timeouts().size(), 2u);
    EXPECT_FALSE(t.waited_timeouts()[0].infinite);
    EXPECT_FALSE(t.waited_timeouts()[1].infinite);
    EXPECT_LE(t.waited_timeouts()[1].microseconds,
              t.waited_timeouts()[0].microseconds);
    EXPECT_LT(t.waited_timeouts()[1].microseconds, 1000000u);
}
TEST(HwComDevice, IgnoresUndefinedHighBitsInReceiveErrorStatus) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    t.set_event(1);
    t.preset(0x30000, 3);
    t.preset(0x30004, 0xABCD0000u);
    t.preset(0x20000, 0x40400000u);
    t.preset(0x00000, 0x0000AB01u);
    ComDevice d(t);
    uint8_t data[4]{};

    auto r = d.receive({data, sizeof(data)}, Timeout::poll());

    ASSERT_TRUE(r);
    EXPECT_EQ(r.value(), 1u);
    EXPECT_EQ(data[0], 0xABu);
}
TEST(HwComDevice, ClearsLowByteReceiveErrorAndRearmsReceive) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    t.set_event(1);
    // 文档规定 D0=0 同时覆盖“未完成”和“接收有错”，因此必须先判断 Error。
    t.preset(0x30000, 2);
    t.preset(0x30004, 0xABCD0004u);
    ComDevice d(t);
    uint8_t data[4]{};

    auto r = d.receive({data, sizeof(data)}, Timeout::poll());

    ASSERT_FALSE(r);
    EXPECT_EQ(r.status().code, StatusCode::ProtocolError);
    const auto& a = t.accesses();
    ASSERT_EQ(a.size(), 4u);
    EXPECT_FALSE(a[0].write);
    EXPECT_EQ(a[0].offset, 0x30000u);
    EXPECT_FALSE(a[1].write);
    EXPECT_EQ(a[1].offset, 0x30004u);
    EXPECT_TRUE(a[2].write);
    EXPECT_EQ(a[2].offset, 0x30004u);
    EXPECT_EQ(a[2].value, 0xCCCCu);
    EXPECT_TRUE(a[3].write);
    EXPECT_EQ(a[3].offset, 0x30000u);
    EXPECT_EQ(a[3].value, 0x82u);
}
TEST(HwComDevice, BufferTooSmallReturnsError) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    t.set_event(1);
    t.preset(0x30000, 3);
    t.preset(0x30004, 0);
    t.preset(0x20000, 0);
    t.preset(0x3000C, 5);
    ComDevice d(t);
    uint8_t data[4]{};
    auto r = d.receive({data, 4}, Timeout::poll());
    EXPECT_FALSE(r);
    EXPECT_EQ(r.status().code, StatusCode::BufferTooSmall);
}
TEST(HwComDevice, LatchesReceivedByteCountBeforeSwitchingPingPongRam) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    t.set_event(1);
    t.preset(0x30000, 3);
    t.preset(0x30004, 0);
    t.preset(0x20000, 0);
    t.preset(0x3000C, 1);
    t.preset(0x00000, 0xABu);
    ComDevice d(t);
    uint8_t data[4]{};

    auto r = d.receive({data, sizeof(data)}, Timeout::poll());

    ASSERT_TRUE(r);
    ASSERT_EQ(r.value(), 1u);
    size_t count_read = t.accesses().size();
    size_t switch_write = t.accesses().size();
    for (size_t i = 0; i < t.accesses().size(); ++i) {
        const auto& access = t.accesses()[i];
        if (!access.write && access.offset == 0x3000Cu) count_read = i;
        if (access.write && access.offset == 0x30000u && access.value == 0x82u) {
            switch_write = i;
        }
    }
    ASSERT_LT(count_read, t.accesses().size());
    ASSERT_LT(switch_write, t.accesses().size());
    EXPECT_LT(count_read, switch_write);
}
TEST(HwComDevice, SwitchesPingPongRamBeforeReadingCompletedFrame) {
    RecordingTransport t;
    ASSERT_TRUE(t.open());
    t.set_event(1);
    t.preset(0x30000, 3);
    t.preset(0x30004, 0);
    t.preset(0x20000, 0);
    t.preset(0x3000C, 1);
    t.preset(0x00000, 0xABu);
    ComDevice d(t);
    uint8_t data[4]{};

    auto r = d.receive({data, sizeof(data)}, Timeout::poll());

    ASSERT_TRUE(r);
    ASSERT_EQ(r.value(), 1u);
    EXPECT_EQ(data[0], 0xABu);
    bool switched = false;
    size_t receive_ram_reads = 0;
    bool read_receive_ram = false;
    for (const auto& access : t.accesses()) {
        if (access.write && access.offset == 0x30000u && access.value == 0x82u) {
            switched = true;
        }
        if (!access.write && access.offset == 0x00000u) {
            ++receive_ram_reads;
            read_receive_ram = true;
            EXPECT_TRUE(switched);
        }
    }
    EXPECT_TRUE(read_receive_ram);
    EXPECT_GE(receive_ram_reads, 1u);
}
