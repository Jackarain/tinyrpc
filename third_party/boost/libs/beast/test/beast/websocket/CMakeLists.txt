#
# Copyright (c) 2016-2017 Vinnie Falco (vinnie dot falco at gmail dot com)
# Copyright (c) 2024 Mohammad Nejati
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/boostorg/beast
#

file(GLOB_RECURSE PFILES CONFIGURE_DEPENDS Jamfile *.cpp *.hpp)

add_executable(boost_beast_tests_websocket ${PFILES})

source_group("" FILES ${PFILES})

target_link_libraries(boost_beast_tests_websocket
    boost_beast_lib_asio_ssl
    boost_beast_lib_test)

set_target_properties(boost_beast_tests_websocket
    PROPERTIES FOLDER "tests")

add_test(NAME boost_beast_tests_websocket COMMAND boost_beast_tests_websocket)
add_dependencies(tests boost_beast_tests_websocket)
