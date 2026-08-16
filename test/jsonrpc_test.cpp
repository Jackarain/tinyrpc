//
// jsonrpc_test.cpp
// ~~~~~~~~~~~~~~~~
//
// tinyrpc jsonrpc 单元测试
//
// 使用 Boost.Test (header-only 模式), 通过 beast::test::stream 在内存中
// 建立 websocket 连接进行端到端测试, 无需真实网络.
//
// 覆盖范围:
//   - jsonrpc_id 辅助函数
//   - 模式 A (start 驱动) 的 RPC 调用
//   - 模式 B (手动 dispatch) 的 RPC 调用
//   - bind_method 的三种返回形式 (json::object / awaitable<json::object> / void+reply)
//   - 未注册方法的 -32601 错误响应 (数字/字符串 id)
//   - 通知 (notification) 与 notify() 门面 (含 null params 省略 params 字段)
//   - 回调形式的 async_call
//   - 字符串 id 保留
//   - 双向 RPC
//   - default_method_callback / data_callback / closed_callback 回调
//   - 手工错误响应 reply(..., error=true)
//   - stop() 后发起调用的立即失败
//   - 错误回调路径 (非对象 JSON / 无 jsonrpc 字段 / method 非字符串 /
//     invalid id format / invalid session id)
//   - release() 与移动语义
//

#define BOOST_TEST_MODULE jsonrpc_test
#include <boost/test/unit_test.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/as_tuple.hpp>

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/_experimental/test/stream.hpp>

#include <boost/json.hpp>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include "tinyrpc/jsonrpc.hpp"

namespace net = boost::asio;
namespace beast = boost::beast;
namespace json = boost::json;

// 使用 beast::test::stream 作为底层流, 在内存中模拟双向连接.
using test_ws = beast::websocket::stream<beast::test::stream>;
using session_type = jsonrpc::jsonrpc_session<test_ws>;

// 用于跟踪底层流析构的流类型.
// 当 jsonrpc_session_service 析构时, 其底层流成员随之析构,
// 通过该标志可验证 service 是否被正常析构.
struct tracking_stream : beast::test::stream
{
  std::shared_ptr<bool> destroyed;

  tracking_stream(net::any_io_executor ex, std::shared_ptr<bool> d)
    : beast::test::stream(std::move(ex))
    , destroyed(std::move(d))
  {}

  tracking_stream(net::io_context& ctx, std::shared_ptr<bool> d)
    : beast::test::stream(ctx)
    , destroyed(std::move(d))
  {}

  tracking_stream(tracking_stream&&) noexcept = default;
  tracking_stream& operator=(tracking_stream&&) noexcept = default;

  ~tracking_stream()
  {
    if (destroyed)
      *destroyed = true;
  }
};

using tracking_ws = beast::websocket::stream<tracking_stream>;
using tracking_session = jsonrpc::jsonrpc_session<tracking_ws>;

// beast 的 teardown/async_teardown 是为 basic_stream 精确类型提供的 friend
// 函数 (位于 boost::beast::test 命名空间, 通过 ADL 查找). 派生的
// tracking_stream 需要显式转发到基类实现, 否则 websocket 关闭时会因
// "Unknown Socket type" 静态断言失败.
namespace boost {
namespace beast {
namespace test {

template<class TeardownHandler>
void
async_teardown(
    role_type role,
    ::tracking_stream& s,
    TeardownHandler&& handler)
{
    boost::beast::test::async_teardown(
        role,
        static_cast<::boost::beast::test::stream&>(s),
        std::forward<TeardownHandler>(handler));
}

void
teardown(
    role_type role,
    ::tracking_stream& s,
    boost::system::error_code& ec)
{
    boost::beast::test::teardown(
        role,
        static_cast<::boost::beast::test::stream&>(s),
        ec);
}

} // namespace test
} // namespace beast
} // namespace boost

namespace {

// 运行一个端到端测试场景.
// 内部建立一对内存 websocket 连接, 分别驱动 server/client 的 start(),
// 握手完成后调用 fn(server, client) 执行测试逻辑, 结束后自动关闭连接.
template <class Fn>
void run_with_pair(Fn&& fn)
{
  net::io_context ioc;

  beast::test::stream s1(ioc), s2(ioc);
  s1.connect(s2);

  test_ws server_ws(std::move(s1));
  test_ws client_ws(std::move(s2));

  session_type server(std::move(server_ws));
  session_type client(std::move(client_ws));

  // 默认静默错误回调, 避免测试结束时连接关闭触发断言.
  server.error_callback([](std::string_view) {});
  client.error_callback([](std::string_view) {});

  bool done = false;
  net::steady_timer watchdog(ioc, std::chrono::seconds(15));

  // server 端握手 + 启动
  net::co_spawn(ioc,
    [&]() -> net::awaitable<void>
    {
      try
      {
        co_await server.stream().async_accept(net::use_awaitable);
        server.start();
      }
      catch (const std::exception&)
      {}
      co_return;
    }, net::detached);

  // client 端握手 + 启动 + 测试逻辑
  net::co_spawn(ioc,
    [&]() -> net::awaitable<void>
    {
      try
      {
        co_await client.stream().async_handshake("test", "/", net::use_awaitable);
        client.start();
        co_await fn(server, client);
      }
      catch (const std::exception&)
      {}
      done = true;
      watchdog.cancel();
      beast::get_lowest_layer(server.stream()).close();
      beast::get_lowest_layer(client.stream()).close();
      co_return;
    }, net::detached);

  watchdog.async_wait([&](boost::system::error_code ec)
  {
    if (ec != net::error::operation_aborted && !done)
      BOOST_ERROR("test timed out");
  });

  ioc.run();
}

// 运行一个需要手动读写底层数据的端到端测试场景.
// 与 run_with_pair 的区别: client 不调用 start(), 由测试逻辑通过
// stream() 手动发送/接收数据帧, 用于验证 server 端的解析与响应.
template <class Fn>
void run_manual_pair(Fn&& fn)
{
  net::io_context ioc;

  beast::test::stream s1(ioc), s2(ioc);
  s1.connect(s2);

  test_ws server_ws(std::move(s1));
  test_ws client_ws(std::move(s2));

  session_type server(std::move(server_ws));
  session_type client(std::move(client_ws));

  server.error_callback([](std::string_view) {});
  client.error_callback([](std::string_view) {});

  bool done = false;
  net::steady_timer watchdog(ioc, std::chrono::seconds(15));

  // server 端握手 + 启动
  net::co_spawn(ioc,
    [&]() -> net::awaitable<void>
    {
      try
      {
        co_await server.stream().async_accept(net::use_awaitable);
        server.start();
      }
      catch (const std::exception&)
      {}
      co_return;
    }, net::detached);

  // client 端握手后执行测试逻辑 (不调用 start)
  net::co_spawn(ioc,
    [&]() -> net::awaitable<void>
    {
      try
      {
        co_await client.stream().async_handshake("test", "/", net::use_awaitable);
        co_await fn(server, client);
      }
      catch (const std::exception&)
      {}
      done = true;
      watchdog.cancel();
      beast::get_lowest_layer(server.stream()).close();
      beast::get_lowest_layer(client.stream()).close();
      co_return;
    }, net::detached);

  watchdog.async_wait([&](boost::system::error_code ec)
  {
    if (ec != net::error::operation_aborted && !done)
      BOOST_ERROR("test timed out");
  });

  ioc.run();
}

// 轮询等待某个条件, 避免 busy-loop 占满 io_context.
template <class Pred>
net::awaitable<void> wait_until(net::any_io_executor ex, Pred&& pred)
{
  while (!pred())
  {
    co_await net::steady_timer(ex, std::chrono::milliseconds(5))
      .async_wait(net::use_awaitable);
  }
}

} // namespace

