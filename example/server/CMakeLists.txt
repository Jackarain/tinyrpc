PROTOBUF_GENERATE_CPP(PROTO_RPC_TEST_SRC PROTO_RPC_TEST_HDRS helloworld.proto)

include_directories({CMAKE_CURRENT_BINARY_DIR})

file(GLOB cxxrpc_SRCS
	websocket_server.cpp
)

add_executable(websocket-server ${cxxrpc_SRCS} ${PROTO_RPC_TEST_SRC} ${PROTO_RPC_TEST_HDRS})
