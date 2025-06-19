//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2021-2025 Alexander Grund
//  Copyright (c) 2002, 2009, 2014 Peter Dimov
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_UNIT_TEST_HPP
#define BOOST_LOCALE_UNIT_TEST_HPP

#include <boost/locale/config.hpp>
#include <boost/config/helper_macros.hpp>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <vector>

#if defined(_MSC_VER) && defined(_CPPLIB_VER) && defined(_DEBUG)
#    include <crtdbg.h>
#endif

#ifndef BOOST_LOCALE_ERROR_LIMIT
#    define BOOST_LOCALE_ERROR_LIMIT 20
#endif

#define BOOST_LOCALE_STRINGIZE(x) #x

namespace boost { namespace locale { namespace test {
    /// Name/path of current executable
    std::string exe_name;

    class test_context;

    struct test_result {
        test_result()
        {
#if defined(_MSC_VER) && (_MSC_VER > 1310)
            // disable message boxes on assert(), abort()
            ::_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
#if defined(_MSC_VER) && defined(_CPPLIB_VER) && defined(_DEBUG)
            // disable message boxes on iterator debugging violations
            _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
            _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
        }
        int error_counter = 0;
        int test_counter = 0;
        const test_context* context = nullptr;
    };
    inline test_result& results()
    {
        static test_result instance;
        return instance;
    }

    class test_context {
        const test_context* oldCtx_;
        const std::string msg_;

    public:
        test_context(std::string ctx) : oldCtx_(results().context), msg_(std::move(ctx)) { results().context = this; }
        ~test_context() { results().context = oldCtx_; }
        friend std::ostream& operator<<(std::ostream& os, const test_context& c)
        {
            const test_context* current = &c;
            os << "CONTEXT: ";
            std::string indent = "\n\t";
            do {
                os << indent << current->msg_;
                indent += '\t';
            } while((current = current->oldCtx_) != nullptr);
            return os;
        }
    };

    inline void report_error(const char* expr, const char* file, int line)
    {
        std::cerr << "Error at " << file << '#' << line << ": " << expr << std::endl;
        const auto* context = results().context;
        if(context)
            std::cerr << ' ' << *context << std::endl;
        if(++boost::locale::test::results().error_counter > BOOST_LOCALE_ERROR_LIMIT)
            throw std::runtime_error("Error limits reached, stopping unit test");
    }

    template<bool require = false>
    bool test_impl(const char* expr, const char* file, int line, const bool v)
    {
        boost::locale::test::results().test_counter++;
        if(!v) {
            boost::locale::test::report_error(expr, file, line);
            if(require)
                throw std::runtime_error("Critical test " + std::string(expr) + " failed");
        }
        return v;
    }
}}} // namespace boost::locale::test

#define BOOST_LOCALE_TEST_REPORT_ERROR(expr) boost::locale::test::report_error(expr, __FILE__, __LINE__)

#define TEST(X) (::boost::locale::test::test_impl(#X, __FILE__, __LINE__, (X) ? true : false))

#define TEST_REQUIRE(X) (::boost::locale::test::test_impl<true>(#X, __FILE__, __LINE__, (X) ? true : false))

