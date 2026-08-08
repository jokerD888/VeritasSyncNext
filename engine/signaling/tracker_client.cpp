#include "engine/signaling/tracker_client.h"

#include "engine/common/content_hash.h"
#include "engine/common/uuid.h"

#include <Windows.h>
#include <winhttp.h>

#include <array>
#include <charconv>
#include <chrono>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace veritassync::signaling {
namespace {

constexpr std::size_t kMaxTrackerResponse = 1024U * 1024U;

std::wstring Widen(const std::string_view text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) throw std::invalid_argument("tracker URL is not valid UTF-8");
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                          result.data(), size) != size) {
    throw std::invalid_argument("tracker URL is not valid UTF-8");
  }
  return result;
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

std::string RoleName(const protocol::Role role) {
  switch (role) {
    case protocol::Role::kSource:
      return "source";
    case protocol::Role::kTarget:
      return "target";
    case protocol::Role::kPeer:
      return "peer";
  }
  throw std::invalid_argument("invalid tracker role");
}

protocol::Role ParseRole(const std::string_view role) {
  if (role == "source") return protocol::Role::kSource;
  if (role == "target") return protocol::Role::kTarget;
  if (role == "peer") return protocol::Role::kPeer;
  throw std::runtime_error("tracker returned an invalid role");
}

std::string KindName(const MessageKind kind) {
  switch (kind) {
    case MessageKind::kOffer:
      return "offer";
    case MessageKind::kAnswer:
      return "answer";
    case MessageKind::kIceCandidate:
      return "ice";
    case MessageKind::kIceRestart:
      return "ice_restart";
  }
  throw std::invalid_argument("invalid signal kind");
}

MessageKind ParseKind(const std::string_view kind) {
  if (kind == "offer") return MessageKind::kOffer;
  if (kind == "answer") return MessageKind::kAnswer;
  if (kind == "ice") return MessageKind::kIceCandidate;
  if (kind == "ice_restart") return MessageKind::kIceRestart;
  throw std::runtime_error("tracker returned an invalid signal kind");
}

std::vector<std::string_view> Fields(const std::string_view line) {
  std::vector<std::string_view> fields;
  std::size_t first = 0;
  while (first <= line.size()) {
    const auto separator = line.find('\t', first);
    fields.push_back(line.substr(
        first, separator == std::string_view::npos ? line.size() - first : separator - first));
    if (separator == std::string_view::npos) break;
    first = separator + 1;
  }
  return fields;
}

std::int64_t ParseInteger(const std::string_view value, const char* field) {
  std::int64_t result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
    throw std::runtime_error(std::string("tracker returned invalid ") + field);
  }
  return result;
}

class InternetHandle {
 public:
  explicit InternetHandle(HINTERNET handle = nullptr) : handle_(handle) {}
  ~InternetHandle() {
    if (handle_ != nullptr) WinHttpCloseHandle(handle_);
  }
  InternetHandle(const InternetHandle&) = delete;
  InternetHandle& operator=(const InternetHandle&) = delete;
  [[nodiscard]] HINTERNET Get() const { return handle_; }

 private:
  HINTERNET handle_;
};

}  // namespace

