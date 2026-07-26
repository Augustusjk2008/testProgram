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

struct TestRunOptions {
    QString mode = QStringLiteral("single");
    int intervalMs = 1000;
    quint64 maxCycles = 1;
};

struct ApplicationSample {
    QString taskId;
    QString stepId;
    QString channelId;
    qint64 timestampUs = 0;
    quint64 cycleIndex = 1;
    QVariantMap values;
    QVariantMap tags;
};

struct TestMeasurementDescriptor {
    QString id;
    QString label;
    QString unit;
    bool primary = false;
};

struct TestDescriptor {
    QString configId;
    QString productModel;
    QString productName;
    QString configVersion;
    QString stepId;
    QString testItemId;
    QString algorithmId;
    QString title;
    QString description;
    QVector<QString> supportedRunModes;
    QVector<TestMeasurementDescriptor> measurements;
};

struct DigitalSwitchDescriptor {
    QString switchId;
    int dutBit = -1;
    QString label;
    QString activeLevel;
};

struct DigitalStimulusSnapshot {
    bool available = false;
    bool configured = false;
    QVector<DigitalSwitchDescriptor> switches;
    quint64 appliedMask = 0;
    quint64 revision = 0;
    qint64 lastWriteTimestampUs = 0;
    int settlingMs = 0;
    QString errorCode;
    QString message;
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
    QString runMode = QStringLiteral("single");
    int intervalMs = 1000;
    quint64 maxCycles = 1;
    quint64 cycleIndex = 0;
    quint64 sampleCount = 0;
    TestDescriptor descriptor;
    DigitalStimulusSnapshot digitalStimulus;
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
    ActionResult start(const TestRunOptions& options);
    ActionResult pause();
    ActionResult resume();
    ActionResult stop(int timeoutMs = 5000);
    // Completion is reported by stopCompleted on this object's affinity thread.
    ActionResult stopAsync(int timeoutMs = 5000);
    ActionResult waitForTerminal(int timeoutMs = -1);
    ActionResult setDigitalStimulus(const QString& switchId,
                                    bool active,
                                    quint64 expectedRevision);
    ActionResult resetDigitalStimulus();
    ActionResult shutdown();
    ApplicationSnapshot snapshot() const;

signals:
    void snapshotChanged(const hwtest::app::ApplicationSnapshot& snapshot);
    void sampleReceived(const hwtest::app::ApplicationSample& sample);
    void stopCompleted(const hwtest::app::ActionResult& result);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace hwtest::app

Q_DECLARE_METATYPE(hwtest::app::ActionResult)
Q_DECLARE_METATYPE(hwtest::app::ApplicationSample)
Q_DECLARE_METATYPE(hwtest::app::ApplicationSnapshot)
Q_DECLARE_METATYPE(hwtest::app::SerialPortInfo)
Q_DECLARE_METATYPE(hwtest::app::TestDescriptor)
Q_DECLARE_METATYPE(hwtest::app::TestMeasurementDescriptor)
Q_DECLARE_METATYPE(hwtest::app::TestRunOptions)
Q_DECLARE_METATYPE(hwtest::app::DigitalSwitchDescriptor)
Q_DECLARE_METATYPE(hwtest::app::DigitalStimulusSnapshot)
Q_DECLARE_METATYPE(QVector<hwtest::app::SerialPortInfo>)
