//
// Copyright (C) 2019 Jack.
//
// Author: jack
// Email:  jack.wgm at gmail dot com
//

#pragma once

#include <atomic>
#include <memory>
#include <vector>
#include <functional>
#include <unordered_map>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <type_traits>

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include "jsonrpc_error_code.hpp"
#include "jsoncpp/json.h"

namespace jsonrpc {
namespace detail {
	template<typename T>
	void json_append(Json::Value& value, T t)
	{
		value.append(t);
	}

	template<typename T, class ... Args>
	void json_append(Json::Value& value, T first, Args ... params)
	{
		value.append(first);
		json_append(value, params...);
	}
}

// 转换json对象为字符串.
inline std::string make_json_string(const Json::Value& value)
{
	Json::StreamWriterBuilder writer;
	return Json::writeString(writer, value);
}

inline std::string make_json_string_oneline(const Json::Value& value)
{
	Json::StreamWriterBuilder writer;
	writer.settings_["indentation"] = "";
	return Json::writeString(writer, value);
}

inline std::string make_json_string_online_withend(const Json::Value& value)
{
	Json::StreamWriterBuilder writer;
	writer.settings_["indentation"] = "";
	return Json::writeString(writer, value) + "\n";
}

inline Json::Value string_to_json(const std::string& str, boost::system::error_code& ec)
{
	Json::Value root;
	Json::CharReaderBuilder rbuilder;
	rbuilder["collectComments"] = false;
	std::string errs;
	std::istringstream iss(str);

	if (!Json::parseFromStream(rbuilder, iss, &root, &errs))
	{
		ec = jsonrpc::errc::parse_json_failed;
		return root;
	}

	ec = boost::system::error_code{};
	return root;
}

// 构建json rpc请求, 参数1为请求方法, 按params构造json数组作为请求params.
template <class ... Args>
Json::Value make_rpc_request_json(const std::string& method, Args ... params)
{
	Json::Value value;
	Json::Value params_array;

	detail::json_append(params_array, params...);

	value["jsonrpc"] = "2.0";
	value["method"] = method;
	value["params"] = params_array;
	value["id"] = rand();

	return value;
}

// 指定params为Json Obejct而非Json Array的情况下, 自动使用该函数重载.
inline Json::Value make_rpc_request_json(const std::string& method, const Json::Value& params)
{
	Json::Value value;

	value["jsonrpc"] = "2.0";
	value["method"] = method;
	value["params"] = params;
	value["id"] = rand();

	return value;
}


namespace detail {

#if defined(JSONRPC_DISABLE_THREADS)
	template<class Lock>
	struct unique_lock {
		explicit unique_lock(const Lock&) {}
		inline void lock() {}
		inline void unlock() {}
	};
	template<class Lock>
	struct lock_guard {
		explicit lock_guard(const Lock&) {}
	};
	template<class Lock>
	struct shared_lock {
		explicit shared_lock(const Lock&) {}
		inline void lock() {}
		inline void unlock() {}
	};
#else
	template<class Mutex>
	using unique_lock = std::unique_lock<Mutex>;
	template<class Mutex>
	using lock_guard = std::lock_guard<Mutex>;
	template<class Mutex>
	using shared_lock = std::shared_lock<Mutex>;
#endif

	//////////////////////////////////////////////////////////////////////////
	template<class R, class C, class ...A>
	auto is_invocable_test(C&& c, int, A&& ...a)
		-> decltype(std::is_convertible<decltype(c(std::forward<A>(a)...)), R>::value
			|| std::is_same<R, void>::value, std::true_type());

	template<class R, class C, class ...A>
	std::false_type
		is_invocable_test(C&& c, long, A&& ...a);

	template<class C, class F>
	struct is_invocable : std::false_type
	{};

	template<class C, class R, class ...A>
	struct is_invocable<C, R(A...)>
		: decltype(is_invocable_test<R>(
			std::declval<C>(), 1, std::declval<A>()...))
	{};

	template<class T, class Signature>
	using is_completion_handler = std::integral_constant<bool,
		std::is_move_constructible<typename std::decay<T>::type>::value &&
		detail::is_invocable<T, Signature>::value>;

#define TINYRPC_HANDLER_TYPE_CHECK(type, sig) \
	static_assert(jsonrpc::detail::is_completion_handler< \
		BOOST_ASIO_HANDLER_TYPE(type, sig), sig>::value, \
			"CompletionHandler signature requirements not met")

	//////////////////////////////////////////////////////////////////////////
	struct rpc_bind_handler
	{
		virtual ~rpc_bind_handler() = default;
		virtual void operator()(const Json::Value&, Json::Value&) = 0;
	};

	template <typename Handler>
	class rpc_remote_handler : public rpc_bind_handler
	{
	public:
		rpc_remote_handler(Handler&& handler)
			: handler_(std::forward<Handler>(handler))
		{}

