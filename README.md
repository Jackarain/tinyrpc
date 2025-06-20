
# c++ tinyrpc 基于 boost.asio 的 jsonrpc 的实现

## 介绍

使用 boost.asio 底层实现, 实现异步（支持 asio 回调、协程等支持的方式）的 jsonrpc 2.0 调用。

## 使用

这个库本身实现只有一个 .hpp 头文件实现，在能使用 boost 的项目中，将 jsonrpc.hpp 复制到项目即可使用。

## 快速上手

用法参考 example，你可以编译运行并调试它们，以了解它的实现原理。

在 jsonrpc.hpp 中，它并不严格区分 server 或 client，也就是说，双方都可以使用 jsonrpc_session 的 async_call 来发起向对方的 RPC 调用，只要对方使用 bind_method 绑定了对应的 method 回调

async_call 可以是回调，也可以是 asio 协程，如：

``` c++
    json::object subtract_req{
        {"a", 42}, {"b", 5}
    };

    // subtract RPC 调用, 通过异步回调的方式, 它将使用 JSONRPC 协议将上面 add_req
    // 对象作为 params 发送到对方, 在 async_call 这个接口中, 只需要给定 method 及
    // params 即可
    session.async_call("subtract", subtract_req,
        [&](boost::system::error_code ec, json::object result) {
            if (!ec)
                std::cout << "[subtract] result: " << json::serialize(result) << std::endl;
            else
                std::cerr << "subtract error: " << ec.message() << std::endl;
        }
    );
```

再如通过 asio 协程来调用：

``` c++
    json::object add_req{
        {"a", 10}, {"b", 3}
    };
    // add RPC 调用, 通过C++20协程的方式, 它将使用 JSONRPC 协议将上面 add_req 对象作为
    // params 发送到对方, 在 async_call 这个接口中, 只需要给定 method 及 params 即可
    auto result = co_await session.async_call("add", add_req, net::use_awaitable);
    std::cout << "[add] result: " << json::serialize(result) << std::endl;
```
