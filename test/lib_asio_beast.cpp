//
// lib_asio_beast.cpp
// ~~~~~~~~~~~~~~~~~~
//
// 为单元测试提供 asio/beast 的分离编译实现源.
// 由于项目全局定义了 BOOST_ASIO_SEPARATE_COMPILATION 和
// BOOST_BEAST_SEPARATE_COMPILATION, 必须单独编译 asio/beast 的实现,
// 否则链接阶段会缺少相关符号.
//

#include <boost/asio/impl/src.hpp>

#if __has_include(<openssl/ssl.h>)
#	include <boost/asio/ssl/impl/src.hpp>
#endif

#include <boost/beast/src.hpp>
