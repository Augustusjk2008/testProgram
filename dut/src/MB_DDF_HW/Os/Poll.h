#pragma once

#include "MB_DDF_HW/Core/Result.h"
#include "MB_DDF_HW/Core/Timeout.h"

namespace MB_DDF::HW::Os::Poll {

Result<int> wait_readable(int fd, Timeout timeout);

} // namespace MB_DDF::HW::Os::Poll
