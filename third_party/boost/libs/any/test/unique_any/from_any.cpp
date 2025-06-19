// Copyright Antony Polukhin, 2013-2025.
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#include <boost/any.hpp>
#include <boost/any/unique_any.hpp>

#include <boost/core/lightweight_test.hpp>

#include <vector>

void test_basic() {
    boost::any from = 42;

    boost::anys::unique_any a(std::move(from));
    BOOST_TEST(from.empty());
    BOOST_TEST(a.has_value());

    BOOST_TEST_EQ(boost::any_cast<int>(a), 42);
    BOOST_TEST_EQ(boost::anys::any_cast<int>(a), 42);
    BOOST_TEST_EQ(boost::any_cast<int&>(a), 42);
    BOOST_TEST_EQ(boost::anys::any_cast<int&>(a), 42);

    boost::anys::unique_any b = std::move(a);
    BOOST_TEST(!a.has_value());
    BOOST_TEST(b.has_value());
    BOOST_TEST_EQ(boost::any_cast<int&>(b), 42);

    b.reset();
    BOOST_TEST(!b.has_value());
}

void test_const() {
    boost::any from = 42;

    const boost::anys::unique_any a = std::move(from);
    BOOST_TEST(a.has_value());
    BOOST_TEST_EQ(boost::any_cast<int>(a), 42);
    BOOST_TEST_EQ(boost::anys::any_cast<int>(a), 42);
    BOOST_TEST_EQ(boost::any_cast<const int&>(a), 42);
    BOOST_TEST_EQ(boost::anys::any_cast<const int&>(a), 42);
}

void test_bad_any_cast() {
    boost::any from = 42;

    const boost::anys::unique_any a = std::move(from);
    try {
        boost::any_cast<char>(a);
        BOOST_TEST(false);
    } catch (const boost::bad_any_cast&) {
    }

    try {
        boost::any_cast<int*>(a);
        BOOST_TEST(false);
    } catch (const boost::bad_any_cast&) {
    }
}

struct counting_destroy {
    static int destructor_called;

    ~counting_destroy() {
      ++destructor_called;
    }
};

int counting_destroy::destructor_called = 0;

void test_destructor() {
    boost::any from = counting_destroy{};
    BOOST_TEST_EQ(counting_destroy::destructor_called, 1);
    boost::anys::unique_any a = std::move(from);
    BOOST_TEST_EQ(counting_destroy::destructor_called, 1);

    a.reset();
    BOOST_TEST_EQ(counting_destroy::destructor_called, 2);
}

int main() {
    test_basic();
    test_const();
    test_bad_any_cast();
    test_destructor();

    return boost::report_errors();
}

