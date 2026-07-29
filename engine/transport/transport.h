#pragma once

#include "engine/common/protocol.h"

#include <functional>
#include <vector>

namespace veritassync::transport {
class Transport {
 public:
  using ReceiveCallback = std::function<void(protocol::Channel, std::vector<std::uint8_t>)>;
  virtual ~Transport() = default;
  virtual void Send(protocol::Channel channel, std::vector<std::uint8_t> wire) = 0;
  virtual void SetReceiveCallback(ReceiveCallback callback) = 0;
};
}  // namespace veritassync::transport