#define TEST_THROWS(X, E)                              \
    do {                                               \
        boost::locale::test::results().test_counter++; \
        try {                                          \
            X;                                         \
        } catch(E const& /*e*/) {                      \
            break;                                     \
        } catch(...) {                                 \
        }                                              \
        BOOST_LOCALE_TEST_REPORT_ERROR(#X);            \
        BOOST_LOCALE_START_CONST_CONDITION             \
    } while(0) BOOST_LOCALE_END_CONST_CONDITION

#define TEST_CONTEXT(expr)                                                    \
    boost::locale::test::test_context BOOST_JOIN(test_context_, __COUNTER__)( \
      static_cast<const std::stringstream&>(std::stringstream{} << expr).str())

void test_main(int argc, char** argv);

int main(int argc, char** argv)
{
    {
        using namespace boost::locale::test;
        exe_name = argv[0];
        if(exe_name.substr(exe_name.length() - 4) == ".exe")
            exe_name.resize(exe_name.length() - 4);
        results(); // Instantiate
    }
    try {
        test_main(argc, argv);
    } catch(const std::exception& e) {
        std::cerr << "Failed with exception "                                 // LCOV_EXCL_LINE
                  << typeid(e).name() << '(' << e.what() << ')' << std::endl; // LCOV_EXCL_LINE
        return EXIT_FAILURE;                                                  // LCOV_EXCL_LINE
    }
    using boost::locale::test::results;
    if(results().test_counter > 0) {
        int passed = results().test_counter - results().error_counter;
        std::cout << std::endl;
        std::cout << "Passed " << passed << " tests\n";
        if(results().error_counter > 0)
            std::cout << "Failed " << results().error_counter << " tests\n"; // LCOV_EXCL_LINE
        std::cout << " " << std::fixed << std::setprecision(1) << std::setw(5)
                  << 100.0 * passed / results().test_counter << "% of tests completed successfully\n";
    }
    return results().error_counter == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

template<typename T>
std::string to_string(T const& s)
{
    std::stringstream ss;
    ss << s;
    return ss.str();
}

const std::string& to_string(const std::string& s)
{
    return s;
}

std::string to_string(std::nullptr_t)
{
    return "<nullptr>";
}

template<typename T>
std::string to_string(const std::vector<T>& v)
{
    std::stringstream ss;
    bool first = true;
    for(const T& e : v) {
        if(!first)
            ss << ", ";
        first = false;
        ss << to_string(e);
    }
    return ss.str();
}

/// Put the char into the stream making sure it is readable
/// Fallback to the Unicode representation of it (e.g. U+00A0)
template<typename Char>
void stream_char(std::ostream& s, const Char c)
{
    if((c >= '!' && c <= '~') || c == ' ')
        s << static_cast<char>(c);
    else
        s << "U+" << std::hex << std::uppercase << std::setw(sizeof(Char)) << std::setfill('0')
          << static_cast<unsigned>(c);
}

template<typename Char, class Alloc>
std::string to_string(const std::basic_string<Char, std::char_traits<Char>, Alloc>& s)
{
    std::stringstream ss;
    for(const Char c : s)
        stream_char(ss, c);
    return ss.str();
}

template<size_t size>
std::string to_string(const wchar_t (&s)[size])
{
    std::stringstream ss;
    for(size_t i = 0; i < size; ++i)
        stream_char(ss, s[i]);
    return ss.str();
}

std::string to_string(const wchar_t* s)
{
    std::stringstream ss;
    for(; *s; ++s)
        stream_char(ss, *s);
    return ss.str();
}

// Unicode chars cannot be streamed directly (deleted overloads in C++20)
template<typename Char>
std::string to_string_char_impl(const Char c)
{
    std::stringstream ss;
    stream_char(ss, c);
    return ss.str();
}

std::string to_string(const wchar_t c)
{
    return to_string_char_impl(c);
}
std::string to_string(const char16_t c)
{
    return to_string_char_impl(c);
}
std::string to_string(const char32_t c)
{
    return to_string_char_impl(c);
}

#if BOOST_WORKAROUND(BOOST_CLANG_VERSION, < 120000) && BOOST_WORKAROUND(__cplusplus, >= 202002L)
// Avoid warning due to comparison-to-spaceship-rewrite, happening e.g. for string comparison
// see https://github.com/llvm/llvm-project/issues/43670
#    define BOOST_LOCALE_SPACESHIP_NULLPTR_WARNING 1
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wzero-as-null-pointer-constant"
#else
#    define BOOST_LOCALE_SPACESHIP_NULLPTR_WARNING 0
#endif

template<typename T, typename U>
void test_impl(bool success,
               T const& l,
               U const& r,
               const char* expr,
               const char* fail_expr,
               const char* file,
               int line)
{
    boost::locale::test::results().test_counter++;
    if(!success) {
        if(fail_expr) {
            std::ostringstream s;
            s << expr << '\n' << "---- [" << to_string(l) << "] " << fail_expr << " [" << to_string(r) << "]";
            boost::locale::test::report_error(s.str().c_str(), file, line);
        } else
            boost::locale::test::report_error(expr, file, line);
    }
}

void test_impl(bool success, const char* reason, const char* file, int line)
{
    test_impl(success, nullptr, nullptr, reason, nullptr, file, line);
}

#define BOOST_LOCALE_TEST_OP_IMPL(name, test_op, fail_op)                                         \
    template<typename T, typename U>                                                              \
    void test_##name##_impl(T const& l, U const& r, const char* expr, const char* file, int line) \
    {                                                                                             \
        test_impl(l test_op r, l, r, expr, #fail_op, file, line);                                 \
    }

BOOST_LOCALE_TEST_OP_IMPL(eq, ==, !=)
BOOST_LOCALE_TEST_OP_IMPL(ne, !=, ==)
BOOST_LOCALE_TEST_OP_IMPL(le, <=, >)
BOOST_LOCALE_TEST_OP_IMPL(lt, <, >=)
BOOST_LOCALE_TEST_OP_IMPL(ge, >=, <)
BOOST_LOCALE_TEST_OP_IMPL(gt, >, <=)

#undef BOOST_LOCALE_TEST_OP_IMPL

#define TEST_EQ(x, y) test_eq_impl(x, y, BOOST_LOCALE_STRINGIZE(x == y), __FILE__, __LINE__)
#define TEST_NE(x, y) test_ne_impl(x, y, BOOST_LOCALE_STRINGIZE(x != y), __FILE__, __LINE__)
#define TEST_LE(x, y) test_le_impl(x, y, BOOST_LOCALE_STRINGIZE(x <= y), __FILE__, __LINE__)
#define TEST_LT(x, y) test_lt_impl(x, y, BOOST_LOCALE_STRINGIZE(x < y), __FILE__, __LINE__)
#define TEST_GE(x, y) test_ge_impl(x, y, BOOST_LOCALE_STRINGIZE(x >= y), __FILE__, __LINE__)
#define TEST_GT(x, y) test_gt_impl(x, y, BOOST_LOCALE_STRINGIZE(x > y), __FILE__, __LINE__)

#if BOOST_LOCALE_SPACESHIP_NULLPTR_WARNING
#    pragma clang diagnostic pop
#endif

#ifdef BOOST_MSVC
#    define BOOST_LOCALE_DISABLE_UNREACHABLE_CODE_WARNING __pragma(warning(disable : 4702))
#else
#    define BOOST_LOCALE_DISABLE_UNREACHABLE_CODE_WARNING
#endif

#endif
