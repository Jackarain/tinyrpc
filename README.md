
# c++ rpc 基于boost.beast、protobuf的异步rpc实现.

[![Build Status](https://travis-ci.org/Jackarain/tinyrpc.svg?branch=master)](https://travis-ci.org/Jackarain/tinyrpc)
## 介绍

使用boost.beast为websocket底层使用, 数据协议使用protobuf，实现纯异步的rpc调用.

## 动机

在参考了各种c++实现的rpc库之后，都有这样或那样的'缺点'，然而那些缺点恰恰是我本人非常在乎的，比如ZeroC Ice，不仅体型十分庞大，使用它需要掌握复杂的IDL语言，再比如gRPC，使用的接口并非十分直观，它被设计成一套框架而不是工具库，项目的应用需要掌握它发明的各种概念，然而这此概念往往是与它底层实现有关，如果不是经验十分丰富，恐怕一时难以理解并掌握，其它的细节不在些多叙了。
鉴于此，我想要的是一个拥有简单接口，像调用本地函数一样，并支持异步，不需要编写IDL，于是有了此项目。

## 使用

依赖boost protobuf。
这个库本身实现只有一个.hpp头文件实现，和一个做为协议底层封装的proto，包含rpc_websocket_service.hpp和rpc_service_ptl.proto生成的.pb.h/.pb.cc。


## 快速上手

用法参考example，你可以编译运行并调试它们，以了解它的实现原理。

tinyrpc 的 nodejs 实现 https://github.com/omegacoleman/node-tinyrpc
