PROTOBUF_GENERATE_CPP(PROTO_RPC_TEST_SRC PROTO_RPC_TEST_HDRS helloworld.proto)

file(GLOB cxxrpc_SRCS
	websocket_server.cpp
)

add_executable(websocket-server ${cxxrpc_SRCS} ${PROTO_RPC_TEST_SRC} ${PROTO_RPC_TEST_HDRS})
