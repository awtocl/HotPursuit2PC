// minitest - tiny single-header test harness for HotPursuit2PC.
//
//   TEST(name) { CHECK(x); CHECK_EQ(a, b); REQUIRE(ptr); SKIP("why"); }
//   MINITEST_MAIN()
//
// Exit codes: 0 = all passed, 1 = at least one failure,
//             77 = nothing failed and at least one test was skipped and none ran
//                  to completion (CTest SKIP_RETURN_CODE).
// Usage: test.exe [substring-filter]
#pragma once

#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace minitest {

struct Case {
  const char* name;
  void (*fn)();
};

inline std::vector<Case>& registry() {
  static std::vector<Case> r;
  return r;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

struct RequireFailed {};
struct Skipped {
  std::string why;
};

inline int& current_failures() {
  static int n = 0;
  return n;
}

inline void report(const char* file, int line, const std::string& what) {
  ++current_failures();
  std::printf("    FAILED %s:%d: %s\n", file, line, what.c_str());
}

template <typename T>
std::string show(const T& v) {
  std::ostringstream os;
  os << v;
  return os.str();
}
inline std::string show(const std::filesystem::path& p) { return p.string(); }
inline std::string show(unsigned char v) { return std::to_string(static_cast<unsigned>(v)); }
inline std::string show(bool v) { return v ? "true" : "false"; }

inline int run_all(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int passed = 0, failed = 0, skipped = 0;
  for (const Case& c : registry()) {
    if (filter && !std::strstr(c.name, filter)) continue;
    current_failures() = 0;
    std::printf("[ RUN  ] %s\n", c.name);
    bool was_skipped = false;
    try {
      c.fn();
    } catch (const RequireFailed&) {
      // already reported
    } catch (const Skipped& s) {
      was_skipped = true;
      std::printf("    skipped: %s\n", s.why.c_str());
    } catch (const std::exception& e) {
      report("(exception)", 0, e.what());
    } catch (...) {
      report("(exception)", 0, "unknown exception");
    }
    if (current_failures()) {
      ++failed;
      std::printf("[ FAIL ] %s\n", c.name);
    } else if (was_skipped) {
      ++skipped;
      std::printf("[ SKIP ] %s\n", c.name);
    } else {
      ++passed;
      std::printf("[  OK  ] %s\n", c.name);
    }
    std::fflush(stdout);
  }
  std::printf("%d passed, %d failed, %d skipped\n", passed, failed, skipped);
  if (failed) return 1;
  if (passed == 0 && skipped > 0) return 77;
  if (passed == 0) {
    std::printf("no tests matched\n");
    return 1;
  }
  return 0;
}

}  // namespace minitest

#define TEST(name)                                                        \
  static void minitest_fn_##name();                                       \
  static ::minitest::Registrar minitest_reg_##name(#name, &minitest_fn_##name); \
  static void minitest_fn_##name()

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) ::minitest::report(__FILE__, __LINE__, "CHECK(" #cond ")"); \
  } while (0)

#define CHECK_EQ(a, b)                                                    \
  do {                                                                    \
    const auto& minitest_a = (a);                                         \
    const auto& minitest_b = (b);                                         \
    if (!(minitest_a == minitest_b))                                      \
      ::minitest::report(__FILE__, __LINE__,                              \
                         std::string("CHECK_EQ(" #a ", " #b ") -> ") +    \
                             ::minitest::show(minitest_a) + " vs " +      \
                             ::minitest::show(minitest_b));               \
  } while (0)

#define REQUIRE(cond)                                                     \
  do {                                                                    \
    if (!(cond)) {                                                        \
      ::minitest::report(__FILE__, __LINE__, "REQUIRE(" #cond ")");       \
      throw ::minitest::RequireFailed{};                                  \
    }                                                                     \
  } while (0)

#define CHECK_THROWS(expr)                                                \
  do {                                                                    \
    bool minitest_threw = false;                                          \
    try {                                                                 \
      (void)(expr);                                                       \
    } catch (...) {                                                       \
      minitest_threw = true;                                              \
    }                                                                     \
    if (!minitest_threw)                                                  \
      ::minitest::report(__FILE__, __LINE__, "CHECK_THROWS(" #expr ")");  \
  } while (0)

#define SKIP(why) throw ::minitest::Skipped{why}

#define MINITEST_MAIN()                                                   \
  int main(int argc, char** argv) { return ::minitest::run_all(argc, argv); }
