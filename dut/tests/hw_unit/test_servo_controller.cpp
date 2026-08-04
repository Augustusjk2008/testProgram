#include "HelmControl/Servo/ServoController.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

struct FeedbackCase {
    int16_t raw;
    double expected_degrees;
};

TEST(ServoControllerTest, ConvertsSignedAd7606FeedbackWithApprovedCalibration) {
    constexpr std::array<FeedbackCase, 4> cases{{
        {-32768, 148.41225108720531},
        {0, 43.125},
        {13422, -0.0013880643453834671},
        {32767, -62.159037975294687},
    }};

    for (const auto& sample : cases) {
        HelmControl controller;
        controller.update(sample.raw, 0.0);
        EXPECT_NEAR(controller.fdb_out, sample.expected_degrees, 1.0e-9)
            << "raw=" << sample.raw;
    }
}

} // namespace
