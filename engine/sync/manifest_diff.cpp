#include "engine/sync/manifest_diff.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace veritassync::sync {
namespace {

[[nodiscard]] bool IsSameContent(const storage::SnapshotEntry& entry,
                                 const storage::FileRecord& record) {
  if ((entry.kind == storage::SnapshotKind::kFile) != (record.kind == storage::FileKind::kFile)) {
    return false;
  }
  if (entry.kind == storage::SnapshotKind::kDirectory) {
    return record.kind == storage::FileKind::kDirectory;
  }
  if (!entry.content_hash.has_value()) {
    throw std::invalid_argument("file snapshots require a content hash");
  }
  return entry.size == record.size &&
         std::equal(entry.content_hash->begin(), entry.content_hash->end(), record.content_hash.begin(), record.content_hash.end());
}

template <typename Entry>
class OrderedEntries {
 public:
  explicit OrderedEntries(const std::vector<Entry>& entries) : entries_(entries) {
    const auto ordered = std::ranges::is_sorted(entries_, {}, &Entry::relative_path);
    if (!ordered) {
      sorted_.reserve(entries_.size());
      for (const auto& entry : entries_) sorted_.push_back(&entry);
      std::ranges::sort(sorted_, {}, [](const Entry* entry) -> const std::string& {
        return entry->relative_path;
      });
    }
    for (std::size_t index = 1; index < Size(); ++index) {
      if (At(index - 1).relative_path == At(index).relative_path) {
        throw std::invalid_argument("manifest contains duplicate paths");
      }
    }
  }

  [[nodiscard]] std::size_t Size() const { return entries_.size(); }
  [[nodiscard]] const Entry& At(const std::size_t index) const {
    return sorted_.empty() ? entries_[index] : *sorted_[index];
  }

 private:
  const std::vector<Entry>& entries_;
  std::vector<const Entry*> sorted_;
};

}  // namespace

ManifestDiff DiffManifest(const std::vector<storage::SnapshotEntry>& snapshot,
                          const std::vector<storage::FileRecord>& known_records) {
  const OrderedEntries scanned(snapshot);
  const OrderedEntries known(known_records);
  ManifestDiff result;
  std::size_t scanned_index = 0;
  std::size_t known_index = 0;
  while (scanned_index < scanned.Size() && known_index < known.Size()) {
    const auto& entry = scanned.At(scanned_index);
    const auto& record = known.At(known_index);
    if (entry.relative_path < record.relative_path) {
      result.created_or_changed.push_back(entry);
      ++scanned_index;
    } else if (record.relative_path < entry.relative_path) {
      if (record.kind != storage::FileKind::kTombstone) {
        result.deleted_paths.push_back(record.relative_path);
      }
      ++known_index;
    } else {
      if (record.kind == storage::FileKind::kTombstone || !IsSameContent(entry, record)) {
        result.created_or_changed.push_back(entry);
      }
      ++scanned_index;
      ++known_index;
    }
  }
  while (scanned_index < scanned.Size()) {
    result.created_or_changed.push_back(scanned.At(scanned_index++));
  }
  while (known_index < known.Size()) {
    const auto& record = known.At(known_index++);
    if (record.kind != storage::FileKind::kTombstone) {
      result.deleted_paths.push_back(record.relative_path);
    }
  }
  return result;
}

}  // namespace veritassync::sync
