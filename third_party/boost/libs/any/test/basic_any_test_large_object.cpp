// Copyright Ruslan Arutyunyan, 2019-2021.
// Copyright Antony Polukhin, 2021-2025.
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#include <boost/any/basic_any.hpp>

#include <boost/core/lightweight_test.hpp>

#include <cassert>

static int move_ctors_count = 0;
static int destructors_count = 0;

struct A {
    char a[32];

    A() {}
    A(const A&) {}

    A(A&&) noexcept {
        ++move_ctors_count;
    }

    ~A() {
        ++destructors_count;
    }
};

int main() {
    {
        A a;
        boost::anys::basic_any<24, 8> any1(a);
        boost::anys::basic_any<24, 8> any2(std::move(any1));
        boost::anys::basic_any<24, 8> any3(std::move(any2));
        BOOST_TEST_EQ(move_ctors_count, 0);
    }

    BOOST_TEST_EQ(destructors_count, 2);

    return boost::report_errors();
}