//////////////////////////////////////////////////////////////////////////////

BOOST_AUTO_TEST_SUITE(jsonrpc_unit)

// jsonrpc_id 辅助函数: 整数 id
BOOST_AUTO_TEST_CASE(jsonrpc_id_int)
{
  json::object obj{{"id", 42}};
  auto id = jsonrpc::jsonrpc_id(obj);
  BOOST_TEST(id.is_int64());
  BOOST_TEST(id.as_int64() == 42);
}

// jsonrpc_id 辅助函数: 字符串 id
BOOST_AUTO_TEST_CASE(jsonrpc_id_string)
{
  json::object obj{{"id", std::string("abc")}};
  auto id = jsonrpc::jsonrpc_id(obj);
  BOOST_TEST(id.is_string());
  BOOST_TEST(id.as_string() == "abc");
}

// jsonrpc_id 辅助函数: 无 id 字段返回 null
BOOST_AUTO_TEST_CASE(jsonrpc_id_missing)
{
  json::object obj{{"method", "x"}};
  auto id = jsonrpc::jsonrpc_id(obj);
  BOOST_TEST(id.is_null());
}

// 模式 A 基本 RPC: bind_method 同步返回 json::object
BOOST_AUTO_TEST_CASE(rpc_sync_return)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    server.bind_method("add", [](json::object obj) -> json::object
    {
      auto p = obj["params"].as_object();
      json::object r{{"val", p["a"].as_int64() + p["b"].as_int64()}};
      return r;
    });

    auto [ec, result] = co_await client.async_call(
      "add", json::object{{"a", 2}, {"b", 3}}, net::as_tuple(net::use_awaitable));
    BOOST_TEST(!ec);
    BOOST_TEST(result.if_contains("result"));
    BOOST_TEST(result["result"].as_object()["val"].as_int64() == 5);
    co_return;
  });
}

// bind_method 协程返回 awaitable<json::object>
BOOST_AUTO_TEST_CASE(rpc_coroutine_return)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    server.bind_method("mul", [](json::object obj) -> net::awaitable<json::object>
    {
      auto p = obj["params"].as_object();
      co_return json::object{{"val", p["a"].as_int64() * p["b"].as_int64()}};
    });

    auto [ec, result] = co_await client.async_call(
      "mul", json::object{{"a", 6}, {"b", 7}}, net::as_tuple(net::use_awaitable));
    BOOST_TEST(!ec);
    BOOST_TEST(result["result"].as_object()["val"].as_int64() == 42);
    co_return;
  });
}

// bind_method 返回 void + 手工 reply (带延迟, 验证异步回复与 id 匹配)
BOOST_AUTO_TEST_CASE(rpc_manual_reply_with_delay)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    server.bind_method("slow_add", [&server](json::object obj) -> net::awaitable<void>
    {
      auto id = jsonrpc::jsonrpc_id(obj);
      auto p = obj["params"].as_object();

      // 模拟异步操作
      co_await net::steady_timer(server.get_executor(), std::chrono::milliseconds(100))
        .async_wait(net::use_awaitable);

      json::object r{{"val", p["a"].as_int64() + p["b"].as_int64()}};
      server.reply(r, std::move(id));
      co_return;
    });

    auto [ec, result] = co_await client.async_call(
      "slow_add", json::object{{"a", 3}, {"b", 4}}, net::as_tuple(net::use_awaitable));
    BOOST_TEST(!ec);
    BOOST_TEST(result["result"].as_object()["val"].as_int64() == 7);
    co_return;
  });
}

