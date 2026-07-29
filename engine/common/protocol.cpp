#include "engine/common/protocol.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace veritassync::protocol {
namespace {
class Writer {
 public:
  void U8(std::uint8_t value) { bytes_.push_back(value); }
  void U16(std::uint16_t value) { Int(value); }
  void U32(std::uint32_t value) { Int(value); }
  void U64(std::uint64_t value) { Int(value); }
  void Bytes(std::span<const std::uint8_t> value) { bytes_.insert(bytes_.end(), value.begin(), value.end()); }
  void String(std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint16_t>::max()) throw std::invalid_argument("string too long");
    U16(static_cast<std::uint16_t>(value.size()));
    Bytes({reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
  }
  std::vector<std::uint8_t> Take() { return std::move(bytes_); }
 private:
  template <typename T> void Int(T value) { for (std::size_t i = 0; i < sizeof(T); ++i) U8(static_cast<std::uint8_t>(value >> (i * 8U))); }
  std::vector<std::uint8_t> bytes_;
};
class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
  std::uint8_t U8() { Need(1); return bytes_[position_++]; }
  std::uint16_t U16() { return Int<std::uint16_t>(); }
  std::uint32_t U32() { return Int<std::uint32_t>(); }
  std::uint64_t U64() { return Int<std::uint64_t>(); }
  std::vector<std::uint8_t> Bytes(std::size_t count) { Need(count); auto begin = bytes_.begin() + static_cast<std::ptrdiff_t>(position_); position_ += count; return {begin, begin + static_cast<std::ptrdiff_t>(count)}; }
  std::string String() { auto bytes = Bytes(U16()); return {reinterpret_cast<const char*>(bytes.data()), bytes.size()}; }
  void Finish() const { if (position_ != bytes_.size()) throw std::invalid_argument("trailing bytes"); }
 private:
  template <typename T> T Int() { Need(sizeof(T)); T value = 0; for (std::size_t i = 0; i < sizeof(T); ++i) value |= static_cast<T>(bytes_[position_++]) << (i * 8U); return value; }
  void Need(std::size_t count) const { if (count > bytes_.size() - position_) throw std::invalid_argument("truncated payload"); }
  std::span<const std::uint8_t> bytes_; std::size_t position_ = 0;
};
bool ValidType(std::uint8_t type) { return type == 1 || type == 2 || type == 3 || type == 4 || type == 5 || type == 64 || type == 65 || type == 66; }
}

