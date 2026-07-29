#pragma once

#include "engine/transport/transport.h"

#include <functional>
#include <memory>
#include <vector>

namespace veritassync::transport {

class MockNetwork;
class MockEndpoint final : public Transport {
 public:
  explicit MockEndpoint(MockNetwork& network);
  void Send(protocol::Channel channel, std::vector<std::uint8_t> wire) override;
  void SetReceiveCallback(ReceiveCallback callback) override;
 private:
  MockNetwork& network_;
  ReceiveCallback callback_;
  friend class MockNetwork;
};

class MockNetwork {
 public:
  struct Pair { std::unique_ptr<MockEndpoint> first; std::unique_ptr<MockEndpoint> second; };
  [[nodiscard]] Pair CreatePair();
  void PumpUntilIdle();
 private:
  struct Pending { MockEndpoint* recipient; protocol::Channel channel; std::vector<std::uint8_t> wire; };
  void Enqueue(MockEndpoint* sender, protocol::Channel channel, std::vector<std::uint8_t> wire);
  MockEndpoint* first_ = nullptr;
  MockEndpoint* second_ = nullptr;
  std::vector<Pending> pending_;
  friend class MockEndpoint;
};
}  // namespace veritassync::transport
