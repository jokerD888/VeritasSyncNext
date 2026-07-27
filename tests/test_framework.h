#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace veritassync::test {
struct Test { std::string name; std::function<void()> run; };
inline std::vector<Test>& Registry() { static std::vector<Test> tests; return tests; }
class Registration { public: Registration(std::string name, std::function<void()> run) { Registry().push_back({std::move(name), std::move(run)}); } };
inline void Check(bool condition, const char* expression) { if (!condition) throw std::runtime_error(std::string("check failed: ") + expression); }
template <typename Callable> void CheckThrows(Callable&& callable) { bool threw = false; try { callable(); } catch (const std::exception&) { threw = true; } Check(threw, "expected exception"); }
}
#define VSYNC_TEST(name) void name(); static veritassync::test::Registration registration_##name(#name, name); void name()
#define VSYNC_CHECK(expression) veritassync::test::Check((expression), #expression)
#define VSYNC_CHECK_THROWS(expression) veritassync::test::CheckThrows([&] { (void)(expression); })
