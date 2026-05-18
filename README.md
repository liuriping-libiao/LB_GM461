# LBGM461 Camera Service

基于 C++ 的相机采集服务工程，目标是替换旧 L515 驱动服务，保持对上位机调用协议兼容。

## 当前状态

1. 已完成项目骨架、共享内存协议写入和服务逻辑闭环。
2. 已完成 gRPC 服务入口代码：依赖可用时启动真实 gRPC，依赖缺失时自动降级到本地 smoke 模式。
3. 相机层当前默认使用 synthetic 帧，便于先验证链路与共享内存格式。

## 对齐旧工程

本项目参考了旧工程 `C:\Users\zzulr\Desktop\RealSenseL515-main` 的以下关键约定：

1. `CaptureMachine.proto` 的服务和消息定义。
2. 共享内存文件写入格式：`int32长度 + payload`。
3. `ProtoYoloResponse` 的核心返回字段：`status/path/ticks`。

## 目录

- `proto/`：协议文件
- `src/camera/`：相机适配层
- `src/service/`：服务层与 gRPC 适配
- `src/shm/`：共享内存写入
- `src/common/`：通用结构和编码逻辑
- `cmake/toolchains/`：交叉编译工具链模板

## 构建

### Windows 本地验证（VS 2022）

1. 打开本目录，使用 CMake 生成器 `Visual Studio 17 2022`。
2. 构建 `camera_service` 目标。

### ARM64 Linux 交叉编译（模板）

1. 使用 `cmake/toolchains/arm64-linux.cmake` 作为 toolchain。
2. 按需补充交叉编译器和依赖库路径。

## gRPC 依赖说明

`CMakeLists.txt` 中 `LBGM461_ENABLE_GRPC` 默认开启。

1. 若找到 `protobuf + gRPC`：自动生成 `CaptureMachine.pb/grpc.pb` 并编译真实 gRPC 服务。
2. 若未找到依赖：构建不失败，程序进入本地 smoke 降级模式。

## 下一步

1. 接入 Percipio SDK 真机采集（替换 synthetic 帧）。
2. 补齐 ARM64 Linux 下 protobuf/gRPC 依赖并启用真实网络服务。
3. 与上位机联调 `GetYoloResult` 调用链。
