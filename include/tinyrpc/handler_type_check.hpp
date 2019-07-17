//
// Copyright (C) 2019 Jack.
//
// Author: jack
// Email:  jack.wgm at gmail dot com
//

#pragma once

#include <type_traits>
#include <boost/asio/async_result.hpp>

namespace tinyrpc {
	namespace detail {

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
}