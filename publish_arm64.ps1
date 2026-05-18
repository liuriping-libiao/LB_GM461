<#
.SYNOPSIS
    LBGM461 camera_service - Windows 交叉编译 ARM64 Linux + 部署脚本

.DESCRIPTION
    在 Windows 上交叉编译 camera_service (ARM64 Linux)，打包发布目录并部署到目标设备。
    支持多摄像头：自动生成 start_all.sh 启动脚本，每个摄像头运行独立进程，
    分配不同的 gRPC 端口和共享内存前缀。

.PARAMETER ToolchainRoot
    Arm GNU Toolchain 根目录路径。
    下载: https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads

.PARAMETER TargetIP
    部署目标设备的 IP 地址。

.PARAMETER TargetUser
    目标设备的 SSH 用户名。

.PARAMETER TargetPassword
    目标设备的 SSH 密码。

.PARAMETER TargetPort
    目标设备的 SSH 端口，默认 22。

.PARAMETER RemotePath
    目标设备上的部署目录。

.PARAMETER CameraIPs
    摄像头 IP 地址数组。支持 1 个或多个摄像头。
    每个摄像头会启动一个独立的 camera_service 进程。

.PARAMETER BasePort
    第一个摄像头实例的 gRPC 监听端口，后续实例端口依次递增。
    例如 BasePort=5111，两个摄像头分别监听 5111 和 5112。

.PARAMETER SkipBuild
    跳过编译步骤，仅执行部署。

.PARAMETER SkipDeploy
    跳过部署步骤，仅执行编译和打包。

.EXAMPLE
    # 默认双摄像头编译+部署
    .\publish_arm64.ps1

.EXAMPLE
    # 自定义摄像头 IP
    .\publish_arm64.ps1 -CameraIPs @("192.168.1.10", "192.168.1.11")

.EXAMPLE
    # 单摄像头
    .\publish_arm64.ps1 -CameraIPs @("169.254.0.10")

.EXAMPLE
    # 三摄像头，自定义端口起始
    .\publish_arm64.ps1 -CameraIPs @("169.254.0.10", "169.254.0.11", "169.254.0.12") -BasePort 6000

.EXAMPLE
    # 仅编译，不部署
    .\publish_arm64.ps1 -SkipDeploy

.EXAMPLE
    # 仅部署（已编译过），指定目标
    .\publish_arm64.ps1 -SkipBuild -TargetIP "10.1.0.100" -TargetUser "admin" -TargetPassword "pass123"

.NOTES
    前置条件:
      1. Arm GNU Toolchain 解压到 C:\arm-gnu-toolchain
      2. vcpkg 安装在 C:\vcpkg
      3. UpdateTool_1.0.0 在 ..\UpdateTool_1.0.0\
    
    目标设备运行:
      cd /home/cat/camera_service && chmod +x start_all.sh camera_service && ./start_all.sh
#>

