#include "engine/security/device_identity.h"
#include "tests/test_framework.h"

#include <array>
#include <cstdint>
#include <string>

VSYNC_TEST(DeviceIdentitySignsAndVerifiesCanonicalRequests) {
  auto identity = veritassync::security::DeviceIdentity::Generate();
  const std::string request = "POST\n/v1/rooms/create\n1720000000\nnonce-1\nbody-hash";
  const auto bytes = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(request.data()), request.size());
  const auto signature = identity.SignBase64(bytes);
  VSYNC_CHECK(veritassync::security::DeviceIdentity::VerifyBase64(
      identity.PublicKeyBase64(), bytes, signature));

  const std::string tampered = request + "!";
  const auto tampered_bytes = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(tampered.data()), tampered.size());
  VSYNC_CHECK(!veritassync::security::DeviceIdentity::VerifyBase64(
      identity.PublicKeyBase64(), tampered_bytes, signature));
  VSYNC_CHECK(identity.DeviceId().size() == 32);
  VSYNC_CHECK(identity.Fingerprint().size() == 64);
}

VSYNC_TEST(DeviceIdentityRoundTripsStoredSecretKey) {
  auto original = veritassync::security::DeviceIdentity::Generate();
  const auto secret = original.ExportSecretKeyForStorage();
  auto restored = veritassync::security::DeviceIdentity::FromSecretKey(secret);
  VSYNC_CHECK(restored.DeviceId() == original.DeviceId());
  VSYNC_CHECK(restored.Fingerprint() == original.Fingerprint());
  VSYNC_CHECK(restored.PublicKeyBase64() == original.PublicKeyBase64());
}
