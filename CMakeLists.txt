cmake_minimum_required(VERSION 3.5)
cmake_policy(SET CMP0091 NEW) # for CMAKE_MSVC_RUNTIME_LIBRARY
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW) # cmake_policy(SET CMP0077 NEW)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
project(tinyrpc LANGUAGES CXX)

################################################################################

include(CheckCXXCompilerFlag)
include(CheckLibraryExists)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

################################################################################

# Disable in-source builds to prevent source tree corruption.
if("${CMAKE_CURRENT_SOURCE_DIR}" STREQUAL "${CMAKE_BINARY_DIR}")
  message(FATAL_ERROR "
FATAL: In-source builds are not allowed.
       You should create a separate directory for build files.
")
endif()
message(STATUS "CMAKE Version: ${CMAKE_VERSION}")

message(STATUS "Source Dir: ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "Host System name: ${CMAKE_HOST_SYSTEM_NAME}")

set(default_build_type "Release")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "${default_build_type}" CACHE
        STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

message(STATUS "System name: ${CMAKE_SYSTEM_NAME}")
message(STATUS "System version: ${CMAKE_SYSTEM_VERSION}")
if (WIN32)
	message(STATUS "Platform version: ${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "SYSTEM NAME: ${CMAKE_SYSTEM_NAME}")


################################################################################

option(ENABLE_BUILD_WERROR			"All warnings being treated as errors" OFF)

option(ENABLE_USE_IO_URING			"Enable auto use io_uring" OFF)
option(ENABLE_BUILD_WITH_LIBCXX		"Build with libc++" OFF)

option(ENABLE_MOLD					"Build using mold" OFF)
option(ENABLE_LLD					"Build using lld" OFF)

option(ENABLE_STATIC_LINK_TO_GCC	"Build static link to gcc" ON)

option(ENABLE_TCMALLOC_STATIC		"Build with Tcmalloc support" OFF)
option(ENABLE_JEMALLOC_STATIC		"Build with Jemalloc support" OFF)
option(ENABLE_MIMALLOC_STATIC		"Build with mimalloc support" OFF)
option(ENABLE_SNMALLOC_STATIC		"Build with snmalloc support" OFF)

option(ENABLE_LINKE_TO_LIBATOMIC	"Link to libatomic.so - some platform requires it" OFF)

################################################################################

find_program(MOLD_LINKER mold)
find_program(LLD_LINKER lld)

################################################################################

if (ENABLE_MIMALLOC_STATIC)
	if (UNIX AND NOT APPLE)
		# 必须先编译安装 mimalloc 库(这里限制版本为2.1.2) 编译安装办法：
		# git clone https://github.com/microsoft/mimalloc.git
		# mkdir build && cd build && git checkout tags/v2.1.2
		# cmake -DCMAKE_BUILD_TYPE=Release -DMI_BUILD_STATIC=ON -DMI_BUILD_SHARED=OFF -DMI_BUILD_TESTS=OFF -DMI_BUILD_OBJECT=ON ..
		# make -j4 && make install
		set(MIMALLOC_OBJECT "/usr/local/lib/mimalloc-2.1/mimalloc.o")
		set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${MIMALLOC_OBJECT} -lpthread")
	else()
		message(FATAL_ERROR "mimalloc only support linux!!!")
	endif()
endif()

################################################################################

if (ENABLE_SNMALLOC_STATIC)
	set(SNMALLOC_HEADER_ONLY_LIBRARY ON)

	add_definitions(-DUSE_SNMALLOC)
	add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/snmalloc EXCLUDE_FROM_ALL)

	link_libraries(snmalloc)
	include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third_party/snmalloc)
endif()

################################################################################

if (WIN32)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

	add_definitions(
		-DWIN32_LEAN_AND_MEAN
		-D_WIN32_WINNT=0x0601
		#-DNOMINMAX # Comment out NOMINMAX for wolfSSL dependency on min/max macro in MSVC.
		-DUNICODE
		-D_UNICODE
	)

	add_definitions(
		-DBOOST_ALL_STATIC_LINK
		-DBOOST_THREAD_USE_LIB
		-DBOOST_FILESYSTEM_STATIC_LINK
		-DBOOST_USE_WINAPI_VERSION=0x0601
	)

endif()

################################################################################

if (MSVC)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

	add_definitions(
		-D_CRT_SECURE_NO_DEPRECATE
		-D_CRT_SECURE_NO_WARNINGS
		-D_CRT_NONSTDC_NO_DEPRECATE
		-D_CRT_NONSTDC_NO_WARNINGS
		-D_SCL_SECURE_NO_DEPRECATE
		-D_SCL_SECURE_NO_WARNINGS
	)

	add_definitions(
		-D_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS
		-D_SILENCE_ALL_CXX23_DEPRECATION_WARNINGS
		-D_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING
		-D_SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS
	)

	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /bigobj /MP /Zc:__cplusplus")
	message(STATUS "Using parallel compiling (/MP)")
	set(CMAKE_CXX_STACK_SIZE "100000000") #set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /STACK:100000000")
	message(STATUS "Set stack size: 100000000 bytes")

	CHECK_CXX_COMPILER_FLAG(/std:c++20 COMPILER_HAS_STD_CXX20)

	if (COMPILER_HAS_STD_CXX20)
		set(CMAKE_CXX_STANDARD 20)
		set(CMAKE_CXX_STANDARD_REQUIRED ON)
		message(STATUS "Set default cxx standard: C++20")
	elseif (MSVC_VERSION GREATER_EQUAL "1900")
		set(CMAKE_CXX_STANDARD 17)
		set(CMAKE_CXX_STANDARD_REQUIRED ON)
		message(STATUS "Set default cxx standard: C++17")
	endif()
endif()

################################################################################

CHECK_CXX_COMPILER_FLAG(-fvisibility-inlines-hidden COMPILER_HAS_VISIBILITY_INLINE_HIDDEN)
CHECK_CXX_COMPILER_FLAG(-fvisibility=hidden COMPILER_HAS_VISIBILITY_HIDDEN)
CHECK_CXX_COMPILER_FLAG(-fdiagnostics-color=always COMPILER_HAS_COLOR)
CHECK_CXX_COMPILER_FLAG(-fcoroutines COMPILER_HAS_FCOROUTINES)
CHECK_CXX_COMPILER_FLAG(-fcoroutines-ts COMPILER_HAS_FCOROUTINES_TS)
CHECK_CXX_COMPILER_FLAG(-std=c++20 COMPILER_HAS_STD_CXX20)

if (${COMPILER_HAS_VISIBILITY_INLINE_HIDDEN})
	SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility-inlines-hidden")
endif()

if (${COMPILER_HAS_VISIBILITY_HIDDEN})
	SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility=hidden")
endif()

if (${COMPILER_HAS_COLOR})
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fdiagnostics-color=always")
endif()

################################################################################

if (NOT MSVC)
	if(NOT APPLE)
		if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
			if (ENABLE_BUILD_WITH_LIBCXX)
				set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
				CHECK_LINKER_FLAG(CXX "-rtlib=compiler-rt" IS_COMPILER_RT_SUPPORTED)

				if (IS_COMPILER_RT_SUPPORTED)
					message(STATUS "clang supports compiler-rt, use it")
					add_link_options(-rtlib=compiler-rt -unwindlib=libunwind)
				endif()
			else()
				set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libstdc++ -static-libgcc")
			endif()
		elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
			if (MINGW)
				set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libstdc++")
			else()
				set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libstdc++")
			endif()
			if (ENABLE_STATIC_LINK_TO_GCC)
				set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc")
			endif()
		endif()

		if (ENABLE_REUSEPORT)
			add_definitions(-DENABLE_REUSEPORT)
		endif()
	endif()
	if (ANDROID)
		add_definitions(-DDISABLE_WRITE_LOGGING)
		set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fPIE -pie")
	endif()
	if(COMPILER_HAS_STD_CXX20)
		set(CMAKE_CXX_STANDARD 20)
		add_definitions(-DUSE_STD_STRING_VIEW)
		message(STATUS "Set default cxx standard: C++20")
	else()
		message(FATAL_ERROR "need at least GCC 11 or clang 14")
	endif()

	if (ENABLE_MOLD)
		if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
			if (MOLD_LINKER)
				list(APPEND CMAKE_EXE_LINKER_FLAGS "-fuse-ld=mold")
			endif()
		endif()
	endif()

	if (ENABLE_LLD)
		if (LLD_LINKER)
			list(APPEND CMAKE_EXE_LINKER_FLAGS "-fuse-ld=lld")
		endif()
	endif()
endif(NOT MSVC)

################################################################################
# 全局使用静态链接库

set(BUILD_SHARED_LIBS OFF)

################################################################################
# 使用 boost asio 和 boost beast

if (ENABLE_BUILD_TESTS)
	set(USE_BOOST_TEST ON)
endif()

set(Boost_USE_STATIC_LIBS ON)
set(Boost_USE_STATIC_RUNTIME ON)

#独立编译asio时, 必须要在使用者项目定义, 否则会将
#定义BOOST_ASIO_HEADER_ONLY, 使用者将会导致符号冲突.

add_definitions(-DBOOST_ASIO_SEPARATE_COMPILATION)
add_definitions(-DBOOST_BEAST_SEPARATE_COMPILATION)

#此BOOST_ASIO_DYN_LINK只有在编译asio为动态链接的时候
#才能启用, 它在msvc下将BOOST_ASIO_DECL分别自动作为
#dllimport、dllexport添加.
#add_definitions(-DBOOST_ASIO_DYN_LINK)

add_definitions(-DBOOST_LOCALE_HIDE_AUTO_PTR)
add_definitions(-DBOOST_BIND_GLOBAL_PLACEHOLDERS)
add_definitions(-DBOOST_DISABLE_PRAGMA_MESSAGE)
add_definitions(-DBOOST_COROUTINES_NO_DEPRECATION_WARNING)
add_definitions(-DBOOST_PROCESS_USE_STD_FS)
if (ENABLE_USE_IO_URING)
	find_package(IOUring)

	if (IOUring_FOUND)
		message(STATUS "Linux using io_uring...")
		add_definitions(-DBOOST_ASIO_HAS_IO_URING -DBOOST_ASIO_DISABLE_EPOLL)
		link_libraries(${IOUring_LIBRARIES})
	endif()
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
	add_definitions(-DBOOST_ASIO_HAS_CO_AWAIT)
endif()

add_subdirectory(third_party/boost EXCLUDE_FROM_ALL)

set_property(TARGET boost_assert PROPERTY FOLDER "third_party/boost")
set_property(TARGET boost_atomic PROPERTY FOLDER "third_party/boost")
set_property(TARGET boost_chrono PROPERTY FOLDER "third_party/boost")
set_property(TARGET boost_container PROPERTY FOLDER "third_party/boost")
set_property(TARGET boost_context PROPERTY FOLDER "third_party/boost")
set_property(TARGET boost_core PROPERTY FOLDER "third_party/boost")
set_property(TARGET boost_coroutine PROPERTY FOLDER "third_party/boost")
set_property(TARGET boost_date_time PROPERTY FOLDER "third_party/boost")
set_property(TARGET boost_exception PROPERTY FOLDER "third_party/boost")
set_property(TARGET boost_filesystem PROPERTY FOLDER "third_party/boost")
set_property(TARGET boost_json PROPERTY FOLDER "third_party/boost")
set_property(TARGET boost_url PROPERTY FOLDER "third_party/boost")
set_property(TARGET boost_nowide PROPERTY FOLDER "third_party/boost")
set_property(TARGET boost_program_options PROPERTY FOLDER "third_party/boost")
set_property(TARGET boost_random PROPERTY FOLDER "third_party/boost")
set_property(TARGET boost_system PROPERTY FOLDER "third_party/boost")
set_property(TARGET boost_thread PROPERTY FOLDER "third_party/boost")
set_property(TARGET boost_variant2 PROPERTY FOLDER "third_party/boost")
set_property(TARGET boost_unordered PROPERTY FOLDER "third_party/boost")
set_property(TARGET boost_optional PROPERTY FOLDER "third_party/boost")

################################################################################
# 使用 Threads

find_package(Threads)
link_libraries(${CMAKE_THREAD_LIBS_INIT} ${CMAKE_DL_LIBS})

################################################################################
# 使用 tcmalloc/jemalloc

if (ENABLE_TCMALLOC_STATIC)
	find_package(Tcmalloc)
	link_libraries(${Tcmalloc_LIBRARY})
endif()

if (ENABLE_JEMALLOC_STATIC)
	find_package(JeMalloc)
	link_libraries(${JEMALLOC_LIBRARIES})
endif()

################################################################################
# 添加 tinyrpc 包含项目

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

################################################################################
# windows 下的链接库

if (WIN32)

	if (MINGW)
		link_libraries(
			secur32
			ws2_32
			mswsock
			bcrypt
			iphlpapi
		)
	else()
		link_libraries(
			Secur32.lib
			Bcrypt.lib
			Winmm.lib
			Mswsock.lib
		)
	endif()
elseif (UNIX AND NOT APPLE)
	# aviod link std::filesystem fail.
	set(std_fs_workaround "")
	if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
		set(std_fs_workaround stdc++fs)
	endif()

	link_libraries(
		${std_fs_workaround}
	)
endif()

if (ENABLE_LINKE_TO_LIBATOMIC)
	link_libraries(atomic)
endif()

################################################################################
# 设置输出目录

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if (WIN32)
	set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin/debug)
	set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin/release)
endif()

################################################################################
# 添加 example 项目

add_subdirectory(example)

