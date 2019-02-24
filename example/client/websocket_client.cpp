#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>

#include "helloworld.pb.h"

#include "cxxrpc/rpc_websocket_service.hpp"
using namespace cxxrpc;


using tcp = boost::asio::ip::tcp;               // from <boost/asio/ip/tcp.hpp>
namespace websocket = boost::beast::websocket;  // from <boost/beast/websocket.hpp>

using ws = websocket::stream<tcp::socket>;

class rpc_session : public std::enable_shared_from_this<rpc_session>
{
public:
	rpc_session(websocket::stream<tcp::socket>&& s)
		: rpc_(std::make_shared<rpc_websocket_service<ws>>(std::move(s)))
	{}

	~rpc_session()
	{
		// 停止rpc服务.
		rpc_->stop();
	}

	void run()
	{
		// 绑定rpc函数, 并通过模板参数指定request, reply.
		rpc_->rpc_bind<helloworld::HelloRequest, helloworld::HelloReply>(
			std::bind(&rpc_session::hello_request, this,
				std::placeholders::_1, std::placeholders::_2));

		// 绑定第二个rpc函数.
		rpc_->rpc_bind<helloworld::WorldRequest, helloworld::WorldReply>(
			std::bind(&rpc_session::world_request, this,
				std::placeholders::_1, std::placeholders::_2));

		// 启动rpc服务.
		rpc_->start();
	}

	void test_proc(boost::asio::yield_context yield)
	{
		helloworld::HelloRequest req;
		req.set_name("alice");

		helloworld::HelloReply reply;

		// 第一次rpc异步调用.
		boost::system::error_code ec;
		rpc_->call(req, reply, yield[ec]);

		// 输出远程主机返回值reply, 如果发生错误, 则需return, 避免在一个发
		// 生过错误的rpc链接上通信. 这只不ec不作判断仅仅打印输出, 实际中必须判断.
		std::cout << reply.message() << ", ec: " << ec.message() << std::endl;

		// 第二次rpc异步调用.
		req.set_name("alice");
		rpc_->call(req, reply, yield[ec]);

		std::cout << reply.message() << "" << ec.message() << std::endl;

		// 第三次rpc异步调用.
		helloworld::WorldRequest req2;
		req2.set_name("helloworld::WorldRequest");
		helloworld::WorldReply reply2;
		rpc_->call(req2, reply2, yield[ec]);

		std::cout << reply2.message() << "" << ec.message() << std::endl;
	}

	// 远程调用HelloRequest过来, 通过修改reply实现返回给远程主机.
	void hello_request(const helloworld::HelloRequest& req, helloworld::HelloReply& reply)
	{
		reply.set_message("client hello request");
		std::cout << "recv: " << req.name() << ", reply: " << reply.message() << std::endl;
	}

	// 远程调用WorldRequest过来.
	void world_request(const helloworld::WorldRequest& req, helloworld::WorldReply& reply)
	{
		reply.set_message("client world request");
		std::cout << "recv: " << req.name() << ", reply: " << reply.message() << std::endl;
	}

private:
	std::shared_ptr<rpc_websocket_service<ws>> rpc_;
};


void fail(boost::system::error_code ec, char const* what)
{
	std::cerr << what << ": " << ec.message() << "\n";
}

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

	// 完成websocket握手事宜之后开始进入rpc服务.
	auto ses = std::make_shared<rpc_session>(std::move(s));
	ses->run();
	ses->test_proc(yield);
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

