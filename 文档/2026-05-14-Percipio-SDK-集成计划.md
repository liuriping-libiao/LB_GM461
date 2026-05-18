# Percipio C++ SDK 集成计划

**更新时间**: 2026年5月14日  
**状态**: Phase 1 完成 ✓ — SDK 集成 + 真实相机验证通过

## 1. 重构总结

### 1.1 Pimpl 模式应用
**目标**: 隐藏 Percipio SDK 依赖，支持 synthetic 与真实 SDK 的动态切换

**变更**:
- [src/camera/PercipioCamera.h](src/camera/PercipioCamera.h)
  - 移除私有成员（原 atomic/mutex/thread 等）
  - 新增 `class Impl; std::unique_ptr<Impl> impl_;`
  - 所有公有方法保持签名不变（兼容现有 CameraService）

- [src/camera/PercipioCamera.cpp](src/camera/PercipioCamera.cpp)
  - 新建 `class PercipioCamera::Impl` 包含所有原有实现
  - 公有方法都委托给 impl_ 对应实现
  - 保留 synthetic frame 生成逻辑（当 SDK 未集成时）
  - 在 `#ifdef LBGM461_ENABLE_PERCIPIO_SDK` 中标记了 6 处 TODO 位置

### 1.2 编译构型
- **Windows Debug**: 继续使用 synthetic frames（VcameraSDK x86_64 可选）
- **ARM64 Linux Release**: 准备 cross-compile（需要 ARM64 SDK 库）
- **Fallback 机制**: 无 SDK 时自动降级到 synthetic 模式

---

## 2. Percipio SDK 集成清单

### 2.1 准备工作

#### 2.1.1 获取 SDK 库
**Source**: `C:\Users\zzulr\Desktop\GM461\VcameraSDK-26.1.5`

**需要的文件**:
```
SDK Root
  ├─ cpp/
  │  ├─ include/vcamera/     (头文件)
  │  │  ├─ camera.h
  │  │  ├─ cameraFactory.h
  │  │  ├─ cameraDefines.h
  │  │  ├─ feature.h
  │  │  └─ ...
  │  └─ linux/
  │     ├─ Release/lib/
  │     │  ├─ libvcam_core.so.x86_64
  │     │  ├─ libvcam_api.so.x86_64
  │     │  └─ ...
  │     └─ Debug/lib/
```

**说明**: 当前 SDK 仅提供 x86_64 Linux/.so 文件，无 ARM64。ARM64 版本需另行获取或联系厂商。

#### 2.1.2 CMakeLists.txt 配置
**已完成**：
- `LBGM461_ENABLE_PERCIPIO_SDK`: 默认 OFF（可改为 ON 并指定 LBGM461_PERCIPIO_SDK_ROOT）
- `LBGM461_PERCIPIO_SDK_ROOT`: 默认空，当设置时 CMake 自动链接 SDK 库和头文件

**使用方式**:
```bash
cmake -DLBGM461_ENABLE_PERCIPIO_SDK=ON \
      -DLBGM461_PERCIPIO_SDK_ROOT=C:\Users\zzulr\Desktop\GM461\VcameraSDK-26.1.5 \
      -B out/build/vs2022-x64
```

---

### 2.2 SDK 集成代码位置

#### 位置 1: Init() 函数 (PercipioCamera.cpp, Line ~44)
```cpp
bool Impl::Init() {
    // ...existing code...
#ifdef LBGM461_ENABLE_PERCIPIO_SDK
    // TODO: Call vcamera::CameraUtils::Init(true);
    // if (!status.IsSuccess()) {
    //     SetError(status.message());
    //     return false;
    // }
#endif
    // ...existing code...
}
```

**替换代码**:
```cpp
#ifdef LBGM461_ENABLE_PERCIPIO_SDK
if (config_.synthetic_camera) {
    initialized_ = true;
    SetError("");
    return true;
}

CameraApiStatus status = CameraUtils::Init(true);
if (!status.IsSuccess()) {
    SetError(std::string("SDK Init failed: ") + status.message());
    return false;
}
#endif
```

#### 位置 2: Connect() 函数 (PercipioCamera.cpp, Line ~58)
```cpp
bool Impl::Connect() {
    // ...validation code...
#ifdef LBGM461_ENABLE_PERCIPIO_SDK
    // TODO: Implement real Percipio SDK connection:
    // 1. Call vcamera::CameraUtils::DiscoverCameras()
    // 2. Get camera by SN or IP: vcamera::CameraFactory::GetCamera(sn, ip)
    // 3. camera.Connect()
    // 4. Extract intrinsics from camera calibration info
    
    // For now, use synthetic intrinsics
#endif
```