		void operator()(const Json::Value& req, Json::Value& reply) override
		{
			handler_(req, reply);
		}

		Handler handler_;
	};

	//////////////////////////////////////////////////////////////////////////

	class rpc_operation
	{
	public:
		virtual ~rpc_operation() = default;
		virtual void operator()(const boost::system::error_code&) = 0;
		virtual Json::Value& result() = 0;
	};

	template<class Handler, class ExecutorType>
	class rpc_call_op : public rpc_operation
	{
	public:
		rpc_call_op(Json::Value& data, Handler&& h, ExecutorType executor)
			: handler_(std::forward<Handler>(h))
			, executor_(executor)
			, data_(data)
		{}

		rpc_call_op(const rpc_call_op& other)
			: handler_(std::forward<Handler>(other.handler_))
			, executor_(other.executor_)
			, data_(other.data_)
		{}

		rpc_call_op(rpc_call_op&& other)
			: handler_(std::forward<Handler>(other.handler_))
			, executor_(other.executor_)
			, data_(other.data_)
		{}

		void operator()(const boost::system::error_code& ec) override
		{
#if defined(JSONRPC_DISABLE_THREADS)
			handler_(ec);
#else
			boost::asio::post(executor_,
				[handler = std::forward<Handler>(handler_), ec]() mutable
			{
				handler(ec);
			});
#endif
		}

		Json::Value& result() override
		{
			return data_;
		}

	private:
		Handler handler_;
		ExecutorType executor_;
		Json::Value& data_;
	};

}

template <class Websocket>
class jsonrpc_ws_service
{
	// c++11 noncopyable.
	jsonrpc_ws_service(const jsonrpc_ws_service&) = delete;
	jsonrpc_ws_service& operator=(const jsonrpc_ws_service&) = delete;

	using executor_type = typename Websocket::executor_type;

	using rpc_bind_handler_ptr = std::unique_ptr<detail::rpc_bind_handler>;
	using call_op_ptr = std::unique_ptr<detail::rpc_operation>;

	using write_context = std::unique_ptr<std::string>;
	using write_message_queue = std::deque<write_context>;

public:
	explicit jsonrpc_ws_service(Websocket& ws)
		: m_websocket(ws)
	{}

	virtual ~jsonrpc_ws_service()
	{}

	executor_type get_executor() noexcept
	{
		return m_websocket.get_executor();
	}

	Websocket& websocket() noexcept
	{
		return m_websocket;
	}

	jsonrpc_ws_service(jsonrpc_ws_service&& rhs) noexcept
		: m_websocket(rhs.m_websocket)
		, m_recycle(std::move(rhs.m_recycle))
		, m_call_ops(std::move(rhs.m_call_ops))
		, m_message_queue(std::move(rhs.m_message_queue))
	{}

	int dispatch(boost::beast::multi_buffer& buf)
	{
		boost::system::error_code ec;
		auto bytes = dispatch(buf, ec);
		if (ec)
		{
			boost::throw_exception(boost::system::system_error(ec));
		}
		return bytes;
	}

	int dispatch(boost::beast::multi_buffer& buf, boost::system::error_code& ec)
	{
		// parser jsonrpc protocol.
		auto result = boost::beast::buffers_to_string(buf.data());
		Json::Value root = string_to_json(result, ec);
		if (ec)
		{
			ec = make_error_code(errc::parse_json_failed);
			abort_rpc(ec);
			return 0;
		}

		// rpc dispatch.
		rpc_dispatch(std::move(root), ec);
		if (ec)
		{
			abort_rpc(ec);
			return 0;
		}

		return static_cast<int>(result.size());
	}

	template<class Handler>
	void rpc_bind(const std::string& method, Handler&& handler)
	{
		TINYRPC_HANDLER_TYPE_CHECK(Handler, void(const Json::Value&, Json::Value&));

		detail::lock_guard<std::mutex> l(m_methods_mutex);
		using handler_type = std::decay_t<Handler>;
		using rpc_remote_handler_type = detail::rpc_remote_handler<handler_type>;
		auto h = std::make_unique<rpc_remote_handler_type>(std::forward<handler_type>(handler));
		m_remote_methods[method] = std::move(h);
	}

