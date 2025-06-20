
# C++ tinyrpc 基于 boost.asio 的 JSONRPC-2.0 标准的实现

## 介绍

使用 `boost.asio` 底层实现, 实现异步（支持 `asio` 回调、协程等支持的方式）的标准 `JSONRPC-2.0` 调用。

## 使用

这个库本身实现只有一个 .hpp 头文件实现，在能使用 boost 的项目中，将 `jsonrpc.hpp` 复制到项目即可使用。

## 快速上手

用法参考 `example`，你可以编译运行并调试它们，以了解它的实现原理。

在 `jsonrpc.hpp` 中，它并不严格区分 `server` 或 `client`，也就是说，双方都可以使用 `jsonrpc_session` 的 `async_call` 来发起向对方的 `RPC` 调用，只要对方使用 `bind_method` 绑定了对应的 `method` 回调

`async_call` 可以是回调，也可以是 `asio` 协程，如：

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

再如通过 `asio` 协程来调用：

``` c++
    json::object add_req{
        {"a", 10}, {"b", 3}
    };
    // add RPC 调用, 通过C++20协程的方式, 它将使用 JSONRPC 协议将上面 add_req 对象作为
    // params 发送到对方, 在 async_call 这个接口中, 只需要给定 method 及 params 即可
    auto result = co_await session.async_call("add", add_req, net::use_awaitable);
    std::cout << "[add] result: " << json::serialize(result) << std::endl;
```

处理 `RPC` 请求示例：

``` c++
    // 绑定 subtract 方法
    session.bind_method("subtract", [&session](json::object obj) {
        std::cout << "[subtract] method called with obj: " << json::serialize(obj) << "\n";

        // 回复请求, 回复的内容是一个 JSON 对象, 作为 JSONRPC 协议的 result 部分
        // 我们不需要关心 JSONRPC 协议的其它字段

        auto params = obj["params"].as_object();
        auto a = params["a"].as_int64();
        auto b = params["b"].as_int64();

        json::object response = {
            {"val", a + b},
        };

        // 回复请求, 使用 jsonrpc_id(obj) 获取请求的 ID 使客户端能够匹配响应
        session.reply(response, jsonrpc::jsonrpc_id(obj));
    });
```

如果是一些很费时的操作，可以为了避免阻塞 `bind_method`，可以在 `bind_method` 之外的地方调用 `reply` 来回复客户端，在这里必须要说明的是，以往形式是

``` c++
void (request, reply) {
    // 对 reply 复制，待函数据返回，自动将 reply 内赋值的信息发送给对方，这种形
    // 式有一个麻烦，它必须限制在 method 响应函数中必须修改 reply 以回应远程调用
    // 通常我们并不能在这个 method 响应作长时间停留，因为这样会导致整个消息处理循
    // 环阻塞在这里
}
```

所以，当前的设计是取消了 `reply` 参数机制，而是使用 `jsonrpc_session` 的成员函数 `reply` 来回应客户端的 `RPC` 请求，这样就不会导致限制在 `method` 响应回调函数中了，如：

``` c++
    // 绑定 add 方法
    session.bind_method("add", [&session, executor](json::object obj) {
        std::cout << "[add] method called with obj: " << json::serialize(obj) << "\n";

        // 我们不必限定在 bind_method 这个回调函数中回应对方，我们亦可以
        // 通过异步处理 add 方法, 这里发起一个 asio 协程模拟一些异步操作
        net::co_spawn(executor, [&session, executor, obj = std::move(obj)]() mutable -> net::awaitable<void>
        {
            // 模拟一些异步操作, 例如等待 3 秒钟
            co_await net::steady_timer(executor, std::chrono::seconds(3)).async_wait(net::use_awaitable);

            auto params = obj["params"].as_object();
            auto a = params["a"].as_int64();
            auto b = params["b"].as_int64();

            json::object response = {
                {"val", a + b},
            };

            // 回复请求, 使用 jsonrpc_id(obj) 获取请求的 ID 使客户端能够匹配响应
            session.reply(response, jsonrpc::jsonrpc_id(obj));

            co_return;
        }, net::detached);
    });
```
