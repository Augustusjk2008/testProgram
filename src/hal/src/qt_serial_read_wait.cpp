#include "qt_serial_read_wait.h"

namespace hwtest::hal {

SerialReadWaitOutcome waitForSerialRead(const SerialReadWaitCallbacks& callbacks,
                                        int timeoutMs)
{
    const int normalizedTimeoutMs = qMax(0, timeoutMs);
    const qint64 startedAtMs = callbacks.elapsedMs();
    bool firstWait = true;

    for (;;) {
        if (callbacks.bytesAvailable() > 0) {
            return SerialReadWaitOutcome::Ready;
        }

        const qint64 elapsedMs = qMax<qint64>(0, callbacks.elapsedMs() - startedAtMs);
        const int remainingMs = static_cast<int>(
            qMax<qint64>(0, static_cast<qint64>(normalizedTimeoutMs) - elapsedMs));
        if (!firstWait && remainingMs == 0) {
            return SerialReadWaitOutcome::Timeout;
        }

        const int waitMs = qMin(kQtSerialReadWaitSliceMs, remainingMs);
        firstWait = false;
        // Qt 5.15 on Windows can buffer a multipart response while reporting
        // a timeout, so the observable buffer/error state is authoritative.
        callbacks.waitForReadyRead(waitMs);

        const SerialReadWaitError error = callbacks.error();
        if (error == SerialReadWaitError::IoError) {
            return SerialReadWaitOutcome::IoError;
        }
        if (callbacks.bytesAvailable() > 0) {
            if (error == SerialReadWaitError::Timeout) {
                callbacks.clearError();
            }
            return SerialReadWaitOutcome::Ready;
        }

        const qint64 elapsedAfterWaitMs =
            qMax<qint64>(0, callbacks.elapsedMs() - startedAtMs);
        if (elapsedAfterWaitMs >= normalizedTimeoutMs) {
            return SerialReadWaitOutcome::Timeout;
        }
        if (error == SerialReadWaitError::Timeout) {
            callbacks.clearError();
        }
    }
}

} // namespace hwtest::hal