	template<class Handler>
	void async_call(const Json::Value& req, Json::Value& reply, Handler&& handler)
	{
		TINYRPC_HANDLER_TYPE_CHECK(Handler, void(boost::system::error_code));

		boost::asio::async_completion<Handler, void(boost::system::error_code)> init(handler);
		auto json_request = req;

		{
			using completion_handler_type = std::decay_t<decltype(init.completion_handler)>;
			using rpc_call_op_type = detail::rpc_call_op<completion_handler_type, executor_type>;

			auto&& op = std::make_unique<rpc_call_op_type>(reply,
				std::forward<completion_handler_type>(init.completion_handler), this->get_executor());

			detail::lock_guard<std::mutex> l(m_call_op_mutex);
			if (m_recycle.empty())
			{
				auto session = m_call_ops.size();
				m_call_ops.emplace_back(std::move(op));
				json_request["id"] = static_cast<int>(session);
			}
			else
			{
				auto session = m_recycle.back();
				m_recycle.pop_back();
				json_request["id"] = session;
				m_call_ops[session] = std::move(op);
			}
		}

		json_request["jsonrpc"] = "2.0";
		rpc_write(std::make_unique<std::string>(make_json_string(json_request)));

		return init.result.get();
	}

	void abort_rpc(const boost::system::error_code& ec)
	{
		// clear all calling.
		{
			detail::lock_guard<std::mutex> l(m_call_op_mutex);
			for (auto& h : m_call_ops)
			{
				if (!h) continue;
				(*h)(ec);
				h.reset();
			}
		}

		// clear all rpc method.
		clean_remote_methods();
	}

protected:
	void rpc_write(std::unique_ptr<std::string>&& context)
	{
		detail::unique_lock<std::mutex> l(m_msg_mutex);

		bool write_in_progress = !m_message_queue.empty();
		m_message_queue.emplace_back(std::move(context));
		if (!write_in_progress)
		{
			auto& front = m_message_queue.front();
			l.unlock();

			m_websocket.async_write(boost::asio::buffer(*front),
				std::bind(&jsonrpc_ws_service<Websocket>::rpc_write_handle,
					this, std::placeholders::_1));
		}
	}

	void rpc_write_handle(boost::system::error_code ec)
	{
		if (ec)
		{
			abort_rpc(ec);
			return;
		}

		detail::unique_lock<std::mutex> l(m_msg_mutex);

		m_message_queue.pop_front();
		if (!m_message_queue.empty())
		{
			auto& context = m_message_queue.front();
			l.unlock();

			m_websocket.async_write(boost::asio::buffer(*context),
				std::bind(&jsonrpc_ws_service<Websocket>::rpc_write_handle,
					this, std::placeholders::_1));
		}
	}

	void clean_remote_methods()
	{
		detail::lock_guard<std::mutex> l(m_methods_mutex);
		for (auto& h : m_remote_methods)
		{
			if (h.second)
				h.second.reset();
		}
		m_remote_methods.clear();
	}

	void rpc_dispatch(Json::Value&& json, boost::system::error_code& ec)
	{
		// 检查是否为被调用或调用者.
		bool caller = true;
		if (json.isMember("result") || json.isMember("error"))
			caller = false;

		auto session = json.get("id", -1).asInt();
		if (caller && session != -1)
		{
			auto method_name = json["method"].asString();
			detail::rpc_bind_handler* method = nullptr;

			{
				detail::lock_guard<std::mutex> l(m_methods_mutex);
				method = m_remote_methods[method_name].get();
				BOOST_ASSERT(method && "method is nullptr!");
			}

			Json::Value reply;
			if (method)
				(*method)(json, reply);
			reply["id"] = session;
			reply["jsonrpc"] = "2.0";

			rpc_write(std::make_unique<std::string>(make_json_string(reply)));
			return;
		}

		bool is_notify = false;
		call_op_ptr handler;
		do
		{
			detail::lock_guard<std::mutex> l(m_call_op_mutex);

			if (session < 0) // 通知
			{
				is_notify = true;
				break;
			}
			else if (session >= m_call_ops.size())
			{
				ec = make_error_code(errc::out_of_range);
				return;
			}
			else
			{
				handler = std::move(m_call_ops[session]);
				BOOST_ASSERT(handler && "call op is nullptr!"); // for debug
				if (!handler)
				{
					ec = make_error_code(errc::invalid_id);
					return;
				}

				// recycle session.
				m_recycle.push_back(session);
			}
		} while (0);

		if (!is_notify)
		{
			// 保存结果到json.
			handler->result() = json;

			// 回调调用处的handler.
			(*handler)(boost::system::error_code{});
		}
		else
		{
			auto method_name = json["method"].asString();
			detail::rpc_bind_handler* method = nullptr;

			{
				detail::lock_guard<std::mutex> l(m_methods_mutex);
				method = m_remote_methods[method_name].get();
				BOOST_ASSERT(method && "method is nullptr!");
			}

			Json::Value reply;
			if (method)
				(*method)(json, reply);
			return;
		}
	}

private:
	Websocket& m_websocket;
	std::mutex m_methods_mutex;
	std::unordered_map<std::string, rpc_bind_handler_ptr> m_remote_methods;

	std::mutex m_call_op_mutex;
	std::vector<int> m_recycle;
	std::vector<call_op_ptr> m_call_ops;

	std::mutex m_msg_mutex;
	write_message_queue m_message_queue;
};


}
