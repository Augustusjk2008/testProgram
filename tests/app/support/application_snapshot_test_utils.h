#pragma once

#include <app/test_application_controller.h>

#include <gtest/gtest.h>

namespace hwtest::app::test {

inline void expectSemanticallyEquivalentSnapshots(const ApplicationSnapshot& left,
                                                   const ApplicationSnapshot& right)
{
    EXPECT_EQ(left.phase, right.phase);
    EXPECT_EQ(left.testState, right.testState);
    EXPECT_EQ(left.controlResourceId, right.controlResourceId);
    EXPECT_EQ(left.providerId, right.providerId);
    EXPECT_EQ(left.serialPortName, right.serialPortName);
    EXPECT_EQ(left.taskId.isEmpty(), right.taskId.isEmpty());
    EXPECT_EQ(left.stepId, right.stepId);
    EXPECT_EQ(left.testItemId, right.testItemId);
    EXPECT_EQ(left.algorithmId, right.algorithmId);
    // Stop requests can arrive at different progress ticks across frontends.
    // Full progress-field projection is covered by WebProtocolTest.
    EXPECT_EQ(left.hasResult, right.hasResult);
    EXPECT_EQ(left.verdict, right.verdict);
    EXPECT_EQ(left.errorCode, right.errorCode);
    EXPECT_EQ(left.message, right.message);
    EXPECT_EQ(left.attempts, right.attempts);
    EXPECT_EQ(left.rawData, right.rawData);
}

} // namespace hwtest::app::test
