#include "engine/sync/chunk_source.h"
#include "engine/common/content_hash.h"

#include <fstream>
#include <limits>
#include <stdexcept>

namespace veritassync::sync {
ChunkSource::ChunkSource(std::filesystem::path source_path, std::array<std::uint8_t, 16> transfer_id,
                         std::array<std::uint8_t, 32> file_hash)
    : source_path_(std::move(source_path)), transfer_id_(transfer_id), file_hash_(file_hash) {}
void ChunkSource::ValidateRequest(const protocol::FileRequest& request) const {
  if (request.transfer_id != transfer_id_ || request.file_hash != file_hash_) throw std::invalid_argument("file request does not match transfer");
  if (common::Blake3File(source_path_) != file_hash_) throw std::runtime_error("transfer source changed");
}
protocol::Chunk ChunkSource::ReadChunk(const std::uint64_t chunk_index) const {
  const auto size = std::filesystem::file_size(source_path_);
  const auto total_chunks = (size + protocol::kLogicalChunkSize - 1U) / protocol::kLogicalChunkSize;
  if (chunk_index >= total_chunks) throw std::invalid_argument("requested chunk is outside source");
  if (!stream_.is_open()) {
    stream_.open(source_path_, std::ios::binary);
  }
  if (!stream_) throw std::runtime_error("cannot read transfer source");
  const auto offset = chunk_index * protocol::kLogicalChunkSize;
  const auto length = static_cast<std::size_t>((std::min)(protocol::kLogicalChunkSize, size - offset));
  stream_.clear();
  stream_.seekg(static_cast<std::streamoff>(offset));
  std::vector<std::uint8_t> bytes(length);
  stream_.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (stream_.gcount() != static_cast<std::streamsize>(bytes.size())) throw std::runtime_error("cannot read transfer chunk");
  return {transfer_id_, file_hash_, offset, protocol::TestHash(bytes), std::move(bytes)};
}
}  // namespace veritassync::sync