**替换代码**:
```cpp
#ifdef LBGM461_ENABLE_PERCIPIO_SDK
if (!config_.synthetic_camera) {
    auto devicelist = CameraUtils::DiscoverCameras();
    if (devicelist.empty()) {
        SetError("No Percipio camera found");
        return false;
    }
    
    Camera camera = CameraFactory::GetCamera(config_.camera_sn, config_.camera_ip);
    CameraApiStatus status = camera.Connect();
    if (!status.IsSuccess()) {
        SetError(std::string("Camera Connect failed: ") + status.message());
        return false;
    }
    
    // Cache camera for StartCapture
    camera_ = std::move(camera);
    
    // Extract intrinsics
    // TODO: Call camera_.GetCameraInfo() or similar to populate intrinsics_
}
#endif
```

#### 位置 3: Start() 函数 (PercipioCamera.cpp, Line ~89)
```cpp
bool Impl::Start() {
    // ...validation code...
#ifdef LBGM461_ENABLE_PERCIPIO_SDK
    // TODO: Implement Percipio SDK capture start:
    // 1. camera.SetSensorEnabled("Depth", true)
    // 2. camera.SetSensorEnabled("Texture", true)
    // 3. camera.RegisterFrameSetCallback(...)
    // 4. camera.StartCapture()
#endif
```

**替换代码**:
```cpp
#ifdef LBGM461_ENABLE_PERCIPIO_SDK
if (!config_.synthetic_camera) {
    camera_.StopCapture();
    
    camera_.SetSensorEnabled(SensorType::Depth, true);
    camera_.SetSensorEnabled(SensorType::Texture, true);
    camera_.SetMapDepthToTextureEnabled(true);
    
    camera_.RegisterFrameSetCallback([this](const FrameSet& frameset) {
        OnFrameSetCallback(frameset);
    });
    
    CameraApiStatus status = camera_.StartCapture();
    if (!status.IsSuccess()) {
        SetError(std::string("StartCapture failed: ") + status.message());
        return false;
    }
}
#endif
```

#### 位置 4: Stop() 函数 (PercipioCamera.cpp, Line ~113)
```cpp
bool Impl::Stop() {
    // ...existing code...
#ifdef LBGM461_ENABLE_PERCIPIO_SDK
    // TODO: Implement Percipio SDK capture stop:
    // 1. camera.StopCapture()
    // 2. camera.Disconnect()
#endif
```

**替换代码**:
```cpp
#ifdef LBGM461_ENABLE_PERCIPIO_SDK
if (!config_.synthetic_camera) {
    camera_.StopCapture();
    camera_.Disconnect();
}
#endif
```

#### 位置 5: WorkerLoop() 函数 (PercipioCamera.cpp, Line ~178)
```cpp
void Impl::WorkerLoop() {
    while (!stop_requested_) {
#ifdef LBGM461_ENABLE_PERCIPIO_SDK
        // TODO: In real Percipio SDK mode:
        // Frames are delivered via RegisterFrameSetCallback
        // Update latest_frame_ and latest_ticks_ in the callback
        // For now, fall through to synthetic mode
#endif
```

**说明**: 此处保持不变，因为 SDK 通过回调方式传递帧。回调在 Start() 中注册，自动更新 latest_frame_

#### 位置 6: 新增回调函数 (在 Impl 类中添加)
```cpp
private:
    void OnFrameSetCallback(const FrameSet& frameset) {
        int64_t ticks = NowTicks();
        
        Image depth_img = frameset.GetImage(SensorType::Depth);
        Image color_img = frameset.GetImage(SensorType::Texture);
        
        FrameSnapshot frame;
        frame.ip = config_.camera_ip;
        frame.ticks = ticks;
        frame.depth_scale = config_.depth_scale;
        
        if (depth_img.IsValid()) {
            frame.depth.width = depth_img.width();
            frame.depth.height = depth_img.height();
            frame.depth.mat_type = 2;  // CV_16UC1
            frame.depth.data.assign(depth_img.data(), 
                                    depth_img.data() + depth_img.data_size());
        }
        
        if (color_img.IsValid()) {
            frame.color.width = color_img.width();
            frame.color.height = color_img.height();
            frame.color.mat_type = 16;  // CV_8UC3
            frame.color.data.assign(color_img.data(),
                                    color_img.data() + color_img.data_size());
        }
        
        {
            std::lock_guard<std::mutex> lock(mutex_);
            latest_frame_ = std::move(frame);
            latest_ticks_ = ticks;
        }
        cv_.notify_all();
    }
```