bool IsAllowedOn(Channel channel, FrameType type) { return channel == Channel::kControl ? static_cast<std::uint8_t>(type) < 64 : static_cast<std::uint8_t>(type) >= 64; }
std::vector<std::uint8_t> EncodeFrame(const Frame& frame) {
  if (!ValidType(static_cast<std::uint8_t>(frame.type)) || frame.payload.size() > kMaxFrameSize - kFrameHeaderSize) throw std::invalid_argument("invalid frame");
  Writer writer; writer.U8('V'); writer.U8('S'); writer.U8(kProtocolVersion); writer.U8(static_cast<std::uint8_t>(frame.type)); writer.U64(frame.request_id); writer.U32(static_cast<std::uint32_t>(frame.payload.size())); writer.Bytes(frame.payload); return writer.Take();
}
Frame DecodeFrame(std::span<const std::uint8_t> wire) {
  if (wire.size() < kFrameHeaderSize || wire.size() > kMaxFrameSize) throw std::invalid_argument("invalid frame size");
  Reader reader(wire); if (reader.U8() != 'V' || reader.U8() != 'S') throw std::invalid_argument("bad magic"); if (reader.U8() != kProtocolVersion) throw std::invalid_argument("unsupported version"); const auto type = reader.U8(); if (!ValidType(type)) throw std::invalid_argument("unknown frame type"); const auto request_id = reader.U64(); const auto payload_length = reader.U32(); if (payload_length != wire.size() - kFrameHeaderSize) throw std::invalid_argument("payload length mismatch"); auto payload = reader.Bytes(payload_length); reader.Finish(); return {static_cast<FrameType>(type), request_id, std::move(payload)};
}
std::vector<std::uint8_t> EncodeHello(const Hello& hello) { Writer writer; writer.String(hello.task_id); writer.U8(static_cast<std::uint8_t>(hello.role)); writer.String(hello.device_id); writer.String(hello.device_fingerprint); writer.String(hello.authorization_digest); return writer.Take(); }
Hello DecodeHello(std::span<const std::uint8_t> payload) { Reader reader(payload); Hello hello{reader.String(), static_cast<Role>(reader.U8()), reader.String(), reader.String(), reader.String()}; if (hello.task_id.empty() || hello.device_id.empty() || hello.authorization_digest.empty() || (hello.role != Role::kSource && hello.role != Role::kTarget && hello.role != Role::kPeer)) throw std::invalid_argument("invalid hello"); reader.Finish(); return hello; }
std::vector<std::uint8_t> EncodeManifest(const Manifest& manifest) { if (manifest.entries.size() > 1000000U) throw std::invalid_argument("too many manifest entries"); Writer writer; writer.U64(manifest.revision); writer.U32(static_cast<std::uint32_t>(manifest.entries.size())); for (const auto& e : manifest.entries) { writer.String(e.relative_path); writer.U64(e.size); writer.String(e.content_hash); } return writer.Take(); }
Manifest DecodeManifest(std::span<const std::uint8_t> payload) { Reader reader(payload); Manifest manifest{reader.U64(), {}}; const auto count = reader.U32(); if (count > 1000000U) throw std::invalid_argument("too many manifest entries"); manifest.entries.reserve(count); for (std::uint32_t i = 0; i < count; ++i) { auto path = reader.String(); auto size = reader.U64(); auto hash = reader.String(); if (path.empty()) throw std::invalid_argument("empty manifest path"); manifest.entries.push_back({std::move(path), size, std::move(hash)}); } reader.Finish(); return manifest; }
std::vector<std::uint8_t> EncodeChunk(const Chunk& chunk) { if (chunk.bytes.size() > kLogicalChunkSize) throw std::invalid_argument("chunk exceeds logical chunk size"); Writer writer; writer.Bytes(chunk.transfer_id); writer.Bytes(chunk.file_hash); writer.U64(chunk.offset); writer.U32(static_cast<std::uint32_t>(chunk.bytes.size())); writer.Bytes(chunk.chunk_hash); writer.Bytes(chunk.bytes); return writer.Take(); }
Chunk DecodeChunk(std::span<const std::uint8_t> payload) { Reader reader(payload); Chunk chunk{}; auto transfer = reader.Bytes(chunk.transfer_id.size()); std::copy(transfer.begin(), transfer.end(), chunk.transfer_id.begin()); auto hash = reader.Bytes(chunk.file_hash.size()); std::copy(hash.begin(), hash.end(), chunk.file_hash.begin()); chunk.offset = reader.U64(); const auto length = reader.U32(); if (length > kLogicalChunkSize) throw std::invalid_argument("chunk too large"); auto chunk_hash = reader.Bytes(chunk.chunk_hash.size()); std::copy(chunk_hash.begin(), chunk_hash.end(), chunk.chunk_hash.begin()); chunk.bytes = reader.Bytes(length); reader.Finish(); if (TestHash(chunk.bytes) != chunk.chunk_hash) throw std::invalid_argument("chunk hash mismatch"); return chunk; }
std::vector<std::uint8_t> EncodeFileRequest(const FileRequest& request) { if (request.missing_ranges.empty() || request.missing_ranges.size() > 1000000U) throw std::invalid_argument("invalid missing ranges"); Writer writer; writer.Bytes(request.transfer_id); writer.Bytes(request.file_hash); writer.U32(static_cast<std::uint32_t>(request.missing_ranges.size())); std::uint64_t previous_end = 0; for (const auto& range : request.missing_ranges) { if (range.chunk_count == 0 || range.first_chunk < previous_end || range.chunk_count > std::numeric_limits<std::uint64_t>::max() - range.first_chunk) throw std::invalid_argument("invalid missing range"); writer.U64(range.first_chunk); writer.U32(range.chunk_count); previous_end = range.first_chunk + range.chunk_count; } return writer.Take(); }
FileRequest DecodeFileRequest(std::span<const std::uint8_t> payload) { Reader reader(payload); FileRequest request{}; auto transfer = reader.Bytes(request.transfer_id.size()); std::copy(transfer.begin(), transfer.end(), request.transfer_id.begin()); auto hash = reader.Bytes(request.file_hash.size()); std::copy(hash.begin(), hash.end(), request.file_hash.begin()); const auto count = reader.U32(); if (count == 0 || count > 1000000U) throw std::invalid_argument("invalid missing ranges"); request.missing_ranges.reserve(count); std::uint64_t previous_end = 0; for (std::uint32_t i = 0; i < count; ++i) { const auto first = reader.U64(); const auto length = reader.U32(); if (length == 0 || first < previous_end || length > std::numeric_limits<std::uint64_t>::max() - first) throw std::invalid_argument("invalid missing range"); request.missing_ranges.push_back({first, length}); previous_end = first + length; } reader.Finish(); return request; }
std::array<std::uint8_t, 32> TestHash(std::span<const std::uint8_t> bytes) { std::array<std::uint8_t, 32> result{}; std::uint64_t state = 1469598103934665603ULL; for (auto byte : bytes) { state ^= byte; state *= 1099511628211ULL; state ^= state >> 29U; } for (std::size_t i = 0; i < result.size(); ++i) { state ^= state << 13U; state ^= state >> 7U; state ^= state << 17U; result[i] = static_cast<std::uint8_t>(state >> ((i % 8U) * 8U)); } return result; }
std::string ErrorCodeFor(std::string_view reason) { return std::string(reason); }
}  // namespace veritassync::protocol
