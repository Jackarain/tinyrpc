# tinyrpc 单元测试
#
# 使用 Boost.Test 单元测试框架, 通过 beast::test::stream 在内存中
# 建立 websocket 连接进行端到端测试, 无需真实网络.

if (ENABLE_BUILD_TESTS)
	add_executable(jsonrpc_test
		jsonrpc_test.cpp
		lib_asio_beast.cpp
	)

	target_link_libraries(jsonrpc_test
		Boost::asio
		Boost::beast
		Boost::json
		Boost::system
		Boost::thread
		Boost::date_time
		Boost::unit_test_framework
	)

	set_property(TARGET jsonrpc_test PROPERTY FOLDER "test")

	if (MSVC AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 19.29.30129 AND CMAKE_VERSION VERSION_GREATER 3.20.3)
		set_property(TARGET jsonrpc_test PROPERTY CXX_STANDARD 23)
		message(STATUS "jsonrpc_test set cxx standard: C++23")
	else()
		set_property(TARGET jsonrpc_test PROPERTY CXX_STANDARD 20)
		message(STATUS "jsonrpc_test set cxx standard: C++20")
	endif()

	add_test(NAME jsonrpc_test COMMAND jsonrpc_test)
endif()
