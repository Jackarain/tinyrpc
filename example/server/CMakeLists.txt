file(GLOB cxxrpc_SRCS
	websocket_server.cpp
)

add_executable(websocket-server ${cxxrpc_SRCS})