// 模式 B: server 手动 dispatch (不调用 start), client 使用 start 驱动
BOOST_AUTO_TEST_CASE(rpc_mode_b_manual_dispatch)
{
  net::io_context ioc;

  beast::test::stream s1(ioc), s2(ioc);
  s1.connect(s2);

  test_ws server_ws(std::move(s1));
  test_ws client_ws(std::move(s2));

  session_type server(std::move(server_ws));
  session_type client(std::move(client_ws));

  server.error_callback([](std::string_view) {});
  client.error_callback([](std::string_view) {});

  server.bind_method("add", [](json::object obj) -> json::object
  {
    auto p = obj["params"].as_object();
    json::object r{{"val", p["a"].as_int64() + p["b"].as_int64()}};
    return r;
  });

  bool done = false;
  net::steady_timer watchdog(ioc, std::chrono::seconds(15));

  // server: 手动读循环 + dispatch (模式 B)
  net::co_spawn(ioc,
    [&]() -> net::awaitable<void>
    {
      try
      {
        co_await server.stream().async_accept(net::use_awaitable);

        beast::flat_buffer buf;
        while (true)
        {
          auto bytes = co_await server.stream().async_read(buf, net::use_awaitable);
          json::value jv = json::parse(beast::buffers_to_string(buf.data()),
            boost::system::error_code{},
            json::storage_ptr{},
            {64, json::number_precision::imprecise, true, true, true});
          buf.consume(bytes);

          if (!jv.is_object() || !jv.as_object().if_contains("jsonrpc"))
            break;

          server.dispatch(jv.as_object());
        }
      }
      catch (const std::exception&)
      {}
      co_return;
    }, net::detached);

  // client: 握手 + start + RPC 调用
  net::co_spawn(ioc,
    [&]() -> net::awaitable<void>
    {
      try
      {
        co_await client.stream().async_handshake("test", "/", net::use_awaitable);
        client.start();

        auto [ec, result] = co_await client.async_call(
          "add", json::object{{"a", 20}, {"b", 22}}, net::as_tuple(net::use_awaitable));
        BOOST_TEST(!ec);
        BOOST_TEST(result["result"].as_object()["val"].as_int64() == 42);
      }
      catch (const std::exception&)
      {}
      done = true;
      watchdog.cancel();
      beast::get_lowest_layer(server.stream()).close();
      beast::get_lowest_layer(client.stream()).close();
      co_return;
    }, net::detached);

  watchdog.async_wait([&](boost::system::error_code ec)
  {
    if (ec != net::error::operation_aborted && !done)
      BOOST_ERROR("test timed out");
  });

  ioc.run();
}

// 未注册方法返回 -32601 错误
BOOST_AUTO_TEST_CASE(rpc_unregistered_method)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    // server 不绑定任何方法

    auto [ec, result] = co_await client.async_call(
      "no_such_method", json::object{}, net::as_tuple(net::use_awaitable));
    BOOST_TEST(!ec);
    BOOST_TEST(result.if_contains("error"));
    BOOST_TEST(result["error"].as_object()["code"].as_int64() == -32601);
    co_return;
  });
}

// 通知 (无 id 消息)
BOOST_AUTO_TEST_CASE(rpc_notification)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    bool got = false;
    json::object got_params;
    server.notify_callback([&](json::object obj)
    {
      got = true;
      got_params = obj["params"].as_object();
    });

    // client 手动发送无 id 的通知消息
    json::object notif{{"jsonrpc", "2.0"}, {"method", "event"}, {"params", json::object{{"x", 1}}}};
    auto data = json::serialize(notif);
    co_await client.stream().async_write(net::buffer(data), net::use_awaitable);

    co_await wait_until(client.get_executor(), [&]() { return got; });

    BOOST_TEST(got_params["x"].as_int64() == 1);
    co_return;
  });
}

// 回调形式的 async_call (非协程 token)
BOOST_AUTO_TEST_CASE(rpc_callback_style)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    server.bind_method("add", [](json::object obj) -> json::object
    {
      auto p = obj["params"].as_object();
      json::object r{{"val", p["a"].as_int64() + p["b"].as_int64()}};
      return r;
    });

    bool cb_done = false;
    bool cb_ok = false;
    json::object cb_result;

    client.async_call("add", json::object{{"a", 10}, {"b", 5}},
      [&](boost::system::error_code ec, json::object result)
      {
        cb_done = true;
        cb_ok = !ec;
        cb_result = std::move(result);
      });

    co_await wait_until(client.get_executor(), [&]() { return cb_done; });

    BOOST_TEST(cb_ok);
    BOOST_TEST(cb_result["result"].as_object()["val"].as_int64() == 15);
    co_return;
  });
}

// 连续多个 RPC 调用 (连接复用)
BOOST_AUTO_TEST_CASE(rpc_multiple_calls)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    server.bind_method("echo", [](json::object obj) -> json::object
    {
      json::object r{{"got", obj["params"]}};
      return r;
    });

    for (int i = 0; i < 10; ++i)
    {
      auto [ec, result] = co_await client.async_call(
        "echo", json::object{{"n", i}}, net::as_tuple(net::use_awaitable));
      BOOST_TEST(!ec);
      BOOST_TEST(result["result"].as_object()["got"].as_object()["n"].as_int64() == i);
    }
    co_return;
  });
}

// 字符串 id 保留 (手动构造请求验证)
BOOST_AUTO_TEST_CASE(rpc_string_id_preserved)
{
  net::io_context ioc;

  beast::test::stream s1(ioc), s2(ioc);
  s1.connect(s2);

  test_ws server_ws(std::move(s1));
  test_ws client_ws(std::move(s2));

  session_type server(std::move(server_ws));
  session_type client(std::move(client_ws));

  server.error_callback([](std::string_view) {});

  server.bind_method("echo", [](json::object obj) -> json::object
  {
    json::object r{{"got", obj["params"]}};
    return r;
  });

  bool done = false;
  net::steady_timer watchdog(ioc, std::chrono::seconds(15));

  // server: 握手 + start (模式 A)
  net::co_spawn(ioc,
    [&]() -> net::awaitable<void>
    {
      try
      {
        co_await server.stream().async_accept(net::use_awaitable);
        server.start();
      }
      catch (const std::exception&)
      {}
      co_return;
    }, net::detached);

  // client: 握手后手动发送带字符串 id 的请求并手动读取响应
  net::co_spawn(ioc,
    [&]() -> net::awaitable<void>
    {
      try
      {
        co_await client.stream().async_handshake("test", "/", net::use_awaitable);

        json::object req{
          {"jsonrpc", "2.0"},
          {"method", "echo"},
          {"params", json::object{{"v", 7}}},
          {"id", std::string("str-id-001")}
        };
        auto data = json::serialize(req);
        co_await client.stream().async_write(net::buffer(data), net::use_awaitable);

        beast::flat_buffer buf;
        auto n = co_await client.stream().async_read(buf, net::use_awaitable);
        json::value jv = json::parse(beast::buffers_to_string(buf.data()));
        buf.consume(n);

        BOOST_TEST(jv.is_object());
        auto resp = jv.as_object();
        // id 必须保留为字符串, 且与请求一致.
        BOOST_TEST(resp["id"].is_string());
        BOOST_TEST(resp["id"].as_string() == "str-id-001");
        BOOST_TEST(resp["result"].as_object()["got"].as_object()["v"].as_int64() == 7);
      }
      catch (const std::exception&)
      {}
      done = true;
      watchdog.cancel();
      beast::get_lowest_layer(server.stream()).close();
      beast::get_lowest_layer(client.stream()).close();
      co_return;
    }, net::detached);

  watchdog.async_wait([&](boost::system::error_code ec)
  {
    if (ec != net::error::operation_aborted && !done)
      BOOST_ERROR("test timed out");
  });

  ioc.run();
}

