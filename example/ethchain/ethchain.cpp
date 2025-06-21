//
// ethchain.cpp
// ~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack dot wgm at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include <iostream>

#include <boost/system/error_code.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/ssl.hpp>

#include <boost/url.hpp>
#include <boost/multiprecision/cpp_int.hpp>
#include <boost/multiprecision/cpp_dec_float.hpp>

#include "tinyrpc/jsonrpc.hpp"

using boost::multiprecision::cpp_int;
using boost::multiprecision::cpp_dec_float_50;

namespace net = boost::asio;
namespace beast = boost::beast;
namespace ssl = boost::asio::ssl;
namespace json = boost::json;
using boost::url_view;

using wss = beast::websocket::stream<ssl::stream<beast::tcp_stream>>;
using session_type = jsonrpc::jsonrpc_session<wss>;

net::awaitable<void> ethchain_session(url_view url)
{
    ssl::context ctx{ ssl::context::tlsv12_client };

    auto port = std::string(url.port());
    if (!url.has_port())
		port = std::to_string(boost::urls::default_port(url.scheme_id()));

    auto host = url.host();

    auto executor = co_await net::this_coro::executor;
    net::ip::tcp::resolver resolver(executor);
    auto results = co_await resolver.async_resolve(host, port, net::use_awaitable);

    auto path = std::string(url.path());

    wss wss_stream(executor, ctx);

	if (!SSL_set_tlsext_host_name(wss_stream.next_layer().native_handle(), host.c_str()))
	{
		beast::error_code ec{
			static_cast<int>(::ERR_get_error()),
			net::error::get_ssl_category() };
		std::cerr << ec.message() << "\n";
        co_return;
	}

    wss_stream.next_layer().set_verify_callback(ssl::host_name_verification(host));

    co_await beast::get_lowest_layer(wss_stream).async_connect(results, net::use_awaitable);
    try
    {
		std::string origin = "all";
		auto decorator = [origin](boost::beast::websocket::request_type& m) {
			m.insert(boost::beast::http::field::origin, origin);
			};

		wss_stream.set_option(boost::beast::websocket::stream_base::decorator(decorator));

        co_await wss_stream.next_layer().async_handshake(ssl::stream_base::client, net::use_awaitable);
        co_await wss_stream.async_handshake(host, path.empty() ? "/" : path, net::use_awaitable);
    }
    catch (const std::exception& e)
    {
        std::cout << "async_handshake exception: " << e.what() << std::endl;
        co_return;
    }

    wss_stream.binary(true);

    session_type session(std::move(wss_stream));

    // 设置 error 回调函数, 可选
    session.error_callback([](std::string_view error) {
        // 处理错误消息, 这里可以添加错误处理逻辑
        std::cerr << "Error: " << error << "\n";
    });

    // 启动 JSONRPC 会话消息处理
    session.start();

    //////////////////////////////////////////////////////////////////////////

    json::array params;

	{
		// eth_blockNumber RPC 调用, 通过异步回调的方式
		session.async_call("eth_blockNumber", params,
			[&](boost::system::error_code ec, json::object result) {
				if (ec) {
					std::cerr << "eth_blockNumber error: " << ec.message() << std::endl;
					return;
				}

				std::cout << "[eth_blockNumber] result: " << json::serialize(result) << std::endl;
				cpp_int number(std::string(result["result"].as_string()));
				std::cout << "ETH latest block number: " << number << std::endl;
			}
		);

	}

	{
		params = json::array{
			"0xd8dA6BF26964aF9D7eEd9e03E53415D37aA96045",
			"latest"
		};

		// eth_getBalance RPC 调用, 通过 C++20 协程的方式
		auto result = co_await session.async_call("eth_getBalance", params, net::use_awaitable);

		std::cout << "[eth_getBalance] result: " << json::serialize(result) << std::endl;

		// 计算 ETH 余额数量
		cpp_dec_float_50 balance(cpp_int(std::string(result["result"].as_string())));
		balance /= 1000000000000000000;
		std::cout << "0xd8dA6BF26964aF9D7eEd9e03E53415D37aA96045 balance: " << balance << " ETH" << std::endl;
	}

	{
		params = json::array{
			"0x0000000000000000000000000000000000000000",
			"latest"
		};

		// eth_getBalance RPC 调用, 通过 C++20 协程的方式
		auto result = co_await session.async_call("eth_getBalance", params, net::use_awaitable);

		std::cout << "[eth_getBalance] result: " << json::serialize(result) << std::endl;

		// 计算 ETH 余额数量
		cpp_dec_float_50 balance(cpp_int(std::string(result["result"].as_string())));
		balance /= 1000000000000000000;
		std::cout << "0x0000000000000000000000000000000000000000 balance: " << balance << " ETH" << std::endl;
	}
}

int main(int argc, char* argv[])
{
    if (argc != 2) {
        std::cerr << "Usage: ethchain <url>\n";
        return 1;
    }

    boost::url_view url(argv[1]);

    net::io_context ioc;
      net::co_spawn(ioc, ethchain_session(url), net::detached);
    ioc.run();

    return 0;
}
