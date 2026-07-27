#include <gtest/gtest.h>

#include "MB_DDF_Demo/HardwareExamples.h"
#include "MB_DDF_HW/Endpoint/IByteEndpoint.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace MB_DDF;

namespace {

class LoopbackEndpoint final : public HW::IByteEndpoint {
public:
    explicit LoopbackEndpoint(bool stale_second_bank)
        : stale_second_bank_(stale_second_bank) {}

    HW::Result<size_t> send(HW::BufferView data) override {
        sent_.emplace_back(data.data, data.data + data.size);
        return data.size;
    }

    HW::Result<size_t> receive(HW::MutableBufferView buffer, HW::Timeout) override {
        ++receive_count_;
        const auto& current = sent_.back();
        const auto& response = stale_second_bank_ && receive_count_ % 2u == 0u
                                   ? sent_.front()
                                   : current;
        if (response.size() > buffer.size) {
            return HW::Status::error(HW::StatusCode::BufferTooSmall, 0,
                                     "fake receive buffer is too small");
        }
        std::copy(response.begin(), response.end(), buffer.data);
        return response.size();
    }

    size_t mtu() const override { return 65536; }

    const std::vector<std::vector<uint8_t>>& sent() const { return sent_; }
    size_t receive_count() const { return receive_count_; }

private:
    bool stale_second_bank_{false};
    size_t receive_count_{0};
    std::vector<std::vector<uint8_t>> sent_;
};

bool all_payloads_unique(const std::vector<std::vector<uint8_t>>& payloads) {
    for (size_t left = 0; left < payloads.size(); ++left) {
        for (size_t right = left + 1; right < payloads.size(); ++right) {
            if (payloads[left] == payloads[right]) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

TEST(HwComLoopbackWorkflow, PassesAllRoundsWhenBothReceiveBanksReturnCurrentFrame) {
    constexpr size_t kIterations = 16;
    LoopbackEndpoint endpoint(false);

    const bool result = Demo::TestHooks::run_com_loopback_iterations(
        endpoint, kIterations, 1000000);

    EXPECT_TRUE(result);
    ASSERT_EQ(endpoint.sent().size(), kIterations);
    EXPECT_EQ(endpoint.receive_count(), kIterations);
    EXPECT_TRUE(all_payloads_unique(endpoint.sent()));
}

TEST(HwComLoopbackWorkflow, FailsButCompletesAllRoundsWhenOneReceiveBankIsStale) {
    constexpr size_t kIterations = 16;
    LoopbackEndpoint endpoint(true);

    const bool result = Demo::TestHooks::run_com_loopback_iterations(
        endpoint, kIterations, 1000000);

    EXPECT_FALSE(result);
    EXPECT_EQ(endpoint.sent().size(), kIterations);
    EXPECT_EQ(endpoint.receive_count(), kIterations);
    EXPECT_TRUE(all_payloads_unique(endpoint.sent()));
}
