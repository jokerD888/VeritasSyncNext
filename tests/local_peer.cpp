// Local integration harness. Deliberately loopback-only, and never bundled.
#include <winsock2.h>
#include <ws2tcpip.h>
#include "engine/common/path.h"
#include "engine/ipc/ipc_service.h"
#include "engine/ipc/named_pipe_server.h"
#include "engine/runtime/task_runtime_manager.h"
#include "engine/sync/bidirectional_sync.h"
#include "engine/sync/one_way_sync.h"
#include "engine/transport/transport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <iostream>
#include <map>
#include <thread>

namespace {
class SocketTransport final : public veritassync::transport::Transport {
 public:
  explicit SocketTransport(SOCKET socket, std::size_t send_budget) : socket_(socket), send_budget_(send_budget) {
    u_long nonblocking = 1;
    if (ioctlsocket(socket_, FIONBIO, &nonblocking) != 0) throw std::runtime_error("nonblocking socket failed");
  }
  ~SocketTransport() override { closesocket(socket_); }
  void Send(veritassync::protocol::Channel channel, std::vector<std::uint8_t> wire) override {
    if (wire.empty() || wire.size() > veritassync::protocol::kMaxFrameSize ||
        queued_ + wire.size() > 32U * 1024U * 1024U) throw std::runtime_error("loopback send budget exceeded");
    std::vector<std::uint8_t> packet;
    packet.reserve(wire.size() + 5);
    packet.push_back(channel == veritassync::protocol::Channel::kControl ? 0 : 1);
    for (unsigned index = 0; index < 4; ++index) packet.push_back(static_cast<std::uint8_t>(wire.size() >> (index * 8)));
    packet.insert(packet.end(), wire.begin(), wire.end());
    queued_ += packet.size();
    outbound_.push_back(std::move(packet));
  }
  std::size_t BufferedAmount(veritassync::protocol::Channel) const override { return queued_; }
  void SetReceiveCallback(ReceiveCallback callback) override { callback_ = std::move(callback); }
  void Pump() {
    auto budget = send_budget_;
    while (!outbound_.empty() && budget > 0) {
      const auto& packet = outbound_.front();
      const int count = send(socket_, reinterpret_cast<const char*>(packet.data() + offset_),
                             static_cast<int>((std::min)({packet.size() - offset_, std::size_t{65536}, budget})), 0);
      if (count < 0 && WSAGetLastError() == WSAEWOULDBLOCK) break;
      if (count <= 0) throw std::runtime_error("loopback connection closed while sending");
      offset_ += count;
      queued_ -= count;
      budget -= count;
      if (offset_ == packet.size()) { outbound_.pop_front(); offset_ = 0; }
    }
    std::uint8_t bytes[65536];
    while (true) {
      const int count = recv(socket_, reinterpret_cast<char*>(bytes), sizeof(bytes), 0);
      if (count < 0 && WSAGetLastError() == WSAEWOULDBLOCK) break;
      if (count <= 0) throw std::runtime_error("loopback connection closed while receiving");
      inbound_.insert(inbound_.end(), bytes, bytes + count);
      std::size_t consumed = 0;
      while (inbound_.size() - consumed >= 5) {
        const auto* header = inbound_.data() + consumed;
        std::uint32_t length = 0;
        for (unsigned index = 0; index < 4; ++index) length |= std::uint32_t{header[index + 1]} << (index * 8);
        if (header[0] > 1 || length == 0 || length > veritassync::protocol::kMaxFrameSize)
          throw std::runtime_error("invalid loopback envelope");
        if (inbound_.size() - consumed < length + 5ULL) break;
        if (callback_) callback_(header[0] == 0 ? veritassync::protocol::Channel::kControl : veritassync::protocol::Channel::kBulk,
                                {header + 5, header + 5 + length});
        consumed += 5 + length;
      }
      inbound_.erase(inbound_.begin(), inbound_.begin() + static_cast<std::ptrdiff_t>(consumed));
    }
  }
 private:
  SOCKET socket_;
  ReceiveCallback callback_;
  std::deque<std::vector<std::uint8_t>> outbound_;
  std::vector<std::uint8_t> inbound_;
  std::size_t offset_ = 0, queued_ = 0;
  std::size_t send_budget_;
};

SOCKET Connect(unsigned short port, bool listen_mode, const std::atomic_bool& stopping) {
  SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == INVALID_SOCKET) throw std::runtime_error("cannot create local socket");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (listen_mode) {
    BOOL exclusive = TRUE;
    setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(socket, 1) != 0) {
      closesocket(socket);
      throw std::runtime_error("cannot bind loopback listener");
    }
    u_long nonblocking = 1;
    ioctlsocket(socket, FIONBIO, &nonblocking);
    SOCKET peer = INVALID_SOCKET;
    while (!stopping) {
      peer = accept(socket, nullptr, nullptr);
      if (peer != INVALID_SOCKET) break;
      if (WSAGetLastError() != WSAEWOULDBLOCK) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    closesocket(socket);
    return peer;
  }
  if (connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) return socket;
  closesocket(socket);
  return INVALID_SOCKET;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  using namespace veritassync;
  try {
    std::map<std::string, std::string> args;
    const auto utf8_argument = [](const wchar_t* value) {
      const auto text = std::filesystem::path(value).u8string();
      return std::string(text.begin(), text.end());
    };
    for (int index = 1; index < argc; ++index) {
      const std::string option = utf8_argument(argv[index]);
      if (option == "--listen") args[option] = "1";
      else if (index + 1 < argc) args[option] = utf8_argument(argv[++index]);
      else throw std::invalid_argument("missing argument");
    }
    for (const auto* option : {"--db", "--root", "--role", "--mode", "--device", "--peer", "--port", "--pipe"})
      if (!args.contains(option)) throw std::invalid_argument(std::string("required: ") + option);
    const auto number = std::stoul(args.at("--port"));
    if (number == 0 || number > 65535) throw std::invalid_argument("invalid loopback port");
    // Diagnostic-only pacing makes a reproducible mid-transfer process crash possible.
    const auto send_budget = args.contains("--send-budget") ? std::stoul(args.at("--send-budget")) : 32U * 1024U * 1024U;
    if (send_budget == 0 || send_budget > 32U * 1024U * 1024U) throw std::invalid_argument("invalid send budget");
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) throw std::runtime_error("Winsock startup failed");
    storage::Database database(common::Utf8Path(args.at("--db")));
    database.ApplyMigrations();
    if (!database.FindTask("local").has_value())
      database.CreateTask({"local", args.at("--mode"), args.at("--role"), args.at("--root")});
    runtime::TaskRuntimeManager runtime(database, args.at("--device"));
    std::atomic_bool stopping{false}, changed{true};
    std::mutex proposal_mutex;
    std::vector<std::array<std::string, 3>> proposals;
    runtime.SetScanCompletedCallback([&](const std::string&) { changed = true; });
    runtime.Start();
    std::thread worker([&] {
      while (!stopping) {
        try {
          const SOCKET socket = Connect(static_cast<unsigned short>(number), args.contains("--listen"), stopping);
          if (socket == INVALID_SOCKET) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
          SocketTransport transport(socket, send_budget);
          std::unique_ptr<sync::OneWaySyncNode> one_way;
          std::unique_ptr<sync::BidirectionalSyncNode> bidirectional;
          {
            std::scoped_lock lock(database.AccessMutex());
            if (args.at("--mode") == "bidirectional") {
              bidirectional = std::make_unique<sync::BidirectionalSyncNode>(
                  sync::BidirectionalSyncConfig{"local", args.at("--device"), args.at("--peer"),
                      args.at("--device") + "-fingerprint", args.at("--peer") + "-fingerprint",
                      "local-test-only", common::Utf8Path(args.at("--root")), database}, transport);
              bidirectional->Start();
            } else {
              const auto role = args.at("--role") == "source" ? protocol::Role::kSource : protocol::Role::kTarget;
              one_way = std::make_unique<sync::OneWaySyncNode>(
                  sync::OneWaySyncConfig{"local", role, args.at("--device"), args.at("--peer"),
                      args.at("--device") + "-fingerprint", "local-test-only", common::Utf8Path(args.at("--root")), database}, transport);
              one_way->Start();
            }
          }
          while (!stopping) {
            {
              std::scoped_lock lock(database.AccessMutex());
              if (database.RuntimeState("local").enabled) {
                transport.Pump();
                std::vector<std::array<std::string, 3>> pending_proposals;
                { std::scoped_lock proposal_lock(proposal_mutex); pending_proposals.swap(proposals); }
                for (const auto& proposal : pending_proposals) {
                  if (!bidirectional) throw std::runtime_error("policy proposal requires a bidirectional node");
                  bidirectional->ProposeIgnorePolicy(proposal[0], proposal[1], proposal[2]);
                }
                if (changed.exchange(false)) {
                  if (bidirectional) bidirectional->RefreshLocal();
                  else if (args.at("--role") == "source") one_way->RefreshSource();
                }
                if (bidirectional) bidirectional->Pump(); else one_way->Pump();
                const auto error = bidirectional ? bidirectional->LastError() : one_way->LastError();
                if (error) throw std::runtime_error(*error);
                database.UpdateTaskNetworkState("local", "online");
              }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
          }
        } catch (const std::exception& error) {
          std::cerr << error.what() << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
    ipc::IpcService service(database, nullptr, &runtime, nullptr,
        [&](const std::string&, std::string hash, std::string rules, std::string source) {
          std::scoped_lock lock(proposal_mutex);
          proposals.push_back({std::move(hash), std::move(rules), std::move(source)});
        });
    const int result = ipc::RunNamedPipeServer(service, args.at("--pipe"));
    stopping = true;
    worker.join();
    runtime.Stop();
    WSACleanup();
    return result;
  } catch (const std::exception& error) { std::cerr << error.what() << std::endl; return 1; }
}
