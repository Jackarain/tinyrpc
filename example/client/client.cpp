//
// server.cpp
// ~~~~~~~~~~
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

#include "tinyrpc/jsonrpc.hpp"

namespace net = boost::asio;
namespace beast = boost::beast;
namespace json = boost::json;

using ws = beast::websocket::stream<net::ip::tcp::socket>;
using session_type = jsonrpc::jsonrpc_session<ws>;

net::awaitable<void> client_session(std::string host, std::string port)
{
    auto executor = co_await net::this_coro::executor;
    net::ip::tcp::resolver resolver(executor);
    auto results = co_await resolver.async_resolve(host, port, net::use_awaitable);

    ws ws_stream(executor);
    co_await net::async_connect(ws_stream.next_layer(), results, net::use_awaitable);
    co_await ws_stream.async_handshake(host, "/", net::use_awaitable);
    ws_stream.binary(true);

    session_type session(std::move(ws_stream));

    // 设置 error 回调函数, 可选
    session.error_callback([](std::string_view error) {
        // 处理错误消息, 这里可以添加错误处理逻辑
        std::cerr << "Error: " << error << "\n";
    });

    // 启动 JSONRPC 会话消息处理
    session.start();

    //////////////////////////////////////////////////////////////////////////

    json::object subtract_req{
        {"a", 42}, {"b", 5}
    };

    // subtract RPC 调用, 通过异步回调的方式
    session.async_call("subtract", subtract_req,
        [&](boost::system::error_code ec, json::object result) {
            if (!ec)
                std::cout << "[subtract] result: " << json::serialize(result) << std::endl;
            else
                std::cerr << "subtract error: " << ec.message() << std::endl;
        }
    );

    json::object add_req{
        {"a", 10}, {"b", 3}
    };
    // add RPC 调用, 通过C++20协程的方式
    auto result = co_await session.async_call("add", add_req, net::use_awaitable);
    std::cout << "[add] result: " << json::serialize(result) << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: websocket-client <host> <port>\n";
        return 1;
    }
    std::string host = argv[1], port = argv[2];
    net::io_context ioc;
    net::co_spawn(ioc, client_session(host, port), net::detached);
    ioc.run();
    return 0;
}
