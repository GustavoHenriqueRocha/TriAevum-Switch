// Minimal gtest-style checks for oot3d_grass_switch_tests (the game-runtime
// tests run without gtest): TEST, EXPECT_* and ASSERT_* with a main.
#pragma once
#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <vector>
namespace shim {
struct Case { const char* Name; std::function<void()> Run; };
inline std::vector<Case>& Cases() { static std::vector<Case> c; return c; }
inline int& Failures() { static int f = 0; return f; }
struct Register { Register(const char* n, std::function<void()> f) { Cases().push_back({n, f}); } };
struct Sink { bool Fail; std::ostringstream S; ~Sink() { if (Fail) { std::cerr << S.str() << "\n"; ++Failures(); } }
  template <class T> Sink& operator<<(const T& v) { if (Fail) S << v; return *this; } };
}
#define TEST(a, b) static void a##_##b(); static shim::Register a##_##b##_r(#a "." #b, a##_##b); static void a##_##b()
#define SHIM_CHECK(c, text) shim::Sink{!(c)} << __FILE__ << ":" << __LINE__ << ": " text " "
#define EXPECT_TRUE(c) SHIM_CHECK((c), #c)
#define EXPECT_FALSE(c) SHIM_CHECK(!(c), "!" #c)
#define EXPECT_EQ(a, b) SHIM_CHECK((a) == (b), #a " == " #b)
#define EXPECT_NE(a, b) SHIM_CHECK((a) != (b), #a " != " #b)
#define EXPECT_GE(a, b) SHIM_CHECK((a) >= (b), #a " >= " #b) << "(" << (a) << " vs " << (b) << ") "
#define EXPECT_LE(a, b) SHIM_CHECK((a) <= (b), #a " <= " #b) << "(" << (a) << " vs " << (b) << ") "
#define EXPECT_GT(a, b) SHIM_CHECK((a) > (b), #a " > " #b) << "(" << (a) << " vs " << (b) << ") "
#define EXPECT_LT(a, b) SHIM_CHECK((a) < (b), #a " < " #b) << "(" << (a) << " vs " << (b) << ") "
#define EXPECT_NEAR(a, b, e) SHIM_CHECK(std::abs((a) - (b)) <= (e), #a " ~ " #b) << "(" << (a) << " vs " << (b) << ") "
#define EXPECT_FLOAT_EQ(a, b) EXPECT_NEAR((a), (b), 1.0e-5F)
#define SHIM_ASSERT(c, text) if (!(c)) { shim::Sink{true} << __FILE__ << ":" << __LINE__ << ": " text; return; }
#define ASSERT_TRUE(c) SHIM_ASSERT((c), #c)
#define ASSERT_EQ(a, b) SHIM_ASSERT((a) == (b), #a " == " #b)
#define ASSERT_FALSE(c) SHIM_ASSERT(!(c), "!" #c)
#define ASSERT_GT(a, b) SHIM_ASSERT((a) > (b), #a " > " #b)
int main() { for (auto& c : shim::Cases()) { int before = shim::Failures(); c.Run(); std::cout << (shim::Failures() == before ? "ok   " : "FAIL ") << c.Name << "\n"; } std::cout << shim::Failures() << " failures\n"; return shim::Failures() != 0; }
