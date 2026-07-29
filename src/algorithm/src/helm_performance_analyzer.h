#pragma once

#include <algorithm/post_run_analysis.h>

namespace hwtest::algorithm::mbddf {

class HelmPerformanceAnalyzer final : public IPostRunAnalyzer {
public:
    AnalysisResult analyze(const AnalysisInputSeal& input,
                           const AnalysisProgressCallback& progress,
                           const AnalysisCancelToken& cancel) override;
};

} // namespace hwtest::algorithm::mbddf
