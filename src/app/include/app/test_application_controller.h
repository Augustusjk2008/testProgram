#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QVector>

#include <memory>

namespace hwtest::app {

struct ActionResult {
    bool ok = true;
    QString code;
    QString message;
};

struct ControlResource {
    QString resourceId;
    QString providerId;
};

struct SerialPortInfo {
    QString portName;
    QString description;
    QString manufacturer;
    QString serialNumber;
    QString systemLocation;
};

struct ApplicationSnapshot {
    QString phase = QStringLiteral("empty");
    QString testState = QStringLiteral("Uninitialized");
    QString controlResourceId;
    QString providerId;
    QString serialPortName;
    QString taskId;
    QString stepId;
    QString testItemId;
    QString algorithmId;
    int progress = 0;
    QString progressStep;
    bool hasResult = false;
    QString verdict;
    QString errorCode;
    QString message;
    int attempts = 0;
    QVariantMap rawData;
};

class TestApplicationController final : public QObject {
    Q_OBJECT

public:
    // All actions and snapshot reads must run on this QObject's affinity thread.
    // GUI/Web adapters must marshal calls with a queued invocation.
    explicit TestApplicationController(QObject* parent = nullptr);
    ~TestApplicationController() override;

    ActionResult loadConfigurations(const QString& testConfigPath,
                                    const QString& halConfigPath);
    QVector<ControlResource> availableControls() const;
    QVector<SerialPortInfo> availableSerialPorts() const;
    ActionResult selectControl(const QString& resourceId);
    ActionResult selectSerialPort(const QString& portName);
    ActionResult prepare();
    ActionResult start();
    ActionResult pause();
    ActionResult resume();
    ActionResult stop(int timeoutMs = 5000);
    // Completion is reported by stopCompleted on this object's affinity thread.
    ActionResult stopAsync(int timeoutMs = 5000);
    ActionResult waitForTerminal(int timeoutMs = -1);
    ActionResult shutdown();
    ApplicationSnapshot snapshot() const;

signals:
    void snapshotChanged(const hwtest::app::ApplicationSnapshot& snapshot);
    void stopCompleted(const hwtest::app::ActionResult& result);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace hwtest::app

Q_DECLARE_METATYPE(hwtest::app::ActionResult)
Q_DECLARE_METATYPE(hwtest::app::ApplicationSnapshot)
Q_DECLARE_METATYPE(hwtest::app::SerialPortInfo)
Q_DECLARE_METATYPE(QVector<hwtest::app::SerialPortInfo>)
