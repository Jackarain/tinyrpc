/* tests using constexpr on boost:array
 * (C) Copyright Marshall Clow 2012
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 */ 

#include <boost/array.hpp>
#include <boost/config.hpp>

#ifndef BOOST_NO_CXX11_CONSTEXPR

constexpr boost::array<int, 10> arr  {{ 0,1,2,3,4,5,6,7,8,9 }};

int main()
{
    constexpr int three = arr.at (3);
    int whatever [ arr.at(4) ];
    (void)three;
    (void) whatever;
}

#else   // no constexpr means no constexpr tests!

int main()
{
}

#endif
