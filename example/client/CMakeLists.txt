file(GLOB cxxrpc_SRCS
	websocket_client.cpp
)

add_executable(websocket-client ${cxxrpc_SRCS})
