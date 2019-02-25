PROTOBUF_GENERATE_CPP(PROTO_RPC_SERVICE_PTL_SRC PROTO_RPC_SERVICE_PTL_HDRS rpc_service_ptl.proto)

add_library(baseproto STATIC ${PROTO_RPC_SERVICE_PTL_SRC} ${PROTO_RPC_SERVICE_PTL_HDRS})
set_target_properties(baseproto PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_link_libraries(baseproto protobuf::libprotobuf)
target_include_directories(baseproto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
