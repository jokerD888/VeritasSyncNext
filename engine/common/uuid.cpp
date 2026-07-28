#include "engine/common/uuid.h"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <stdexcept>

namespace veritassync::common {

std::string NewUuidV4() {
  std::array<unsigned char, 16> bytes{};
  if (::BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
    throw std::runtime_error("cannot generate UUID randomness");
  }
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
  constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(36);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) result.push_back('-');
    result.push_back(kHex[bytes[index] >> 4U]);
    result.push_back(kHex[bytes[index] & 0x0fU]);
  }
  return result;
}

}  // namespace veritassync::common
