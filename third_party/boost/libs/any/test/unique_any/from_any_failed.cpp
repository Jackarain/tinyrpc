//  Copyright Antony Polukhin, 2013-2025.
//
//  Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt).

#include <boost/any.hpp>
#include <boost/any/unique_any.hpp>

int main() {
    boost::any a;
    boost::anys::unique_any b(a);
    (void)b;
}