// 双向 RPC: server 端主动调用 client 端绑定的方法
BOOST_AUTO_TEST_CASE(rpc_bidirectional)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    client.bind_method("ping", [](json::object) -> json::object
    {
      return json::object{{"pong", true}};
    });

    auto [ec, result] = co_await server.async_call(
      "ping", json::object{}, net::as_tuple(net::use_awaitable));
    BOOST_TEST(!ec);
    BOOST_TEST(result["result"].as_object()["pong"].as_bool() == true);
    co_return;
  });
}

// 并发请求: 同时发起多个 async_call, 验证 id 匹配互不串扰
BOOST_AUTO_TEST_CASE(rpc_concurrent_calls)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    server.bind_method("add", [](json::object obj) -> json::object
    {
      auto p = obj["params"].as_object();
      json::object r{{"val", p["a"].as_int64() + p["b"].as_int64()}};
      return r;
    });

    constexpr int n = 20;
    int pending = n;
    std::vector<boost::system::error_code> ecs(n);
    std::vector<json::object> results(n);

    for (int i = 0; i < n; ++i)
    {
      client.async_call("add", json::object{{"a", i}, {"b", 1}},
        [&, i](boost::system::error_code ec, json::object result)
        {
          ecs[i] = ec;
          results[i] = std::move(result);
          --pending;
        });
    }

    // 等待所有回调完成
    co_await wait_until(client.get_executor(), [&]() { return pending == 0; });

    for (int i = 0; i < n; ++i)
    {
      BOOST_TEST(!ecs[i]);
      // 每个请求的结果必须与自身的参数匹配, 不能串扰.
      BOOST_TEST(results[i]["result"].as_object()["val"].as_int64() == i + 1);
    }
    co_return;
  });
}

// error_callback: 收到无效 JSON 时触发 parse 失败回调
BOOST_AUTO_TEST_CASE(rpc_error_callback_on_invalid_json)
{
  net::io_context ioc;

  beast::test::stream s1(ioc), s2(ioc);
  s1.connect(s2);

  test_ws server_ws(std::move(s1));
  test_ws client_ws(std::move(s2));

  session_type server(std::move(server_ws));
  session_type client(std::move(client_ws));

  server.error_callback([](std::string_view) {});

  bool got_error = false;
  std::string error_msg;
  client.error_callback([&](std::string_view msg)
  {
    got_error = true;
    error_msg = std::string(msg);
  });

  bool done = false;
  net::steady_timer watchdog(ioc, std::chrono::seconds(15));

  net::co_spawn(ioc,
    [&]() -> net::awaitable<void>
    {
      try
      {
        co_await server.stream().async_accept(net::use_awaitable);
        server.start();
      }
      catch (const std::exception&)
      {}
      co_return;
    }, net::detached);

  net::co_spawn(ioc,
    [&]() -> net::awaitable<void>
    {
      try
      {
        co_await client.stream().async_handshake("test", "/", net::use_awaitable);
        client.start();

        // 通过 server 的 stream 向 client 发送无效 JSON
        std::string garbage = "this is not valid json {";
        co_await server.stream().async_write(net::buffer(garbage), net::use_awaitable);

        co_await wait_until(client.get_executor(), [&]() { return got_error; });

        BOOST_TEST(error_msg == "parse json failed");
      }
      catch (const std::exception&)
      {}
      done = true;
      watchdog.cancel();
      beast::get_lowest_layer(server.stream()).close();
      beast::get_lowest_layer(client.stream()).close();
      co_return;
    }, net::detached);

  watchdog.async_wait([&](boost::system::error_code ec)
  {
    if (ec != net::error::operation_aborted && !done)
      BOOST_ERROR("test timed out");
  });

  ioc.run();
}

// params 为数组的 RPC 调用
BOOST_AUTO_TEST_CASE(rpc_array_params)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    server.bind_method("sum", [](json::object obj) -> json::object
    {
      int64_t s = 0;
      for (auto& v : obj["params"].as_array())
        s += v.as_int64();
      json::object r{{"val", s}};
      return r;
    });

    auto [ec, result] = co_await client.async_call(
      "sum", json::array{1, 2, 3, 4}, net::as_tuple(net::use_awaitable));
    BOOST_TEST(!ec);
    BOOST_TEST(result["result"].as_object()["val"].as_int64() == 10);
    co_return;
  });
}

// 大数据量往返
BOOST_AUTO_TEST_CASE(rpc_large_payload)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    server.bind_method("echo", [](json::object obj) -> json::object
    {
      json::object r{{"got", obj["params"]}};
      return r;
    });

    json::object big_params;
    for (int i = 0; i < 10000; ++i)
      big_params["k" + std::to_string(i)] = i;

    auto [ec, result] = co_await client.async_call(
      "echo", big_params, net::as_tuple(net::use_awaitable));
    BOOST_TEST(!ec);
    auto got = result["result"].as_object()["got"].as_object();
    BOOST_TEST(got.size() == 10000);
    BOOST_TEST(got["k9999"].as_int64() == 9999);
    co_return;
  });
}

// 通过门面 notify() 发送无 id 的通知消息
BOOST_AUTO_TEST_CASE(rpc_notify_method)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    bool got = false;
    json::object got_params;
    server.notify_callback([&](json::object obj)
    {
      got = true;
      got_params = obj["params"].as_object();
    });

    // 使用 notify() 发送通知, 无需手工构造消息.
    client.notify("event", json::object{{"x", 1}});

    co_await wait_until(client.get_executor(), [&]() { return got; });

    BOOST_TEST(got_params["x"].as_int64() == 1);
    co_return;
  });
}

