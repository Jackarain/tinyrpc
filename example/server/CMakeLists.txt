
add_executable(server
    server.cpp
    lib_asio_beast.cpp
)

target_link_libraries(server
	Boost::thread
	Boost::date_time
	Boost::program_options
	Boost::filesystem
	Boost::signals2
	Boost::nowide
	Boost::url
	Boost::json
	Boost::uuid
	Boost::asio
	Boost::beast
	Boost::system
	Boost::scope
)

set_property(TARGET server PROPERTY FOLDER "example/server")

if (MSVC AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 19.29.30129 AND CMAKE_VERSION VERSION_GREATER 3.20.3)
	set_property(TARGET server PROPERTY CXX_STANDARD 23)
	message(STATUS "server set cxx standard: C++23")
else()
	set_property(TARGET server PROPERTY CXX_STANDARD 20)
	message(STATUS "server set cxx standard: C++20")
endif()

if(ENABLE_BUILD_WERROR)
	if(MSVC)
		target_compile_options(server PRIVATE /W4 /WX)
	elseif(UNIX AND NOT APPLE)
		if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.1)
			target_compile_options(server PRIVATE -Werror -Wall -Wextra -Wno-unknown-pragmas -Wno-deprecated)
		else()
			target_compile_options(server PRIVATE -Werror -Wall -Wextra -Wno-unknown-pragmas -Wno-deprecated -Wno-deprecated-copy -Wno-error=deprecated-copy -Wno-pessimizing-move)
		endif()
	endif()
endif()
