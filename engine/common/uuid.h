#pragma once

#include <string>
#include <array>
#include <cstdint>

namespace veritassync::common {

// Returns a cryptographically random UUIDv4 used as a file version identifier.
[[nodiscard]] std::string NewUuidV4();
[[nodiscard]] std::array<std::uint8_t, 16> NewTransferId();

}  // namespace veritassync::common
