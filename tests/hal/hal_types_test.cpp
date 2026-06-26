#include "hal/hal_types.h"

#include <gtest/gtest.h>

using namespace hwtest::hal;

TEST(HalTypesTest, RegistersMetaTypesAndFormatsEnums)
{
    registerHalMetaTypes();

    EXPECT_EQ(toString(HalStatusCode::Ok), QStringLiteral("Ok"));
    EXPECT_EQ(toString(AnalogUnit::RawCount), QStringLiteral("RawCount"));
    EXPECT_EQ(toString(DigitalLevel::Unknown), QStringLiteral("Unknown"));
    EXPECT_EQ(toString(SerialParity::Even), QStringLiteral("Even"));
    EXPECT_EQ(toString(SerialStopBits::OneAndHalf), QStringLiteral("OneAndHalf"));
    EXPECT_EQ(toString(SerialFlowControl::Software), QStringLiteral("Software"));
}
