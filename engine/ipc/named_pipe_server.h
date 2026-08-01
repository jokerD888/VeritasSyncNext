#pragma once

#include "engine/ipc/ipc_service.h"

#include <string>

namespace veritassync::ipc {

// Runs a Windows message-mode named pipe until the `shutdown` IPC command is
// received. The desktop shell is only a client; terminating it does not close
// this server process.
int RunNamedPipeServer(IpcService& service, const std::string& pipe_name);

}  // namespace veritassync::ipc
