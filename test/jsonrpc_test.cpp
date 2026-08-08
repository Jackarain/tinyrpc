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
//   - 未注册方法的 -32601 错误响应
//   - 通知 (notification)
//   - 回调形式的 async_call
//   - 字符串 id 保留
//   - 双向 RPC
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

BOOST_AUTO_TEST_SUITE_END()
