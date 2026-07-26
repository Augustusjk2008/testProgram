#include "hal_error_mapper.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>

using namespace hwtest::hal;

TEST(HalErrorMapperTest, MapsAdapterStatusAndCopiesContext)
{
    const HalAdapterStatus status {HAL_ADAPTER_TIMEOUT, 42, "Timed out"};
    const QVariantMap detail {{QStringLiteral("retry"), 3}};

    const HalStatus mapped = fromAdapterStatus(status,
                                               QStringLiteral("serial.read"),
                                               QStringLiteral("dev-1"),
                                               QStringLiteral("SERIAL_A"),
                                               detail);

    EXPECT_EQ(mapped.code, HalStatusCode::Timeout);
    EXPECT_EQ(mapped.error.code, HalStatusCode::Timeout);
    EXPECT_EQ(mapped.error.message, QStringLiteral("Timed out"));
    EXPECT_EQ(mapped.error.deviceId, QStringLiteral("dev-1"));
    EXPECT_EQ(mapped.error.resourceId, QStringLiteral("SERIAL_A"));
    EXPECT_EQ(mapped.error.adapterCode, QStringLiteral("42"));
    EXPECT_EQ(mapped.error.detail.value(QStringLiteral("retry")).toInt(), 3);
}

TEST(HalErrorMapperTest, MapsUnknownAdapterStatusToAdapterError)
{
    EXPECT_EQ(mapAdapterStatus(999), HalStatusCode::AdapterError);
}

TEST(HalErrorMapperTest, BoundsVendorMessageWithoutRequiringNullTermination)
{
    HalAdapterStatus adapterStatus{};
    adapterStatus.code = HAL_ADAPTER_IO_ERROR;
    std::fill(std::begin(adapterStatus.message),
              std::end(adapterStatus.message),
              'x');

    const HalStatus status = fromAdapterStatus(
        adapterStatus, QStringLiteral("adapter.digitalWrite"));

    EXPECT_EQ(status.error.message.size(), HAL_ADAPTER_MAX_TEXT);
    EXPECT_EQ(status.error.message, QString(HAL_ADAPTER_MAX_TEXT, QLatin1Char('x')));
}

TEST(HalErrorMapperTest, MakeErrorPopulatesTopLevelAndNestedCode)
{
    const HalStatus status = makeError(HalStatusCode::NotFound,
                                       QStringLiteral("hal.openDevice"),
                                       QStringLiteral("Device missing"),
                                       QStringLiteral("dev-1"),
                                       QStringLiteral("CANFD_A"));

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code, HalStatusCode::NotFound);
    EXPECT_EQ(status.error.code, HalStatusCode::NotFound);
    EXPECT_EQ(status.error.operation, QStringLiteral("hal.openDevice"));
    EXPECT_EQ(status.error.message, QStringLiteral("Device missing"));
}