// bind_method 返回 json::value (非 object), 例如数组
BOOST_AUTO_TEST_CASE(rpc_return_json_value)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    server.bind_method("list", [](json::object) -> json::value
    {
      return json::array{1, 2, 3};
    });

    auto [ec, result] = co_await client.async_call(
      "list", json::object{}, net::as_tuple(net::use_awaitable));
    BOOST_TEST(!ec);
    auto arr = result["result"].as_array();
    BOOST_TEST(arr.size() == 3);
    BOOST_TEST(arr[0].as_int64() == 1);
    BOOST_TEST(arr[2].as_int64() == 3);
    co_return;
  });
}

// bind_method 协程返回 awaitable<json::value>, 例如字符串
BOOST_AUTO_TEST_CASE(rpc_return_awaitable_json_value)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    server.bind_method("greet", [](json::object obj) -> net::awaitable<json::value>
    {
      auto name = obj["params"].as_object()["name"].as_string();
      co_return json::value("hello " + std::string(name));
    });

    auto [ec, result] = co_await client.async_call(
      "greet", json::object{{"name", "world"}}, net::as_tuple(net::use_awaitable));
    BOOST_TEST(!ec);
    BOOST_TEST(result["result"].as_string() == "hello world");
    co_return;
  });
}

// 手工 reply 接受任意 json::value (非 object), 例如数组
BOOST_AUTO_TEST_CASE(rpc_manual_reply_json_value)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    server.bind_method("arr_reply", [&server](json::object obj) -> net::awaitable<void>
    {
      auto id = jsonrpc::jsonrpc_id(obj);
      // 手工回复 json::value 数组.
      server.reply(json::array{7, 8}, std::move(id));
      co_return;
    });

    auto [ec, result] = co_await client.async_call(
      "arr_reply", json::object{}, net::as_tuple(net::use_awaitable));
    BOOST_TEST(!ec);
    auto arr = result["result"].as_array();
    BOOST_TEST(arr.size() == 2);
    BOOST_TEST(arr[0].as_int64() == 7);
    BOOST_TEST(arr[1].as_int64() == 8);
    co_return;
  });
}

// closed_callback: 调用 stop() 停止会话后触发
BOOST_AUTO_TEST_CASE(rpc_closed_callback_on_stop)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    bool closed = false;
    client.closed_callback([&]() { closed = true; });

    // 先确认连接正常
    server.bind_method("ping", [](json::object) -> json::object
    {
      return json::object{{"pong", true}};
    });
    auto [ec, result] = co_await client.async_call(
      "ping", json::object{}, net::as_tuple(net::use_awaitable));
    BOOST_TEST(!ec);

    // 停止 client 会话, 消息循环结束应触发 closed_callback
    client.stop();

    co_await wait_until(client.get_executor(), [&]() { return closed; });
    BOOST_TEST(closed);
    co_return;
  });
}

// closed_callback: 对端关闭连接后触发
BOOST_AUTO_TEST_CASE(rpc_closed_callback_on_peer_close)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    bool closed = false;
    client.closed_callback([&]() { closed = true; });

    // 对端 (server) 关闭底层连接, client 的消息循环应检测到并结束
    beast::get_lowest_layer(server.stream()).close();

    co_await wait_until(client.get_executor(), [&]() { return closed; });
    BOOST_TEST(closed);
    co_return;
  });
}

// fail_pending_calls: 调用 stop() 时, 挂起的 RPC 调用以 operation_aborted 完成.
BOOST_AUTO_TEST_CASE(fail_pending_calls_on_stop)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    bool request_received = false;
    server.bind_method("never_reply", [&](json::object) -> void
    {
      request_received = true;
    });

    bool call_done = false;
    boost::system::error_code call_ec;
    client.async_call("never_reply", json::object{},
      [&](boost::system::error_code ec, json::object)
      {
        call_done = true;
        call_ec = ec;
      });

    // 等待 server 收到请求, 确认调用确实处于挂起状态.
    co_await wait_until(client.get_executor(), [&]() { return request_received; });
    BOOST_TEST(request_received);
    BOOST_TEST(!call_done);

    // 停止 client 会话, 挂起的调用应以 operation_aborted 完成,
    // 避免等待响应的协程永久挂起导致 io_context 无法退出.
    client.stop();

    co_await wait_until(client.get_executor(), [&]() { return call_done; });
    BOOST_TEST(call_done);
    BOOST_TEST(call_ec == boost::asio::error::operation_aborted);
    co_return;
  });
}

// fail_pending_calls: 对端关闭连接时, 挂起的 RPC 调用以 operation_aborted 完成.
BOOST_AUTO_TEST_CASE(fail_pending_calls_on_peer_close)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    bool request_received = false;
    server.bind_method("never_reply", [&](json::object) -> void
    {
      request_received = true;
    });

    bool call_done = false;
    boost::system::error_code call_ec;
    client.async_call("never_reply", json::object{},
      [&](boost::system::error_code ec, json::object)
      {
        call_done = true;
        call_ec = ec;
      });

    // 等待 server 收到请求, 确认调用确实处于挂起状态.
    co_await wait_until(client.get_executor(), [&]() { return request_received; });
    BOOST_TEST(request_received);
    BOOST_TEST(!call_done);

    // 对端 (server) 直接关闭底层连接, client 的挂起调用应以 operation_aborted 完成.
    beast::get_lowest_layer(server.stream()).close();

    co_await wait_until(client.get_executor(), [&]() { return call_done; });
    BOOST_TEST(call_done);
    BOOST_TEST(call_ec == boost::asio::error::operation_aborted);
    co_return;
  });
}

// fail_pending_calls: 多个挂起调用在 stop() 时全部以 operation_aborted 完成.
BOOST_AUTO_TEST_CASE(fail_all_pending_calls_on_stop)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    constexpr int n = 8;
    int received = 0;
    server.bind_method("never_reply", [&](json::object) -> void
    {
      ++received;
    });

    int call_done = 0;
    int aborted = 0;
    for (int i = 0; i < n; ++i)
    {
      client.async_call("never_reply", json::object{{"n", i}},
        [&](boost::system::error_code ec, json::object)
        {
          ++call_done;
          if (ec == boost::asio::error::operation_aborted)
            ++aborted;
        });
    }

    // 等待所有请求到达对端, 确认所有调用均处于挂起状态.
    co_await wait_until(client.get_executor(), [&]() { return received == n; });
    BOOST_TEST(received == n);
    BOOST_TEST(call_done == 0);

    client.stop();

    co_await wait_until(client.get_executor(), [&]() { return call_done == n; });
    BOOST_TEST(call_done == n);
    BOOST_TEST(aborted == n);
    co_return;
  });
}

