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
#include <cstdlib>
#include <string>
#include <chrono>

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

// 处理 JSONRPC 协议的协程函数, 该函数会在新的 WebSocket 连接建立后被调用
net::awaitable<void> do_jsonrpc(session_type session)
{
    // 设置 method 回调函数, 可选
    session.default_method_callback([](json::object request) {
        // 处理请求消息, 这里可以添加处理逻辑
        std::cout << "Received request: " << json::serialize(request) << "\n";
    });

    // 设置 notify 回调函数, 可选
    session.notify_callback([](json::object notification) {
        // 处理通知消息, 这里可以添加处理逻辑
        std::cout << "Received notification: " << json::serialize(notification) << "\n";
    });

    // 设置 error 回调函数, 可选
    session.error_callback([](std::string_view error) {
        // 处理错误消息, 这里可以添加错误处理逻辑
        std::cerr << "Error: " << error << "\n";
    });


    // 绑定 subtract 方法
    session.bind_method("subtract", [&session](json::object obj) {
        // 处理 subtract 方法调用, 这里只是作为示例打印输出请求 JSON 对象
        std::cout << "subtract method called with obj: " << json::serialize(obj) << "\n";

        // 回复请求, 回复的内容是一个 JSON 对象, 作为 JSONRPC 协议的 result 部分
        // 我们不需要关心 JSONRPC 协议的其它字段
        json::object response = {
            {"val", "47"},
        };

        // 回复请求, 使用 jsonrpc_id(obj) 获取请求的 ID 使客户端能够匹配响应
        session.reply(response, jsonrpc::jsonrpc_id(obj));
    });


    auto executor = co_await net::this_coro::executor;

    // 绑定 add 方法
    session.bind_method("add", [&session, executor](json::object obj) {
        // 处理 add 方法调用, 这里只是作为示例打印输出请求 JSON 对象
        std::cout << "add method called with obj: " << json::serialize(obj) << "\n";

        // 我们不必限定在 bind_method 这个回调函数中回应对方，我们亦可以
        // 通过异步处理 add 方法, 这里发起一个 asio 协程模拟一些异步操作
        net::co_spawn(executor, [&session, obj = std::move(obj)]() -> net::awaitable<void>
        {
            // 模拟一些异步操作, 例如等待 3 秒钟
            co_await net::steady_timer(session.get_executor(), std::chrono::seconds(3)).async_wait(net::use_awaitable);

            // 回复请求, 回复的内容是一个 JSON 对象, 作为 JSONRPC 协议的 result 部分
            // 我们不需要关心 JSONRPC 协议的其它字段
            json::object response = {
                {"val", "19"},
            };

            // 回复请求, 使用 jsonrpc_id(obj) 获取请求的 ID 使客户端能够匹配响应
            session.reply(response, jsonrpc::jsonrpc_id(obj));

            co_return;
        }, net::detached);
    });

    //////////////////////////////////////////////////////////////////////////
    // 处理 WebSocket 消息的循环
    // 该循环会持续接收 WebSocket 消息, 并将其解析为 JSON 对象, 然后调用
    // session.dispatch() 方法来处理接收到的 JSONRPC 请求.
    //
    // 整个消息循环部分也可以直接运行 session.start() 来替代, 在 start()
    // 方法中也实现了类似的消息循环, 但这里我们手动实现了一个简单的消息循环
    // , 这样可以更灵活地处理消息.

    boost::system::error_code ec;
    beast::flat_buffer buf;
    auto& stream = session.stream();

    try
    {
        while (true)
        {
            auto bytes = co_await stream.async_read(buf, net::use_awaitable);
            auto bufdata = buf.data();
            std::string_view sv((const char*)bufdata.data(), bufdata.size());

            json::value jv = json::parse(
                sv,
                ec,
                json::storage_ptr{},
                { 64, json::number_precision::imprecise, true, true, true });
            if (ec)
                break;

            buf.consume(bytes);

            if (!jv.is_object())
                break;

            auto obj = jv.as_object();
            if (!obj.if_contains("jsonrpc"))
                break;

            // 处理 JSONRPC 请求
            session.dispatch(std::move(obj));
        }
    }
    catch (const std::exception&)
    {}

    // 打印关闭 WebSocket 连接
    std::cerr << "WebSocket session ended\n";

    co_return;
}

net::awaitable<void> do_listen(const net::ip::address &address, unsigned short port)
{
    try
    {
        net::ip::tcp::acceptor acceptor(co_await net::this_coro::executor);
        net::ip::tcp::endpoint listen_endp{ address, port };

        // 打开接受器
        acceptor.open(listen_endp.protocol());
        // 绑定接受器到指定的地址和端口
        acceptor.bind(listen_endp);
        // 开始监听连接
        acceptor.listen(net::socket_base::max_listen_connections);

        while (true)
        {
            beast::websocket::stream<net::ip::tcp::socket>
                ws_stream(net::ip::tcp::socket{ co_await net::this_coro::executor });

            // 等待新的连接
            co_await acceptor.async_accept(ws_stream.next_layer(), net::use_awaitable);

            // 接受 WebSocket 连接
            co_await ws_stream.async_accept(net::use_awaitable);

            // 设置 WebSocket 流为二进制模式
            ws_stream.binary(true);

            // 创建 JSON-RPC 会话
            session_type session(std::move(ws_stream));

            net::co_spawn(
                session.get_executor(),
                [&session]() -> net::awaitable<void>
                {
                    // 启动 JSON-RPC 会话
                    co_await do_jsonrpc(std::move(session));
                    co_return;
                },
                net::detached);
        }
    }
    catch (const std::exception&)
    {}

    std::cerr << "Server stopped listening on " << address.to_string() << ":" << port << "\n";

    co_return;
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        std::cerr <<
            "Usage: websocket-server <address> <port>\n" <<
            "Example:\n" <<
            "    websocket-server 0.0.0.0 8000\n";
        return EXIT_FAILURE;
    }

    auto const address = net::ip::make_address(argv[1]);
    auto const port = static_cast<unsigned short>(std::atoi(argv[2]));

    boost::asio::io_context ioc;

    // Create and run the JSON-RPC server
    try
    {
        net::co_spawn(ioc,
            [&ioc, address, port]() -> net::awaitable<void>
            {
                co_await do_listen(address, port);
                co_return;
            },
            net::detached);

        ioc.run();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
