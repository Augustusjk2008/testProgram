#include <gtest/gtest.h>

#include "MB_DDF_HW/Os/Fd.h"
#include "MB_DDF_HW/Transport/NullTransport.h"
#include "MB_DDF_HW/Transport/SpidevTransport.h"
#include "MB_DDF_HW/Transport/XdmaTransport.h"

#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace MB_DDF::HW;

namespace {
class TemporaryXdmaNodes {
public:
    bool create() {
        char directory_template[] = "/tmp/mb_ddf_xdma_XXXXXX";
        char* directory = ::mkdtemp(directory_template);
        if (directory == nullptr) {
            return false;
        }
        directory_path_ = directory;
        base_path_ = directory_path_ + "/xdma0";
        user_path_ = base_path_ + "_user";
        event_path_ = base_path_ + "_events_0";

        Os::Fd user_fd(::open(user_path_.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600));
        if (!user_fd.valid() || ::ftruncate(user_fd.get(), 4096) != 0) {
            return false;
        }
        return ::mkfifo(event_path_.c_str(), 0600) == 0;
    }

    ~TemporaryXdmaNodes() {
        if (!event_path_.empty()) {
            ::unlink(event_path_.c_str());
        }
        if (!user_path_.empty()) {
            ::unlink(user_path_.c_str());
        }
        if (!directory_path_.empty()) {
            ::rmdir(directory_path_.c_str());
        }
    }

    const std::string& base_path() const {
        return base_path_;
    }
    const std::string& event_path() const {
        return event_path_;
    }

private:
    std::string directory_path_;
    std::string base_path_;
    std::string user_path_;
    std::string event_path_;
};
} // namespace

TEST(HwSpidevTransport, RestoreBeforeOpenReturnsNotOpen) {
    SpidevTransport transport;

    const auto result = transport.restore_configuration();

    EXPECT_FALSE(result);
    EXPECT_EQ(result.status().code, StatusCode::NotOpen);
}

TEST(HwNullTransport, ReadBeforeOpenReturnsNotOpen) {
    NullTransport transport;

    auto result = transport.read32(0);

    EXPECT_FALSE(result);
    EXPECT_EQ(result.status().code, StatusCode::NotOpen);
}

TEST(HwNullTransport, Write32AndRead32RoundTrip) {
    NullTransport transport;
    ASSERT_TRUE(transport.open());

    ASSERT_TRUE(transport.write32(4, 0x11223344u));
    auto result = transport.read32(4);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), 0x11223344u);
}

TEST(HwNullTransport, OutOfRangeAccessReturnsInvalidArgument) {
    NullTransport transport(8);
    ASSERT_TRUE(transport.open());

    auto write_result = transport.write32(8, 0xA5A5A5A5u);
    auto read_result = transport.read8(8);

    EXPECT_FALSE(write_result);
    EXPECT_EQ(write_result.status().code, StatusCode::InvalidArgument);
    EXPECT_FALSE(read_result);
    EXPECT_EQ(read_result.status().code, StatusCode::InvalidArgument);
}

TEST(HwNullTransport, ReadAfterCloseReturnsNotOpen) {
    NullTransport transport;
    ASSERT_TRUE(transport.open());
    transport.close();

    auto result = transport.read8(0);

    EXPECT_FALSE(result);
    EXPECT_EQ(result.status().code, StatusCode::NotOpen);
}

TEST(HwXdmaTransport, WaitEventConsumesDriverEventCount) {
    TemporaryXdmaNodes nodes;
    ASSERT_TRUE(nodes.create());

    XdmaTransport transport({nodes.base_path(), 0, 4096, -1, -1, 0});
    ASSERT_TRUE(transport.open());
    Os::Fd writer_fd(::open(nodes.event_path().c_str(), O_WRONLY | O_NONBLOCK));
    ASSERT_TRUE(writer_fd.valid());
    const uint32_t event_count = 7;
    ASSERT_EQ(::write(writer_fd.get(), &event_count, sizeof(event_count)),
              static_cast<ssize_t>(sizeof(event_count)));

    const auto observed = transport.wait_event(Timeout::after_us(100000));
    transport.close();
    ASSERT_TRUE(transport.open());
    const auto after_reopen = transport.wait_event(Timeout::poll());

    ASSERT_TRUE(observed);
    EXPECT_EQ(observed.value(), static_cast<int>(event_count));
    ASSERT_TRUE(after_reopen);
    EXPECT_EQ(after_reopen.value(), 0);
}
