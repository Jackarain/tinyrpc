//
// jsonrpc.hpp
// ~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack dot wgm at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef INCLUDE__2023_10_18__JSONRPC_HPP
#define INCLUDE__2023_10_18__JSONRPC_HPP

#include <atomic>
#include <functional>
#include <memory>
#include <utility>
#include <mutex>
#include <vector>
#include <deque>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>

#include <boost/system/error_code.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/detached.hpp>

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/stream.hpp>

#include <boost/json/value.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

namespace jsonrpc
{
  namespace beast = boost::beast;
  namespace net = boost::asio;
  namespace json = boost::json;

  namespace detail
  {
    // 这里可以放一些私有的辅助函数或类型定义
    class rpc_operation
    {
    public:
      virtual ~rpc_operation() = default;
      virtual void operator()(const boost::system::error_code &) = 0;
      virtual json::object &result() = 0;
    };

    template <class Handler, class ExecutorType>
    class rpc_call_op : public rpc_operation
    {
    public:
      rpc_call_op(Handler &&h, ExecutorType executor)
        : handler_(std::forward<Handler>(h))
        , executor_(executor)
      {
      }

      rpc_call_op(const rpc_call_op &other)
        : handler_(std::forward<Handler>(other.handler_))
        , executor_(other.executor_)
        , data_(other.data_)
      {
      }

      rpc_call_op(rpc_call_op &&other) noexcept
        : handler_(std::forward<Handler>(other.handler_))
        , executor_(other.executor_)
        , data_(other.data_)
      {
      }

      void operator()(const boost::system::error_code &ec) override
      {
        // 使用 net::dispatch 将结果发送到指定的执行器上
        // 这样可以确保在正确的线程或上下文中调用处理程序
        net::dispatch(
          executor_,
          [handler = std::move(handler_), data = std::move(data_), ec]() mutable
          {
            handler(ec, data);
          });
      }

      json::object &result() override
      {
        return data_;
      }

    private:
      Handler handler_;
      ExecutorType executor_;
      json::object data_;
    };

    using call_op_ptr = std::unique_ptr<rpc_operation>;


    std::string jsonrpc_id(const json::object &obj)
    {
      if (obj.if_contains("id"))
      {
        const auto &id = obj.at("id");
        if (id.is_string())
          return std::string(id.as_string());
        else if (id.is_int64())
          return std::to_string(id.as_int64());
      }
      return {};
    }
  }

  using detail::jsonrpc_id;

  template <class StreamType>
  class jsonrpc_session
  {
    // c++11 noncopyable.
    jsonrpc_session(const jsonrpc_session &) = delete;
    jsonrpc_session &operator=(const jsonrpc_session &) = delete;

  public:
    using stream_type = StreamType;
      using next_layer_type = std::remove_reference_t<stream_type>;
      using executor_type = next_layer_type::executor_type;

    using call_op_ptr = detail::call_op_ptr;

    using write_context = std::unique_ptr<std::string>;
    using write_message_queue = std::deque<write_context>;

    friend class initiate_async_call;

    //////////////////////////////////////////////////////////////////////////

    // 构造函数, 接受一个 WebSocket 对象.
    jsonrpc_session(stream_type ws)
      : stream_(std::move(ws))
    {
    }

    jsonrpc_session(jsonrpc_session &&rhs) noexcept
      : stream_(std::move(rhs.stream_))
      , call_ops_(std::move(rhs.call_ops_))
      , id_recycle_(std::move(rhs.id_recycle_))
      , method_cb_(std::move(rhs.method_cb_))
      , error_cb_(std::move(rhs.error_cb_))
      , notify_cb_(std::move(rhs.notify_cb_))
      , remote_methods_(std::move(rhs.remote_methods_))
      , write_msgs_(std::move(rhs.write_msgs_))
    {
      if (rhs.running_)
      {
        BOOST_ASSERT(false && "cannot move a running session");
        running_ = true;
      }
      rhs.running_ = false;
    }

    jsonrpc_session& operator=(jsonrpc_session&& rhs) noexcept = delete;

