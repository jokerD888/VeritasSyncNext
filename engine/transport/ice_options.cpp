#include "engine/transport/ice_options.h"
#include <cstdlib>
#include <stdexcept>

namespace veritassync::transport {
namespace {
std::string Environment(const char* name) {
  char* value = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&value, &size, name) != 0) throw std::runtime_error("cannot read ICE configuration");
  const std::string result = value == nullptr ? "" : value;
  std::free(value);
  return result;
}
}
void IceOptions::Validate() const {
  if (servers.size() > 16) throw std::invalid_argument("at most 16 ICE servers are allowed");
  bool turn = false;
  for (const auto& server : servers) {
    const bool relay = server.url.starts_with("turn:") || server.url.starts_with("turns:");
    if (!relay && !server.url.starts_with("stun:") && !server.url.starts_with("stuns:"))
      throw std::invalid_argument("ICE server URL must use stun, stuns, turn or turns");
    const auto host = server.url.substr(server.url.find(':') + 1);
    if (host.empty() || host.find_first_of(" \t\r\n@") != std::string::npos ||
        server.url.size() > 2048 || server.username.size() > 1024 || server.credential.size() > 4096)
      throw std::invalid_argument("invalid ICE server configuration");
    if (relay && (server.username.empty() || server.credential.empty()))
      throw std::invalid_argument("TURN servers require a username and credential");
    turn = turn || relay;
  }
  if (relay_only && !turn) throw std::invalid_argument("relay-only mode requires a TURN server");
}
IceOptions IceOptions::FromEnvironment() {
  IceOptions options;
  const auto urls = Environment("VERITASSYNC_ICE_URLS");
  const auto username = Environment("VERITASSYNC_TURN_USERNAME");
  const auto credential = Environment("VERITASSYNC_TURN_CREDENTIAL");
  const auto relay = Environment("VERITASSYNC_ICE_RELAY_ONLY");
  if (!relay.empty() && relay != "0" && relay != "1")
    throw std::invalid_argument("VERITASSYNC_ICE_RELAY_ONLY must be 0 or 1");
  options.relay_only = relay == "1";
  for (std::size_t begin = 0; begin < urls.size();) {
    const auto end = urls.find(';', begin);
    const auto url = urls.substr(begin, end == std::string::npos ? end : end - begin);
    const bool turn = url.starts_with("turn:") || url.starts_with("turns:");
    options.servers.push_back({url, turn ? username : "", turn ? credential : ""});
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  options.Validate();
  return options;
}
}  // namespace veritassync::transport
