#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>

#include "chat.pb.h"

#include "tinyrpc/rpc_websocket_service.hpp"
using namespace tinyrpc;


using tcp = boost::asio::ip::tcp;               // from <boost/asio/ip/tcp.hpp>
namespace websocket = boost::beast::websocket;  // from <boost/beast/websocket.hpp>

using ws = websocket::stream<tcp::socket>;

void fail(boost::system::error_code ec, char const* what)
{
	std::cerr << what << ": " << ec.message() << "\n";
}

class rpc_session : public std::enable_shared_from_this<rpc_session>
{
public:
	explicit rpc_session(ws&& s)
		: ws_(std::move(s))
		, rpc_stub_(ws_)
	{}

	~rpc_session()
	{
	}

	void run(boost::asio::yield_context yield)
	{
		boost::beast::multi_buffer buf;
		boost::system::error_code ec;

		while (true)
		{
			auto bytes = ws_.async_read(buf, yield[ec]);
			if (ec)
				return fail(ec, "async_read");
			rpc_stub_.dispatch(buf, ec);
			if (ec)
				return fail(ec, "dispatch");
			buf.consume(bytes);
		}
	}

	void chat_proc(boost::asio::yield_context yield)
	{
		chat::ChatSendMessage msg;

		std::cout << "input your name: ";
		std::string context;
		std::getline(std::cin, context);
		msg.set_name(context);

		chat::ChatReplyMessage reply;

		while (true)
		{
			std::cout << msg.name() << ": ";

			context.clear();
			std::getline(std::cin, context);

			msg.set_message(context);

			boost::system::error_code ec;
			rpc_stub_.async_call(msg, reply, yield[ec]);
			if (ec)
				return fail(ec, "async_call");

			std::cout << reply.name() << " reply: " << reply.message() << std::endl;
		}
	}

private:
	ws ws_;
	rpc_websocket_service<ws> rpc_stub_;
};

void do_session(
	std::string const& host,
	std::string const& port,
	boost::asio::io_context& ioc,
	boost::asio::yield_context yield)
{
	boost::system::error_code ec;

	tcp::resolver resolver{ ioc };
	ws s{ ioc };

	auto const results = resolver.async_resolve(host, port, yield[ec]);
	if (ec)
		return fail(ec, "resolve");

	boost::asio::async_connect(s.next_layer(), results.begin(), results.end(), yield[ec]);
	if (ec)
		return fail(ec, "connect");

	s.async_handshake(host, "/test", yield[ec]);
	if (ec)
		return fail(ec, "handshake");

	s.binary(true);

	boost::asio::ip::tcp::socket ss(ioc);

	// 完成websocket握手事宜之后开始进入rpc服务.
	auto ses = std::make_shared<rpc_session>(std::move(s));
	boost::asio::spawn(ioc,
		std::bind(&rpc_session::run, ses, std::placeholders::_1));
	ses->chat_proc(yield);
}

int main(int argc, char** argv)
{
	if (argc != 3)
	{
		std::cerr <<
			"Usage: websocket-client <host> <port>\n" <<
			"Example:\n" <<
			"    websocket-client 127.0.0.1 8000\n";
		return EXIT_FAILURE;
	}

	auto const host = argv[1];
	auto const port = argv[2];

	boost::asio::io_context ioc;

	boost::asio::spawn(ioc, std::bind(
		&do_session,
		std::string(host),
		std::string(port),
		std::ref(ioc),
		std::placeholders::_1));

	ioc.run();

	return EXIT_SUCCESS;
}