### 2.3 头文件添加

在 [src/camera/PercipioCamera.cpp](src/camera/PercipioCamera.cpp) 顶部条件编译块中取消注释：

```cpp
#ifdef LBGM461_ENABLE_PERCIPIO_SDK
#include <vcamera/camera.h>
#include <vcamera/cameraFactory.h>
#include <vcamera/commonUtils.h>
using namespace vcamera;
#endif
```

### 2.4 Impl 类成员添加

```cpp
class PercipioCamera::Impl {
private:
#ifdef LBGM461_ENABLE_PERCIPIO_SDK
    vcamera::Camera camera_;
#endif
    // ... existing members ...
};
```

---

## 3. 测试计划

### 3.1 单元测试
- [ ] Init(): SDK 初始化成功/失败路径
- [ ] Connect(): 设备发现、连接、内参提取
- [ ] Start(): 采集启动、回调注册
- [ ] Grab(): 帧队列正确性、超时处理
- [ ] Stop(): 资源清理、线程退出

### 3.2 集成测试
- [ ] Windows Debug: synthetic + real SDK (x86_64)
- [ ] Linux x86_64: 交叉编译 + real SDK (如果有)
- [ ] Linux ARM64: 交叉编译 + real SDK (需要 ARM64 库)
- [ ] C# 上位机端对端测试：GetYoloResult → /dev/shm → 读取验证

### 3.3 性能测试
- [ ] 帧率稳定性 (30fps target)
- [ ] 内存泄漏检查
- [ ] CPU 占用率
- [ ] 延迟测量 (采集→共享内存写→上位机读)

---

## 4. 依赖关系矩阵

| 模块 | 依赖 | 状态 |
|------|------|------|
| PercipioCamera.h | FrameTypes.h | ✓ 完成 |
| PercipioCamera.cpp | vcamera/*.h | ⏳ 待集成 |
| CameraService.h | PercipioCamera | ✓ 完成 |
| GrpcServer.h | CameraService | ✓ 完成 |
| main.cpp | CameraService + GrpcServer | ✓ 完成 |
| CMakeLists.txt | SDK 路径配置 | ✓ 完成 |

---

## 5. 后续里程碑

### Phase 1: SDK 集成 ✓ 已完成
- [x] 从 VcameraSDK 复制头文件到项目
- [x] 实现 6 个 TODO 位置代码
- [x] Windows x86_64 编译 + 运行测试
- [x] C# 端对端验证
- [x] 真实相机内参提取 (fx=400.7, fy=400.7, cx=326.6, cy=247.6)
- [x] 性能测试: 102.5fps, 延迟 9.4ms avg

### Phase 2: ARM64 交叉编译 (当前位置)
- [ ] 获取/编译 ARM64 版 Percipio SDK 库
- [ ] 配置 arm64-linux.cmake 工具链
- [ ] 生成 ARM64 可执行文件
- [ ] 部署到目标 ARM64 Linux 设备

### Phase 3: 生产部署
- [ ] 性能优化（如需）
- [ ] 错误处理完善
- [ ] 日志记录系统
- [ ] 容器化部署（如需）

---

## 6. 故障排除指南

### 编译错误

**错误**: `vcamera/camera.h: No such file or directory`
- **原因**: SDK 头文件路径未配置
- **解决**: 设置 `LBGM461_PERCIPIO_SDK_ROOT` 环境变量或 CMake 参数

**错误**: `undefined reference to vcamera::CameraUtils::Init()`
- **原因**: 链接器找不到 SDK 库
- **解决**: CMake 配置中检查 `link_directories()` 和 `target_link_libraries()`

### 运行时错误

**错误**: `Init failed: SDK not initialized`
- **原因**: SDK Init(true) 调用失败
- **解决**: 检查 Percipio 驱动是否安装、USB 连接是否正常

**错误**: `No Percipio camera found`
- **原因**: 设备未被系统识别或 SN/IP 不匹配
- **解决**: 运行 `ListDevices.cpp` 示例查看可用设备

---

## 7. 参考资源

- **SDK 示例代码**: `C:\Users\zzulr\Desktop\GM461\VcameraSDK-26.1.5\cpp\example\`
  - `FetchFrame.cpp`: 基础帧采集
  - `FullExample1.cpp`: 完整特性演示
  - `utils.cpp`: 命令行解析工具

- **Proto 定义**: [proto/CaptureMachine.proto](proto/CaptureMachine.proto)
- **现有实现**: [src/service/CameraService.cpp](src/service/CameraService.cpp)
