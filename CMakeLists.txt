cmake_minimum_required(VERSION 3.25)

project(VeritasSyncNext VERSION 0.1.0 LANGUAGES CXX)

option(VERITASSYNC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(unofficial-sqlite3 CONFIG REQUIRED)

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
  engine/storage/database.cpp
  engine/transport/mock_transport.cpp
  engine/sync/phase0_node.cpp
)
target_include_directories(veritassync_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(veritassync_engine PUBLIC unofficial::sqlite3::sqlite3)

veritassync_apply_warnings(veritassync_engine)

add_executable(veritassync-engine engine/app/main.cpp)
target_link_libraries(veritassync-engine PRIVATE veritassync_engine)
veritassync_apply_warnings(veritassync-engine)

include(CTest)
if(BUILD_TESTING)
  add_executable(veritassync_tests
    tests/test_main.cpp
    tests/protocol_test.cpp
    tests/database_test.cpp
    tests/mock_sync_test.cpp
  )
  target_link_libraries(veritassync_tests PRIVATE veritassync_engine)
  veritassync_apply_warnings(veritassync_tests)
  add_test(NAME veritassync_tests COMMAND veritassync_tests)
endif()
