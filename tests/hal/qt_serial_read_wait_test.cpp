#include "qt_serial_read_wait.h"

#include <gtest/gtest.h>

#include <vector>

using namespace hwtest::hal;

namespace {

struct FakeSerialRead {
    qint64 available = 0;
    qint64 elapsed = 0;
    qint64 availableAfterFirstWait = 0;
    SerialReadWaitError serialError = SerialReadWaitError::Timeout;
    bool waitResult = false;
    int clearCount = 0;
    std::vector<int> waits;

    SerialReadWaitCallbacks callbacks()
    {
        return {
            [this] { return available; },
            [this](int waitMs) {
                waits.push_back(waitMs);
                elapsed += waitMs;
                if (!waitResult && serialError != SerialReadWaitError::IoError) {
                    serialError = SerialReadWaitError::Timeout;
                }
                if (waits.size() == 1 && availableAfterFirstWait > 0) {
                    available = availableAfterFirstWait;
                }
                return waitResult;
            },
            [this] { return serialError; },
            [this] {
                ++clearCount;
                serialError = SerialReadWaitError::None;
            },
            [this] { return elapsed; },
        };
    }
};

TEST(QtSerialReadWaitTest, AcceptsBytesBufferedDuringQtFalseTimeout)
{
    FakeSerialRead serial;
    serial.availableAfterFirstWait = 128;

    EXPECT_EQ(waitForSerialRead(serial.callbacks(), 20),
              SerialReadWaitOutcome::Ready);
    EXPECT_EQ(serial.waits, std::vector<int>({kQtSerialReadWaitSliceMs}));
    EXPECT_EQ(serial.clearCount, 1);
}

TEST(QtSerialReadWaitTest, UsesFiveMillisecondSlicesUntilOverallTimeout)
{
    FakeSerialRead serial;

    EXPECT_EQ(waitForSerialRead(serial.callbacks(), 12),
              SerialReadWaitOutcome::Timeout);
    EXPECT_EQ(serial.waits, std::vector<int>({5, 5, 2}));
    EXPECT_EQ(serial.clearCount, 2);
}

TEST(QtSerialReadWaitTest, StopsImmediatelyOnIoError)
{
    FakeSerialRead serial;
    serial.serialError = SerialReadWaitError::IoError;

    EXPECT_EQ(waitForSerialRead(serial.callbacks(), 20),
              SerialReadWaitOutcome::IoError);
    EXPECT_EQ(serial.waits, std::vector<int>({kQtSerialReadWaitSliceMs}));
    EXPECT_EQ(serial.clearCount, 0);
}

TEST(QtSerialReadWaitTest, DoesNotHideIoErrorBehindBufferedBytes)
{
    FakeSerialRead serial;
    serial.serialError = SerialReadWaitError::IoError;
    serial.availableAfterFirstWait = 64;

    EXPECT_EQ(waitForSerialRead(serial.callbacks(), 20),
              SerialReadWaitOutcome::IoError);
    EXPECT_EQ(serial.waits, std::vector<int>({kQtSerialReadWaitSliceMs}));
    EXPECT_EQ(serial.clearCount, 0);
}

TEST(QtSerialReadWaitTest, ReturnsReadyWithoutWaitingForAlreadyBufferedBytes)
{
    FakeSerialRead serial;
    serial.available = 53;

    EXPECT_EQ(waitForSerialRead(serial.callbacks(), 20),
              SerialReadWaitOutcome::Ready);
    EXPECT_TRUE(serial.waits.empty());
    EXPECT_EQ(serial.clearCount, 0);
}

} // namespace
