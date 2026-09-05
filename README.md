
# C++ tinyrpc 基于 boost.asio 的 JSONRPC-2.0 标准的实现

## 介绍

使用 `boost.asio` 底层实现, 实现异步（支持 `asio` 回调、协程等支持的方式）的标准 `JSONRPC-2.0` 调用。

## 使用

这个库本身实现只有一个 .hpp 头文件实现，在能使用 boost 的项目中，将 `jsonrpc.hpp` 复制到项目即可使用，tinyrpc 是非侵入式设计，遵循 `boost.asio` 的分层设计理念，因此可将其做为 `tcp` 或 `websocket` 的上层协议来使用，因此 `tinyrpc` 并不严格区分 `server` 或 `client`，双方都可以使用 `jsonrpc_session` 做为 `tcp` 或 `websocket` 甚至是 `ssl` 加密等任何符合 `asio` 分层理念的上层协议，然后调用 `jsonrpc_session` 的 `async_call` 来发起向对方的 `RPC` 调用，只要对方使用 `bind_method` 绑定了对应的 `method` 回调即可。

## 快速上手

用法参考 `example`，你可以编译运行并调试它们，以了解它的实现原理，下面是一些简单的示例以介绍基本使用方法。

`async_call` 可以是回调，也可以是 `asio` 协程，如：

``` c++
json::object sub_req{
    {"a", 42}, {"b", 5}
};

// sub RPC 调用, 通过异步回调的方式, 它将使用 JSONRPC 协议将上面 sub_req
// 对象作为 params 发送到对方, 在 async_call 这个接口中, 只需要给定 method 及
// params 即可
session.async_call("sub", sub_req,
    [&](boost::system::error_code ec, json::object result) {
        if (!ec)
            std::cout << "[sub] result: " << json::serialize(result) << std::endl;
        else
            std::cerr << "sub error: " << ec.message() << std::endl;
    }
);
```

再比如通过 `asio` 协程来调用：

``` c++
json::object add_req{
    {"a", 10}, {"b", 3}
};
// add RPC 调用, 通过C++20协程的方式, 它将使用 JSONRPC 协议将上面 add_req 对象作为
// params 发送到对方, 在 async_call 这个接口中, 只需要给定 method 及 params 即可
auto result = co_await session.async_call("add", add_req, net::use_awaitable);
std::cout << "[add] result: " << json::serialize(result) << std::endl;
```

对端处理上述 `RPC` 请求示例：

``` c++
// 绑定 sub 方法
session.bind_method("sub", [&session](json::object obj) {
    std::cout << "[sub] method called with obj: " << json::serialize(obj) << "\n";

    // 回复请求, 回复的内容是一个 JSON 对象, 作为 JSONRPC 协议的 result 部分
    // 我们不需要关心 JSONRPC 协议的其它字段

    auto params = obj["params"].as_object();
    auto a = params["a"].as_int64();
    auto b = params["b"].as_int64();

    json::object response = {
        {"val", a - b},
    };

    // 手工回复请求, 使用 jsonrpc_id(obj) 获取请求的原始 id 使客户端能够匹配响应
    session.reply(response, jsonrpc::jsonrpc_id(obj));
});
```

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

        // 手工回复请求, 使用 jsonrpc_id(obj) 获取请求的原始 id 使客户端能够匹配响应
        session.reply(response, jsonrpc::jsonrpc_id(obj));
        co_return;
    }, net::detached);
});
```

上面处理 `RPC` 请求示例中，我们可以在 `bind_method` 中的回调函数中处理 `RPC` 请求，也可以创建一个异步协程来处理 `RPC` 请求，这取决于我们的需求，下面是使用 `bind_method` 异步协程处理 `RPC` 请求的示例：

``` c++
// 绑定 mul 方法, 协程返回 void, 需手工回复请求.
session.bind_method("mul",
    [&session, executor](json::object obj) mutable -> net::awaitable<void> {
        // 处理 mul 方法调用, 这里只是作为示例打印输出请求 JSON 对象
        std::cout << "[mul] method called with obj: " << json::serialize(obj) << "\n";

        // 模拟一些异步操作, 例如等待 3 秒钟
        co_await net::steady_timer(session.get_executor(), std::chrono::seconds(3)).async_wait(net::use_awaitable);

        auto params = obj["params"].as_object();
        auto a = params["a"].as_int64();
        auto b = params["b"].as_int64();

        json::object response = {
            {"val", a * b},
        };

        // 回复请求, 使用 jsonrpc_id(obj) 获取请求的原始 id 使客户端能够匹配响应
        session.reply(response, jsonrpc::jsonrpc_id(obj));
        co_return;
    });
```

亦可通过返回值的方式来回应 `RPC` 请求，例如：

``` c++
// 绑定 mul 方法, 注意, 与上面不同, 协程返回值为 json::object, 它会被自动序列化为 JSONRPC 协议
// 的 result 部分发送给请求端.
session.bind_method("mul",
    [&session, executor](json::object obj) mutable -> net::awaitable<json::object> {
        // 处理 mul 方法调用, 这里只是作为示例打印输出请求 JSON 对象
        std::cout << "[mul] method called with obj: " << json::serialize(obj) << "\n";

        // 模拟一些异步操作, 例如等待 3 秒钟
        co_await net::steady_timer(session.get_executor(), std::chrono::seconds(3)).async_wait(net::use_awaitable);

        auto params = obj["params"].as_object();
        auto a = params["a"].as_int64();
        auto b = params["b"].as_int64();

        json::object response = {
            {"val", a * b},
        };

        // 回复请求, 返回值会自动被序列化为 JSONRPC 协议的 result 部分发送给请求端.
        co_return response;
    });