    ~jsonrpc_session() noexcept
    {
      // 确保在析构时停止服务.
      if (running_)
        stop();
    }

    //////////////////////////////////////////////////////////////////////////

    // 启动服务, 开始接收 WebSocket 消息, 如果服务已经在运行, 则什么都不做.
    // 注意: 调用此函数相当于从 stream 中接收 JSON 数据并调用 dispatch()
    // 函数来派发 JSONRPC 协议消息.
    // 亦可手工调用 dispatch() 来处理 JSONRPC 消息, 但请注意这种情况下，我
    // 们不可以调用 start() 来驱动服务, 否则会导致逻辑错误.
    void start()
    {
      if (running_)
      {
        BOOST_ASSERT(false && "already running");
        return;
      }

      running_ = true;

      net::co_spawn(stream_.get_executor(), [this]() mutable -> net::awaitable<void>
      {
        co_await run();
        running_ = false;
        co_return;
      }, net::detached);
    }

    // 停止服务, 关闭 WebSocket 连接, 如果服务没有运行, 则什么都不做.
    // 注意: 调用此函数后, 不能再调用 start() 启动服务, 如果需要重新
    // 启动服务, 请创建一个新的 jsonrpc_session 实例并调用 start()
    // 方法.
    void stop()
    {
      if (!running_)
      {
        BOOST_ASSERT(false && "not running");
        return;
      }

      boost::system::error_code ec;
      if (stream_.is_open())
        stream_.close(beast::websocket::close_code::normal, ec);
    }

    // 手工调度一个 JSONRPC 协议, 这个函数可以用于在不运行 start 的前提下
    // 手工调度协议, obj 对象必须符合 JSONRPC 协议的要求.
    // 比如: 在用户程序中接收到一个 JSONRPC 请求消息, 可以直接调用这个函数
    // 将该消息传递给会话进行处理.
    void dispatch(json::object obj)
    {
      if (running_)
      {
        BOOST_ASSERT(false && "session is running");
        return;
      }
      if (!obj.if_contains("jsonrpc"))
      {
        BOOST_ASSERT(false && "jsonrpc field not found");
        return;
      }

      net::co_spawn(stream_.get_executor(),
        [this, obj = std::move(obj)]() mutable -> net::awaitable<void>
        {
          running_ = true;
          co_await dispath(std::move(obj));
          running_ = false;
          co_return;
        }, net::detached);
    }

    // 获取 stream 流对象, 该对象可以用于直接进行 stream 操作.
    StreamType& stream() noexcept
    {
      return stream_;
    }

    //////////////////////////////////////////////////////////////////////////

    class initiate_async_call
    {
    public:
      using executor_type = jsonrpc_session::executor_type;

      explicit initiate_async_call(jsonrpc_session* self)
        : self_(self)
      {
      }

      executor_type get_executor() const noexcept
      {
        return self_->get_executor();
      }

      template <typename CallHandler>
      void operator()(CallHandler&& handler,
        const std::string& method, const json::value& params) const
      {
        auto executor = net::get_associated_executor(handler);
        using handler_executor_type = std::decay_t<decltype(executor)>;
        using rpc_call_op_type = detail::rpc_call_op<CallHandler, handler_executor_type>;

        auto op = std::make_unique<rpc_call_op_type>(
          std::forward<CallHandler>(handler), executor);

        json::object data;

        data["jsonrpc"] = "2.0";
        data["method"] = method;
        data["params"] = params;

        if (self_->id_recycle_.empty())
        {
          auto session_id = static_cast<int>(self_->call_ops_.size());
          data["id"] = session_id;
          self_->call_ops_.emplace_back(std::move(op));
        }
        else
        {
          auto session_id = self_->id_recycle_.back();
          self_->id_recycle_.pop_back();

          data["id"] = session_id;
          self_->call_ops_[session_id] = std::move(op);
        }

        // 发送 JSON 请求数据
        auto context = std::make_unique<std::string>(json::serialize(data));
        self_->write_message(std::move(context));
      }

    private:
      jsonrpc_session* self_;
    };

