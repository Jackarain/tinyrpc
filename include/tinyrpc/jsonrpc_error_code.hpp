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
			parse_json_failed = 1,
			out_of_range,
			invalid_id,
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
				return "JsonRPC";
			}

			virtual std::string message(int e) const
			{
				switch (e)
				{
				case errc::parse_json_failed:
					return "Parse json failed";
				case errc::out_of_range:
					return "Out of range";
				case errc::invalid_id:
					return "Invalid ID";
				default:
					return "Unknown TinyRPC error";
				}
			}
		};
	}
}
