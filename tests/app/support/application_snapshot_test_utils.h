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
    EXPECT_EQ(left.descriptor.configId, right.descriptor.configId);
    EXPECT_EQ(left.descriptor.productModel, right.descriptor.productModel);
    EXPECT_EQ(left.descriptor.productName, right.descriptor.productName);
    EXPECT_EQ(left.descriptor.configVersion, right.descriptor.configVersion);
    EXPECT_EQ(left.descriptor.stepId, right.descriptor.stepId);
    EXPECT_EQ(left.descriptor.testItemId, right.descriptor.testItemId);
    EXPECT_EQ(left.descriptor.algorithmId, right.descriptor.algorithmId);
    EXPECT_EQ(left.descriptor.title, right.descriptor.title);
    EXPECT_EQ(left.descriptor.description, right.descriptor.description);
    EXPECT_EQ(left.descriptor.supportedRunModes, right.descriptor.supportedRunModes);
    EXPECT_EQ(left.descriptor.stoppable, right.descriptor.stoppable);
    ASSERT_EQ(left.descriptor.measurements.size(), right.descriptor.measurements.size());
    for (int index = 0; index < left.descriptor.measurements.size(); ++index) {
        EXPECT_EQ(left.descriptor.measurements.at(index).id,
                  right.descriptor.measurements.at(index).id);
        EXPECT_EQ(left.descriptor.measurements.at(index).label,
                  right.descriptor.measurements.at(index).label);
        EXPECT_EQ(left.descriptor.measurements.at(index).unit,
                  right.descriptor.measurements.at(index).unit);
        EXPECT_EQ(left.descriptor.measurements.at(index).primary,
                  right.descriptor.measurements.at(index).primary);
    }
}

} // namespace hwtest::app::test
