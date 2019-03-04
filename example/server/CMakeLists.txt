PROTOBUF_GENERATE_CPP(PROTO_RPC_TEST_SRC PROTO_RPC_TEST_HDRS chat.proto)

include_directories(${CMAKE_CURRENT_BINARY_DIR})

file(GLOB websocket_server_SRCS
	websocket_server.cpp
)

add_executable(websocket-server ${websocket_server_SRCS} ${PROTO_RPC_TEST_SRC} ${PROTO_RPC_TEST_HDRS})
