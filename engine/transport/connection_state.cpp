#include "engine/transport/connection_state.h"

#include <stdexcept>

namespace veritassync::transport {
bool IsValidTransition(ConnectionState from, ConnectionState to) {
  switch (from) {
    case ConnectionState::kNew:
      return to == ConnectionState::kSignaling || to == ConnectionState::kClosed;
    case ConnectionState::kSignaling:
      return to == ConnectionState::kConnecting || to == ConnectionState::kFailed || to == ConnectionState::kClosed;
    case ConnectionState::kConnecting:
      return to == ConnectionState::kConnected || to == ConnectionState::kReconnecting || to == ConnectionState::kFailed || to == ConnectionState::kClosed;
    case ConnectionState::kConnected:
      return to == ConnectionState::kReconnecting || to == ConnectionState::kFailed || to == ConnectionState::kClosed;
    case ConnectionState::kReconnecting:
      return to == ConnectionState::kConnecting || to == ConnectionState::kFailed || to == ConnectionState::kClosed;
    case ConnectionState::kFailed:
      return to == ConnectionState::kClosed;
    case ConnectionState::kClosed:
      return false;
  }
  return false;
}

ConnectionStateMachine::ConnectionStateMachine(Observer observer) : observer_(std::move(observer)) {}
void ConnectionStateMachine::TransitionTo(ConnectionState next) {
  if (!IsValidTransition(state_, next)) throw std::logic_error("invalid peer connection state transition");
  const auto previous = state_;
  state_ = next;
  if (observer_) observer_(previous, next);
}
ConnectionState ConnectionStateMachine::State() const { return state_; }
}  // namespace veritassync::transport
