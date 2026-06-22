#!/bin/bash
set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
INSTALL_PREFIX="/usr/local"

echo "============================================="
echo "🔨 本地构建模式"
echo "============================================="

# ====================== 1. 安装系统依赖 ======================
echo -e "\n[1/4] 校验并安装系统原生依赖"
REQUIRED_APT_PACKAGES=(
    "build-essential"
    "cmake"
    "libcurl4-openssl-dev"
    "libssl-dev"
    "nlohmann-json3-dev"
    "libsqlite3-dev"
    "git"
)

MISSING=()
for pkg in "${REQUIRED_APT_PACKAGES[@]}"; do
    if ! dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null | grep -q "install ok installed"; then
        echo "❌ 缺失: $pkg"
        MISSING+=("$pkg")
    else
        echo "✓ $pkg"
    fi
done

if [ ${#MISSING[@]} -gt 0 ]; then
    echo "安装缺失依赖..."
    sudo apt update -y
    sudo apt install -y "${MISSING[@]}"
fi

# ====================== 2. 编译安装 IXWebSocket ======================
echo -e "\n[2/4] 编译安装 IXWebSocket"
IXWS_INARK="/usr/local/include/ixwebsocket/IXWebSocket.h"
if [ -f "$IXWS_INARK" ]; then
    echo "✓ IXWebSocket 已安装，跳过"
else
    echo "编译 IXWebSocket..."
    git clone --depth 1 https://github.com/machinezone/IXWebSocket.git /tmp/ixws
    cmake -B /tmp/ixws/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX"
    cmake --build /tmp/ixws/build -j$(nproc)
    sudo cmake --install /tmp/ixws/build
    rm -rf /tmp/ixws
    echo "✓ IXWebSocket 安装完成"
fi

# ====================== 3. 项目 CMake 编译 ======================
echo -e "\n[3/4] 编译 qbot-chen"
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
echo "✅ 构建成功: $BINARY"

# ====================== 4. 完成 ======================
echo -e "\n============================================="
echo "✅ 构建全部完成！"
echo "📍 二进制: $BINARY"
echo "🚀 运行: ./$BINARY"
echo "============================================="
