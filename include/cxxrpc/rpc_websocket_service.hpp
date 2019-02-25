//
// Copyright (C) 2019 Jack.
//
// Author: jack
// Email:  jack.wgm at gmail dot com
//

#pragma once

#include <any>
#include <atomic>
#include <memory>
#include <vector>
#include <functional>
#include <deque>

#include "boost/beast/core.hpp"
#include "boost/beast/websocket.hpp"

#include "boost/smart_ptr/local_shared_ptr.hpp"
#include "boost/smart_ptr/make_local_shared.hpp"

#include "rpc_service_ptl.pb.h"

namespace cxxrpc {

	template <class Websocket>
	class rpc_websocket_service :
		public std::enable_shared_from_this<rpc_websocket_service<Websocket>>
	{
		// c++11 noncopyable.
		rpc_websocket_service(const rpc_websocket_service&) = delete;
		rpc_websocket_service& operator=(const rpc_websocket_service&) = delete;

		class rpc_operation
		{
		public:
			virtual void result_back(boost::system::error_code&&) = 0;
			virtual ::google::protobuf::Message& result() = 0;
		};

		template<class Handler>
		class rpc_call_op : public rpc_operation
		{
		public:
			rpc_call_op(::google::protobuf::Message& data, Handler& h)
				: handler_(std::move(h))
				, data_(data)
			{}

			virtual void result_back(boost::system::error_code&& ec) override
			{
				handler_(ec);
			}

			virtual ::google::protobuf::Message& result() override
			{
				return data_;
			}

		private:
			Handler handler_;
			::google::protobuf::Message& data_;
		};

		template <typename Handler, typename Request, typename Reply>
		class type_erasure_handler
		{
		public:
			type_erasure_handler(Handler&& handler)
				: handler_(std::move(handler))
			{}
			~type_erasure_handler()
			{}

			static void true_func_call(std::any handler,
				const ::google::protobuf::Message& req, ::google::protobuf::Message& ret)
			{
				auto this_object = std::any_cast<type_erasure_handler<Handler, Request, Reply>>(handler);
				this_object.handler_(
					static_cast<const Request&>(req), static_cast<Reply&>(ret));
			}

			Handler handler_;
		};

		typedef void(*type_erasure_call_function)(std::any handler,
			const ::google::protobuf::Message& req, ::google::protobuf::Message& ret);

		struct rpc_event_type
		{
			boost::local_shared_ptr<::google::protobuf::Message> msg_;
			boost::local_shared_ptr<::google::protobuf::Message> ret_;

			type_erasure_call_function func_call_;
			std::any handler_;
		};
		using remote_function = std::vector<rpc_event_type>;

		using call_op_ptr = boost::local_shared_ptr<rpc_operation>;
		using call_op = std::vector<call_op_ptr>;
		using strand = boost::asio::strand<boost::asio::io_context::executor_type>;
		using write_context = boost::local_shared_ptr<std::string>;
		using write_message_queue = std::deque<write_context>;

	public:
		rpc_websocket_service(Websocket&& ws)
			: m_websocket(std::move(ws))
			, m_abort(true)
		{}

		virtual ~rpc_websocket_service()
		{}

		boost::asio::io_context::executor_type get_executor()
		{
			return m_websocket.get_executor();
		}

		Websocket& websocket()
		{
			return m_websocket;
		}

		void start()
		{
			m_abort = false;

			auto self = this->shared_from_this();
			boost::asio::spawn(m_websocket.get_executor(),
				[self, this](boost::asio::yield_context yield)
			{
				rpc_read_loop(yield);
			});
		}

		void stop()
		{
			m_abort = true;
			if (m_websocket.is_open())
			{
				boost::system::error_code ignore_ec;
				m_websocket.close(boost::beast::websocket::close_code::normal, ignore_ec);
			}
		}

		template<class Request, class Reply, class Handler>
		void rpc_bind(Handler&& handler)
		{
			auto desc = Request::descriptor();
			if (m_remote_functions.empty())
			{
				auto fdesc = desc->file();
				m_remote_functions.resize(fdesc->message_type_count());
			}

			rpc_event_type value;

			value.msg_.reset(new Request);
			value.ret_.reset(new Reply);

			value.handler_ = type_erasure_handler<Handler, Request, Reply>(std::forward<Handler>(handler));
			value.func_call_ = type_erasure_handler<Handler, Request, Reply>::true_func_call;

			m_remote_functions[desc->index()] = value;
		}

		template<class Handler>
		void start_call_op(int session, ::google::protobuf::Message& msg, Handler&& handler)
		{
			boost::asio::async_completion<Handler,
				void(boost::system::error_code)> init(handler);

			auto& ptr = m_call_ops[session];
			ptr.reset(new rpc_call_op<decltype(init.completion_handler)>{ msg, init.completion_handler });

			return init.result.get();
		}

		template<class T, class R, class Handler>
		void call(T& msg, R& ret, Handler&& handler)
		{
			rpc_service_ptl::rpc_base_ptl rb;

			rb.set_message(msg.GetTypeName());
			rb.set_payload(msg.SerializeAsString());
			rb.set_call(rpc_service_ptl::rpc_base_ptl::caller);

			int session = 0;
			if (m_recycle.empty())
			{
				session = static_cast<int>(m_call_ops.size());
				m_call_ops.push_back(call_op_ptr{});
				rb.set_session(session);
			}
			else
			{
				session = m_recycle.back();
				m_recycle.pop_back();
				rb.set_session(session);
			}

			auto self = this->shared_from_this();
			auto context = boost::make_local_shared<std::string>(rb.SerializeAsString());
			rpc_write(context);

			start_call_op(session, ret, std::forward<Handler>(handler));

			return;
		}

	protected:
		void rpc_write(boost::local_shared_ptr<std::string> context)
		{
			bool write_in_progress = !m_message_queue.empty();
			m_message_queue.emplace_back(context);

			if (!write_in_progress)
			{
				auto self = this->shared_from_this();
				m_websocket.async_write(boost::asio::buffer(*context),
					std::bind(&rpc_websocket_service<Websocket>::rpc_write_handle,
						self, std::placeholders::_1));
			}
		}

		void rpc_write_handle(boost::system::error_code ec)
		{
			if (!ec)
			{
				m_message_queue.pop_front();
				if (!m_message_queue.empty())
				{
					auto context = m_message_queue.front();
					auto self = this->shared_from_this();
					m_websocket.async_write(boost::asio::buffer(*context),
						std::bind(&rpc_websocket_service<Websocket>::rpc_write_handle,
							self, std::placeholders::_1));
				}
			}
		}

		void reset_call_ops()
		{
			for (auto& c : m_call_ops)
			{
				if (!c) continue;
				c->result_back(make_error_code(
					boost::asio::error::operation_aborted));
				c.reset();
			}
		}

		void rpc_read_loop(boost::asio::yield_context yield)
		{
			boost::system::error_code ec;

			auto fail = [&](const std::string& reason)
			{
				m_abort = true;

				m_websocket.async_close(boost::beast::websocket::close_code::normal, yield[ec]);

				reset_call_ops();
			};

			while (!m_abort)
			{
				boost::beast::multi_buffer buf;
				m_websocket.async_read(buf, yield[ec]);
				if (ec)
					return fail("read " + ec.message());

				rpc_service_ptl::rpc_base_ptl rb;
				if (!rb.ParseFromString(boost::beast::buffers_to_string(buf.data())))
					return fail("parse protocol error");

				auto session = rb.session();

				// 远程调用过来, 找到对应的event并响应.
				if (rb.call() == rpc_service_ptl::rpc_base_ptl::caller)
				{
					const auto descriptor =
						::google::protobuf::DescriptorPool::generated_pool()->FindMessageTypeByName(rb.message());
					if (!descriptor)
						return fail("caller parse protocol descriptor error");

					auto& e = m_remote_functions[descriptor->index()];	// O(1) 查找.

					std::unique_ptr<::google::protobuf::Message> msg(e.msg_->New());
					if (!msg->ParseFromString(rb.payload()))
						return fail("caller parse protocol payload error");

					std::unique_ptr<::google::protobuf::Message> ret(e.ret_->New());

					// call function.
					e.func_call_(e.handler_, *msg, *ret);

					// send back return.
					rpc_service_ptl::rpc_base_ptl rpc_ret;
					rpc_ret.set_call(rpc_service_ptl::rpc_base_ptl::callee);
					rpc_ret.set_session(session);
					rpc_ret.set_message(ret->GetTypeName());
					rpc_ret.set_payload(ret->SerializeAsString());

					auto self = this->shared_from_this();
					auto context = boost::make_local_shared<std::string>(rpc_ret.SerializeAsString());
					rpc_write(context);
					continue;
				}

				// 本地调用远程, 远程返回的return.
				if (rb.call() == rpc_service_ptl::rpc_base_ptl::callee)
				{
					auto h = m_call_ops[session]; // O(1) 查找.
					if (!h)
					{
						// 不可能达到这里, 因为m_call_op是可增长的容器.
						BOOST_ASSERT(0);
						continue;
					}

					// 将远程返回的protobuf对象序列化到ret中, 并'唤醒'call处的协程.
					auto& ret = h->result();
					if (!ret.ParseFromString(rb.payload()))
						return fail("callee parse protocol payload error");
					h->result_back(std::move(ec));

					m_call_ops[session].reset();
					m_recycle.push_back(session);

					continue;
				}
			}

			reset_call_ops();
		}

	private:
		Websocket m_websocket;
		write_message_queue m_message_queue;
		remote_function m_remote_functions;
		call_op m_call_ops;
		std::vector<int> m_recycle;
		std::atomic_bool m_abort;
	};
}

