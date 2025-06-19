//  Copyright Antony Polukhin, 2013-2025.
//
//  Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt).

#include <boost/any/unique_any.hpp>

int main() {
    boost::anys::unique_any const cvalue(10);
    int i = boost::any_cast<int&&>(cvalue);
    (void)i;
}