    // 异步发送 JSONRPC 请求, 返回一个 JSON 对象作为响应.
    // 参数 params 代表要发送的请求数据的 JSONRPC 的 params 字段,
    // 该参数可以是一个 JSON 对象或数组, 须满足 JSONRPC 规范的要求.
    // method 参数代表要调用的远程方法名.
    template<BOOST_ASIO_COMPLETION_TOKEN_FOR(
      void(boost::system::error_code, json::value))
        CallToken = net::default_completion_token_t<executor_type>>
    auto async_call(const std::string& method, const json::value& params,
      CallToken&& token = net::default_completion_token_t<executor_type>()) ->
      decltype(
        net::async_initiate<CallToken,
        void(boost::system::error_code, json::value)>(
          std::declval<initiate_async_call>(), token, method, params))
    {
      return net::async_initiate<CallToken,
        void(boost::system::error_code, json::value)>(
          initiate_async_call(this), token, method, params);
    }

    // 回复 JSONRPC 请求, 该函数接受一个 JSON 对象作为参数代表响应数据,
    // 以及一个字符串 id 代表请求的 ID. 如果 error 参数为 true, 则表示
    // 这是一个错误 error 响应, 否则表示正常 result 响应.
    void reply(json::object response, const std::string& id, bool error = false)
    {
      json::object data;

      data["jsonrpc"] = "2.0";
      data["id"] = id;
      if (error)
        data["error"] = response;
      else
        data["result"] = response;

      // 将响应数据序列化为 JSON 字符串并发送
      auto context = std::make_unique<std::string>(json::serialize(data));
      write_message(std::move(context));
    }

    // 当接收到对应的 JSON-RPC 方法调用时会调用该函数, 方法名是一个
    // 字符串, 代表远程方法的名称, handler 是一个函数对象,
    // 接受一个 json::object 作为参数, 代表接收到的请求消息.
    void bind_method(
      std::string_view method_name,
      std::function<void(json::object)> handler)
    {
      if (method_name.empty() || !handler)
      {
        BOOST_ASSERT(false && "method name or handler is invalid");
        return;
      }

      remote_methods_[std::string(method_name)] = std::move(handler);
    }

    // 设置请求回调函数, 当接收到请求消息时会调用该函数.
    // 请求消息在 JSONRPC 中是指包含 id 字段的 json 对象.
    // 回调函数的参数是一个 json::object, 代表接收到的请求消息.
    // 如果传入的回调函数为空, 则清除之前设置的回调函数.
    void default_method_callback(std::function<void(json::object)> cb)
    {
      method_cb_ = cb;
    }

    // 清除请求回调函数, 之后接收到请求消息时不会调用任何函数.
    void default_method_callback()
    {
      method_cb_ = {};
    }

    // 设置通知回调函数, 当接收到通知消息时会调用该函数.
    // 通知消息在 JSONRPC 中是指没有 id 字段的 json 对象.
    // 如果传入的回调函数为空, 则清除之前设置的回调函数.
    // 回调函数的参数是一个 json::object, 代表接收到的通知消息.
    void notify_callback(std::function<void(json::object)> cb)
    {
      notify_cb_ = cb;
    }

    // 清除通知回调函数, 之后接收到通知消息时不会调用任何函数.
    void notify_callback()
    {
      notify_cb_ = {};
    }

    // 设置错误回调函数, 当接收到无法执行 JSON 解析的错误消息时会调用该函数.
    // 回调函数的参数是接收到的消息数据.
    void error_callback(std::function<void(std::string_view)> cb)
    {
      error_cb_ = cb;
    }

    // 清除错误回调函数, 之后接收到错误消息时不会调用任何函数.
    void error_callback()
    {
      error_cb_ = {};
    }

    // 获取当前 jsonrpc_session 的执行器, 该执行器可以用于在协程中调度任务.
    net::any_io_executor get_executor() noexcept
    {
      return stream_.get_executor();
    }

    //////////////////////////////////////////////////////////////////////////