TrackerHttpResponse WinHttpTrackerTransport::Post(const std::string_view base_url,
                                                  const std::string_view path,
                                                  const std::map<std::string, std::string>& headers,
                                                  const std::string_view body) {
  if (path.empty() || path.front() != '/')
    throw std::invalid_argument("tracker path must be absolute");
  const auto full_url = std::string(base_url) + std::string(path);
  const auto wide_url = Widen(full_url);
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  parts.dwSchemeLength = static_cast<DWORD>(-1);
  parts.dwHostNameLength = static_cast<DWORD>(-1);
  parts.dwUrlPathLength = static_cast<DWORD>(-1);
  if (WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts) == FALSE) {
    throw std::invalid_argument("tracker URL is invalid");
  }
  const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
  const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  const bool loopback = host == L"localhost" || host == L"127.0.0.1" || host == L"[::1]";
  if (!secure && !loopback) throw std::invalid_argument("non-loopback tracker URL must use HTTPS");

  InternetHandle session(WinHttpOpen(L"VeritasSyncNext/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (session.Get() == nullptr) throw std::runtime_error("cannot initialize Tracker HTTP session");
  WinHttpSetTimeouts(session.Get(), 5000, 5000, 10000, 15000);
  InternetHandle connection(WinHttpConnect(session.Get(), host.c_str(), parts.nPort, 0));
  if (connection.Get() == nullptr) throw std::runtime_error("cannot connect to Tracker");
  const std::wstring url_path(parts.lpszUrlPath, parts.dwUrlPathLength);
  InternetHandle request(WinHttpOpenRequest(connection.Get(), L"POST", url_path.c_str(), nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            secure ? WINHTTP_FLAG_SECURE : 0));
  if (request.Get() == nullptr) throw std::runtime_error("cannot create Tracker request");
  std::wstring header_block = L"Content-Type: application/x-veritassync-v1\r\n";
  for (const auto& [name, value] : headers) {
    header_block += Widen(name + ": " + value + "\r\n");
  }
  if (WinHttpSendRequest(request.Get(), header_block.c_str(),
                         static_cast<DWORD>(header_block.size()), const_cast<char*>(body.data()),
                         static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()),
                         0) == FALSE ||
      WinHttpReceiveResponse(request.Get(), nullptr) == FALSE) {
    throw std::runtime_error("Tracker request failed");
  }
  DWORD status = 0;
  DWORD status_size = sizeof(status);
  if (WinHttpQueryHeaders(request.Get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                          WINHTTP_NO_HEADER_INDEX) == FALSE) {
    throw std::runtime_error("Tracker response has no status");
  }
  std::string response;
  while (true) {
    DWORD available = 0;
    if (WinHttpQueryDataAvailable(request.Get(), &available) == FALSE) {
      throw std::runtime_error("cannot read Tracker response size");
    }
    if (available == 0) break;
    if (response.size() + available > kMaxTrackerResponse) {
      throw std::runtime_error("Tracker response exceeds limit");
    }
    const auto offset = response.size();
    response.resize(offset + available);
    DWORD read = 0;
    if (WinHttpReadData(request.Get(), response.data() + offset, available, &read) == FALSE) {
      throw std::runtime_error("cannot read Tracker response");
    }
    response.resize(offset + read);
  }
  return {static_cast<std::uint16_t>(status), std::move(response)};
}

TrackerClient::TrackerClient(std::string base_url, const security::DeviceIdentity& identity,
                             std::unique_ptr<TrackerHttpTransport> transport)
    : base_url_(std::move(base_url)), identity_(identity), transport_(std::move(transport)) {
  while (!base_url_.empty() && base_url_.back() == '/') base_url_.pop_back();
  if (base_url_.empty() || transport_ == nullptr)
    throw std::invalid_argument("Tracker client configuration is invalid");
}

std::string TrackerClient::EncodeField(const std::string_view value) {
  constexpr char kHex[] = "0123456789ABCDEF";
  std::string encoded;
  for (const unsigned char character : value) {
    if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') || character == '-' || character == '_' ||
        character == '.' || character == '~') {
      encoded.push_back(static_cast<char>(character));
    } else {
      encoded.push_back('%');
      encoded.push_back(kHex[character >> 4U]);
      encoded.push_back(kHex[character & 0x0fU]);
    }
  }
  return encoded;
}

std::string TrackerClient::DecodeField(const std::string_view value) {
  auto digit = [](const char character) -> int {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    return -1;
  };
  std::string decoded;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] != '%') {
      decoded.push_back(value[index]);
      continue;
    }
    if (index + 2 >= value.size()) throw std::runtime_error("Tracker returned malformed escaping");
    const int high = digit(value[index + 1]);
    const int low = digit(value[index + 2]);
    if (high < 0 || low < 0) throw std::runtime_error("Tracker returned malformed escaping");
    decoded.push_back(static_cast<char>((high << 4) | low));
    index += 2;
  }
  return decoded;
}

