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
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <type_traits>

#include "boost/beast/core.hpp"
#include "boost/beast/websocket.hpp"

#include "rpc_service_ptl.pb.h"

#include "rpc_error_code.hpp"

namespace tinyrpc {

	//////////////////////////////////////////////////////////////////////////

	namespace detail {
#if defined(TINYRPC_DISABLE_THREADS)
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

		class rpc_operation
		{
		public:
			virtual ~rpc_operation() = default;
			virtual void operator()(const boost::system::error_code&) = 0;
			virtual ::google::protobuf::Message& result() = 0;
		};

		template<class Handler, class ExecutorType>
		class rpc_call_op : public rpc_operation
		{
		public:
			rpc_call_op(::google::protobuf::Message& data, Handler&& h, ExecutorType executor)
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
#if defined(TINYRPC_DISABLE_THREADS)
				handler_(ec);
#else
				boost::asio::post(executor_,
					[handler = std::forward<Handler>(handler_), ec]() mutable
				{
					handler(ec);
				});
#endif
			}

			::google::protobuf::Message& result() override
			{
				return data_;
			}

		private:
			Handler handler_;
			ExecutorType executor_;
			::google::protobuf::Message& data_;
		};

		//////////////////////////////////////////////////////////////////////////

		struct rpc_bind_handler
		{
			virtual ~rpc_bind_handler() = default;
			virtual void operator()(const ::google::protobuf::Message&, ::google::protobuf::Message&) = 0;

			::google::protobuf::Message* msg_;
			::google::protobuf::Message* ret_;
		};

		template <typename Handler, typename Request, typename Reply>
		class rpc_remote_handler : public rpc_bind_handler
		{
		public:
			rpc_remote_handler(Handler&& handler)
				: handler_(std::forward<Handler>(handler))
			{
				static Request req;
				static Reply reply;

				msg_ = &req;
				ret_ = &reply;
			}

			void operator()(const ::google::protobuf::Message& req, ::google::protobuf::Message& ret) override
			{
				handler_(static_cast<const Request&>(req), static_cast<Reply&>(ret));
			}

