#include "hal/hal_factory.h"
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

TEST(HalTypesTest, EnumeratesSerialPortsAsStableDescriptors)
{
    const QVector<SerialPortDescriptor> ports = availableSerialPorts();

    QString previousPortName;
    for (const SerialPortDescriptor& port : ports) {
        EXPECT_FALSE(port.portName.trimmed().isEmpty());
        if (!previousPortName.isEmpty()) {
            EXPECT_LE(QString::compare(previousPortName,
                                       port.portName,
                                       Qt::CaseInsensitive),
                      0);
        }
        previousPortName = port.portName;
    }
}
