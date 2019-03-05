//
// Copyright (C) 2019 Jack.
//
// Author: jack
// Email:  jack.wgm at gmail dot com
//

#pragma once

#include "boost/system/system_error.hpp"
#include "boost/system/error_code.hpp"

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
			session_out_of_range = 4,
			invalid_session = 5,
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
					return "Unknow protocol descriptor";
				case errc::parse_payload_failed:
					return "Parse protobuf payload failed";
				case errc::session_out_of_range:
					return "Session out of range";
				case errc::invalid_session:
					return "Invalid session";
				default:
					return "Unknown TinyRPC error";
				}
			}
		};
	}
}
