#include "tests/test_framework.h"

#include <exception>
#include <cstdlib>
#include <iostream>
#include <string_view>

int main() {
  int failures = 0;
  char* filter = nullptr;
  std::size_t filter_size = 0;
  if (_dupenv_s(&filter, &filter_size, "VSYNC_TEST_FILTER") != 0) return 2;
  for (const auto& test : veritassync::test::Registry()) {
    if (filter != nullptr && std::string_view(test.name).find(filter) == std::string_view::npos) continue;
    try { test.run(); std::cout << "[PASS] " << test.name << std::endl; }
    catch (const std::exception& error) { ++failures; std::cerr << "[FAIL] " << test.name << ": " << error.what() << std::endl; }
  }
  std::free(filter);
  return failures == 0 ? 0 : 1;
}
