#pragma once

#include "engine/common/protocol.h"
#include "engine/storage/database.h"
#include "engine/storage/safe_file_writer.h"

#include <cstdint>
#include <string>

namespace veritassync::sync {

class DownloadReceiver {
 public:
  DownloadReceiver(storage::Database& database, const storage::TransferId& transfer_id,
                   storage::SafeFileWriter& writer, std::string relative_path,
                   std::uint64_t expected_size, common::ContentHash expected_hash,
                   std::uint64_t chunk_count);
  void AcceptChunk(std::uint64_t chunk_index, std::uint64_t offset,
                   std::span<const std::uint8_t> bytes, const common::ContentHash& chunk_hash,
                   std::int64_t updated_at_ms, bool persist = true);
  void PersistAcceptedChunks(std::span<const std::uint64_t> chunk_indices,
                             std::int64_t updated_at_ms);
  [[nodiscard]] protocol::FileRequest ResumeRequest() const;
  void Cancel(const protocol::Cancel& cancel, std::int64_t cancelled_at_ms);
  void Commit(std::int64_t completed_at_ms);

 private:
  storage::Database& database_;
  storage::TransferId transfer_id_;
  storage::SafeFileWriter& writer_;
  std::string relative_path_;
  std::uint64_t expected_size_;
  common::ContentHash expected_hash_;
  std::uint64_t chunk_count_;
  bool cancelled_ = false;
};

}  // namespace veritassync::sync