param(
    [string]$ToolchainRoot = "C:\arm-gnu-toolchain",
    [string]$TargetIP = "10.1.2.206",
    [string]$TargetUser = "cat",
    [string]$TargetPassword = "temppwd",
    [int]$TargetPort = 22,
    [string]$RemotePath = "/home/cat/camera_service",
    [string[]]$CameraIPs = @("169.254.0.10", "169.254.0.11"),
    [int]$BasePort = 5111,
    [switch]$SkipBuild,
    [switch]$SkipDeploy
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $ProjectRoot "out\build\linux-arm64-release"
$PublishDir = Join-Path $ProjectRoot "out\publish\linux-arm64"
$CMake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

# Add ninja to PATH if not already available
$NinjaDir = "C:\vcpkg\downloads\tools\ninja-1.13.2-windows"
if ((Test-Path $NinjaDir) -and -not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    $env:PATH = "$NinjaDir;$env:PATH"
}

# Validate toolchain
if (-not (Test-Path "$ToolchainRoot\bin\aarch64-none-linux-gnu-gcc.exe")) {
    Write-Error "ARM64 cross-compiler not found at: $ToolchainRoot\bin\aarch64-none-linux-gnu-gcc.exe"
    Write-Host "Please download and extract Arm GNU Toolchain to: $ToolchainRoot"
    Write-Host "Download: https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads"
    exit 1
}

Write-Host "=== LBGM461 Cross-Compile for Linux ARM64 ===" -ForegroundColor Cyan
Write-Host "Toolchain: $ToolchainRoot"
Write-Host "Build dir: $BuildDir"
Write-Host "Publish:   $PublishDir"
Write-Host ""

if (-not $SkipBuild) {
    # Step 1: Install gRPC for arm64-linux via vcpkg (with overlay triplet)
    Write-Host "[1/4] Installing vcpkg dependencies (arm64-linux)..." -ForegroundColor Yellow
    $VcpkgRoot = "C:\vcpkg"
    $OverlayTriplets = Join-Path $ProjectRoot "cmake\triplets"

    $env:ARM_TOOLCHAIN_ROOT = $ToolchainRoot
    & "$VcpkgRoot\vcpkg.exe" install grpc protobuf `
        --triplet arm64-linux `
        --overlay-triplets="$OverlayTriplets" `
        --host-triplet=x64-windows
    if ($LASTEXITCODE -ne 0) { Write-Error "vcpkg install failed"; exit 1 }

    # Step 2: CMake configure
    Write-Host "[2/4] Configuring CMake..." -ForegroundColor Yellow
    New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null

    & $CMake -S $ProjectRoot -B $BuildDir `
        -G "Ninja" `
        -DCMAKE_BUILD_TYPE=Release `
        -DCMAKE_TOOLCHAIN_FILE="$VcpkgRoot\scripts\buildsystems\vcpkg.cmake" `
        -DVCPKG_TARGET_TRIPLET=arm64-linux `
        -DVCPKG_OVERLAY_TRIPLETS="$OverlayTriplets" `
        -DVCPKG_HOST_TRIPLET=x64-windows `
        -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="$ProjectRoot\cmake\toolchains\arm64-linux.cmake" `
        -DARM_TOOLCHAIN_ROOT="$ToolchainRoot" `
        -DLBGM461_ENABLE_PERCIPIO_SDK=ON `
        -DLBGM461_ENABLE_GRPC=ON `
        -DLBGM461_ENABLE_SYNTHETIC_CAMERA=OFF
    if ($LASTEXITCODE -ne 0) { Write-Error "CMake configure failed"; exit 1 }

    # Step 3: Build
    Write-Host "[3/4] Building..." -ForegroundColor Yellow
    & $CMake --build $BuildDir --config Release
    if ($LASTEXITCODE -ne 0) { Write-Error "Build failed"; exit 1 }

    # Step 4: Package publish directory
    Write-Host "[4/4] Packaging publish directory..." -ForegroundColor Yellow
    New-Item -ItemType Directory -Path $PublishDir -Force | Out-Null

    # Copy executable
    Copy-Item "$BuildDir\camera_service" "$PublishDir\camera_service" -Force

    # Copy SDK shared libraries
    $SdkLibDir = Join-Path $ProjectRoot "lib\linux\aarch64"
    Copy-Item "$SdkLibDir\libtycam.so*" $PublishDir -Force
    Copy-Item "$SdkLibDir\libtyimgproc.so*" $PublishDir -Force

    # Generate startup script for dual-camera
    $StartScript = @"
#!/bin/bash
SCRIPT_DIR=`$(cd "`$(dirname "`$0")" && pwd)
cd "`$SCRIPT_DIR"
export LD_LIBRARY_PATH="`$SCRIPT_DIR:`$LD_LIBRARY_PATH"

# Kill existing camera_service processes
pkill -9 -x camera_service 2>/dev/null
sleep 3

"@
    for ($i = 0; $i -lt $CameraIPs.Count; $i++) {
        $camIP = $CameraIPs[$i]
        $port = $BasePort + $i
        $prefix = "/dev/shm/cam$($i + 1)"
        $StartScript += @"
echo "Starting camera_service instance $($i + 1): camera=$camIP listen=0.0.0.0:$port shm=$prefix"
./camera_service --camera-ip $camIP --listen 0.0.0.0:$port --shared-prefix $prefix &`n
"@
    }
    $StartScript += @"
echo "All camera_service instances started."
wait
"@
    $StartScriptPath = Join-Path $PublishDir "start_all.sh"
    # Use UTF8 without BOM and LF line endings for Linux
    $StartScript = $StartScript -replace "`r`n", "`n"
    $Utf8NoBom = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($StartScriptPath, $StartScript, $Utf8NoBom)

    Write-Host ""
    Write-Host "=== Build Complete ===" -ForegroundColor Green
    Write-Host "Published to: $PublishDir"
    Get-ChildItem $PublishDir | Format-Table Name, Length -AutoSize
}

if (-not $SkipDeploy) {
    Write-Host ""
    Write-Host "=== Deploying to $TargetIP ===" -ForegroundColor Cyan

    # Verify target is reachable
    if (-not (Test-Connection $TargetIP -Count 1 -Quiet)) {
        Write-Error "Target $TargetIP is not reachable. Check network connection."
        exit 1
    }

    # Deploy using UpdateTool
    $UpdateTool = Join-Path (Split-Path $ProjectRoot -Parent) "UpdateTool_1.0.0\UpdateTool.exe"
    if (-not (Test-Path $UpdateTool)) {
        $UpdateTool = "C:\Users\zzulr\Desktop\UpdateTool_1.0.0\UpdateTool.exe"
    }

    if (-not (Test-Path $UpdateTool)) {
        Write-Error "UpdateTool.exe not found"
        exit 1
    }

    Write-Host "UpdateTool: $UpdateTool"
    Write-Host "Source:     $PublishDir"
    Write-Host "Target:     ${TargetUser}@${TargetIP}:${RemotePath}"

    & $UpdateTool $TargetIP $TargetUser $TargetPassword $TargetPort $PublishDir $RemotePath
    if ($LASTEXITCODE -ne 0) { Write-Error "Deployment failed"; exit 1 }

    Write-Host ""
    Write-Host "=== Deployment Complete ===" -ForegroundColor Green
    Write-Host "Run on target: cd $RemotePath && chmod +x start_all.sh camera_service && ./start_all.sh"
    Write-Host ""
    Write-Host "Camera instances:" -ForegroundColor Yellow
    for ($i = 0; $i -lt $CameraIPs.Count; $i++) {
        Write-Host "  [$($i+1)] camera=$($CameraIPs[$i])  listen=0.0.0.0:$($BasePort + $i)  shm=/dev/shm/cam$($i+1)"
    }
}
