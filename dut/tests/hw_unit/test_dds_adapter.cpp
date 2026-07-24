#include <gtest/gtest.h>
#include "MB_DDF_HW/DdsAdapter/CallbackExternalEndpoint.h"
using namespace MB_DDF::HW;
TEST(HwDdsAdapter, MapsTimeoutAndPayload) {
    Timeout received{};
    CallbackExternalEndpoint endpoint([](BufferView v) { return Result<size_t>(v.size); },
                                      [&](MutableBufferView, Timeout t) {
                                          received = t;
                                          return Result<size_t>(size_t{0});
                                      },
                                      [] { return size_t{64}; });
    uint8_t data[4]{};
    EXPECT_TRUE(endpoint.send(data, 4));
    EXPECT_EQ(endpoint.receive(data, 4, 123), 0);
    EXPECT_EQ(received.microseconds, 123u);
    EXPECT_EQ(endpoint.mtu(), 64u);
    EXPECT_EQ(endpoint.control(0, nullptr, 0), -1);
}
