#include "engine/transport/frame_fragments.h"
#include <algorithm>
#include <stdexcept>

namespace veritassync::transport {
namespace {
constexpr std::size_t kHeader = 20;
void Write(std::vector<std::uint8_t>& output, std::uint64_t value, unsigned bytes) {
  for (unsigned index = 0; index < bytes; ++index) output.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
}
std::uint64_t Read(std::span<const std::uint8_t> input) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < input.size(); ++index) value |= std::uint64_t{input[index]} << (index * 8);
  return value;
}
}
std::vector<std::vector<std::uint8_t>> FragmentFrame(std::uint64_t id, std::span<const std::uint8_t> wire) {
  if (id == 0 || wire.empty() || wire.size() > protocol::kMaxFrameSize) throw std::invalid_argument("invalid fragment frame");
  std::vector<std::vector<std::uint8_t>> fragments;
  for (std::size_t offset = 0; offset < wire.size(); offset += kFragmentPayloadSize) {
    auto& fragment = fragments.emplace_back();
    const auto size = (std::min)(kFragmentPayloadSize, wire.size() - offset);
    fragment.reserve(kHeader + size);
    fragment.insert(fragment.end(), {'V','F',2,0});
    Write(fragment, id, 8); Write(fragment, wire.size(), 4); Write(fragment, offset, 4);
    fragment.insert(fragment.end(), wire.begin() + offset, wire.begin() + offset + size);
  }
  return fragments;
}
std::optional<std::vector<std::uint8_t>> FrameReassembler::Accept(std::span<const std::uint8_t> fragment) {
  if (fragment.size() <= kHeader || fragment.size() > kHeader + kFragmentPayloadSize ||
      fragment[0] != 'V' || fragment[1] != 'F' || fragment[2] != 2 || fragment[3] != 0)
    throw std::invalid_argument("invalid DataChannel fragment");
  const auto id = Read(fragment.subspan(4, 8));
  const auto total = static_cast<std::size_t>(Read(fragment.subspan(12, 4)));
  const auto offset = static_cast<std::size_t>(Read(fragment.subspan(16, 4)));
  if (id == 0 || total == 0 || total > protocol::kMaxFrameSize || offset >= total ||
      offset % kFragmentPayloadSize != 0 || fragment.size() - kHeader != (std::min)(kFragmentPayloadSize, total - offset))
    throw std::invalid_argument("invalid fragment range");
  const auto now = std::chrono::steady_clock::now();
  for (auto iterator = pending_.begin(); iterator != pending_.end();) {
    if (now - iterator->second.touched > std::chrono::seconds(60)) {
      bytes_ -= iterator->second.wire.size(); iterator = pending_.erase(iterator);
    } else ++iterator;
  }
  auto found = pending_.find(id);
  if (found == pending_.end()) {
    if (pending_.size() >= 32 || total > 32U * 1024U * 1024U - bytes_)
      throw std::length_error("per-channel reassembly budget exceeded");
    found = pending_.emplace(id, Partial{std::vector<std::uint8_t>(total),
        std::vector<bool>((total + kFragmentPayloadSize - 1) / kFragmentPayloadSize), 0, now}).first;
    bytes_ += total;
  }
  auto& partial = found->second;
  if (partial.wire.size() != total) throw std::invalid_argument("fragment total changed");
  const auto index = offset / kFragmentPayloadSize;
  const auto payload = fragment.subspan(kHeader);
  if (partial.received[index]) {
    if (!std::equal(payload.begin(), payload.end(), partial.wire.begin() + offset))
      throw std::invalid_argument("duplicate fragment changed bytes");
  } else {
    std::copy(payload.begin(), payload.end(), partial.wire.begin() + offset);
    partial.received[index] = true; ++partial.count;
  }
  partial.touched = now;
  if (partial.count != partial.received.size()) return std::nullopt;
  bytes_ -= total;
  auto wire = std::move(partial.wire);
  pending_.erase(found);
  return wire;
}
}  // namespace veritassync::transport
