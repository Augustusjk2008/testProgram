#include <algorithm/post_run_analysis.h>

#include <type_traits>

static_assert(std::is_default_constructible<hwtest::algorithm::mbddf::PostRunSample>::value,
              "The generic post-run sample must remain independently usable.");
static_assert(std::is_copy_constructible<hwtest::algorithm::mbddf::AnalysisCancelToken>::value,
              "Cancellation tokens must be shareable with a worker.");
