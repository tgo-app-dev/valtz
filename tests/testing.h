// A deliberately small test harness: TEST(suite, name) registers a
// function; CHECK records a failure and continues; REQUIRE records a
// failure and RETURNS from the test, so nothing after it runs against
// state that just failed.

#ifndef VALTZ_TESTS_TESTING_H
#define VALTZ_TESTS_TESTING_H

#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace valtz::test {

struct Case {
  const char*           suite;
  const char*           name;
  std::function<void()> fn;
};

std::vector<Case>& registry();
void record_failure(const char* file, int line, const std::string& what);
void record_skip(const std::string& why);

// A fresh directory under the test temp root, removed at exit.
std::filesystem::path temp_dir(const std::string& tag);

struct Registrar {
  Registrar(const char* s, const char* n, std::function<void()> fn)
  {
    registry().push_back({s, n, std::move(fn)});
  }
};

}

#define VALTZ_TEST_CAT_(a, b) a##b
#define VALTZ_TEST_CAT(a, b) VALTZ_TEST_CAT_(a, b)

#define TEST(suite, name)                                               \
  static void VALTZ_TEST_CAT(test_##suite##_, name)();                  \
  static ::valtz::test::Registrar VALTZ_TEST_CAT(reg_##suite##_, name)( \
      #suite, #name, &VALTZ_TEST_CAT(test_##suite##_, name));           \
  static void VALTZ_TEST_CAT(test_##suite##_, name)()

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      ::valtz::test::record_failure(__FILE__, __LINE__, #cond);         \
    }                                                                   \
  } while (0)

#define REQUIRE(cond)                                                   \
  do {                                                                  \
    if (!(cond)) {                                                      \
      ::valtz::test::record_failure(__FILE__, __LINE__, #cond);         \
      return;                                                           \
    }                                                                   \
  } while (0)

// REQUIRE on a Result/Status, printing its error message.
#define REQUIRE_OK(expr)                                                \
  do {                                                                  \
    auto&& _r = (expr);                                                 \
    if (!_r.ok()) {                                                     \
      ::valtz::test::record_failure(__FILE__, __LINE__,                 \
          std::string(#expr) + ": " + _r.error().message);              \
      return;                                                           \
    }                                                                   \
  } while (0)

#define SKIP(why)                                                       \
  do {                                                                  \
    ::valtz::test::record_skip(why);                                    \
    return;                                                             \
  } while (0)

#endif
