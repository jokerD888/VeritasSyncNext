#include "engine/sync/chunk_source.h"
#include "engine/common/content_hash.h"

#include <fstream>
#include <limits>
#include <stdexcept>

namespace veritassync::sync {
ChunkSource::ChunkSource(std::filesystem::path source_path, std::array<std::uint8_t, 16> transfer_id,
                         std::array<std::uint8_t, 32> file_hash)
    : source_path_(std::move(source_path)), transfer_id_(transfer_id), file_hash_(file_hash) {}
std::vector<protocol::Chunk> ChunkSource::ReadRequested(const protocol::FileRequest& request) const {
  if (request.transfer_id != transfer_id_ || request.file_hash != file_hash_) throw std::invalid_argument("file request does not match transfer");
  if (common::Blake3File(source_path_) != file_hash_) throw std::runtime_error("transfer source changed");
  const auto size = std::filesystem::file_size(source_path_);
  const auto total_chunks = (size + protocol::kLogicalChunkSize - 1U) / protocol::kLogicalChunkSize;
  std::ifstream stream(source_path_, std::ios::binary);
  if (!stream) throw std::runtime_error("cannot read transfer source");
  std::vector<protocol::Chunk> chunks;
  for (const auto& range : request.missing_ranges) {
    if (range.first_chunk >= total_chunks || range.chunk_count > total_chunks - range.first_chunk) throw std::invalid_argument("requested chunk is outside source");
    for (std::uint64_t index = range.first_chunk; index < range.first_chunk + range.chunk_count; ++index) {
      const auto offset = index * protocol::kLogicalChunkSize;
      const auto length = static_cast<std::size_t>((std::min)(protocol::kLogicalChunkSize, size - offset));
      stream.clear(); stream.seekg(static_cast<std::streamoff>(offset));
      std::vector<std::uint8_t> bytes(length); stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      if (stream.gcount() != static_cast<std::streamsize>(bytes.size())) throw std::runtime_error("cannot read transfer chunk");
      chunks.push_back({transfer_id_, file_hash_, offset, protocol::TestHash(bytes), std::move(bytes)});
    }
  }
  return chunks;
}
}  // namespace veritassync::sync