  private:
    // 运行服务的协程, 负责接收 WebSocket 消息并解析为 JSON 对象, 然
    // 后通过创建一个新的协程来处理接收到的 JSON 对象.
    net::awaitable<void> run()
    {
      try
      {
        boost::system::error_code ec;
        beast::flat_buffer buf;
        auto executor = co_await net::this_coro::executor;

        while (running_)
        {
          auto bytes = co_await stream_.async_read(buf, net::use_awaitable);

          auto bufdata = buf.data();
          std::string_view sv((const char*)bufdata.data(), bufdata.size());

          json::value jv = json::parse(
            sv,
            ec,
            json::storage_ptr{},
            {64, json::number_precision::imprecise, true, true, true});
          if (ec)
          {
            // 解析失败, 可能是因为接收到的消息不是有效的 JSON, 忽略该消息
            // 并继续等待下一个消息.
            if (error_cb_)
              error_cb_("parse json failed");
            else
              BOOST_ASSERT(false && "parse json failed");

            buf.consume(bytes);

            continue;
          }

          buf.consume(bytes);
          if (!jv.if_object())
          {
            // 解析结果不是一个 JSON 对象, 忽略该消息并继续等待下一个消息.
            if (error_cb_)
              error_cb_("parsed json is not an object");
            else
              BOOST_ASSERT(false && "parsed json is not an object");
            continue;
          }
          auto obj = jv.as_object();
          if (!obj.if_contains("jsonrpc"))
          {
            // 解析结果不是一个 JSONRPC 协议.
            if (error_cb_)
              error_cb_("jsonrpc field not found");
            else
              BOOST_ASSERT(false && "jsonrpc field not found");
            continue;
          }

          net::co_spawn(executor, [this, obj = std::move(obj)]() mutable -> net::awaitable<void>
          {
            co_await dispath(std::move(obj));
            co_return;
          }, net::detached);
        }
      }
      catch (const std::exception &e)
      {
        std::string error_msg = e.what();
        // 捕获异常并调用错误回调函数
        if (error_cb_)
          error_cb_("exception occurred while running jsonrpc session");
        else
          BOOST_ASSERT(false && "exception occurred while running jsonrpc session");
      }
    }

    net::awaitable<void> dispath(json::object obj)
    {
      auto try_id = obj.try_at("id");
      if (!try_id.has_value())
      {
        // 这是一个通知消息，回调通知处理函数
        if (notify_cb_)
          notify_cb_(std::move(obj));
        co_return;
      }

      // 这是一个请求或响应消息，检查 id 字段
      auto id = *try_id;
      if (!id.is_string() && !id.is_number())
      {
        // id 字段不是字符串或数字，忽略该消息
        BOOST_ASSERT(false && "id must be string or number");
        co_return;
      }

      if (obj.if_contains("result") || obj.if_contains("error"))
      {
        // 包含 result 或 error 的 json 对象说明当前是作为调用者身份
        // 向远端发起 RPC 请求的回应.
        if (call_ops_.empty())
        {
          // 如果没有正在进行的调用操作，忽略该消息
          BOOST_ASSERT(false && "no call operation in progress");
          co_return;
        }

        int session_id = -1;

        if (id.is_string())
        {
          // 尝试将字符串 id 转换为数字
          try
          {
            session_id = std::stoi(std::string(id.as_string()));
          }
          catch (const std::exception &)
          {
            // 转换失败，忽略该消息
            if (error_cb_)
              error_cb_("invalid id format");
            else
              BOOST_ASSERT(false && "invalid id format");
            co_return;
          }
        }

        co_await handle_call(std::move(obj), session_id);

        co_return;
      }
      else if (obj.if_contains("method"))
      {
        if (!obj["method"].is_string())
        {
          // method 字段不是字符串，忽略该消息
          if (error_cb_)
            error_cb_("method must be string");
          else
            BOOST_ASSERT(false && "method must be string");
          co_return;
        }

        // 包含 method 字段的 json 对象说明当前是作为服务端身份
        std::string method (obj["method"].as_string());
        co_await handle_method(std::move(obj), method);

        // 处理方法调用消息
        co_return;
      }
      else
      {
        // 既不是请求也不是响应，忽略该消息
        BOOST_ASSERT(false && "not a request or response");
      }

      co_return;
    }

