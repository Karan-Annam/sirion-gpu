// test_framework.hpp — dependency-free, self-registering C++ test harness.
//
// Same spirit as the mesi/orderbook projects' test_framework: no GoogleTest, no deps.
// Declare tests with TEST(name){...}; assert with CHECK / CHECK_EQ; call tf::run_all()
// from main(). Prints [PASS]/[FAIL] per test and a summary, returns the failure count.
#pragma once
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <exception>

namespace tf {

struct AssertFail : std::exception {
  std::string msg;
  explicit AssertFail(std::string m) : msg(std::move(m)) {}
  const char* what() const noexcept override { return msg.c_str(); }
};

struct TestCase {
  const char* name;
  std::function<void()> fn;
};

inline std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
inline long& check_count() { static long c = 0; return c; }

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) {
    registry().push_back({name, std::move(fn)});
  }
};

inline int run_all() {
  int failures = 0;
  for (auto& t : registry()) {
    try {
      t.fn();
      std::printf("[PASS] %s\n", t.name);
    } catch (const std::exception& e) {
      std::printf("[FAIL] %s: %s\n", t.name, e.what());
      ++failures;
    }
  }
  std::printf("\n%zu tests, %d failures, %ld checks\n",
              registry().size(), failures, check_count());
  return failures;
}

} // namespace tf

#define TEST(name)                                                        \
  static void tf_test_##name();                                           \
  static tf::Registrar tf_reg_##name(#name, tf_test_##name);              \
  static void tf_test_##name()

#define CHECK(cond)                                                       \
  do {                                                                    \
    ++tf::check_count();                                                  \
    if (!(cond))                                                          \
      throw tf::AssertFail(std::string(__FILE__) + ":" +                  \
        std::to_string(__LINE__) + " CHECK failed: " #cond);             \
  } while (0)

#define CHECK_EQ(a, b)                                                    \
  do {                                                                    \
    ++tf::check_count();                                                  \
    long long _va = (long long)(a);                                       \
    long long _vb = (long long)(b);                                       \
    if (_va != _vb)                                                       \
      throw tf::AssertFail(std::string(__FILE__) + ":" +                  \
        std::to_string(__LINE__) + " CHECK_EQ failed: " #a " == " #b      \
        " (" + std::to_string(_va) + " vs " + std::to_string(_vb) + ")"); \
  } while (0)
