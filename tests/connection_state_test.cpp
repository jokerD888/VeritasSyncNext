#include "engine/transport/connection_state.h"
#include "tests/test_framework.h"

#include <vector>

VSYNC_TEST(ConnectionStateMachineModelsConnectAndIceRestart) {
  using veritassync::transport::ConnectionState;
  std::vector<ConnectionState> observed;
  veritassync::transport::ConnectionStateMachine machine([&](ConnectionState, ConnectionState next) { observed.push_back(next); });
  machine.TransitionTo(ConnectionState::kSignaling);
  machine.TransitionTo(ConnectionState::kConnecting);
  machine.TransitionTo(ConnectionState::kConnected);
  machine.TransitionTo(ConnectionState::kReconnecting);
  machine.TransitionTo(ConnectionState::kConnecting);
  machine.TransitionTo(ConnectionState::kConnected);
  VSYNC_CHECK(machine.State() == ConnectionState::kConnected);
  VSYNC_CHECK(observed.size() == 6);
}

VSYNC_TEST(ConnectionStateMachineRejectsSkippedStates) {
  using veritassync::transport::ConnectionState;
  veritassync::transport::ConnectionStateMachine machine;
  VSYNC_CHECK_THROWS(machine.TransitionTo(ConnectionState::kConnected));
  machine.TransitionTo(ConnectionState::kSignaling);
  machine.TransitionTo(ConnectionState::kFailed);
  VSYNC_CHECK_THROWS(machine.TransitionTo(ConnectionState::kConnecting));
}