TrackerHttpResponse TrackerClient::SignedPost(const std::string_view path, std::string body,
                                              const bool include_session) {
  const auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
  const auto nonce = common::NewUuidV4();
  const auto body_bytes = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
  const auto body_hash = Hex(common::Blake3(body_bytes));
  const auto canonical = "POST\n" + std::string(path) + "\n" + std::to_string(timestamp) + "\n" +
                         nonce + "\n" + body_hash;
  const auto canonical_bytes = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size());
  std::map<std::string, std::string> headers{
      {"X-VeritasSync-Device-Id", identity_.DeviceId()},
      {"X-VeritasSync-Public-Key", identity_.PublicKeyBase64()},
      {"X-VeritasSync-Timestamp", std::to_string(timestamp)},
      {"X-VeritasSync-Nonce", nonce},
      {"X-VeritasSync-Signature", identity_.SignBase64(canonical_bytes)},
  };
  if (include_session) {
    if (!enrollment_.has_value() || enrollment_->session_token.empty()) {
      throw std::logic_error("Tracker session is not enrolled");
    }
    headers.emplace("X-VeritasSync-Session", enrollment_->session_token);
  }
  auto response = transport_->Post(base_url_, path, headers, body);
  if (response.status < 200 || response.status >= 300) {
    throw std::runtime_error("Tracker rejected request: HTTP " + std::to_string(response.status) +
                             (response.body.empty() ? "" : " " + response.body));
  }
  return response;
}

TrackerEnrollment TrackerClient::ParseEnrollment(const std::string_view response) const {
  std::istringstream stream{std::string(response)};
  std::string line;
  if (!std::getline(stream, line)) throw std::runtime_error("Tracker returned an empty response");
  const auto first = Fields(line);
  if (first.size() != 6 || first[0] != "OK")
    throw std::runtime_error("Tracker enrollment response is invalid");
  TrackerEnrollment enrollment{DecodeField(first[1]),
                               DecodeField(first[2]),
                               DecodeField(first[3]),
                               ParseInteger(first[4], "session expiry"),
                               {}};
  if (first[5] != "MEMBERS") throw std::runtime_error("Tracker enrollment marker is invalid");
  bool ended = false;
  while (std::getline(stream, line)) {
    const auto fields = Fields(line);
    if (fields.size() == 1 && fields[0] == "END") {
      ended = true;
      break;
    }
    if (fields.size() != 4 || fields[0] != "MEMBER")
      throw std::runtime_error("Tracker member row is invalid");
    enrollment.members.push_back(
        {DecodeField(fields[1]), DecodeField(fields[2]), ParseRole(fields[3])});
  }
  if (!ended || enrollment.room_id.empty() || enrollment.authorization_digest.size() != 64 ||
      enrollment.session_token.empty() || enrollment.session_expires_at_ms <= 0) {
    throw std::runtime_error("Tracker enrollment is incomplete");
  }
  return enrollment;
}

CreatedInvitation TrackerClient::CreateRoom(const std::string_view task_id, const Topology topology,
                                            const protocol::Role local_role,
                                            const protocol::Role invited_role) {
  const auto body = EncodeField(task_id) + "\t" +
                    (topology == Topology::kOneWay ? "one_way" : "bidirectional") + "\t" +
                    RoleName(local_role) + "\t" + RoleName(invited_role);
  const auto response = SignedPost("/v1/rooms/create", body, false);
  const auto newline = response.body.find('\n');
  if (newline == std::string::npos)
    throw std::runtime_error("Tracker invitation response is invalid");
  const auto fields = Fields(std::string_view(response.body).substr(0, newline));
  if (fields.size() != 2 || fields[0] != "INVITE")
    throw std::runtime_error("Tracker invitation code is missing");
  auto enrollment = ParseEnrollment(std::string_view(response.body).substr(newline + 1));
  CreatedInvitation created;
  static_cast<TrackerEnrollment&>(created) = enrollment;
  created.invitation_code = DecodeField(fields[1]);
  UseEnrollment(enrollment);
  return created;
}