			Handler handler_;
		};


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
			static_assert(tinyrpc::detail::is_completion_handler< \
				BOOST_ASIO_HANDLER_TYPE(type, sig), sig>::value, \
					"CompletionHandler signature requirements not met")
	}

	//////////////////////////////////////////////////////////////////////////

	template <class Websocket>
	class rpc_websocket_service
	{
		// c++11 noncopyable.
		rpc_websocket_service(const rpc_websocket_service&) = delete;
		rpc_websocket_service& operator=(const rpc_websocket_service&) = delete;

		using rpc_bind_handler_ptr = std::unique_ptr<detail::rpc_bind_handler>;
		using rpc_remote_method = std::vector<rpc_bind_handler_ptr>;
		using call_op_ptr = std::unique_ptr<detail::rpc_operation>;
		using call_op = std::vector<call_op_ptr>;
		using write_context = std::unique_ptr<std::string>;
		using write_message_queue = std::deque<write_context>;

		using executor_type = typename Websocket::executor_type;

	public:
		explicit rpc_websocket_service(Websocket& ws)
			: m_websocket(ws)
		{}

		virtual ~rpc_websocket_service()
		{
			clean_remote_methods();
		}

		rpc_websocket_service(rpc_websocket_service&& rhs) noexcept
		{
			m_websocket = rhs.m_websocket;
			m_msg_mutex = std::move(rhs.m_msg_mutex);
			m_message_queue = std::move(rhs.m_message_queue);
			m_remote_methods = std::move(rhs.m_remote_methods);
			m_methods_mutex = std::move(rhs.m_methods_mutex);
			m_call_ops = std::move(rhs.m_call_ops);
			m_recycle = std::move(rhs.m_recycle);
			m_call_op_mutex = std::move(rhs.m_call_op_mutex);
		}

		rpc_websocket_service& operator=(rpc_websocket_service&& rhs) noexcept
		{
			m_websocket = rhs.m_websocket;
			m_msg_mutex = std::move(rhs.m_msg_mutex);
			m_message_queue = std::move(rhs.m_message_queue);
			m_remote_methods = std::move(rhs.m_remote_methods);
			m_methods_mutex = std::move(rhs.m_methods_mutex);
			m_call_ops = std::move(rhs.m_call_ops);
			m_recycle = std::move(rhs.m_recycle);
			m_call_op_mutex = std::move(rhs.m_call_op_mutex);
			return *this;
		}

		executor_type get_executor() noexcept
		{
			return m_websocket.get_executor();
		}

		Websocket& websocket() noexcept
		{
			return m_websocket;
		}

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
			// parser rpc base protocol.
			rpc_service_ptl::rpc_base_ptl rb;
			auto result = boost::beast::buffers_to_string(buf.data());
			if (!rb.ParseFromString(result))
			{
				ec = make_error_code(errc::parse_rpc_service_ptl_failed);
				abort_rpc(ec);
				return 0;
			}
			// rpc dispatch
			rpc_dispatch(std::move(rb), ec);
			if (ec)
			{
				abort_rpc(ec);
				return 0;
			}

			return static_cast<int>(result.size());
		}

		template<class Request, class Reply, class Handler>
		void rpc_bind(Handler&& handler)
		{
			TINYRPC_HANDLER_TYPE_CHECK(Handler, void(const Request&, Reply&));

			detail::lock_guard<std::mutex> l(m_methods_mutex);
			auto desc = Request::descriptor();
			if (m_remote_methods.empty())
			{
				auto fdesc = desc->file();
				m_remote_methods.resize(fdesc->message_type_count());
			}

			using handler_type = std::decay_t<Handler>;
			using rpc_remote_handler_type = detail::rpc_remote_handler<handler_type, Request, Reply>;
			auto h = std::make_unique<rpc_remote_handler_type>(std::forward<handler_type>(handler));
			m_remote_methods[desc->index()] = std::move(h);
		}

		template<class T, class R, class Handler>
		void async_call(const T& msg, R& ret, Handler&& handler)
		{
			TINYRPC_HANDLER_TYPE_CHECK(Handler, void(boost::system::error_code));

			rpc_service_ptl::rpc_base_ptl rb;

			rb.set_message(msg.GetTypeName());
			rb.set_payload(msg.SerializeAsString());
			rb.set_call(rpc_service_ptl::rpc_base_ptl::caller);

			boost::asio::async_completion<Handler,
				void(boost::system::error_code)> init(handler);

			{
				using completion_handler_type = std::decay_t<decltype(init.completion_handler)>;
				using rpc_call_op_type = detail::rpc_call_op<completion_handler_type, executor_type>;

				auto&& op = std::make_unique<rpc_call_op_type>(ret,
					std::forward<completion_handler_type>(init.completion_handler), this->get_executor());

				detail::lock_guard<std::mutex> l(m_call_op_mutex);
				if (m_recycle.empty())
				{
					auto session = m_call_ops.size();
					m_call_ops.emplace_back(std::move(op));
					rb.set_session(static_cast<google::protobuf::uint32>(session));
				}
				else
				{
					auto session = m_recycle.back();
					m_recycle.pop_back();
					rb.set_session(static_cast<google::protobuf::uint32>(session));
					m_call_ops[session] = std::move(op);
				}
			}

			rpc_write(std::make_unique<std::string>(rb.SerializeAsString()));

			return init.result.get();
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
					std::bind(&rpc_websocket_service<Websocket>::rpc_write_handle,
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
					std::bind(&rpc_websocket_service<Websocket>::rpc_write_handle,
						this, std::placeholders::_1));
			}
		}

		void clean_remote_methods()
		{
			detail::lock_guard<std::mutex> l(m_methods_mutex);
			for (auto& h : m_remote_methods)
			{
				if (h)
					h.reset();
			}
			m_remote_methods.clear();
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

		void rpc_dispatch(rpc_service_ptl::rpc_base_ptl&& rb, boost::system::error_code& ec)
		{
			if (rb.call() == rpc_service_ptl::rpc_base_ptl::caller)
			{
				auto session = rb.session();
				const auto descriptor =
					::google::protobuf::DescriptorPool::generated_pool()->FindMessageTypeByName(rb.message());
				if (!descriptor)
				{
					ec = make_error_code(errc::unknow_protocol_descriptor);
					return;
				}

				rpc_service_ptl::rpc_base_ptl rpc_reply;
				std::unique_ptr<::google::protobuf::Message> msg = nullptr;
				detail::rpc_bind_handler* method = nullptr;

				{
					detail::lock_guard<std::mutex> l(m_methods_mutex);
					method = m_remote_methods[descriptor->index()].get();
					BOOST_ASSERT(method && "method is nullptr!");
					msg.reset(method->msg_->New());
					BOOST_ASSERT(msg && "New message fail!");
				}

				if (!msg->ParseFromString(rb.payload()))
				{
					ec = make_error_code(errc::parse_payload_failed);
					return;
				}

				std::unique_ptr<::google::protobuf::Message> reply(method->ret_->New());
				(*method)(*msg, *reply);

				rpc_reply.set_message(reply->GetTypeName());
				rpc_reply.set_payload(reply->SerializeAsString());

				// send back return.
				rpc_reply.set_call(rpc_service_ptl::rpc_base_ptl::callee);
				rpc_reply.set_session(session);

				rpc_write(std::make_unique<std::string>(rpc_reply.SerializeAsString()));
			}

			if (rb.call() == rpc_service_ptl::rpc_base_ptl::callee)
			{
				auto session = rb.session();

				call_op_ptr handler;
				do
				{
					detail::lock_guard<std::mutex> l(m_call_op_mutex);

					if (session >= m_call_ops.size())
					{
						ec = make_error_code(errc::session_out_of_range);
						return;
					}

					handler = std::move(m_call_ops[session]);
					BOOST_ASSERT(handler && "call op is nullptr!"); // for debug
					if (!handler)
					{
						ec = make_error_code(errc::invalid_session);
						return;
					}

					// recycle session.
					m_recycle.push_back(session);
				} while (0);

				auto& ret = handler->result();
				if (!ret.ParseFromString(rb.payload()))
				{
					ec = make_error_code(errc::parse_payload_failed);
					return;
				}

				(*handler)(boost::system::error_code{});
			}
		}

	private:
		Websocket& m_websocket;
		std::mutex m_msg_mutex;
		write_message_queue m_message_queue;
		rpc_remote_method m_remote_methods;
		std::mutex m_methods_mutex;
		call_op m_call_ops;
		std::vector<int> m_recycle;
		std::mutex m_call_op_mutex;
	};
}

