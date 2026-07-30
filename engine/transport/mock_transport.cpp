#include "engine/transport/mock_transport.h"

#include <stdexcept>

namespace veritassync::transport {
MockEndpoint::MockEndpoint(MockNetwork& network) : network_(network) {}
void MockEndpoint::Send(protocol::Channel channel, std::vector<std::uint8_t> wire) { network_.Enqueue(this, channel, std::move(wire)); }
std::size_t MockEndpoint::BufferedAmount(protocol::Channel) const { return 0; }
void MockEndpoint::SetReceiveCallback(ReceiveCallback callback) { callback_ = std::move(callback); }
MockNetwork::Pair MockNetwork::CreatePair() { if (first_ != nullptr) throw std::logic_error("mock network supports one pair"); auto first = std::make_unique<MockEndpoint>(*this); auto second = std::make_unique<MockEndpoint>(*this); first_ = first.get(); second_ = second.get(); return {std::move(first), std::move(second)}; }
void MockNetwork::Enqueue(MockEndpoint* sender, protocol::Channel channel, std::vector<std::uint8_t> wire) { MockEndpoint* recipient = sender == first_ ? second_ : first_; if (recipient == nullptr) throw std::logic_error("unknown mock endpoint"); pending_.push_back({recipient, channel, std::move(wire)}); }
void MockNetwork::PumpUntilIdle() { while (!pending_.empty()) { auto next = std::move(pending_.front()); pending_.erase(pending_.begin()); if (next.recipient->callback_) next.recipient->callback_(next.channel, std::move(next.wire)); } }
}  // namespace veritassync::transport
