#include "engine/ipc/named_pipe_server.h"

#include <Windows.h>
#include <sddl.h>

#include <array>
#include <stdexcept>
#include <string>

namespace veritassync::ipc {
namespace {
constexpr DWORD kBufferSize = 64U * 1024U;

[[nodiscard]] std::wstring ToWide(const std::string& utf8) {
  if (utf8.empty()) throw std::invalid_argument("IPC pipe name is required");
  const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                          static_cast<int>(utf8.size()), nullptr, 0);
  if (length <= 0) throw std::invalid_argument("IPC pipe name is not valid UTF-8");
  std::wstring wide(static_cast<std::size_t>(length), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                          wide.data(), length) != length) throw std::runtime_error("cannot encode IPC pipe name");
  return wide;
}

class OwnerOnlyPipeSecurity {
 public:
  OwnerOnlyPipeSecurity() {
    // OW resolves to the security descriptor owner (the launching user). The
    // protected DACL prevents inherited broad ACLs from exposing desktop IPC to
    // another local account on the same machine.
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;OW)",
                                                               SDDL_REVISION_1,
                                                               &descriptor_, nullptr)) {
      throw std::runtime_error("cannot create IPC security descriptor");
    }
    attributes_.nLength = sizeof(attributes_);
    attributes_.lpSecurityDescriptor = descriptor_;
    attributes_.bInheritHandle = FALSE;
  }
  ~OwnerOnlyPipeSecurity() { if (descriptor_ != nullptr) LocalFree(descriptor_); }
  [[nodiscard]] SECURITY_ATTRIBUTES* Attributes() { return &attributes_; }

 private:
  PSECURITY_DESCRIPTOR descriptor_ = nullptr;
  SECURITY_ATTRIBUTES attributes_{};
};
}

int RunNamedPipeServer(IpcService& service, const std::string& pipe_name) {
  const auto wide_name = ToWide(pipe_name);
  OwnerOnlyPipeSecurity security;
  bool shutdown = false;
  while (!shutdown) {
    const HANDLE pipe = CreateNamedPipeW(wide_name.c_str(), PIPE_ACCESS_DUPLEX,
                                         PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                         PIPE_UNLIMITED_INSTANCES, kBufferSize, kBufferSize, 0, security.Attributes());
    if (pipe == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot create IPC named pipe");
    const BOOL connected = ConnectNamedPipe(pipe, nullptr);
    if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) { CloseHandle(pipe); throw std::runtime_error("cannot accept IPC client"); }
    std::array<char, kBufferSize> request{};
    DWORD bytes_read = 0;
    if (!ReadFile(pipe, request.data(), static_cast<DWORD>(request.size()), &bytes_read, nullptr)) {
      DisconnectNamedPipe(pipe); CloseHandle(pipe); continue;
    }
    const auto response = service.Handle(std::string_view(request.data(), bytes_read), &shutdown);
    DWORD bytes_written = 0;
    (void)WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()), &bytes_written, nullptr);
    FlushFileBuffers(pipe); DisconnectNamedPipe(pipe); CloseHandle(pipe);
  }
  return 0;
}

}  // namespace veritassync::ipc
