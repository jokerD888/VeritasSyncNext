#pragma once

#include <functional>

namespace veritassync::transport {

enum class ConnectionState {
  kNew,
  kSignaling,
  kConnecting,
  kConnected,
  kReconnecting,
  kFailed,
  kClosed,
};

[[nodiscard]] bool IsValidTransition(ConnectionState from, ConnectionState to);

class ConnectionStateMachine {
 public:
  using Observer = std::function<void(ConnectionState previous, ConnectionState current)>;

  explicit ConnectionStateMachine(Observer observer = {});
  void TransitionTo(ConnectionState next);
  [[nodiscard]] ConnectionState State() const;

 private:
  ConnectionState state_ = ConnectionState::kNew;
  Observer observer_;
};

}  // namespace veritassync::transport
