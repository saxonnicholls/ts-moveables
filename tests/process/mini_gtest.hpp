//
//  mini_gtest.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - the slice of GoogleTest that the process/ tests
//  use, so tests ported from a GoogleTest codebase keep their shape
//  without this library taking a test framework dependency. TEST, TEST_F,
//  EXPECT_ and ASSERT_ comparisons, EXPECT_THROW, EXPECT_STREQ, and death
//  tests (EXPECT_EXIT, EXPECT_DEATH) run in a forked child whose stderr is
//  matched against a regular expression. Any failure prints file:line and
//  the values, and the program exits 1 after the test.
//

#ifndef ts_moveables_tests_mini_gtest_hpp
#define ts_moveables_tests_mini_gtest_hpp

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <exception>
#include <regex>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace mini_gtest {

struct test_case
{
    const char* name;
    void (*run)();
};

inline std::vector<test_case>& registry()
{
    static std::vector<test_case> r;
    return r;
}

inline int& failures()
{
    static int f = 0;
    return f;
}

struct assertion_failed
{
};

template <class T, class = void>
struct printable : std::false_type {};
template <class T>
struct printable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>> : std::true_type {};

template <class T>
std::string show(const T& v)
{
    if constexpr (printable<T>::value) {
        std::ostringstream s;
        s << v;
        return s.str();
    } else {
        return "<unprintable>";
    }
}

inline void fail(const char* file, int line, const std::string& what, bool fatal)
{
    ++failures();
    std::cerr << file << ":" << line << ": FAILED: " << what << "\n";
    if (fatal) throw assertion_failed{};
}

template <class A, class B>
void compare(bool ok, const char* file, int line, const char* op, const char* ea, const char* eb, const A& a,
             const B& b, bool fatal)
{
    if (!ok) fail(file, line, std::string(ea) + " " + op + " " + eb + "  (" + show(a) + " vs " + show(b) + ")", fatal);
}

struct check
{
    bool ok = true;
    std::string text;
    explicit operator bool() const noexcept { return ok; }
};

template <class A, class B, class Op>
check compare_values(const A& a, const B& b, Op op, const char* ea, const char* eop, const char* eb)
{
    if (op(a, b)) return {true, {}};
    return {false, std::string(ea) + " " + eop + " " + eb + "  (" + show(a) + " vs " + show(b) + ")"};
}

// Collects the streamed context and reports at the end of the full expression
class failure
{
public:
    failure(const char* file, int line, std::string what, bool fatal)
        : file_(file), line_(line), what_(std::move(what)), fatal_(fatal) {}
    failure(const failure&) = delete;
    template <class T>
    failure& operator<<(const T& v)
    {
        extra_ << v;
        return *this;
    }
    ~failure() noexcept(false)
    {
        const std::string extra = extra_.str();
        fail(file_, line_, what_ + (extra.empty() ? "" : "  -- " + extra), false);
        if (fatal_ && std::uncaught_exceptions() == 0) throw assertion_failed{};
    }

private:
    const char* file_;
    int line_;
    std::string what_;
    bool fatal_;
    std::ostringstream extra_;
};

struct registrar
{
    registrar(const char* name, void (*run)()) { registry().push_back({name, run}); }
};

// Death tests: run `body` in a child with stderr captured
struct child_outcome
{
    int status = 0;
    std::string err;
};

inline child_outcome run_in_child(const std::function<void()>& body)
{
    int p[2];
    if (::pipe(p) != 0) std::abort();
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(p[0]);
        ::dup2(p[1], 2);
        body();
        std::fflush(nullptr);
        ::_exit(0);
    }
    ::close(p[1]);
    child_outcome out;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(p[0], buf, sizeof buf);
        if (n > 0) out.err.append(buf, static_cast<std::size_t>(n));
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
    ::close(p[0]);
    ::waitpid(pid, &out.status, 0);
    return out;
}

} // namespace mini_gtest

namespace testing {

struct Test
{
    virtual ~Test() = default;
    virtual void SetUp() {}
    virtual void TearDown() {}
};

struct ExitedWithCode
{
    int code;
    explicit ExitedWithCode(int c) : code(c) {}
    bool operator()(int status) const { return WIFEXITED(status) && WEXITSTATUS(status) == code; }
};

struct KilledBySignal
{
    int sig;
    explicit KilledBySignal(int s) : sig(s) {}
    bool operator()(int status) const { return WIFSIGNALED(status) && WTERMSIG(status) == sig; }
};

} // namespace testing

#define MG_CAT2(a, b) a##b
#define MG_CAT(a, b) MG_CAT2(a, b)

