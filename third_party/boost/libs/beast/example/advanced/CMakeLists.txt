#
# Copyright (c) 2013-2017 Vinnie Falco (vinnie dot falco at gmail dot com)
# Copyright (c) 2024 Mohammad Nejati
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/boostorg/beast
#

add_subdirectory(server)

if (OPENSSL_FOUND)
    add_subdirectory(server-flex)
    add_subdirectory(server-flex-awaitable)
endif ()
