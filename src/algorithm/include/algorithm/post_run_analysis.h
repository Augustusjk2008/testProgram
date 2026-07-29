#pragma once

#include <QByteArray>
#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <QVector>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>

namespace hwtest::algorithm::mbddf {

// This header is intentionally independent of BIZ, HAL, QObject, and product
// protocol field names.  The application layer owns orchestration; concrete
// algorithm implementations interpret opaque values only in private sources.

struct AnalysisError {
    QString code;
    QString message;

    bool ok() const noexcept { return code.isEmpty(); }
};

struct AnalysisIdentity {
    QString taskId;
    quint64 analysisGeneration = 0;

    bool isValid() const noexcept { return !taskId.isEmpty(); }
};

struct PostRunSample {
    qint64 streamElapsedUs = -1;
    QString sourceId;
    QVariantMap values;
    QVariantMap tags;
};

struct AnalysisSessionSpec {
    QString algorithmId;
    QString configId;
    QString sourceStepId;
    QString captureFilePath;
    QString schemaVersion = QStringLiteral("1");
    QVariantMap effectiveRunParameters;
    QVariantMap metadata;
    quint64 maxCaptureBytes = 536870912;
    quint64 maxInputSamples = 5000000;
    qint64 maxAnalysisDurationUs = 3600000000LL;
};

enum class AnalysisTerminationKind {
    Finished,
    Stopped,
    Error,
    Cancelled,
};

struct AnalysisTermination {
    AnalysisTerminationKind kind = AnalysisTerminationKind::Stopped;
    QString reasonCode;
    QString message;
    qint64 endStreamElapsedUs = -1;
};

enum class AnalysisAppendDisposition {
    Accepted,
    IgnoredIdentity,
    IgnoredSource,
    Rejected,
    ResourceLimit,
    Late,
};

struct AnalysisAcceptResult {
    AnalysisAppendDisposition disposition = AnalysisAppendDisposition::Rejected;
    AnalysisError error;
    QString message;
    quint64 acceptedSampleCount = 0;
    quint64 lateSampleCount = 0;
    bool accepted = false;
    bool late = false;
};

struct AnalysisInputSeal {
    AnalysisError error;
    bool valid = false;
    QString message;
    AnalysisIdentity identity;
    QString algorithmId;
    QString configId;
    QString schemaVersion;
    QVariantMap effectiveRunParameters;
    QVariantMap metadata;
    QString captureFilePath;
    quint16 captureFormatVersion = 0;
    quint16 captureRecordBytes = 0;
    quint64 acceptedSampleCount = 0;
    quint64 lateSampleCount = 0;
    bool resourceLimitReached = false;
    QByteArray normalizedInputSha256;
    AnalysisTermination termination;
};

enum class AnalysisState {
    None,
    Capturing,
    Queued,
    Validating,
    Preprocessing,
    Calculating,
    Persisting,
    Completed,
    Partial,
    Unavailable,
    Failed,
    Cancelled,
};

enum class AnalysisChannelState {
    NotApplicable,
    Completed,
    Partial,
    Unavailable,
};

enum class AnalysisMetricStatus {
    Valid,
    Unavailable,
    UpperBound,
    Indeterminate,
    NotCovered,
    NotSettled,
    AboveObservedRange,
};

enum class AnalysisPointStatus {
    Valid,
    UpperBound,
    Indeterminate,
    NotCovered,
    WeakExcitation,
    Invalid,
};

struct AnalysisMetric {
    QString key;
    QString label;
    QString unit;
    AnalysisMetricStatus status = AnalysisMetricStatus::Unavailable;
    bool hasValue = false;
    double value = 0.0;
    QString detail;
};

struct AnalysisBodePoint {
    double frequencyHz = 0.0;
    bool hasMagnitude = false;
    double magnitudeDb = 0.0;
    bool hasPhase = false;
    double phaseDeg = 0.0;
    AnalysisPointStatus status = AnalysisPointStatus::Invalid;
    QString detail;
};

struct AnalysisChannelResult {
    int channel = -1;
    bool enabled = false;
    AnalysisChannelState state = AnalysisChannelState::NotApplicable;
    QString reasonCode;
    QString message;
    QVector<QString> warnings;
    QVector<AnalysisMetric> commonMetrics;
    QVector<AnalysisMetric> waveformMetrics;
    QVector<AnalysisBodePoint> bodePoints;
    QVariantMap diagnostics;
};

struct AnalysisProgress {
    AnalysisIdentity identity;
    AnalysisState state = AnalysisState::None;
    int percent = 0;
    int channel = -1;
    QString stage;
    QString message;
};

class AnalysisCancelToken {
public:
    AnalysisCancelToken();

