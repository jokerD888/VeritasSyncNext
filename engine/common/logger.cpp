#include "engine/common/logger.h"

#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <iostream>
#include <mutex>
#include <unordered_map>

namespace veritassync::common {
namespace {

constexpr std::size_t kMaximumFileBytes = 5U * 1024U * 1024U;
constexpr std::size_t kMaximumFiles = 3U;
constexpr std::size_t kQueueSize = 8192U;
std::once_flag g_initialize_once;
std::string g_log_filename;

}  // namespace

std::shared_ptr<spdlog::logger> g_logger;

void InitializeLogger(const std::string& log_filename) {
  g_log_filename = log_filename;
  std::call_once(g_initialize_once, [] {
    try {
      auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
      auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
          g_log_filename, kMaximumFileBytes, kMaximumFiles);
      console->set_level(spdlog::level::info);
      file->set_level(spdlog::level::info);
      spdlog::init_thread_pool(kQueueSize, 1);
      g_logger = std::make_shared<spdlog::async_logger>(
          "veritassync-next", spdlog::sinks_init_list{console, file},
          spdlog::thread_pool(), spdlog::async_overflow_policy::block);
      g_logger->set_level(spdlog::level::info);
      g_logger->flush_on(spdlog::level::warn);
      // Keep the old asynchronous hot path while making the desktop terminal
      // useful for normal info-level lifecycle messages.
      spdlog::flush_every(std::chrono::seconds(1));
      spdlog::register_logger(g_logger);
      spdlog::set_default_logger(g_logger);
    } catch (const spdlog::spdlog_ex& error) {
      std::cerr << "log initialization failed: " << error.what() << "\n";
    }
  });
}

void SetLogLevel(const std::string& level) {
  if (!g_logger) return;
  static const std::unordered_map<std::string, spdlog::level::level_enum> levels{
      {"debug", spdlog::level::debug}, {"info", spdlog::level::info},
      {"warn", spdlog::level::warn}, {"warning", spdlog::level::warn},
      {"error", spdlog::level::err}, {"critical", spdlog::level::critical},
      {"off", spdlog::level::off}};
  const auto found = levels.find(level);
  if (found == levels.end()) {
    g_logger->warn("[Logger] unknown level '{}'; keeping current level", level);
    return;
  }
  g_logger->set_level(found->second);
  for (const auto& sink : g_logger->sinks()) sink->set_level(found->second);
  g_logger->info("[Logger] level changed to {}", level);
}

}  // namespace veritassync::common
