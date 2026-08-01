#pragma once

#include "engine/storage/database.h"

#include <string>
#include <string_view>

namespace veritassync::ipc {

inline constexpr std::string_view kIpcProtocol = "VSYNC_IPC/1";

// Stateless command router for the desktop shell. Requests and responses are
// UTF-8 tab-separated records with percent escaping; every request is bounded
// to one named-pipe message and carries kIpcProtocol as its first field.
class IpcService {
 public:
  explicit IpcService(storage::Database& database);
  [[nodiscard]] std::string Handle(std::string_view request, bool* should_shutdown = nullptr);

 private:
  storage::Database& database_;
};

}  // namespace veritassync::ipc
