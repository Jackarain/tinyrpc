cmake_minimum_required(VERSION 2.8)
project(jsoncpp CXX)

add_library(jsoncpp STATIC json.h jsoncpp.cpp)
target_include_directories(jsoncpp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
#target_compile_options(jsoncpp  PRIVATE -fPIC -fno-rtti -fno-exceptions -Wall)

set_target_properties(jsoncpp PROPERTIES FOLDER "third_party")
set_target_properties(jsoncpp PROPERTIES POSITION_INDEPENDENT_CODE ON)