// stop() 后, 处于连接状态的 service 应被正常析构.
// 验证: 连接建立并 start() 后调用 stop(), 释放门面后底层流析构
// (即 service 析构), 无协程泄漏导致 service 无法释放.
BOOST_AUTO_TEST_CASE(stop_destroys_connected_service)
{
  net::io_context ioc;

  auto server_destroyed = std::make_shared<bool>(false);
  auto client_destroyed = std::make_shared<bool>(false);

  tracking_stream ts_server(ioc, server_destroyed);
  tracking_stream ts_client(ioc, client_destroyed);
  ts_server.connect(ts_client);

  tracking_ws server_ws(std::move(ts_server));
  tracking_ws client_ws(std::move(ts_client));

  auto server = std::make_shared<tracking_session>(std::move(server_ws));
  auto client = std::make_shared<tracking_session>(std::move(client_ws));

  // 静默错误回调, 避免连接关闭触发断言.
  server->error_callback([](std::string_view) {});
  client->error_callback([](std::string_view) {});

  bool done = false;
  net::steady_timer watchdog(ioc, std::chrono::seconds(15));

  // server 握手 + start
  net::co_spawn(ioc,
    [&]() -> net::awaitable<void>
    {
      try
      {
        co_await server->stream().async_accept(net::use_awaitable);
        server->start();
      }
      catch (const std::exception&)
      {}
      co_return;
    }, net::detached);

  // client 握手 + start + stop + 释放门面
  net::co_spawn(ioc,
    [&]() -> net::awaitable<void>
    {
      try
      {
        co_await client->stream().async_handshake("test", "/", net::use_awaitable);
        client->start();

        // 连接已建立
        BOOST_TEST(server->running());
        BOOST_TEST(client->running());

        // 在连接状态下调用 stop(), 然后释放 client 门面.
        client->stop();
        client.reset();

        // 等待 client 的 service 被析构 (底层流析构即代表 service 析构).
        co_await wait_until(ioc.get_executor(), [&]() { return *client_destroyed; });
        BOOST_TEST(*client_destroyed);

        // 清理 server 端: server 可能已因 client 关闭连接而自然停止,
        // 仅在仍处于运行状态时才需要显式 stop(), 否则会触发 "not running" 断言.
        if (server->running())
          server->stop();
        server.reset();

        co_await wait_until(ioc.get_executor(), [&]() { return *server_destroyed; });
        BOOST_TEST(*server_destroyed);
      }
      catch (const std::exception&)
      {}
      done = true;
      watchdog.cancel();
      co_return;
    }, net::detached);

  watchdog.async_wait([&](boost::system::error_code ec)
  {
    if (ec != net::error::operation_aborted && !done)
      BOOST_ERROR("test timed out: service not destroyed");
  });

  ioc.run();

  BOOST_TEST(*client_destroyed);
  BOOST_TEST(*server_destroyed);
}

// 未调用 start() 的空闲 service, 门面析构后应立即析构.
BOOST_AUTO_TEST_CASE(destroy_idle_service)
{
  net::io_context ioc;
  auto destroyed = std::make_shared<bool>(false);

  tracking_stream ts_a(ioc, destroyed), ts_b(ioc, std::make_shared<bool>(false));
  ts_a.connect(ts_b);

  {
    tracking_ws ws(std::move(ts_a));
    tracking_session session(std::move(ws));
    BOOST_TEST(!*destroyed);
  } // 作用域结束, 门面析构

  BOOST_TEST(*destroyed);
}

// default_method_callback: 方法未注册时由兜底回调处理, 不再回复 -32601.
BOOST_AUTO_TEST_CASE(rpc_default_method_callback)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    bool got = false;
    json::object got_obj;
    server.default_method_callback([&](json::object obj)
    {
      got = true;
      got_obj = std::move(obj);
    });

    // client 手动发送一个未注册方法的请求.
    json::object req{
      {"jsonrpc", "2.0"},
      {"method", "unbound"},
      {"params", json::object{{"x", 5}}},
      {"id", 1}
    };
    co_await client.stream().async_write(net::buffer(json::serialize(req)), net::use_awaitable);

    co_await wait_until(client.get_executor(), [&]() { return got; });
    BOOST_TEST(got_obj["method"].as_string() == "unbound");
    BOOST_TEST(got_obj["id"].as_int64() == 1);
    co_return;
  });
}

// notify(): params 为 null 时省略 params 字段.
BOOST_AUTO_TEST_CASE(rpc_notify_null_params)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    bool got = false;
    json::object got_obj;
    server.notify_callback([&](json::object obj)
    {
      got = true;
      got_obj = std::move(obj);
    });

    // params 传 null, notify() 应省略 params 字段.
    client.notify("event", json::value());

    co_await wait_until(client.get_executor(), [&]() { return got; });
    BOOST_TEST(got_obj["method"].as_string() == "event");
    BOOST_TEST(!got_obj.if_contains("params"));
    co_return;
  });
}

// 手工错误响应: reply(..., error=true) 发送 error 字段而非 result.
BOOST_AUTO_TEST_CASE(rpc_manual_error_reply)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    server.bind_method("fail", [&server](json::object obj) -> void
    {
      auto id = jsonrpc::jsonrpc_id(obj);
      json::object err{{"code", -32000}, {"message", "custom error"}};
      server.reply(err, std::move(id), true);
    });

    auto [ec, result] = co_await client.async_call(
      "fail", json::object{}, net::as_tuple(net::use_awaitable));
    BOOST_TEST(!ec);
    BOOST_TEST(!result.if_contains("result"));
    BOOST_TEST(result.if_contains("error"));
    BOOST_TEST(result["error"].as_object()["code"].as_int64() == -32000);
    BOOST_TEST(result["error"].as_object()["message"].as_string() == "custom error");
    co_return;
  });
}

