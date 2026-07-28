#pragma once

#include <string>

namespace veritassync::common {

// Returns a cryptographically random UUIDv4 used as a file version identifier.
[[nodiscard]] std::string NewUuidV4();

}  // namespace veritassync::common
