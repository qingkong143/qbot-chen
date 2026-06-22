#!/bin/bash
set -eo pipefail

echo "============================================="
echo "🔨 CNB 云端构建"
echo "============================================="

# ====================== 1. 替换国内源加速 ======================
echo -e "\n[1/5] 替换 apt 源"
if [ -f /etc/apt/sources.list ]; then
    sed -i 's/deb.debian.org/mirrors.aliyun.com/g' /etc/apt/sources.list
    sed -i 's/security.debian.org/mirrors.aliyun.com/g' /etc/apt/sources.list
fi

# ====================== 2. 安装系统依赖 ======================
echo -e "\n[2/5] 安装系统依赖"
apt update -y
apt install -y --no-install-recommends \
    build-essential \
    cmake \
    libcurl4-openssl-dev \
    libssl-dev \
    nlohmann-json3-dev \
    libsqlite3-dev \
    git

# ====================== 3. 编译 IXWebSocket ======================
echo -e "\n[3/5] 编译 IXWebSocket"
IXWS_MARK="/usr/local/include/ixwebsocket/IXWebSocket.h"
if [ -f "$IXWS_MARK" ]; then
    echo "✓ 已安装，跳过"
else
    git clone --depth 1 https://github.com/machinezone/IXWebSocket.git /tmp/ixws
    cmake -B /tmp/ixws/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DIX_WEBSOCKET_BUILD_UNIT_TESTS=OFF \
        -DIX_WEBSOCKET_BUILD_SAMPLES=OFF
    cmake --build /tmp/ixws/build -j$(nproc)
    cmake --install /tmp/ixws/build
    rm -rf /tmp/ixws
    echo "✓ 安装完成"
fi

# ====================== 4. 编译项目 ======================
echo -e "\n[4/5] 编译 qbot-chen"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake "$SCRIPT_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wno-unused-parameter -O2 -DCPPHTTPLIB_OPENSSL_SUPPORT -DMCP_SSL"

make -j$(nproc)

# ====================== 5. 导出制品 ======================
echo -e "\n[5/5] 导出制品到 /output"
mkdir -p /output
cp "$BUILD_DIR/qbot-chen" /output/
chmod +x /output/qbot-chen

echo "📦 制品:"
ls -lh /output/
echo "============================================="
echo "✅ CNB 构建完成"
