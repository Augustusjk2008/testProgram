#pragma once

#include <QString>
#include <QVector>

#include <memory>

namespace hwtest::biz {
class IAlgorithmExecutor;
}

namespace hwtest::algorithm::mbddf {
class IByteTransport;
}

namespace hwtest::app {

struct MbdDfAlgorithmRegistration {
    QString algorithmId;
    QString requestProfileId;
    QString responseProfileId;
    QString commandName;
    QString postRunAnalyzerId;
    QString postRunAnalysisSchemaVersion;
};

const QVector<MbdDfAlgorithmRegistration>& mbddfAlgorithmRegistry();
const MbdDfAlgorithmRegistration* findMbdDfAlgorithm(const QString& algorithmId);
bool isSupportedMbdDfAlgorithm(const QString& algorithmId);
std::unique_ptr<hwtest::biz::IAlgorithmExecutor> createMbdDfExecutor(
    const QString& algorithmId,
    std::unique_ptr<hwtest::algorithm::mbddf::IByteTransport> transport);

} // namespace hwtest::app
