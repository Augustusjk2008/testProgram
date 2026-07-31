#pragma once

#include <QtGlobal>

#include <functional>

namespace hwtest::hal {

inline constexpr int kQtSerialReadWaitSliceMs = 5;

enum class SerialReadWaitError {
    None,
    Timeout,
    IoError,
};

enum class SerialReadWaitOutcome {
    Ready,
    Timeout,
    IoError,
};

struct SerialReadWaitCallbacks {
    std::function<qint64()> bytesAvailable;
    std::function<bool(int)> waitForReadyRead;
    std::function<SerialReadWaitError()> error;
    std::function<void()> clearError;
    std::function<qint64()> elapsedMs;
};

SerialReadWaitOutcome waitForSerialRead(const SerialReadWaitCallbacks& callbacks,
                                        int timeoutMs);

} // namespace hwtest::hal