std::string TrackerClient::CreateInvitation(const std::string_view room_id,
                                            const protocol::Role invited_role) {
  if (!enrollment_.has_value() || enrollment_->room_id != room_id) {
    throw std::invalid_argument("Tracker client is not enrolled in the invitation room");
  }
  const auto response = SignedPost("/v1/invitations/create",
                                   EncodeField(room_id) + "\t" + RoleName(invited_role), true);
  auto line = std::string_view(response.body);
  if (!line.empty() && line.back() == '\n') line.remove_suffix(1);
  const auto fields = Fields(line);
  if (fields.size() != 2 || fields[0] != "INVITE") {
    throw std::runtime_error("Tracker invitation response is invalid");
  }
  return DecodeField(fields[1]);
}

TrackerEnrollment TrackerClient::RedeemInvitation(const std::string_view invitation_code,
                                                  const std::string_view task_id,
                                                  const protocol::Role requested_role) {
  const auto body =
      EncodeField(invitation_code) + "\t" + EncodeField(task_id) + "\t" + RoleName(requested_role);
  auto enrollment = ParseEnrollment(SignedPost("/v1/invitations/redeem", body, false).body);
  UseEnrollment(enrollment);
  return enrollment;
}

TrackerEnrollment TrackerClient::JoinRoom(const std::string_view room_id,
                                          const std::string_view task_id,
                                          const protocol::Role role) {
  const auto body = EncodeField(room_id) + "\t" + EncodeField(task_id) + "\t" + RoleName(role);
  auto enrollment = ParseEnrollment(SignedPost("/v1/rooms/join", body, false).body);
  UseEnrollment(enrollment);
  return enrollment;
}

void TrackerClient::UseEnrollment(TrackerEnrollment enrollment) {
  if (enrollment.room_id.empty() || enrollment.session_token.empty()) {
    throw std::invalid_argument("Tracker enrollment is invalid");
  }
  enrollment_ = std::move(enrollment);
}

void TrackerClient::Forward(const RelayMessage& message) {
  if (!enrollment_.has_value() || message.sender_device_id != identity_.DeviceId()) {
    throw std::invalid_argument("signal sender does not match enrolled device");
  }
  const auto body = EncodeField(enrollment_->room_id) + "\t" + KindName(message.kind) + "\t" +
                    EncodeField(message.recipient_device_id) + "\t" + EncodeField(message.payload) +
                    "\t" + EncodeField(message.candidate_mid) + "\t" +
                    std::to_string(message.candidate_mline_index);
  const auto response = SignedPost("/v1/signals/send", body, true);
  if (response.body != "OK\n")
    throw std::runtime_error("Tracker signal acknowledgement is invalid");
}

std::vector<RelayMessage> TrackerClient::DrainInbox(const std::string& device_id) {
  if (!enrollment_.has_value() || device_id != identity_.DeviceId()) {
    throw std::invalid_argument("signal inbox does not match enrolled device");
  }
  const auto response = SignedPost("/v1/signals/drain", EncodeField(enrollment_->room_id), true);
  std::istringstream stream{response.body};
  std::string line;
  if (!std::getline(stream, line) || line != "OK")
    throw std::runtime_error("Tracker inbox response is invalid");
  std::vector<RelayMessage> messages;
  bool ended = false;
  while (std::getline(stream, line)) {
    const auto fields = Fields(line);
    if (fields.size() == 1 && fields[0] == "END") {
      ended = true;
      break;
    }
    if (fields.size() != 7 || fields[0] != "SIGNAL")
      throw std::runtime_error("Tracker signal row is invalid");
    const auto index = ParseInteger(fields[6], "candidate index");
    if (index < (std::numeric_limits<std::int32_t>::min)() ||
        index > (std::numeric_limits<std::int32_t>::max)()) {
      throw std::runtime_error("Tracker candidate index is out of range");
    }
    messages.push_back({ParseKind(fields[1]), DecodeField(fields[2]), device_id,
                        DecodeField(fields[4]), DecodeField(fields[5]),
                        static_cast<std::int32_t>(index)});
    if (DecodeField(fields[3]) != device_id)
      throw std::runtime_error("Tracker signal recipient is invalid");
  }
  if (!ended) throw std::runtime_error("Tracker inbox response is incomplete");
  return messages;
}

}  // namespace veritassync::signaling
