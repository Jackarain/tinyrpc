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

#include "boost/system/system_error.hpp"
#include "boost/system/error_code.hpp"

#include "boost/beast/core.hpp"
#include "boost/beast/websocket.hpp"

#include "rpc_service_ptl.pb.h"

namespace tinyrpc {

	//////////////////////////////////////////////////////////////////////////
	namespace detail {
		class error_category_impl;
	}

	template<class error_category>
	const boost::system::error_category& error_category_single()
	{
		static error_category error_category_instance;
		return reinterpret_cast<const boost::system::error_category&>(error_category_instance);
	}

	inline const boost::system::error_category& error_category()
	{
		return error_category_single<detail::error_category_impl>();
	}

	namespace errc {
		enum errc_t
		{
			parse_rpc_service_ptl_failed = 1,
			unknow_protocol_descriptor = 2,
			parse_payload_failed = 3,
		};

		inline boost::system::error_code make_error_code(errc_t e)
		{
			return boost::system::error_code(static_cast<int>(e), tinyrpc::error_category());
		}
	}
}

namespace boost {
	namespace system {
		template <>
		struct is_error_code_enum<tinyrpc::errc::errc_t>
		{
			static const bool value = true;
		};

	} // namespace system
} // namespace boost

namespace tinyrpc {
	namespace detail {

		class error_category_impl
			: public boost::system::error_category
		{
			virtual const char* name() const noexcept
			{
				return "TinyRPC";
			}

			virtual std::string message(int e) const
			{
				switch (e)
				{
				case errc::parse_rpc_service_ptl_failed:
					return "Parse protobuf rpc_service_ptl failed";
				case errc::unknow_protocol_descriptor:
					return "unknow protocol descriptor";
				case errc::parse_payload_failed:
					return "Parse protobuf payload failed";
				default:
					return "Unknown TinyRPC error";
				}
			}
		};
	}
}


namespace tinyrpc {

