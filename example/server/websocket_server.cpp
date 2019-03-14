//
// Copyright (c) 2016-2017 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/boostorg/beast
//

//------------------------------------------------------------------------------
//
// Example: WebSocket server, coroutine
//
//------------------------------------------------------------------------------

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "chat.pb.h"

#include "tinyrpc/rpc_websocket_service.hpp"
using namespace tinyrpc;


using tcp = boost::asio::ip::tcp;               // from <boost/asio/ip/tcp.hpp>
namespace websocket = boost::beast::websocket;  // from <boost/beast/websocket.hpp>

using ws = websocket::stream<tcp::socket>;

class rpc_session : public std::enable_shared_from_this<rpc_session>
{
public:
	rpc_session(ws&& s)
		: ws_(std::move(s))
		, rpc_stub_(ws_)
	{}

	~rpc_session()
	{
		std::cout << "~session\n";
	}

	void run(boost::asio::yield_context yield)
	{
		rpc_stub_.rpc_bind<chat::ChatSendMessage, chat::ChatReplyMessage>(
			std::bind(&rpc_session::chat_request, this,
				std::placeholders::_1, std::placeholders::_2));

		boost::beast::multi_buffer buf;
		boost::system::error_code ec;

		while (true)
		{
			auto bytes = ws_.async_read(buf, yield[ec]);
			if (ec)
				return;
			rpc_stub_.dispatch(buf, ec);
			if (ec)
				return;
			buf.consume(bytes);
		}
	}

	void chat_request(const chat::ChatSendMessage& req, chat::ChatReplyMessage& reply)
	{
		std::cout << req.name() << " say: " << req.message() << std::endl;

		reply.set_name("server");
		reply.set_message(req.message() + " copy!");
	}

private:
	ws ws_;
	rpc_websocket_service<ws> rpc_stub_;
};


void fail(boost::system::error_code ec, char const* what)
{
	std::cerr << what << ": " << ec.message() << "\n";
}

void do_session(tcp::socket& socket, boost::asio::yield_context yield)
{
    boost::system::error_code ec;

    ws s{std::move(socket)};

    s.async_accept(yield[ec]);
    if(ec)
        return fail(ec, "accept");

	s.binary(true);

	// 完成websocket握手事宜之后开始进入rpc服务.
	auto ses = std::make_shared<rpc_session>(std::move(s));
	ses->run(yield);
}

void do_listen(
    boost::asio::io_context& ioc,
    tcp::endpoint endpoint,
    boost::asio::yield_context yield)
{
    boost::system::error_code ec;

    tcp::acceptor acceptor(ioc);
    acceptor.open(endpoint.protocol(), ec);
    if(ec)
        return fail(ec, "open");

    acceptor.set_option(boost::asio::socket_base::reuse_address(true), ec);
    if(ec)
        return fail(ec, "set_option");

    acceptor.bind(endpoint, ec);
    if(ec)
        return fail(ec, "bind");

    acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
    if(ec)
        return fail(ec, "listen");

    for(;;)
    {
        tcp::socket socket(ioc);
        acceptor.async_accept(socket, yield[ec]);
        if(ec)
            fail(ec, "accept");
        else
            boost::asio::spawn(
                acceptor.get_executor().context(),
                std::bind(
                    &do_session,
                    std::move(socket),
                    std::placeholders::_1));
    }
}

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        std::cerr <<
            "Usage: websocket-server <address> <port>\n" <<
            "Example:\n" <<
            "    websocket-server 0.0.0.0 8000\n";
        return EXIT_FAILURE;
    }
    auto const address = boost::asio::ip::make_address(argv[1]);
    auto const port = static_cast<unsigned short>(std::atoi(argv[2]));

    boost::asio::io_context ioc;

    boost::asio::spawn(ioc,
        std::bind(
            &do_listen,
            std::ref(ioc),
            tcp::endpoint{address, port},
            std::placeholders::_1));

    ioc.run();

    return EXIT_SUCCESS;
}
