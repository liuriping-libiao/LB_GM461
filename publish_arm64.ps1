# LBGM461 camera_service - Windows 交叉编译 ARM64 Linux + 部署脚本
# 前置条件:
#   1. Arm GNU Toolchain 解压到 C:\arm-gnu-toolchain
#      (下载: https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
#   2. vcpkg 安装在 C:\vcpkg
#   3. UpdateTool_1.0.0 在 ..\UpdateTool_1.0.0\

param(
    [string]$ToolchainRoot = "C:\arm-gnu-toolchain",
    [string]$TargetIP = "10.1.0.66",
    [string]$TargetUser = "cat",
    [string]$TargetPassword = "temppwd",
    [int]$TargetPort = 22,
    [string]$RemotePath = "/home/cat/camera_service",
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
    Write-Host "Run on target: cd $RemotePath && chmod +x camera_service && LD_LIBRARY_PATH=. ./camera_service --camera-ip <IP> --port 5111"
}
