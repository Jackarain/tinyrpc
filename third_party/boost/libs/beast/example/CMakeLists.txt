#
# Copyright (c) 2013-2017 Vinnie Falco (vinnie dot falco at gmail dot com)
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/boostorg/beast
#

find_package(OpenSSL)

add_subdirectory(advanced)
add_subdirectory(http)
add_subdirectory(websocket)

add_subdirectory(doc)

add_subdirectory(echo-op)
