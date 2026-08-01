#pragma once

#include <memory>
#include <string>

namespace spdlog {
class logger;
}

namespace veritassync::common {

// Matches the proven VeritasSync logging model: an asynchronous logger writes
// human-readable rolling files while warnings are flushed immediately.
extern std::shared_ptr<spdlog::logger> g_logger;

void InitializeLogger(const std::string& log_filename);
void SetLogLevel(const std::string& level);

}  // namespace veritassync::common