    net::awaitable<void> handle_call(json::object obj, int session_id)
    {
      // 查找是否有对应的调用操作
      std::lock_guard<std::mutex> lock(call_op_mutex_);
      if (session_id < 0 || session_id >= static_cast<int>(call_ops_.size()))
      {
        // id 不在有效范围内，忽略该消息
        if (error_cb_)
          error_cb_("invalid session id");
        else
          BOOST_ASSERT(false && "invalid session id");
        co_return;
      }

      // 获取对应的调用操作
      call_op_ptr handler = std::move(call_ops_[session_id]);

      // 回收 RPC 调用操作的 id
      id_recycle_.push_back(session_id);

      BOOST_ASSERT(handler && "call op is nullptr!"); // for debug, call_ops_[session_id].reset(); // 清除对应的调用操作
      if (handler)
      {
        // 调用操作存在，执行它
        handler->result() = std::move(obj);
        (*handler)(boost::system::error_code{});
      }
      else
      {
        // 没有找到对应的调用操作，忽略该消息
        if (error_cb_)
          error_cb_("no call operation found for id");
        else
          BOOST_ASSERT(false && "no call operation found for id");
      }

      co_return;
    }

    net::awaitable<void> handle_method(json::object obj, std::string_view method_name)
    {
      // 检查是否有对应的远程方法
      auto it = remote_methods_.find(std::string(method_name));
      if (it != remote_methods_.end())
      {
        // 找到对应的远程方法，调用它
        it->second(std::move(obj));
      }
      else if (method_cb_)
      {
        // 如果没有找到对应的远程方法，调用默认的 method 回调函数
        method_cb_(std::move(obj));
      }
      else
      {
        // 没有设置 method 回调函数，忽略该消息
        BOOST_ASSERT(false && "no method callback set");
      }

      co_return;
    }

    // 异步写入消息到 WebSocket, 该函数接受一个 write_context, 该上下文包含要写入的消息数据.
    // 如果当前没有正在进行的写入操作, 则直接发送消息, 否则将消息添加到写入队列中.
    // 注意: 该函数是线程安全的, 可以在任何线程中调用.
    void write_message(write_context context)
    {
      if (!context || context->empty())
      {
        BOOST_ASSERT(false && "context is empty");
        return;
      }

      net::dispatch(stream_.get_executor(),
        [this, context = std::move(context)]() mutable
        {
          bool write_in_progress = !write_msgs_.empty();
          write_msgs_.emplace_back(std::move(context));

          if (write_in_progress)
            return;

          // 直接调用协程来处理写入消息
          net::co_spawn(stream_.get_executor(),
            [this]() mutable -> net::awaitable<void>
            {
              co_await write_messages();
              co_return;
            }, net::detached);
        });
    }

    // 处理 WebSocket 写入消息的协程
    net::awaitable<void> write_messages()
    {
      try
      {
        while (!write_msgs_.empty())
        {
          auto msg = std::move(write_msgs_.front());
          write_msgs_.pop_front();

          // 发送消息
          co_await stream_.async_write(net::buffer(*msg), net::use_awaitable);
        }
      }
      catch(const std::exception& e)
      {
        error_cb_(std::string_view(e.what()));
      }
    }

  private:
    // Stream 对象, 用于与远程服务进行通信.
    stream_type stream_;
    std::atomic<bool> running_{false};

    // 回调函数, 用于处理请求、通知消息和错误消息
    std::function<void(json::object)> method_cb_;
    std::function<void(json::object)> notify_cb_;
    std::function<void(std::string_view)> error_cb_;

    std::unordered_map<std::string,
      std::function<void(json::object)>> remote_methods_;

    std::mutex call_op_mutex_;
    std::vector<int> id_recycle_;
    std::vector<call_op_ptr> call_ops_;

    write_message_queue write_msgs_;
  };

  using ws_jsonrpc_session = jsonrpc_session<beast::websocket::stream<beast::tcp_stream>>;
}

#endif // INCLUDE__2023_10_18__JSONRPC_HPP
