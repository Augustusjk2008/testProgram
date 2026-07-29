#pragma once

#include <cstddef>
#include <cstdint>

namespace ProtocolModel {

inline constexpr uint8_t kHelmUnlockInactive = 0x00u;
inline constexpr uint8_t kHelmUnlockRequested = 0xFFu;
inline constexpr size_t kHelmUnlockByteOffset = 26u;

} // namespace ProtocolModel
