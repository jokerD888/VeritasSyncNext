#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace veritassync::security {

// A persistent application identity used above DTLS. The private key never
// leaves this object and is cleared when the object is destroyed.
class DeviceIdentity {
 public:
  static DeviceIdentity Generate();
  static DeviceIdentity FromSecretKey(std::span<const std::uint8_t> secret_key);

  DeviceIdentity(DeviceIdentity&& other) noexcept;
  DeviceIdentity& operator=(DeviceIdentity&& other) noexcept;
  DeviceIdentity(const DeviceIdentity&) = delete;
  DeviceIdentity& operator=(const DeviceIdentity&) = delete;
  ~DeviceIdentity();

  [[nodiscard]] const std::string& DeviceId() const { return device_id_; }
  [[nodiscard]] const std::string& Fingerprint() const { return fingerprint_; }
  [[nodiscard]] std::string PublicKeyBase64() const;
  [[nodiscard]] std::string SignBase64(std::span<const std::uint8_t> message) const;
  [[nodiscard]] std::array<std::uint8_t, 64> ExportSecretKeyForStorage() const;

  [[nodiscard]] static bool VerifyBase64(std::string_view public_key_base64,
                                         std::span<const std::uint8_t> message,
                                         std::string_view signature_base64);

 private:
  DeviceIdentity(std::array<std::uint8_t, 32> public_key,
                 std::array<std::uint8_t, 64> secret_key);
  void Clear() noexcept;

  std::array<std::uint8_t, 32> public_key_{};
  std::array<std::uint8_t, 64> secret_key_{};
  std::string device_id_;
  std::string fingerprint_;
};

// Windows stores the raw Ed25519 secret in Credential Manager. The stable
// target name is per-user; the SQLite database contains no private key bytes.
class DeviceIdentityStore {
 public:
  explicit DeviceIdentityStore(std::wstring credential_target =
                                   L"VeritasSyncNext/DeviceIdentity");
  [[nodiscard]] DeviceIdentity LoadOrCreate() const;

 private:
  std::wstring credential_target_;
};

}  // namespace veritassync::security