```

具体参考 `example` 中的示例代码

## 设计优势

`tinyrpc` 设计的优势在于：

- 协程请求可支持回调、协程等，与 `asio` 相同的 `CompletionToken` 概念的接口，使 `RPC` 调用更加灵活。
- 处理 `RPC` 请求避免固定返回模式，这样就不会导致限制在 `method` 响应回调函数中，甚至可以创建异步协程，在协程中回应 `RPC` 请求，具体解释如下：

大多数传统的 `RPC` 库都是只能以下这种方式处理 `RPC` 请求：

``` c++
void (request, reply) {
    // 对 reply 复制，待函数据返回，自动将 reply 内赋值的信息发送给对方，这种形
    // 式有一个麻烦，它必须限制在 method 响应函数中必须修改 reply 以回应远程调用
    // 通常我们并不能在这个 method 响应作长时间停留，因为这样会导致整个消息处理循
    // 环阻塞在这里
}
```

亦或是这种方式：

``` c++
auto (request) -> reply {
    // 对 request 进行处理, 并返回一个 reply 对象
    // 这种设计的缺点，使我们不能在这个 method 响应作长时间停留，因为这样会导致
    // 整个消息处理循环阻塞在这里.
}
```

以上2种设计都是固定返回模式，所以，当前 `tinyrpc` 的设计是放弃了这种固定返回模式，而是支持通过 `jsonrpc_session` 的 `reply` 方法来回应客户端的 `RPC` 请求，这样就不会将处理 `RPC` 请求的处理流程限制在 `method` 响应回调函数中了。

当然，也可以和传统的方式一样，通过返回值的方式来回应 `RPC` 请求，在一些应用简单的场景下使用会更方便。

## 会话的两种驱动模式

`jsonrpc_session` 并不区分严格的 `server` 或 `client` 身份，`async_call` 与
`bind_method`/`reply` 可以在同一个会话上同时使用。

驱动会话接收消息有两种等价的方式：

- 模式 A：调用 `start()`，由会话内部的消息循环自动接收并派发消息。
- 模式 B：不调用 `start()`，由使用者在自己的消息循环中读取到 JSONRPC 消息后，
  调用 `dispatch()` 手工派发。

两种模式下会话的对外行为一致，都可以发起 `async_call` 调用、应答对方的请求或
发送通知，差别仅在于接收消息的方式，二者不可混用（模式 B 下不能再调用
`start()`，否则会重复读取底层流）。模式 B 的会话不再接收消息时，调用 `stop()`
可以立即以 `operation_aborted` 完成所有挂起的调用并关闭连接。

`stop()` 是终态操作：调用后会话不再接受新的 `async_call`，也不能再次调用
`start()`；此时再手工 `dispatch()` 的消息会被忽略（不会进入方法回调、不会
向已关闭的连接写响应）。需要重新使用时请创建新的 `jsonrpc_session`。

## 挂起调用的取消与 id 生命周期

`async_call` 可通过 `CompletionToken` 关联的 `cancellation_slot` 取消挂起的调用
（如超时场景）。被取消的调用只会回调一次 `operation_aborted`。

为避免串扰，被取消调用的 `id` 不会立即复用：会话会保留该槽位直到对应请求的
迟到响应到达（到达后被丢弃并回收该 `id`），或会话停止（`stop()`/连接关闭）时
统一回收。这样迟到的响应不会误投递给后续复用同一 `id` 的新调用。

因此需要注意：如果持续取消调用而对端从不响应，内部登记表会随取消次数单调
增长，这是防串扰的正确性取舍，所占用资源会在迟到响应到达或会话停止时回收。
长期会话中应避免对永不响应的调用反复取消而不关闭会话。

## 回调异常处理

- 绑定方法回调（`bind_method`）或默认方法回调（`default_method_callback`）
  抛出异常时，会话会向请求方回复 JSON-RPC 标准 `-32603 Internal error` 错误
  响应（异常详情放在 `data` 字段），避免请求方永久挂起；同时若设置了
  `error_callback` 会收到异常通知。
- 用户传入 `async_call` 的完成回调抛出异常时，异常不会逃逸到 asio 执行上下文
  导致进程终止，而是转交 `error_callback` 通知。
- 通知回调（`notify_callback`）或数据转换回调（`data_callback`）抛出异常时，
  没有可回复的请求方，异常会转交 `error_callback` 通知；其中 `data_callback`
  异常只丢弃当前帧，会话继续接收后续消息，不会使消息循环失败关闭。
- 会话在连接错误等路径调用 `error_callback` 时，回调自身抛出的异常会被忽略，
  不会阻断挂起调用的清理流程。
- 注意：若在方法回调中先自行调用 `reply()` 再抛出异常，会话会额外补发一个
  `-32603 Internal error` 错误响应（先前的响应仍会发送），请在方法回调中避免
  先 `reply()` 后抛异常的顺序错误。
