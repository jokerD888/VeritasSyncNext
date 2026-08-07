cmake_minimum_required(VERSION 3.25)

project(VeritasSyncNext VERSION 0.1.0 LANGUAGES CXX)

option(VERITASSYNC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(VERITASSYNC_ENABLE_WEBRTC "Verify the pinned libwebrtc build required by the adapter" OFF)
option(VERITASSYNC_BUILD_BENCHMARKS "Build deterministic local performance benchmarks" ON)
set(VERITASSYNC_WEBRTC_ROOT "" CACHE PATH "Pinned libwebrtc source checkout")
set(VERITASSYNC_WEBRTC_LIBRARY "" CACHE FILEPATH "libwebrtc static library built from the pinned checkout")
set(VERITASSYNC_WEBRTC_BRIDGE_LIBRARY "" CACHE FILEPATH "C ABI bridge DLL built from the pinned libwebrtc checkout")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(blake3 CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)

function(veritassync_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
    if(VERITASSYNC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(VERITASSYNC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

add_library(veritassync_engine
  engine/common/protocol.cpp
  engine/common/content_hash.cpp
  engine/common/uuid.cpp
  engine/security/device_identity.cpp
  engine/storage/database.cpp
  engine/storage/ignore_rules.cpp
  engine/storage/ignore_policy.cpp
  engine/storage/manifest_scanner.cpp
  engine/storage/safe_file_writer.cpp
  engine/ipc/ipc_service.cpp
  engine/ipc/named_pipe_server.cpp
  engine/transport/mock_transport.cpp
  engine/transport/connection_state.cpp
  engine/transport/send_scheduler.cpp
  engine/transport/webrtc_bridge_loader.cpp
  engine/transport/webrtc_transport.cpp
  engine/signaling/tracker_contract.cpp
  engine/signaling/tracker_client.cpp
  engine/signaling/signaling_session.cpp
  engine/sync/manifest_diff.cpp
  engine/sync/download_receiver.cpp
  engine/sync/chunk_source.cpp
  engine/sync/upload_session.cpp
  engine/sync/task_policy.cpp
  engine/sync/resume_request.cpp
  engine/sync/snapshot_reconciler.cpp
  engine/sync/phase0_node.cpp
  engine/sync/one_way_sync.cpp
  engine/sync/multi_target_source.cpp
  engine/sync/version_resolution.cpp
  engine/sync/bidirectional_sync.cpp
)
target_include_directories(veritassync_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(veritassync_engine PUBLIC unofficial::sqlite3::sqlite3)
target_link_libraries(veritassync_engine PUBLIC BLAKE3::blake3)
target_link_libraries(veritassync_engine PUBLIC unofficial-sodium::sodium)
if(WIN32)
  target_link_libraries(veritassync_engine PRIVATE kernel32 bcrypt advapi32 winhttp)
endif()

if(VERITASSYNC_BUILD_BENCHMARKS)
  add_executable(veritassync_benchmarks benchmarks/engine_benchmark.cpp)
  target_link_libraries(veritassync_benchmarks PRIVATE veritassync_engine)
  veritassync_apply_warnings(veritassync_benchmarks)
endif()

veritassync_apply_warnings(veritassync_engine)

if(VERITASSYNC_ENABLE_WEBRTC)
  if(NOT EXISTS "${VERITASSYNC_WEBRTC_ROOT}/.git")
    message(FATAL_ERROR "VERITASSYNC_WEBRTC_ROOT must be a libwebrtc checkout pinned by third_party/libwebrtc.lock")
  endif()
  if(NOT EXISTS "${VERITASSYNC_WEBRTC_LIBRARY}")
    message(FATAL_ERROR "VERITASSYNC_WEBRTC_LIBRARY must name the libwebrtc artifact from the pinned checkout")
  endif()
  if(NOT VERITASSYNC_WEBRTC_BRIDGE_LIBRARY STREQUAL "" AND
     NOT EXISTS "${VERITASSYNC_WEBRTC_BRIDGE_LIBRARY}")
    message(FATAL_ERROR "VERITASSYNC_WEBRTC_BRIDGE_LIBRARY must name the bridge DLL built by scripts/build-webrtc-bridge.ps1")
  endif()
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libwebrtc.lock" veritassync_webrtc_lock)
  string(REGEX MATCH "commit = \"[^\"]+\"" veritassync_webrtc_lock_match "${veritassync_webrtc_lock}")
  string(REGEX REPLACE "commit = \"([^\"]+)\"" "\\1" veritassync_webrtc_commit "${veritassync_webrtc_lock_match}")
  execute_process(
    COMMAND git -C "${VERITASSYNC_WEBRTC_ROOT}" rev-parse HEAD
    OUTPUT_VARIABLE veritassync_webrtc_checkout_commit
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE veritassync_webrtc_revision_result
  )
  if(NOT veritassync_webrtc_revision_result EQUAL 0 OR
     NOT veritassync_webrtc_checkout_commit STREQUAL veritassync_webrtc_commit)
    message(FATAL_ERROR "VERITASSYNC_WEBRTC_ROOT is not at the commit pinned by third_party/libwebrtc.lock")
  endif()
  add_custom_target(veritassync_webrtc_artifact
    COMMENT "Verified pinned libwebrtc ${veritassync_webrtc_commit}: ${VERITASSYNC_WEBRTC_LIBRARY}"
  )
  add_dependencies(veritassync_engine veritassync_webrtc_artifact)
endif()

add_executable(veritassync-engine engine/app/main.cpp)
target_link_libraries(veritassync-engine PRIVATE veritassync_engine)
veritassync_apply_warnings(veritassync-engine)

include(CTest)
if(BUILD_TESTING)
  add_executable(veritassync_tests
    tests/test_main.cpp
    tests/protocol_test.cpp
    tests/content_hash_test.cpp
    tests/uuid_test.cpp
    tests/device_identity_test.cpp
    tests/database_test.cpp
    tests/ipc_service_test.cpp
    tests/ignore_rules_test.cpp
    tests/ignore_policy_test.cpp
    tests/manifest_scanner_test.cpp
    tests/manifest_diff_test.cpp
    tests/download_receiver_test.cpp
    tests/chunk_source_test.cpp
    tests/upload_session_test.cpp
    tests/task_policy_test.cpp
    tests/resume_request_test.cpp
    tests/snapshot_reconciler_test.cpp
    tests/safe_file_writer_test.cpp
    tests/mock_sync_test.cpp
    tests/one_way_sync_test.cpp
    tests/multi_target_source_test.cpp
    tests/version_resolution_test.cpp
    tests/bidirectional_sync_test.cpp
    tests/connection_state_test.cpp
    tests/send_scheduler_test.cpp
    tests/tracker_contract_test.cpp
    tests/tracker_client_test.cpp
  )
  if(VERITASSYNC_ENABLE_WEBRTC AND NOT VERITASSYNC_WEBRTC_BRIDGE_LIBRARY STREQUAL "")
    target_sources(veritassync_tests PRIVATE tests/webrtc_bridge_loader_test.cpp)
  endif()
  target_link_libraries(veritassync_tests PRIVATE veritassync_engine)
  veritassync_apply_warnings(veritassync_tests)
  add_test(NAME veritassync_tests COMMAND veritassync_tests)
  if(VERITASSYNC_ENABLE_WEBRTC AND NOT VERITASSYNC_WEBRTC_BRIDGE_LIBRARY STREQUAL "")
    set_tests_properties(veritassync_tests PROPERTIES
      ENVIRONMENT "VERITASSYNC_WEBRTC_BRIDGE_LIBRARY=${VERITASSYNC_WEBRTC_BRIDGE_LIBRARY}")
  endif()
endif()
