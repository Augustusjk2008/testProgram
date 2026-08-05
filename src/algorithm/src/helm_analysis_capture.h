#pragma once

#include <algorithm/post_run_analysis.h>

#include <QCryptographicHash>
#include <QFile>
#include <QVector>

#include <array>

namespace hwtest::algorithm::mbddf {

// Product-specific capture representation.  It intentionally remains private
// so that callers only see opaque PostRunSample values and generic results.
struct HelmCaptureRecord {
    quint64 acceptedSequence = 0;
    qint64 streamElapsedUs = -1;
    std::array<double, 4> command{};
    std::array<double, 4> feedback{};
    quint32 status = 0;
    quint32 errorCode = 0;
    quint32 timeout = 0;
    quint32 productSequence = 0;
    quint32 serialA = 0;
    quint32 serialB = 0;
    quint64 ddsSequence = 0;
    quint32 batchIndex = 0;
    quint32 flags = 0;
};

struct HelmCaptureData {
    QVector<HelmCaptureRecord> records;
    QVariantMap metadata;
    quint64 acceptedSampleCount = 0;
    quint64 lateSampleCount = 0;
    quint64 invalidInputCount = 0;
    QByteArray normalizedInputSha256;
};

class HelmAnalysisCapture final : public IPostRunAnalysisSession {
public:
    explicit HelmAnalysisCapture(const AnalysisSessionSpec& spec, AnalysisError* error);
    ~HelmAnalysisCapture() override;

    AnalysisError bindIdentity(const AnalysisIdentity& identity) override;
    AnalysisAcceptResult append(const AnalysisIdentity& identity,
                                const PostRunSample& sample) override;
    AnalysisInputSeal seal(const AnalysisTermination& termination) override;

private:
    AnalysisAcceptResult result(AnalysisAppendDisposition disposition,
                                const AnalysisError& error = {}) const;
    bool writeHeader(AnalysisError* error);
    bool writeFooter(AnalysisError* error);
    bool writeRecord(const HelmCaptureRecord& record, AnalysisError* error);
    HelmCaptureRecord decodeSample(const PostRunSample& sample, bool* valid) const;

    AnalysisSessionSpec m_spec;
    QFile m_file;
    AnalysisIdentity m_identity;
    QByteArray m_headerBytes;
    QByteArray m_normalizedHash;
    bool m_open = false;
    bool m_headerWritten = false;
    bool m_bound = false;
    bool m_sealed = false;
    bool m_resourceLimit = false;
    bool m_timeInvalid = false;
    bool m_hasFirstElapsed = false;
    qint64 m_firstElapsedUs = -1;
    qint64 m_lastElapsedUs = -1;
    quint64 m_acceptedCount = 0;
    quint64 m_lateCount = 0;
    quint64 m_invalidInputCount = 0;
    quint64 m_bytesWritten = 0;
    QCryptographicHash* m_hash = nullptr;
    AnalysisInputSeal m_seal;
};

bool readHelmAnalysisCapture(const AnalysisInputSeal& seal,
                             HelmCaptureData* capture,
                             AnalysisError* error);

} // namespace hwtest::algorithm::mbddf
