#include <algorithm/post_run_analysis.h>

#include "helm_analysis_capture.h"
#include "helm_performance_analyzer.h"

namespace hwtest::algorithm::mbddf {
namespace {

constexpr const char* kHelmAlgorithmId = "mbddf.helm_stream";

AnalysisError unsupported(const QString& algorithmId)
{
    return {QStringLiteral("unsupported_algorithm"),
            QStringLiteral("Post-run analysis is not supported for '%1'").arg(algorithmId)};
}

} // namespace

AnalysisCancelToken::AnalysisCancelToken()
    : m_cancelled(std::make_shared<std::atomic_bool>(false))
{
}

void AnalysisCancelToken::requestCancel() const noexcept
{
    m_cancelled->store(true, std::memory_order_relaxed);
}

bool AnalysisCancelToken::isCancellationRequested() const noexcept
{
    return m_cancelled->load(std::memory_order_relaxed);
}

bool supportsPostRunAnalysis(const QString& algorithmId)
{
    return algorithmId == QLatin1String(kHelmAlgorithmId);
}

std::unique_ptr<IPostRunAnalysisSession> createPostRunAnalysisSession(
    const AnalysisSessionSpec& spec, AnalysisError* error)
{
    if (error != nullptr) *error = {};
    if (!supportsPostRunAnalysis(spec.algorithmId)) {
        if (error != nullptr) *error = unsupported(spec.algorithmId);
        return nullptr;
    }
    AnalysisError creationError;
    auto session = std::make_unique<HelmAnalysisCapture>(spec, &creationError);
    if (!creationError.ok()) {
        if (error != nullptr) *error = creationError;
        return nullptr;
    }
    return session;
}

std::unique_ptr<IPostRunAnalysisSession> createPostRunAnalysisSession(
    const AnalysisSessionSpec& spec, QString* error)
{
    AnalysisError details;
    auto session = createPostRunAnalysisSession(spec, &details);
    if (error != nullptr) *error = details.message;
    return session;
}

std::unique_ptr<IPostRunAnalysisSession> createPostRunAnalysisSession(
    const AnalysisSessionSpec& spec, std::nullptr_t)
{
    return createPostRunAnalysisSession(spec, static_cast<QString*>(nullptr));
}

std::unique_ptr<IPostRunAnalyzer> createPostRunAnalyzer(const QString& algorithmId,
                                                         AnalysisError* error)
{
    if (error != nullptr) *error = {};
    if (!supportsPostRunAnalysis(algorithmId)) {
        if (error != nullptr) *error = unsupported(algorithmId);
        return nullptr;
    }
    return std::make_unique<HelmPerformanceAnalyzer>();
}

std::unique_ptr<IPostRunAnalyzer> createPostRunAnalyzer(const QString& algorithmId,
                                                         QString* error)
{
    AnalysisError details;
    auto analyzer = createPostRunAnalyzer(algorithmId, &details);
    if (error != nullptr) *error = details.message;
    return analyzer;
}

std::unique_ptr<IPostRunAnalyzer> createPostRunAnalyzer(const QString& algorithmId,
                                                         std::nullptr_t)
{
    return createPostRunAnalyzer(algorithmId, static_cast<QString*>(nullptr));
}

QString analysisStateName(AnalysisState state)
{
    switch (state) {
    case AnalysisState::None: return QStringLiteral("none");
    case AnalysisState::Capturing: return QStringLiteral("capturing");
    case AnalysisState::Queued: return QStringLiteral("queued");
    case AnalysisState::Validating: return QStringLiteral("validating");
    case AnalysisState::Preprocessing: return QStringLiteral("preprocessing");
    case AnalysisState::Calculating: return QStringLiteral("calculating");
    case AnalysisState::Persisting: return QStringLiteral("persisting");
    case AnalysisState::Completed: return QStringLiteral("completed");
    case AnalysisState::Partial: return QStringLiteral("partial");
    case AnalysisState::Unavailable: return QStringLiteral("unavailable");
    case AnalysisState::Failed: return QStringLiteral("failed");
    case AnalysisState::Cancelled: return QStringLiteral("cancelled");
    }
    return QStringLiteral("none");
}

QString analysisChannelStateName(AnalysisChannelState state)
{
    switch (state) {
    case AnalysisChannelState::NotApplicable: return QStringLiteral("not_applicable");
    case AnalysisChannelState::Completed: return QStringLiteral("completed");
    case AnalysisChannelState::Partial: return QStringLiteral("partial");
    case AnalysisChannelState::Unavailable: return QStringLiteral("unavailable");
    }
    return QStringLiteral("unavailable");
}

QString analysisMetricStatusName(AnalysisMetricStatus status)
{
    switch (status) {
    case AnalysisMetricStatus::Valid: return QStringLiteral("valid");
    case AnalysisMetricStatus::Unavailable: return QStringLiteral("unavailable");
    case AnalysisMetricStatus::UpperBound: return QStringLiteral("upper_bound");
    case AnalysisMetricStatus::Indeterminate: return QStringLiteral("indeterminate");
    case AnalysisMetricStatus::NotCovered: return QStringLiteral("not_covered");
    case AnalysisMetricStatus::NotSettled: return QStringLiteral("not_settled");
    case AnalysisMetricStatus::AboveObservedRange: return QStringLiteral("above_observed_range");
    }
    return QStringLiteral("unavailable");
}

QString analysisPointStatusName(AnalysisPointStatus status)
{
    switch (status) {
    case AnalysisPointStatus::Valid: return QStringLiteral("valid");
    case AnalysisPointStatus::UpperBound: return QStringLiteral("upper_bound");
    case AnalysisPointStatus::Indeterminate: return QStringLiteral("indeterminate");
    case AnalysisPointStatus::NotCovered: return QStringLiteral("not_covered");
    case AnalysisPointStatus::WeakExcitation: return QStringLiteral("weak_excitation");
    case AnalysisPointStatus::Invalid: return QStringLiteral("invalid");
    }
    return QStringLiteral("invalid");
}

QString analysisTerminationKindName(AnalysisTerminationKind kind)
{
    switch (kind) {
    case AnalysisTerminationKind::Finished: return QStringLiteral("finished");
    case AnalysisTerminationKind::Stopped: return QStringLiteral("stopped");
    case AnalysisTerminationKind::Error: return QStringLiteral("error");
    case AnalysisTerminationKind::Cancelled: return QStringLiteral("cancelled");
    }
    return QStringLiteral("stopped");
}

} // namespace hwtest::algorithm::mbddf
