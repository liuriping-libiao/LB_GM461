#!/bin/bash
# LBGM461 camera_service Linux 构建脚本 (支持 x86_64 / aarch64)
# 前置条件:
#   sudo apt install -y build-essential cmake git pkg-config
#   sudo apt install -y libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc
#
# 或用 vcpkg:
#   git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
#   ~/vcpkg/bootstrap-vcpkg.sh
#   ~/vcpkg/vcpkg install grpc protobuf

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ARCH="$(uname -m)"
BUILD_DIR="${SCRIPT_DIR}/out/build/linux-${ARCH}-release"

echo "=== LBGM461 Linux ${ARCH} Build ==="
echo "Source: ${SCRIPT_DIR}"
echo "Build:  ${BUILD_DIR}"

mkdir -p "${BUILD_DIR}"

# 检测 vcpkg
VCPKG_TOOLCHAIN=""
if [ -n "${VCPKG_ROOT}" ] && [ -f "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" ]; then
    VCPKG_TOOLCHAIN="-DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
    echo "Using vcpkg: ${VCPKG_ROOT}"
elif [ -f "$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake" ]; then
    VCPKG_TOOLCHAIN="-DCMAKE_TOOLCHAIN_FILE=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake"
    echo "Using vcpkg: $HOME/vcpkg"
else
    echo "Using system packages (no vcpkg)"
fi

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DLBGM461_ENABLE_PERCIPIO_SDK=ON \
    -DLBGM461_ENABLE_GRPC=ON \
    -DLBGM461_ENABLE_SYNTHETIC_CAMERA=ON \
    ${VCPKG_TOOLCHAIN}

cmake --build "${BUILD_DIR}" -j$(nproc)

echo ""
echo "=== Build Complete ==="
echo "Executable: ${BUILD_DIR}/camera_service"
echo ""

# Determine SDK lib dir based on architecture
if [ "${ARCH}" = "aarch64" ]; then
    SDK_LIB_DIR="${SCRIPT_DIR}/lib/linux/aarch64"
elif [ "${ARCH}" = "x86_64" ]; then
    SDK_LIB_DIR="${SCRIPT_DIR}/lib/linux/x64"
else
    SDK_LIB_DIR="${SCRIPT_DIR}/lib/linux/x64"
fi

echo "Deploy: copy camera_service + ${SDK_LIB_DIR}/*.so* to target"
echo "Run:    LD_LIBRARY_PATH=./lib ./camera_service --listen 0.0.0.0:5111 --camera-ip <IP>"
