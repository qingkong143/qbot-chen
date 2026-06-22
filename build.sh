#!/bin/sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
INSTALL_PREFIX="/usr/local"

echo "============================================="
echo "本地构建模式"
echo "============================================="

# ====================== 1. 安装系统依赖 ======================
echo ""
echo "[1/4] 校验并安装系统原生依赖"

if [ "$(id -u)" -eq 0 ]; then
    APT_CMD="apt"
elif command -v sudo >/dev/null 2>&1; then
    APT_CMD="sudo apt"
else
    APT_CMD=""
fi

if [ -n "$APT_CMD" ]; then
    $APT_CMD update -y
    $APT_CMD install -y --no-install-recommends \
        build-essential \
        cmake \
        libcurl4-openssl-dev \
        libssl-dev \
        nlohmann-json3-dev \
        libsqlite3-dev \
        git
else
    echo "⚠ 无 root/sudo 权限，跳过系统依赖安装（请确认依赖已就绪）"
fi

# ====================== 2. 编译安装 IXWebSocket ======================
echo ""
echo "[2/4] 编译安装 IXWebSocket"
IXWS_MARK="/usr/local/include/ixwebsocket/IXWebSocket.h"
if [ -f "$IXWS_MARK" ]; then
    echo "IXWebSocket 已安装，跳过"
else
    echo "编译 IXWebSocket..."
    git clone --depth 1 https://github.com/machinezone/IXWebSocket.git /tmp/ixws
    cmake -B /tmp/ixws/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX"
    cmake --build /tmp/ixws/build -j$(nproc)
    if [ -n "$APT_CMD" ]; then
        sudo cmake --install /tmp/ixws/build
    else
        cmake --install /tmp/ixws/build || echo "⚠ 安装 IXWebSocket 需要 root 权限，请手动执行: sudo cmake --install /tmp/ixws/build"
    fi
    rm -rf /tmp/ixws
    echo "IXWebSocket 安装完成"
fi

# ====================== 3. 项目 CMake 编译 ======================
echo ""
echo "[3/4] 编译 qbot-chen"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
    -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wno-unused-parameter -O2 -DCPPHTTPLIB_OPENSSL_SUPPORT -DMCP_SSL"

make -j$(nproc)

BINARY="$BUILD_DIR/qbot-chen"
if [ ! -x "$BINARY" ]; then
    echo "ERROR: 未找到编译产物 $BINARY"
    exit 1
fi
echo "构建成功: $BINARY"

# ====================== 4. 完成 ======================
echo ""
echo "============================================="
echo "构建全部完成！"
echo "二进制: $BINARY"
echo "运行: ./$BINARY"
echo "============================================="
