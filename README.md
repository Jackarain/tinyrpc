
# c++ rpc 基于boost.beast、protobuf的异步rpc实现.

## 介绍

使用boost.beast为websocket底层使用, 数据协议使用protobuf，实现纯异步的rpc调用.

## 动机

在参考了各种c++实现的rpc库之后，都有这样或那样的'缺点'，然而那些缺点恰恰是我本人非常在乎的，比如ZeroC Ice，不仅体型十分庞大，使用它需要掌握复杂的IDL语言，再比如gRPC，使用的接口并非十分直观，它被设计成一套框架而不是工具库，项目的应用需要掌握它发明的各种概念，然而这此概念往往是与它底层实现有关，如果不是经验十分丰富，恐怕一时难以理解并掌握，其它的细节不在些多叙了。
鉴于此，我想要的是一个拥有简单接口，像调用本地函数一样，并支持异步，不需要编写IDL，于是有了此项目。

## 使用

依赖boost protobuf。
这个库本身实现只有一个.hpp头文件实现，和一个做为协议底层封装的proto，包含rpc_websocket_service.hpp和rpc_service_ptl.proto生成的.pb.h/.pb.cc。


## 快速上手

```
using tcp = boost::asio::ip::tcp;               // from <boost/asio/ip/tcp.hpp>
namespace websocket = boost::beast::websocket;  // from <boost/beast/websocket.hpp>

using ws = websocket::stream<tcp::socket>;

// rpc_session 在一个已完成握手的websocket链接上通信.
// rpc 并不区分服务器和客户端。

class rpc_session : public std::enable_shared_from_this<rpc_session>
{
public:
	rpc_session(ws&& s)
		: rpc_(std::make_shared<rpc_websocket_service<ws>>(std::move(s)))
	{}

	~rpc_session()
	{
		rpc_->stop();
	}

	void run()
	{
		// hello_request 绑定为rpc函数, 供该连接的远端机器调用, 调用参数为
		// HelloRequest，返回HelloReply.
		// 调用参数和返回参数必须在rpc_bind的模板参数中指定。
		rpc_->rpc_bind<helloworld::HelloRequest, helloworld::HelloReply>(
			std::bind(&rpc_session::hello_request, this,
				std::placeholders::_1, std::placeholders::_2));

		// 同上.
		rpc_->rpc_bind<helloworld::WorldRequest, helloworld::WorldReply>(
			std::bind(&rpc_session::world_request, this,
				std::placeholders::_1, std::placeholders::_2));

		rpc_->start();
	}

	void test_proc(boost::asio::yield_context yield)
	{
		helloworld::HelloRequest req;
		req.set_name("client request 1");

		helloworld::HelloReply reply;

		// 异步调用HelloRequest远程rpc函数, 返回reply。
		boost::system::error_code ec;
		rpc_->async_call(req, reply, yield[ec]);

		std::cout << reply.message() << ", ec: " << ec.message() << std::endl;
		if (ec)
			return;

		req.set_name("client request 1");
		rpc_->async_call(req, reply, yield[ec]);

		std::cout << reply.message() << ", ec: " << ec.message() << std::endl;
		if (ec)
			return;


		helloworld::WorldRequest req2;
		req2.set_name("client world 1");
		helloworld::WorldReply reply2;
		rpc_->async_call(req2, reply2, yield[ec]);

		std::cout << reply2.message() << ", ec: " << ec.message() << std::endl;
	}

	// 供远程机器 rpc 调用的函数定义.
	void hello_request(const helloworld::HelloRequest& req, helloworld::HelloReply& reply)
	{
		reply.set_message("hello reply message to server");
		std::cout << "recv: " << req.name() << ", reply " << reply.message() << std::endl;
	}

	void world_request(const helloworld::WorldRequest& req, helloworld::WorldReply& reply)
	{
		reply.set_message("world reply message to server");
		std::cout << "recv: " << req.name() << ", reply " << reply.message() << std::endl;
	}

private:
	std::shared_ptr<rpc_websocket_service<ws>> rpc_;
};
```

更多的用法参考example，你可以编译运行并调试它们，以了解它的实现原理。

tinyrpc 的 nodejs 实现 https://github.com/omegacoleman/node-tinyrpc