    void requestCancel() const noexcept;
    bool isCancellationRequested() const noexcept;

private:
    std::shared_ptr<std::atomic_bool> m_cancelled;
};

struct AnalysisResult {
    QString schemaVersion = QStringLiteral("1");
    QString analyzerId;
    QString analyzerVersion;
    AnalysisIdentity identity;
    AnalysisState state = AnalysisState::None;
    QString reasonCode;
    QString message;
    QVariantMap effectiveRunParameters;
    quint64 acceptedSampleCount = 0;
    quint64 lateSampleCount = 0;
    QByteArray normalizedInputSha256;
    qint64 generatedAtUtcUs = 0;
    bool reproducible = false;
    QString sourceArtifactPath;
    QByteArray sourceArtifactSha256;
    AnalysisTermination termination;
    QVector<AnalysisChannelResult> channels;
    QVariantMap diagnostics;
};

using AnalysisProgressCallback = std::function<void(const AnalysisProgress&)>;

class IPostRunAnalysisSession {
public:
    virtual ~IPostRunAnalysisSession() = default;

    virtual AnalysisError bindIdentity(const AnalysisIdentity& identity) = 0;
    virtual AnalysisAcceptResult append(const AnalysisIdentity& identity,
                                        const PostRunSample& sample) = 0;
    virtual AnalysisInputSeal seal(const AnalysisTermination& termination) = 0;

    bool bindIdentity(const AnalysisIdentity& identity, QString* error)
    {
        const AnalysisError result = bindIdentity(identity);
        if (error != nullptr) *error = result.message;
        return result.ok();
    }
};

class IPostRunAnalyzer {
public:
    virtual ~IPostRunAnalyzer() = default;

    virtual AnalysisResult analyze(const AnalysisInputSeal& input,
                                   const AnalysisProgressCallback& progress,
                                   const AnalysisCancelToken& cancel) = 0;
};

bool supportsPostRunAnalysis(const QString& algorithmId);
std::unique_ptr<IPostRunAnalysisSession> createPostRunAnalysisSession(
    const AnalysisSessionSpec& spec, QString* error = nullptr);
std::unique_ptr<IPostRunAnalysisSession> createPostRunAnalysisSession(
    const AnalysisSessionSpec& spec, AnalysisError* error);
std::unique_ptr<IPostRunAnalysisSession> createPostRunAnalysisSession(
    const AnalysisSessionSpec& spec, std::nullptr_t);
std::unique_ptr<IPostRunAnalyzer> createPostRunAnalyzer(
    const QString& algorithmId, QString* error = nullptr);
std::unique_ptr<IPostRunAnalyzer> createPostRunAnalyzer(
    const QString& algorithmId, AnalysisError* error);
std::unique_ptr<IPostRunAnalyzer> createPostRunAnalyzer(
    const QString& algorithmId, std::nullptr_t);

AnalysisState reduceAnalysisState(const QVector<AnalysisChannelResult>& channels,
                                  bool infrastructureFailed = false,
                                  bool cancelled = false);

QString analysisStateName(AnalysisState state);
QString analysisChannelStateName(AnalysisChannelState state);
QString analysisMetricStatusName(AnalysisMetricStatus status);
QString analysisPointStatusName(AnalysisPointStatus status);
QString analysisTerminationKindName(AnalysisTerminationKind kind);

QByteArray serializeAnalysisResultJson(const AnalysisResult& result,
                                       QString* error = nullptr);
bool serializeAnalysisResultJson(const AnalysisResult& result,
                                 QByteArray* utf8,
                                 AnalysisError* error = nullptr);

} // namespace hwtest::algorithm::mbddf
