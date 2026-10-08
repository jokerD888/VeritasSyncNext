#pragma once
#include <string>
#include <vector>

namespace veritassync::transport {
struct IceServer {
  std::string url;
  std::string username;
  std::string credential;
};
struct IceOptions {
  std::vector<IceServer> servers;
  bool relay_only = false;
  void Validate() const;
  static IceOptions FromEnvironment();
};
}  // namespace veritassync::transport