#define TEST(suite, name)                                                                          \
    static void MG_CAT(suite, MG_CAT(_, name))();                                                  \
    static const ::mini_gtest::registrar MG_CAT(reg_, MG_CAT(suite, MG_CAT(_, name)))(             \
        #suite "." #name, &MG_CAT(suite, MG_CAT(_, name)));                                        \
    static void MG_CAT(suite, MG_CAT(_, name))()

#define TEST_F(fixture, name)                                                                      \
    struct MG_CAT(fixture, MG_CAT(_, name)) : fixture { void body(); };                            \
    static void MG_CAT(run_, MG_CAT(fixture, MG_CAT(_, name)))()                                   \
    {                                                                                              \
        MG_CAT(fixture, MG_CAT(_, name)) t;                                                        \
        t.SetUp();                                                                                 \
        t.body();                                                                                  \
        t.TearDown();                                                                              \
    }                                                                                              \
    static const ::mini_gtest::registrar MG_CAT(reg_, MG_CAT(fixture, MG_CAT(_, name)))(          \
        #fixture "." #name, &MG_CAT(run_, MG_CAT(fixture, MG_CAT(_, name))));                      \
    void MG_CAT(fixture, MG_CAT(_, name))::body()

// A failed check: streams like GoogleTest's (EXPECT_EQ(a, b) << "context"), reports when the statement ends
#define MG_FAIL(what, fatal) ::mini_gtest::failure(__FILE__, __LINE__, what, fatal)

#define MG_CMP(op, a, b, fatal)                                                                    \
    if (const ::mini_gtest::check mg_c = ::mini_gtest::compare_values(                            \
            (a), (b), [](const auto& mg_x, const auto& mg_y) { return static_cast<bool>(mg_x op mg_y); }, \
            #a, #op, #b))                                                                          \
        ;                                                                                          \
    else                                                                                           \
        MG_FAIL(mg_c.text, fatal)

#define EXPECT_EQ(a, b) MG_CMP(==, a, b, false)
#define EXPECT_NE(a, b) MG_CMP(!=, a, b, false)
#define EXPECT_LT(a, b) MG_CMP(<, a, b, false)
#define EXPECT_LE(a, b) MG_CMP(<=, a, b, false)
#define EXPECT_GT(a, b) MG_CMP(>, a, b, false)
#define EXPECT_GE(a, b) MG_CMP(>=, a, b, false)
#define ASSERT_EQ(a, b) MG_CMP(==, a, b, true)
#define ASSERT_NE(a, b) MG_CMP(!=, a, b, true)
#define ASSERT_GE(a, b) MG_CMP(>=, a, b, true)
#define ASSERT_GT(a, b) MG_CMP(>, a, b, true)

#define EXPECT_TRUE(c) if (c) ; else MG_FAIL("expected true: " #c, false)
#define EXPECT_FALSE(c) if (!(c)) ; else MG_FAIL("expected false: " #c, false)
#define ASSERT_TRUE(c) if (c) ; else MG_FAIL("expected true: " #c, true)
#define ASSERT_FALSE(c) if (!(c)) ; else MG_FAIL("expected false: " #c, true)
#define ADD_FAILURE() MG_FAIL("failure", false)
#define FAIL() MG_FAIL("FAIL", true)

#define EXPECT_STREQ(a, b)                                                                         \
    do {                                                                                           \
        if (std::strcmp((a), (b)) != 0)                                                            \
            ::mini_gtest::fail(__FILE__, __LINE__, std::string(#a " == " #b "  (\"") + (a) + "\" vs \"" + (b) + "\")", false); \
    } while (0)

#define EXPECT_THROW(stmt, type)                                                                   \
    do {                                                                                           \
        bool mg_threw = false;                                                                     \
        try { stmt; } catch (const type&) { mg_threw = true; } catch (...) {}                      \
        if (!mg_threw) ::mini_gtest::fail(__FILE__, __LINE__, "expected " #stmt " to throw " #type, false); \
    } while (0)

#define EXPECT_NO_THROW(stmt)                                                                      \
    do {                                                                                           \
        try { stmt; } catch (...) { ::mini_gtest::fail(__FILE__, __LINE__, "unexpected throw from " #stmt, false); } \
    } while (0)

#define GTEST_FLAG_SET(flag, value) ((void)0)

#define EXPECT_EXIT(stmt, predicate, pattern)                                                      \
    do {                                                                                           \
        const auto mg_out = ::mini_gtest::run_in_child([&] { stmt; });                            \
        if (!(predicate)(mg_out.status) || !std::regex_search(mg_out.err, std::regex(pattern)))   \
            ::mini_gtest::fail(__FILE__, __LINE__, "death test " #stmt ": status " +               \
                               std::to_string(mg_out.status) + ", stderr: " + mg_out.err, false);  \
    } while (0)

#define EXPECT_DEATH(stmt, pattern)                                                                \
    do {                                                                                           \
        const auto mg_out = ::mini_gtest::run_in_child([&] { stmt; });                            \
        const bool mg_died = !(WIFEXITED(mg_out.status) && WEXITSTATUS(mg_out.status) == 0);      \
        if (!mg_died || !std::regex_search(mg_out.err, std::regex(pattern)))                      \
            ::mini_gtest::fail(__FILE__, __LINE__, "death test " #stmt ": stderr: " + mg_out.err, false); \
    } while (0)

// Run every registered test; exit status 1 if any failed
inline int mini_gtest_main()
{
    int ran = 0;
    for (const auto& t : ::mini_gtest::registry()) {
        const int before = ::mini_gtest::failures();
        try {
            t.run();
        } catch (const ::mini_gtest::assertion_failed&) {
        } catch (const std::exception& e) {
            ::mini_gtest::fail(t.name, 0, std::string("uncaught exception: ") + e.what(), false);
        }
        ++ran;
        std::cout << (::mini_gtest::failures() == before ? "PASS  " : "FAIL  ") << t.name << "\n";
    }
    std::cout << "\nprocess: " << ran << " tests, " << ::mini_gtest::failures() << " failed checks\n";
    return ::mini_gtest::failures() == 0 ? 0 : 1;
}

#endif // ts_moveables_tests_mini_gtest_hpp
