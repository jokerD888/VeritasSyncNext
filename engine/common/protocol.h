#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace veritassync::protocol {

inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::size_t kFrameHeaderSize = 16;
inline constexpr std::size_t kMaxFrameSize = 16U * 1024U * 1024U;
inline constexpr std::size_t kLogicalChunkSize = 256U * 1024U;

enum class Channel { kControl, kBulk };
enum class FrameType : std::uint8_t {
  kHello = 1, kManifest = 2, kError = 3, kHeartbeat = 4, kFileRequest = 5,
  kChunk = 64, kChunkAck = 65, kWindowUpdate = 66,
};
enum class Role : std::uint8_t { kSource = 1, kTarget = 2, kPeer = 3 };

struct Frame {
  FrameType type;
  std::uint64_t request_id;
  std::vector<std::uint8_t> payload;
};

struct Hello {
  std::string task_id;
  Role role;
  std::string device_id;
  std::string device_fingerprint;
  std::string authorization_digest;
};
struct ManifestEntry { std::string relative_path; std::uint64_t size; std::string content_hash; };
struct Manifest { std::uint64_t revision; std::vector<ManifestEntry> entries; };
struct Chunk {
  std::array<std::uint8_t, 16> transfer_id;
  std::array<std::uint8_t, 32> file_hash;
  std::uint64_t offset;
  std::array<std::uint8_t, 32> chunk_hash;
  std::vector<std::uint8_t> bytes;
};
struct ChunkRange { std::uint64_t first_chunk; std::uint32_t chunk_count; };
struct FileRequest { std::array<std::uint8_t, 16> transfer_id; std::array<std::uint8_t, 32> file_hash; std::vector<ChunkRange> missing_ranges; };

[[nodiscard]] bool IsAllowedOn(Channel channel, FrameType type);
[[nodiscard]] std::vector<std::uint8_t> EncodeFrame(const Frame& frame);
[[nodiscard]] Frame DecodeFrame(std::span<const std::uint8_t> wire);
[[nodiscard]] std::vector<std::uint8_t> EncodeHello(const Hello& hello);
[[nodiscard]] Hello DecodeHello(std::span<const std::uint8_t> payload);
[[nodiscard]] std::vector<std::uint8_t> EncodeManifest(const Manifest& manifest);
[[nodiscard]] Manifest DecodeManifest(std::span<const std::uint8_t> payload);
[[nodiscard]] std::vector<std::uint8_t> EncodeChunk(const Chunk& chunk);
[[nodiscard]] Chunk DecodeChunk(std::span<const std::uint8_t> payload);
[[nodiscard]] std::vector<std::uint8_t> EncodeFileRequest(const FileRequest& request);
[[nodiscard]] FileRequest DecodeFileRequest(std::span<const std::uint8_t> payload);
[[nodiscard]] std::array<std::uint8_t, 32> TestHash(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::string ErrorCodeFor(std::string_view reason);

}  // namespace veritassync::protocol
