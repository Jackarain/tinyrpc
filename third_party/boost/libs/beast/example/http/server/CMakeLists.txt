#
# Copyright (c) 2013-2017 Vinnie Falco (vinnie dot falco at gmail dot com)
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/boostorg/beast
#

add_subdirectory(async)
add_subdirectory(async-local)
add_subdirectory(awaitable)
add_subdirectory(coro)
add_subdirectory(fast)
add_subdirectory(small)
add_subdirectory(stackless)
add_subdirectory(sync)

if (OPENSSL_FOUND)
    add_subdirectory(async-ssl)
    add_subdirectory(coro-ssl)
    add_subdirectory(flex)
    add_subdirectory(stackless-ssl)
    add_subdirectory(sync-ssl)
endif ()
