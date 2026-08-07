#include "engine/transport/mock_transport.h"

#include <stdexcept>

namespace veritassync::transport {
MockEndpoint::MockEndpoint(MockNetwork& network) : network_(network) {}
void MockEndpoint::Send(protocol::Channel channel, std::vector<std::uint8_t> wire) { network_.Enqueue(this, channel, std::move(wire)); }
std::size_t MockEndpoint::BufferedAmount(const protocol::Channel channel) const {
  return channel == protocol::Channel::kControl ? control_buffered_amount_ : bulk_buffered_amount_;
}
void MockEndpoint::SetBufferedAmount(const protocol::Channel channel, const std::size_t amount) {
  if (channel == protocol::Channel::kControl) control_buffered_amount_ = amount;
  else bulk_buffered_amount_ = amount;
}
void MockEndpoint::SetReceiveCallback(ReceiveCallback callback) { callback_ = std::move(callback); }
MockNetwork::Pair MockNetwork::CreatePair() { if (first_ != nullptr) throw std::logic_error("mock network supports one pair"); auto first = std::make_unique<MockEndpoint>(*this); auto second = std::make_unique<MockEndpoint>(*this); first_ = first.get(); second_ = second.get(); return {std::move(first), std::move(second)}; }
void MockNetwork::Enqueue(MockEndpoint* sender, protocol::Channel channel, std::vector<std::uint8_t> wire) { MockEndpoint* recipient = sender == first_ ? second_ : first_; if (recipient == nullptr) throw std::logic_error("unknown mock endpoint"); pending_.push_back({recipient, channel, std::move(wire)}); }
bool MockNetwork::PumpOne() { if (pending_.empty()) return false; auto next = std::move(pending_.front()); pending_.pop_front(); if (next.recipient->callback_) next.recipient->callback_(next.channel, std::move(next.wire)); return true; }
void MockNetwork::PumpUntilIdle() { while (PumpOne()) {} }
}  // namespace veritassync::transport
