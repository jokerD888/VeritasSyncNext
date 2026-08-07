#include "engine/common/protocol.h"
#include "engine/common/content_hash.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace veritassync::protocol {
namespace {
class Writer {
 public:
  explicit Writer(const std::size_t capacity = 0) { bytes_.reserve(capacity); }
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
  void BytesTo(const std::span<std::uint8_t> output) {
    Need(output.size());
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(position_), output.size(), output.begin());
    position_ += output.size();
  }
  std::string String() {
    const auto length = U16();
    Need(length);
    const auto* begin = reinterpret_cast<const char*>(bytes_.data() + position_);
    position_ += length;
    return {begin, length};
  }
  void Finish() const { if (position_ != bytes_.size()) throw std::invalid_argument("trailing bytes"); }
 private:
  template <typename T> T Int() { Need(sizeof(T)); T value = 0; for (std::size_t i = 0; i < sizeof(T); ++i) value |= static_cast<T>(bytes_[position_++]) << (i * 8U); return value; }
  void Need(std::size_t count) const { if (count > bytes_.size() - position_) throw std::invalid_argument("truncated payload"); }
  std::span<const std::uint8_t> bytes_; std::size_t position_ = 0;
};
bool ValidType(std::uint8_t type) { return type == 1 || type == 2 || type == 3 || type == 4 || type == 5 || type == 6 || type == 7 || type == 64 || type == 65 || type == 66; }

[[nodiscard]] std::size_t StringSize(const std::string_view value) {
  return sizeof(std::uint16_t) + value.size();
}
}