// stop() 后发起 async_call, 应立即以 operation_aborted 完成, 不会挂起.
BOOST_AUTO_TEST_CASE(rpc_async_call_after_stop)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    server.bind_method("ping", [](json::object) -> json::object
    {
      return json::object{{"pong", true}};
    });

    // 先确认连接正常.
    auto [ec0, r0] = co_await client.async_call(
      "ping", json::object{}, net::as_tuple(net::use_awaitable));
    BOOST_TEST(!ec0);

    // 停止会话后, 新发起的调用应立即失败.
    client.stop();

    bool done = false;
    boost::system::error_code ec;
    client.async_call("ping", json::object{},
      [&](boost::system::error_code e, json::object)
      {
        done = true;
        ec = e;
      });

    co_await wait_until(client.get_executor(), [&]() { return done; });
    BOOST_TEST(ec == boost::asio::error::operation_aborted);
    co_return;
  });
}

// error_callback: 收到非对象 JSON (数组) 时触发 "parsed json is not an object".
BOOST_AUTO_TEST_CASE(rpc_error_not_object)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    // 通过 shared_ptr 共享状态: 会话关闭时错误回调仍可能被触发,
    // 捕获共享状态可避免对已析构局部变量的悬垂引用.
    auto st = std::make_shared<std::pair<bool, std::string>>(false, std::string());
    client.error_callback([st](std::string_view m)
    {
      st->first = true;
      st->second = std::string(m);
    });

    // 注意: 需使用 std::string 作为写入载荷, net::buffer("字面量") 会
    // 连同结尾的 '\0' 一起发送, 导致对端解析失败.
    std::string payload = "[1, 2, 3]";
    co_await server.stream().async_write(net::buffer(payload), net::use_awaitable);

    co_await wait_until(client.get_executor(), [&]() { return st->first; });
    BOOST_TEST(st->second == "parsed json is not an object");
    co_return;
  });
}

// error_callback: 收到无 jsonrpc 字段的对象时触发 "jsonrpc field not found".
BOOST_AUTO_TEST_CASE(rpc_error_no_jsonrpc)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    auto st = std::make_shared<std::pair<bool, std::string>>(false, std::string());
    client.error_callback([st](std::string_view m)
    {
      st->first = true;
      st->second = std::string(m);
    });

    std::string payload = "{\"foo\": 1}";
    co_await server.stream().async_write(net::buffer(payload), net::use_awaitable);

    co_await wait_until(client.get_executor(), [&]() { return st->first; });
    BOOST_TEST(st->second == "jsonrpc field not found");
    co_return;
  });
}

// error_callback: 请求 method 字段非字符串时触发 "method must be string".
BOOST_AUTO_TEST_CASE(rpc_error_method_not_string)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    auto st = std::make_shared<std::pair<bool, std::string>>(false, std::string());
    client.error_callback([st](std::string_view m)
    {
      st->first = true;
      st->second = std::string(m);
    });

    std::string payload = "{\"jsonrpc\":\"2.0\",\"method\":42,\"id\":1}";
    co_await server.stream().async_write(net::buffer(payload), net::use_awaitable);

    co_await wait_until(client.get_executor(), [&]() { return st->first; });
    BOOST_TEST(st->second == "method must be string");
    co_return;
  });
}

// error_callback: 响应 id 为非数字字符串时触发 "invalid id format".
BOOST_AUTO_TEST_CASE(rpc_error_invalid_id_format)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    auto st = std::make_shared<std::pair<bool, std::string>>(false, std::string());
    client.error_callback([st](std::string_view m)
    {
      st->first = true;
      st->second = std::string(m);
    });

    // 发起一个挂起调用, 使 call_ops_ 非空.
    bool request_received = false;
    server.bind_method("never_reply", [&](json::object) -> void
    {
      request_received = true;
    });
    client.async_call("never_reply", json::object{},
      [](boost::system::error_code, json::object) {});
    co_await wait_until(client.get_executor(), [&]() { return request_received; });

    // 注入 id 为不可转数字字符串的响应.
    json::object resp{{"jsonrpc", "2.0"}, {"result", json::object{}}, {"id", std::string("abc")}};
    std::string payload = json::serialize(resp);
    co_await server.stream().async_write(net::buffer(payload), net::use_awaitable);

    co_await wait_until(client.get_executor(), [&]() { return st->first; });
    BOOST_TEST(st->second == "invalid id format");
    co_return;
  });
}

// error_callback: 响应 id 越界时触发 "invalid session id".
BOOST_AUTO_TEST_CASE(rpc_error_invalid_session_id)
{
  run_with_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    auto st = std::make_shared<std::pair<bool, std::string>>(false, std::string());
    client.error_callback([st](std::string_view m)
    {
      st->first = true;
      st->second = std::string(m);
    });

    bool request_received = false;
    server.bind_method("never_reply", [&](json::object) -> void
    {
      request_received = true;
    });
    client.async_call("never_reply", json::object{},
      [](boost::system::error_code, json::object) {});
    co_await wait_until(client.get_executor(), [&]() { return request_received; });

    // 注入 id 越界的响应.
    json::object resp{{"jsonrpc", "2.0"}, {"result", json::object{}}, {"id", 999}};
    std::string payload = json::serialize(resp);
    co_await server.stream().async_write(net::buffer(payload), net::use_awaitable);

    co_await wait_until(client.get_executor(), [&]() { return st->first; });
    BOOST_TEST(st->second == "invalid session id");
    co_return;
  });
}

// data_callback: 解析前转换原始数据 (去掉前缀), 转换后再按 JSON 解析.
BOOST_AUTO_TEST_CASE(rpc_data_callback)
{
  run_manual_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    server.data_callback([](std::string_view data) -> std::string
    {
      std::string s(data);
      return (s.rfind("PREFIX", 0) == 0) ? s.substr(6) : s;
    });
    server.bind_method("echo", [](json::object obj) -> json::object
    {
      return json::object{{"got", obj["params"]}};
    });

    // client 手动发送带前缀的请求, 由 server 的 data_callback 去前缀后解析.
    json::object req{
      {"jsonrpc", "2.0"},
      {"method", "echo"},
      {"params", json::object{{"v", 9}}},
      {"id", 1}
    };
    std::string framed = "PREFIX" + json::serialize(req);
    co_await client.stream().async_write(net::buffer(framed), net::use_awaitable);

    beast::flat_buffer buf;
    auto n = co_await client.stream().async_read(buf, net::use_awaitable);
    json::value jv = json::parse(beast::buffers_to_string(buf.data()));
    buf.consume(n);

    BOOST_TEST(jv.is_object());
    auto resp = jv.as_object();
    BOOST_TEST(resp["id"].as_int64() == 1);
    BOOST_TEST(resp["result"].as_object()["got"].as_object()["v"].as_int64() == 9);
    co_return;
  });
}

