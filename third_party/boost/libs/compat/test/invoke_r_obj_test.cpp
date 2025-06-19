// Copyright 2024 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/compat/invoke.hpp>
#include <boost/core/lightweight_test.hpp>

struct F
{
    int operator()()
    {
        return -1;
    }

    int operator()( int x1 ) noexcept
    {
        return x1;
    }

    int operator()( int x1, int x2 ) const
    {
        return 10*x1+x2;
    }

    int operator()( int x1, int x2, int x3 ) const noexcept
    {
        return 100*x1 + 10*x2 + x3;
    }
};

int main()
{
    {
        BOOST_TEST_EQ( boost::compat::invoke_r<long>( F() ), -1 );
        BOOST_TEST_EQ( boost::compat::invoke_r<long>( F(), 1 ), 1 );
        BOOST_TEST_EQ( boost::compat::invoke_r<long>( F(), 1, 2 ), 12 );
        BOOST_TEST_EQ( boost::compat::invoke_r<long>( F(), 1, 2, 3 ), 123 );
    }

    {
        F f;

        BOOST_TEST_EQ( boost::compat::invoke_r<long>( f ), -1 );
        BOOST_TEST_EQ( boost::compat::invoke_r<long>( f, 1 ), 1 );
        BOOST_TEST_EQ( boost::compat::invoke_r<long>( f, 1, 2 ), 12 );
        BOOST_TEST_EQ( boost::compat::invoke_r<long>( f, 1, 2, 3 ), 123 );
    }

    {
        F const f = {};

        BOOST_TEST_EQ( boost::compat::invoke_r<long>( f, 1, 2 ), 12 );
        BOOST_TEST_EQ( boost::compat::invoke_r<long>( f, 1, 2, 3 ), 123 );
    }

    {
        boost::compat::invoke_r<void>( F() );
        boost::compat::invoke_r<void>( F(), 1 );
        boost::compat::invoke_r<void>( F(), 1, 2 );
        boost::compat::invoke_r<void>( F(), 1, 2, 3 );
    }

    {
        F f;

        boost::compat::invoke_r<void>( f );
        boost::compat::invoke_r<void>( f, 1 );
        boost::compat::invoke_r<void>( f, 1, 2 );
        boost::compat::invoke_r<void>( f, 1, 2, 3 );
    }

    {
        F const f = {};

        boost::compat::invoke_r<void>( f, 1, 2 );
        boost::compat::invoke_r<void>( f, 1, 2, 3 );
    }

    return boost::report_errors();
}