	//////////////////////////////////////////////////////////////////////////

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
			virtual void operator()(boost::system::error_code&&) = 0;
			virtual ::google::protobuf::Message& result() = 0;
		};

		template<class Handler>
		class rpc_call_op : public rpc_operation
		{
		public:
			rpc_call_op(::google::protobuf::Message& data, Handler&& h)
				: handler_(std::forward<Handler>(h))
				, data_(data)
			{}

			void operator()(boost::system::error_code&& ec) override
			{
				handler_(std::forward<boost::system::error_code>(ec));
			}

			::google::protobuf::Message& result() override
			{
				return data_;
			}

		private:
			Handler handler_;
			::google::protobuf::Message& data_;
		};

		struct any_handler
		{
			virtual ~any_handler() = default;
			virtual void operator()(const ::google::protobuf::Message& req, ::google::protobuf::Message& ret) = 0;
		};

		template <typename Handler, typename Request, typename Reply>
		class rpc_remote_handler : public any_handler
		{
		public:
			rpc_remote_handler(Handler&& handler)
				: handler_(std::forward<Handler>(handler))
			{}

			void operator()(const ::google::protobuf::Message& req, ::google::protobuf::Message& ret) override
			{
				handler_(static_cast<const Request&>(req), static_cast<Reply&>(ret));
			}

			Handler handler_;
		};

		struct rpc_method_type
		{
			rpc_method_type() = default;
			rpc_method_type(rpc_method_type&& rhs)
			{
				msg_ = std::move(rhs.msg_);
				ret_ = std::move(rhs.ret_);
				any_call_ = std::move(rhs.any_call_);
			}
			rpc_method_type& operator=(rpc_method_type&& rhs)
			{
				msg_ = std::move(rhs.msg_);
				ret_ = std::move(rhs.ret_);
				any_call_ = std::move(rhs.any_call_);
				return *this;
			}

			std::unique_ptr<::google::protobuf::Message> msg_;
			std::unique_ptr<::google::protobuf::Message> ret_;
			std::unique_ptr<any_handler> any_call_;
		};
		using rpc_remote_method = std::vector<rpc_method_type>;

		using call_op_ptr = std::unique_ptr<rpc_operation>;
		using call_op = std::vector<call_op_ptr>;
		using strand = boost::asio::strand<boost::asio::io_context::executor_type>;
		using write_context = std::unique_ptr<std::string>;
		using write_message_queue = std::deque<write_context>;

	public:
		rpc_websocket_service(Websocket&& ws)
			: m_websocket(std::move(ws))
			, m_abort(true)
		{}

		virtual ~rpc_websocket_service()
		{}

		boost::asio::io_context::executor_type get_executor() noexcept
		{
			return m_websocket.get_executor();
		}

		Websocket& websocket() noexcept
		{
			return m_websocket;
		}

		void start()
		{
			m_abort = false;
			start_rpc_read();
		}

		void stop()
		{
			m_abort = true;
			if (m_websocket.is_open())
			{
				boost::system::error_code ignore_ec;
				m_websocket.lowest_layer().close(ignore_ec);
			}
		}

		template<class Request, class Reply, class Handler>
		void rpc_bind(Handler&& handler)
		{
			auto desc = Request::descriptor();
			if (m_remote_methods.empty())
			{
				auto fdesc = desc->file();
				m_remote_methods.resize(fdesc->message_type_count());
			}

			rpc_method_type value;

			value.msg_ = std::make_unique<Request>();
			value.ret_ = std::make_unique<Reply>();
			value.any_call_ = std::make_unique<rpc_remote_handler<Handler, Request, Reply>>(std::forward<Handler>(handler));

			m_remote_methods[desc->index()] = std::move(value);
		}

		template<class Handler>
		void start_call_op(int session, ::google::protobuf::Message& msg, Handler&& handler)
		{
			boost::asio::async_completion<Handler,
				void(boost::system::error_code)> init(handler);
			using completion_handler_type = decltype(init.completion_handler);

			auto& ptr = m_call_ops[session];
			ptr.reset(new rpc_call_op<completion_handler_type>(msg,
				std::forward<completion_handler_type>(init.completion_handler)));

			return init.result.get();
		}

		template<class T, class R, class Handler>
		void call(const T& msg, R& ret, Handler&& handler)
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
			rpc_write(std::make_unique<std::string>(rb.SerializeAsString()));

			start_call_op(session, ret, std::forward<Handler>(handler));

			return;
		}

	protected:
		void rpc_write(std::unique_ptr<std::string>&& context)
		{
			bool write_in_progress = !m_message_queue.empty();
			m_message_queue.emplace_back(std::move(context));

			if (!write_in_progress)
			{
				auto& front = m_message_queue.front();
				auto self = this->shared_from_this();
				m_websocket.async_write(boost::asio::buffer(*front),
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
					auto& context = m_message_queue.front();
					auto self = this->shared_from_this();
					m_websocket.async_write(boost::asio::buffer(*context),
						std::bind(&rpc_websocket_service<Websocket>::rpc_write_handle,
							self, std::placeholders::_1));
				}
			}
		}

		void reset_call_ops(boost::system::error_code&& ec)
		{
			for (auto& h : m_call_ops)
			{
				if (!h) continue;
				(*h)(std::forward<boost::system::error_code>(ec));
				h.reset();
			}
		}

		void start_rpc_read()
		{
			auto self = this->shared_from_this();
			m_websocket.async_read(m_read_buffer, [self, this]
			(boost::system::error_code ec, std::size_t bytes)
			{
				if (ec)
				{
					m_abort = true;
					reset_call_ops(std::forward<boost::system::error_code>(ec));
					return;
				}

				// parser rpc base protocol.
				rpc_service_ptl::rpc_base_ptl rb;
				if (!rb.ParseFromString(boost::beast::buffers_to_string(m_read_buffer.data())))
					return reset_call_ops(make_error_code(errc::parse_rpc_service_ptl_failed));

				m_read_buffer.consume(bytes);

				// rpc dispatch
				rpc_dispatch(std::move(rb));

				// start next read.
				start_rpc_read();
			});
		}

		void rpc_dispatch(rpc_service_ptl::rpc_base_ptl&& rb)
		{
			auto self = this->shared_from_this();

			// 远程调用过来, 找到对应的event并响应.
			if (rb.call() == rpc_service_ptl::rpc_base_ptl::caller)
			{
				boost::asio::post(get_executor(), [self, this, rb = std::move(rb)]()
				{
					auto session = rb.session();

					const auto descriptor =
						::google::protobuf::DescriptorPool::generated_pool()->FindMessageTypeByName(rb.message());
					if (!descriptor)
						return reset_call_ops(make_error_code(errc::unknow_protocol_descriptor));

					auto& e = m_remote_methods[descriptor->index()];	// O(1) 查找.

					std::unique_ptr<::google::protobuf::Message> msg(e.msg_->New());
					if (!msg->ParseFromString(rb.payload()))
						return reset_call_ops(make_error_code(errc::parse_payload_failed));

					std::unique_ptr<::google::protobuf::Message> ret(e.ret_->New());

					// call function.
					(*e.any_call_)(*msg, *ret);

					// send back return.
					rpc_service_ptl::rpc_base_ptl rpc_ret;
					rpc_ret.set_call(rpc_service_ptl::rpc_base_ptl::callee);
					rpc_ret.set_session(session);
					rpc_ret.set_message(ret->GetTypeName());
					rpc_ret.set_payload(ret->SerializeAsString());

					rpc_write(std::make_unique<std::string>(rpc_ret.SerializeAsString()));
				});
			}

			// 本地调用远程, 远程返回的return.
			if (rb.call() == rpc_service_ptl::rpc_base_ptl::callee)
			{
				boost::asio::post(get_executor(), [self, this, rb = std::move(rb)]()
				{
					auto session = rb.session();
					auto& h = m_call_ops[session]; // O(1) 查找.
					if (!h)
					{
						// 不可能达到这里, 因为m_call_op是可增长的容器.
						BOOST_ASSERT(0);
						return;
					}

					// 将远程返回的protobuf对象序列化到ret中, 并'唤醒'call处的协程.
					auto& ret = h->result();
					if (!ret.ParseFromString(rb.payload()))
						return reset_call_ops(make_error_code(errc::parse_payload_failed));
					(*h)(boost::system::error_code{});

					m_call_ops[session].reset();
					m_recycle.push_back(session);
				});
			}
		}

	private:
		Websocket m_websocket;
		write_message_queue m_message_queue;
		boost::beast::multi_buffer m_read_buffer;
		rpc_remote_method m_remote_methods;
		call_op m_call_ops;
		std::vector<int> m_recycle;
		std::atomic_bool m_abort;
	};
}