bool IsAllowedOn(Channel channel, FrameType type) { return channel == Channel::kControl ? static_cast<std::uint8_t>(type) < 64 : static_cast<std::uint8_t>(type) >= 64; }
std::vector<std::uint8_t> EncodeFrame(const Frame& frame) {
  if (!ValidType(static_cast<std::uint8_t>(frame.type)) || frame.payload.size() > kMaxFrameSize - kFrameHeaderSize) throw std::invalid_argument("invalid frame");
  Writer writer(kFrameHeaderSize + frame.payload.size()); writer.U8('V'); writer.U8('S'); writer.U8(kProtocolVersion); writer.U8(static_cast<std::uint8_t>(frame.type)); writer.U64(frame.request_id); writer.U32(static_cast<std::uint32_t>(frame.payload.size())); writer.Bytes(frame.payload); return writer.Take();
}
FrameView DecodeFrameView(std::span<const std::uint8_t> wire) {
  if (wire.size() < kFrameHeaderSize || wire.size() > kMaxFrameSize) throw std::invalid_argument("invalid frame size");
  Reader reader(wire); if (reader.U8() != 'V' || reader.U8() != 'S') throw std::invalid_argument("bad magic"); if (reader.U8() != kProtocolVersion) throw std::invalid_argument("unsupported version"); const auto type = reader.U8(); if (!ValidType(type)) throw std::invalid_argument("unknown frame type"); const auto request_id = reader.U64(); const auto payload_length = reader.U32(); if (payload_length != wire.size() - kFrameHeaderSize) throw std::invalid_argument("payload length mismatch"); return {static_cast<FrameType>(type), request_id, wire.subspan(kFrameHeaderSize, payload_length)};
}
Frame DecodeFrame(const std::span<const std::uint8_t> wire) {
  const auto view = DecodeFrameView(wire);
  return {view.type, view.request_id, {view.payload.begin(), view.payload.end()}};
}
std::vector<std::uint8_t> EncodeHello(const Hello& hello) { Writer writer(StringSize(hello.task_id) + 1U + StringSize(hello.device_id) + StringSize(hello.device_fingerprint) + StringSize(hello.authorization_digest)); writer.String(hello.task_id); writer.U8(static_cast<std::uint8_t>(hello.role)); writer.String(hello.device_id); writer.String(hello.device_fingerprint); writer.String(hello.authorization_digest); return writer.Take(); }
Hello DecodeHello(std::span<const std::uint8_t> payload) { Reader reader(payload); Hello hello{reader.String(), static_cast<Role>(reader.U8()), reader.String(), reader.String(), reader.String()}; if (hello.task_id.empty() || hello.device_id.empty() || hello.authorization_digest.empty() || (hello.role != Role::kSource && hello.role != Role::kTarget && hello.role != Role::kPeer)) throw std::invalid_argument("invalid hello"); reader.Finish(); return hello; }
std::vector<std::uint8_t> EncodeManifest(const Manifest& manifest) { if (manifest.entries.size() > 1000000U) throw std::invalid_argument("too many manifest entries"); std::size_t size = sizeof(std::uint64_t) + sizeof(std::uint32_t); for (const auto& entry : manifest.entries) size += StringSize(entry.relative_path) + sizeof(std::uint64_t) + StringSize(entry.content_hash); Writer writer(size); writer.U64(manifest.revision); writer.U32(static_cast<std::uint32_t>(manifest.entries.size())); for (const auto& e : manifest.entries) { writer.String(e.relative_path); writer.U64(e.size); writer.String(e.content_hash); } return writer.Take(); }
Manifest DecodeManifest(std::span<const std::uint8_t> payload) { Reader reader(payload); Manifest manifest{reader.U64(), {}}; const auto count = reader.U32(); if (count > 1000000U) throw std::invalid_argument("too many manifest entries"); manifest.entries.reserve(count); for (std::uint32_t i = 0; i < count; ++i) { auto path = reader.String(); auto size = reader.U64(); auto hash = reader.String(); if (path.empty()) throw std::invalid_argument("empty manifest path"); manifest.entries.push_back({std::move(path), size, std::move(hash)}); } reader.Finish(); return manifest; }
std::vector<std::uint8_t> EncodeVersionedManifest(const VersionedManifest& manifest) {
  if (manifest.entries.size() > 1000000U) throw std::invalid_argument("too many versioned manifest entries");
  std::size_t size = sizeof(std::uint64_t) + sizeof(std::uint32_t);
  for (const auto& entry : manifest.entries) {
    size += StringSize(entry.relative_path) + sizeof(std::uint8_t) + sizeof(std::uint64_t) +
            StringSize(entry.content_hash) + StringSize(entry.version_id) +
            StringSize(entry.origin_device_id) + sizeof(std::uint64_t) +
            StringSize(entry.parent_version_id) + sizeof(std::uint8_t) +
            (entry.deleted_at_ms.has_value() ? sizeof(std::uint64_t) : 0U);
  }
  Writer writer(size);
  writer.U64(manifest.revision);
  writer.U32(static_cast<std::uint32_t>(manifest.entries.size()));
  for (const auto& entry : manifest.entries) {
    const bool file = entry.kind == VersionedEntryKind::kFile;
    const bool tombstone = entry.kind == VersionedEntryKind::kTombstone;
    if (entry.relative_path.empty() || entry.version_id.empty() || entry.origin_device_id.empty() ||
        (entry.kind != VersionedEntryKind::kFile && entry.kind != VersionedEntryKind::kDirectory && !tombstone) ||
        (file && entry.content_hash.empty()) || (!file && !entry.content_hash.empty()) ||
        (!file && entry.size != 0) || (tombstone != entry.deleted_at_ms.has_value())) {
      throw std::invalid_argument("invalid versioned manifest entry");
    }
    writer.String(entry.relative_path);
    writer.U8(static_cast<std::uint8_t>(entry.kind));
    writer.U64(entry.size);
    writer.String(entry.content_hash);
    writer.String(entry.version_id);
    writer.String(entry.origin_device_id);
    writer.U64(entry.logical_clock);
    writer.String(entry.parent_version_id);
    writer.U8(entry.deleted_at_ms.has_value() ? 1 : 0);
    if (entry.deleted_at_ms.has_value()) writer.U64(*entry.deleted_at_ms);
  }
  return writer.Take();
}
VersionedManifest DecodeVersionedManifest(std::span<const std::uint8_t> payload) {
  Reader reader(payload);
  VersionedManifest manifest{reader.U64(), {}};
  const auto count = reader.U32();
  if (count > 1000000U) throw std::invalid_argument("too many versioned manifest entries");
  manifest.entries.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    VersionedManifestEntry entry{reader.String(), static_cast<VersionedEntryKind>(reader.U8()), reader.U64(),
                                 reader.String(), reader.String(), reader.String(), reader.U64(),
                                 reader.String(), std::nullopt};
    const auto has_deleted_at = reader.U8();
    if (has_deleted_at > 1) throw std::invalid_argument("invalid tombstone timestamp flag");
    if (has_deleted_at == 1) entry.deleted_at_ms = reader.U64();
    const bool file = entry.kind == VersionedEntryKind::kFile;
    const bool tombstone = entry.kind == VersionedEntryKind::kTombstone;
    if (entry.relative_path.empty() || entry.version_id.empty() || entry.origin_device_id.empty() ||
        (entry.kind != VersionedEntryKind::kFile && entry.kind != VersionedEntryKind::kDirectory && !tombstone) ||
        (file && entry.content_hash.empty()) || (!file && !entry.content_hash.empty()) ||
        (!file && entry.size != 0) || (tombstone != entry.deleted_at_ms.has_value())) {
      throw std::invalid_argument("invalid versioned manifest entry");
    }
    manifest.entries.push_back(std::move(entry));
  }
  reader.Finish();
  return manifest;
}
std::vector<std::uint8_t> EncodeChunk(const Chunk& chunk) { if (chunk.bytes.size() > kLogicalChunkSize) throw std::invalid_argument("chunk exceeds logical chunk size"); Writer writer(chunk.transfer_id.size() + chunk.file_hash.size() + sizeof(std::uint64_t) + sizeof(std::uint32_t) + chunk.chunk_hash.size() + chunk.bytes.size()); writer.Bytes(chunk.transfer_id); writer.Bytes(chunk.file_hash); writer.U64(chunk.offset); writer.U32(static_cast<std::uint32_t>(chunk.bytes.size())); writer.Bytes(chunk.chunk_hash); writer.Bytes(chunk.bytes); return writer.Take(); }
std::vector<std::uint8_t> EncodeChunkFrame(const Chunk& chunk, const std::uint64_t request_id) { if (chunk.bytes.size() > kLogicalChunkSize) throw std::invalid_argument("chunk exceeds logical chunk size"); const auto payload_size = chunk.transfer_id.size() + chunk.file_hash.size() + sizeof(std::uint64_t) + sizeof(std::uint32_t) + chunk.chunk_hash.size() + chunk.bytes.size(); Writer writer(kFrameHeaderSize + payload_size); writer.U8('V'); writer.U8('S'); writer.U8(kProtocolVersion); writer.U8(static_cast<std::uint8_t>(FrameType::kChunk)); writer.U64(request_id); writer.U32(static_cast<std::uint32_t>(payload_size)); writer.Bytes(chunk.transfer_id); writer.Bytes(chunk.file_hash); writer.U64(chunk.offset); writer.U32(static_cast<std::uint32_t>(chunk.bytes.size())); writer.Bytes(chunk.chunk_hash); writer.Bytes(chunk.bytes); return writer.Take(); }
Chunk DecodeChunk(std::span<const std::uint8_t> payload) { Reader reader(payload); Chunk chunk{}; reader.BytesTo(chunk.transfer_id); reader.BytesTo(chunk.file_hash); chunk.offset = reader.U64(); const auto length = reader.U32(); if (length > kLogicalChunkSize) throw std::invalid_argument("chunk too large"); reader.BytesTo(chunk.chunk_hash); chunk.bytes = reader.Bytes(length); reader.Finish(); if (TestHash(chunk.bytes) != chunk.chunk_hash) throw std::invalid_argument("chunk hash mismatch"); return chunk; }
std::vector<std::uint8_t> EncodeFileRequest(const FileRequest& request) { if (request.missing_ranges.empty() || request.missing_ranges.size() > 1000000U) throw std::invalid_argument("invalid missing ranges"); Writer writer(request.transfer_id.size() + request.file_hash.size() + sizeof(std::uint32_t) + request.missing_ranges.size() * (sizeof(std::uint64_t) + sizeof(std::uint32_t))); writer.Bytes(request.transfer_id); writer.Bytes(request.file_hash); writer.U32(static_cast<std::uint32_t>(request.missing_ranges.size())); std::uint64_t previous_end = 0; for (const auto& range : request.missing_ranges) { if (range.chunk_count == 0 || range.first_chunk < previous_end || range.chunk_count > std::numeric_limits<std::uint64_t>::max() - range.first_chunk) throw std::invalid_argument("invalid missing range"); writer.U64(range.first_chunk); writer.U32(range.chunk_count); previous_end = range.first_chunk + range.chunk_count; } return writer.Take(); }
FileRequest DecodeFileRequest(std::span<const std::uint8_t> payload) { Reader reader(payload); FileRequest request{}; reader.BytesTo(request.transfer_id); reader.BytesTo(request.file_hash); const auto count = reader.U32(); if (count == 0 || count > 1000000U) throw std::invalid_argument("invalid missing ranges"); request.missing_ranges.reserve(count); std::uint64_t previous_end = 0; for (std::uint32_t i = 0; i < count; ++i) { const auto first = reader.U64(); const auto length = reader.U32(); if (length == 0 || first < previous_end || length > std::numeric_limits<std::uint64_t>::max() - first) throw std::invalid_argument("invalid missing range"); request.missing_ranges.push_back({first, length}); previous_end = first + length; } reader.Finish(); return request; }
std::vector<std::uint8_t> EncodeCancel(const Cancel& cancel) { if (cancel.reason.empty()) throw std::invalid_argument("cancel reason is required"); Writer writer(cancel.transfer_id.size() + StringSize(cancel.reason)); writer.Bytes(cancel.transfer_id); writer.String(cancel.reason); return writer.Take(); }
Cancel DecodeCancel(std::span<const std::uint8_t> payload) { Reader reader(payload); Cancel cancel{}; reader.BytesTo(cancel.transfer_id); cancel.reason = reader.String(); if (cancel.reason.empty()) throw std::invalid_argument("cancel reason is required"); reader.Finish(); return cancel; }
std::array<std::uint8_t, 32> TestHash(std::span<const std::uint8_t> bytes) { return common::Blake3(bytes); }
std::string ErrorCodeFor(std::string_view reason) { return std::string(reason); }
}  // namespace veritassync::protocol
