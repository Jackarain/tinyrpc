PROTOBUF_GENERATE_CPP(PROTO_RPC_TEST_SRC PROTO_RPC_TEST_HDRS helloworld.proto)

include_directories(${CMAKE_CURRENT_BINARY_DIR})

file(GLOB websocket_client_SRCS
	websocket_client.cpp
)

add_executable(websocket-client ${websocket_client_SRCS} ${PROTO_RPC_TEST_SRC} ${PROTO_RPC_TEST_HDRS})