// 未注册方法 + 字符串 id: 错误响应中 id 保留为字符串.
BOOST_AUTO_TEST_CASE(rpc_unregistered_string_id_error)
{
  run_manual_pair([](session_type& server, session_type& client) -> net::awaitable<void>
  {
    // server 不绑定任何方法.

    json::object req{
      {"jsonrpc", "2.0"},
      {"method", "missing"},
      {"params", json::object{}},
      {"id", std::string("abc-id")}
    };
    co_await client.stream().async_write(net::buffer(json::serialize(req)), net::use_awaitable);

    beast::flat_buffer buf;
    auto n = co_await client.stream().async_read(buf, net::use_awaitable);
    json::value jv = json::parse(beast::buffers_to_string(buf.data()));
    buf.consume(n);

    BOOST_TEST(jv.is_object());
    auto resp = jv.as_object();
    BOOST_TEST(resp["id"].is_string());
    BOOST_TEST(resp["id"].as_string() == "abc-id");
    BOOST_TEST(resp["error"].as_object()["code"].as_int64() == -32601);
    BOOST_TEST(resp["error"].as_object()["message"].as_string() == "Method not found");
    co_return;
  });
}

// release(): 释放底层流后, 会话不再拥有该流, 释放出的流仍可直接使用.
BOOST_AUTO_TEST_CASE(rpc_release_stream)
{
  net::io_context ioc;

  beast::test::stream s1(ioc), s2(ioc);
  s1.connect(s2);

  test_ws server_ws(std::move(s1));
  test_ws client_ws(std::move(s2));

  session_type server(std::move(server_ws));
  session_type client(std::move(client_ws));

  server.error_callback([](std::string_view) {});
  client.error_callback([](std::string_view) {});

  bool got = false;
  server.notify_callback([&](json::object) { got = true; });

  bool done = false;
  net::steady_timer watchdog(ioc, std::chrono::seconds(15));

  // server 握手 + start
  net::co_spawn(ioc,
    [&]() -> net::awaitable<void>
    {
      try
      {
        co_await server.stream().async_accept(net::use_awaitable);
        server.start();
      }
      catch (const std::exception&)
      {}
      co_return;
    }, net::detached);

  // client 握手后释放底层流, 再用释放出的流发送通知.
  net::co_spawn(ioc,
    [&]() -> net::awaitable<void>
    {
      try
      {
        co_await client.stream().async_handshake("test", "/", net::use_awaitable);

        // 释放底层流, 会话不再拥有该流.
        test_ws released = client.release();

        json::object notif{{"jsonrpc", "2.0"}, {"method", "after_release"}, {"params", json::object{}}};
        co_await released.async_write(net::buffer(json::serialize(notif)), net::use_awaitable);

        co_await wait_until(ioc.get_executor(), [&]() { return got; });
        BOOST_TEST(got);
      }
      catch (const std::exception&)
      {}
      done = true;
      watchdog.cancel();
      beast::get_lowest_layer(server.stream()).close();
      co_return;
    }, net::detached);

  watchdog.async_wait([&](boost::system::error_code ec)
  {
    if (ec != net::error::operation_aborted && !done)
      BOOST_ERROR("test timed out");
  });

  ioc.run();
}

// 移动构造与移动赋值: 转移 impl_ 所有权后仍可正常发起 RPC.
BOOST_AUTO_TEST_CASE(rpc_move_session)
{
  net::io_context ioc;

  beast::test::stream s1(ioc), s2(ioc);
  s1.connect(s2);

  test_ws server_ws(std::move(s1));
  test_ws client_ws(std::move(s2));

  session_type server(std::move(server_ws));
  session_type client_src(std::move(client_ws));
  server.error_callback([](std::string_view) {});
  client_src.error_callback([](std::string_view) {});

  // 移动构造: 转移 client_src 的会话服务.
  session_type client(std::move(client_src));

  // 移动赋值: 覆盖一个已存在的空闲会话.
  beast::test::stream s3(ioc), s4(ioc);
  s3.connect(s4);
  session_type client_dst(std::move(test_ws(std::move(s3))));
  client_dst = std::move(client);

  server.bind_method("add", [](json::object obj) -> json::object
  {
    auto p = obj["params"].as_object();
    return json::object{{"val", p["a"].as_int64() + p["b"].as_int64()}};
  });

  bool done = false;
  net::steady_timer watchdog(ioc, std::chrono::seconds(15));

  // server 握手 + start
  net::co_spawn(ioc,
    [&]() -> net::awaitable<void>
    {
      try
      {
        co_await server.stream().async_accept(net::use_awaitable);
        server.start();
      }
      catch (const std::exception&)
      {}
      co_return;
    }, net::detached);

  // 移动后的会话握手 + start + RPC
  net::co_spawn(ioc,
    [&]() -> net::awaitable<void>
    {
      try
      {
        co_await client_dst.stream().async_handshake("test", "/", net::use_awaitable);
        client_dst.start();

        auto [ec, result] = co_await client_dst.async_call(
          "add", json::object{{"a", 40}, {"b", 2}}, net::as_tuple(net::use_awaitable));
        BOOST_TEST(!ec);
        BOOST_TEST(result["result"].as_object()["val"].as_int64() == 42);
      }
      catch (const std::exception&)
      {}
      done = true;
      watchdog.cancel();
      beast::get_lowest_layer(server.stream()).close();
      beast::get_lowest_layer(client_dst.stream()).close();
      co_return;
    }, net::detached);

  watchdog.async_wait([&](boost::system::error_code ec)
  {
    if (ec != net::error::operation_aborted && !done)
      BOOST_ERROR("test timed out");
  });

  ioc.run();
}

BOOST_AUTO_TEST_SUITE_END()
