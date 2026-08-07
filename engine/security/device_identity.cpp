#include "engine/security/device_identity.h"

#include "engine/common/content_hash.h"

#include <Windows.h>
#include <wincred.h>
#include <sodium.h>

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace veritassync::security {
namespace {

void EnsureSodium() {
  static const int initialized = sodium_init();
  if (initialized < 0) throw std::runtime_error("cannot initialize libsodium");
}

std::string Hex(const std::span<const std::uint8_t> bytes) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2U);
  for (const auto byte : bytes) {
    result.push_back(kHex[byte >> 4U]);
    result.push_back(kHex[byte & 0x0fU]);
  }
  return result;
}

std::string Base64(const std::span<const std::uint8_t> bytes) {
  const auto size = sodium_base64_encoded_len(bytes.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
  std::string result(size, '\0');
  sodium_bin2base64(result.data(), result.size(), bytes.data(), bytes.size(),
                    sodium_base64_VARIANT_URLSAFE_NO_PADDING);
  result.resize(result.find('\0'));
  return result;
}

std::vector<std::uint8_t> DecodeBase64(const std::string_view encoded,
                                       const std::size_t expected_size) {
  std::vector<std::uint8_t> result(expected_size);
  std::size_t actual_size = 0;
  if (sodium_base642bin(result.data(), result.size(), encoded.data(), encoded.size(), nullptr,
                        &actual_size, nullptr, sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0 ||
      actual_size != expected_size) {
    throw std::invalid_argument("invalid base64 key material");
  }
  return result;
}

}  // namespace

DeviceIdentity::DeviceIdentity(std::array<std::uint8_t, 32> public_key,
                               std::array<std::uint8_t, 64> secret_key)
    : public_key_(public_key), secret_key_(secret_key) {
  const auto digest = common::Blake3(public_key_);
  device_id_ = Hex(std::span<const std::uint8_t>(digest).first<16>());
  fingerprint_ = Hex(digest);
}

DeviceIdentity DeviceIdentity::Generate() {
  EnsureSodium();
  std::array<std::uint8_t, crypto_sign_PUBLICKEYBYTES> public_key{};
  std::array<std::uint8_t, crypto_sign_SECRETKEYBYTES> secret_key{};
  if (crypto_sign_keypair(public_key.data(), secret_key.data()) != 0) {
    throw std::runtime_error("cannot generate Ed25519 identity");
  }
  return DeviceIdentity(public_key, secret_key);
}

DeviceIdentity DeviceIdentity::FromSecretKey(const std::span<const std::uint8_t> secret_key) {
  EnsureSodium();
  if (secret_key.size() != crypto_sign_SECRETKEYBYTES) {
    throw std::invalid_argument("Ed25519 secret key has an invalid size");
  }
  std::array<std::uint8_t, crypto_sign_SECRETKEYBYTES> secret{};
  std::ranges::copy(secret_key, secret.begin());
  std::array<std::uint8_t, crypto_sign_PUBLICKEYBYTES> public_key{};
  if (crypto_sign_ed25519_sk_to_pk(public_key.data(), secret.data()) != 0) {
    sodium_memzero(secret.data(), secret.size());
    throw std::invalid_argument("cannot derive Ed25519 public key");
  }
  return DeviceIdentity(public_key, secret);
}

DeviceIdentity::DeviceIdentity(DeviceIdentity&& other) noexcept
    : public_key_(other.public_key_), secret_key_(other.secret_key_),
      device_id_(std::move(other.device_id_)), fingerprint_(std::move(other.fingerprint_)) {
  other.Clear();
}

DeviceIdentity& DeviceIdentity::operator=(DeviceIdentity&& other) noexcept {
  if (this == &other) return *this;
  Clear();
  public_key_ = other.public_key_;
  secret_key_ = other.secret_key_;
  device_id_ = std::move(other.device_id_);
  fingerprint_ = std::move(other.fingerprint_);
  other.Clear();
  return *this;
}

DeviceIdentity::~DeviceIdentity() { Clear(); }

void DeviceIdentity::Clear() noexcept {
  sodium_memzero(secret_key_.data(), secret_key_.size());
}

std::string DeviceIdentity::PublicKeyBase64() const { return Base64(public_key_); }

std::string DeviceIdentity::SignBase64(const std::span<const std::uint8_t> message) const {
  EnsureSodium();
  std::array<std::uint8_t, crypto_sign_BYTES> signature{};
  if (crypto_sign_detached(signature.data(), nullptr, message.data(), message.size(),
                           secret_key_.data()) != 0) {
    throw std::runtime_error("cannot sign device request");
  }
  return Base64(signature);
}

std::array<std::uint8_t, 64> DeviceIdentity::ExportSecretKeyForStorage() const {
  return secret_key_;
}

bool DeviceIdentity::VerifyBase64(const std::string_view public_key_base64,
                                  const std::span<const std::uint8_t> message,
                                  const std::string_view signature_base64) {
  EnsureSodium();
  try {
    const auto public_key = DecodeBase64(public_key_base64, crypto_sign_PUBLICKEYBYTES);
    const auto signature = DecodeBase64(signature_base64, crypto_sign_BYTES);
    return crypto_sign_verify_detached(signature.data(), message.data(), message.size(),
                                       public_key.data()) == 0;
  } catch (const std::invalid_argument&) {
    return false;
  }
}

DeviceIdentityStore::DeviceIdentityStore(std::wstring credential_target)
    : credential_target_(std::move(credential_target)) {
  if (credential_target_.empty()) throw std::invalid_argument("credential target is required");
}

DeviceIdentity DeviceIdentityStore::LoadOrCreate() const {
  PCREDENTIALW credential = nullptr;
  if (CredReadW(credential_target_.c_str(), CRED_TYPE_GENERIC, 0, &credential) != FALSE) {
    try {
      if (credential->CredentialBlobSize != crypto_sign_SECRETKEYBYTES ||
          credential->CredentialBlob == nullptr) {
        throw std::runtime_error("stored device identity is invalid");
      }
      auto identity = DeviceIdentity::FromSecretKey({credential->CredentialBlob,
                                                     credential->CredentialBlobSize});
      CredFree(credential);
      return identity;
    } catch (...) {
      CredFree(credential);
      throw;
    }
  }
  if (GetLastError() != ERROR_NOT_FOUND) {
    throw std::runtime_error("cannot read device identity from Windows Credential Manager");
  }

  auto identity = DeviceIdentity::Generate();
  auto secret = identity.ExportSecretKeyForStorage();
  CREDENTIALW credential_to_write{};
  credential_to_write.Type = CRED_TYPE_GENERIC;
  credential_to_write.TargetName = const_cast<wchar_t*>(credential_target_.c_str());
  credential_to_write.CredentialBlobSize = static_cast<DWORD>(secret.size());
  credential_to_write.CredentialBlob = secret.data();
  credential_to_write.Persist = CRED_PERSIST_LOCAL_MACHINE;
  credential_to_write.UserName = const_cast<wchar_t*>(L"VeritasSyncNext");
  const auto written = CredWriteW(&credential_to_write, 0) != FALSE;
  sodium_memzero(secret.data(), secret.size());
  if (!written) throw std::runtime_error("cannot persist device identity in Windows Credential Manager");
  return identity;
}

}  // namespace veritassync::security
