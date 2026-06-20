#!/bin/bash
set -eo pipefail

# ====================== 环境自动判断 ======================
# CNB云构建环境会内置环境变量 RUNNER_NAME，以此区分
if [ -n "$RUNNER_NAME" ]; then
    # 云构建模式
    IS_CNB_BUILD=1
    OUTPUT_DIR="/output"
else
    # 本地开发模式
    IS_CNB_BUILD=0
    OUTPUT_DIR="./dist" # 本地输出到项目内dist文件夹
fi

# 全局固定配置
IXWS_INSTALL_MARK="/usr/local/include/ixwebsocket/IXWebSocket.h"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
INSTALL_PREFIX="/usr/local"
BINARY_TARGET_NAME="dicksuck"
BINARY_FULL_PATH="$BUILD_DIR/bin/$BINARY_TARGET_NAME"

echo "============================================="
if [ $IS_CNB_BUILD -eq 1 ]; then
    echo "🔨 CNB 云端构建模式"
else
    echo "🔨 本地开发构建模式"
fi
echo "============================================="

# ====================== 1. 源替换：仅云构建执行 ======================
if [ $IS_CNB_BUILD -eq 1 ]; then
    echo -e "\n[1/6] 替换国内阿里云软件源加速（仅云构建）"
    if [ -f /etc/apt/sources.list ]; then
        sed -i 's/deb.debian.org/mirrors.aliyun.com/g' /etc/apt/sources.list
        sed -i 's/security.debian.org/mirrors.aliyun.com/g' /etc/apt/sources.list
    fi
fi

# ====================== 2. 安装系统依赖 ======================
echo -e "\n[2/6] 校验并安装系统原生依赖"
REQUIRED_APT_PACKAGES=(
    "build-essential"
    "cmake"
    "libcurl4-openssl-dev"
    "nlohmann-json3-dev"
    "libsqlite3-dev"
    "git"
)
NEED_APT_INSTALL=0

for pkg in "${REQUIRED_APT_PACKAGES[@]}"; do
    if ! dpkg -l | grep -q "^ii  $pkg"; then
        echo "❌ 缺失系统依赖: $pkg"
        NEED_APT_INSTALL=1
    else
        echo "✓ 已存在: $pkg"
    fi
done

if [ "$NEED_APT_INSTALL" -eq 1 ]; then
    echo "开始批量安装缺失依赖..."
    # 本地加sudo，云构建容器自带root不用
    if [ $IS_CNB_BUILD -eq 1 ]; then
        apt update -y
        apt install -y "${REQUIRED_APT_PACKAGES[@]}"
        rm -rf /var/lib/apt/lists/*
    else
        sudo apt update -y
        sudo apt install -y "${REQUIRED_APT_PACKAGES[@]}"
    fi
else
    echo "所有系统依赖已就绪，跳过apt安装"
fi

# ====================== 3. 编译安装 IXWebSocket ======================
echo -e "\n[3/6] 校验 IXWebSocket 库"
if [ -f "$IXWS_INSTALL_MARK" ]; then
    echo "✓ IXWebSocket 已预编译安装，跳过源码构建"
else
    echo "IXWebSocket 未安装，源码编译中..."
    git clone https://mirror.ghproxy.com/https://github.com/machinezone/IXWebSocket.git /tmp/ixws
    cd /tmp/ixws
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
    cmake --build build -j$(nproc)
    # 本地安装需要sudo权限
    if [ $IS_CNB_BUILD -eq 1 ]; then
        cmake --install build
    else
        sudo cmake --install build
    fi
    cd "$SCRIPT_DIR"
    rm -rf /tmp/ixws
    echo "✓ IXWebSocket 编译安装完成"
fi

# ====================== 4. 项目CMake编译 ======================
echo -e "\n[4/6] 初始化项目编译目录"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "[CMake] 执行配置..."
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
    -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wno-unused-parameter -O2"

echo "[Make] 多线程编译项目..."
make -j$(nproc)

if [ ! -f "$BINARY_FULL_PATH" ]; then
    echo "ERROR: 编译失败，未生成目标二进制 $BINARY_FULL_PATH"
    exit 1
fi

# ====================== 5. 制品导出 ======================
echo -e "\n[5/6] 导出发布制品至 $OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

cp "$BINARY_FULL_PATH" "$OUTPUT_DIR/"
cp "$SCRIPT_DIR"/*.sh "$OUTPUT_DIR/" 2>/dev/null || true
cp "$SCRIPT_DIR"/config.json "$OUTPUT_DIR/" 2>/dev/null || true

echo "📦 已导出文件列表:"
ls -lh "$OUTPUT_DIR"

# ====================== 6. 完成提示 ======================
echo -e "\n============================================="
echo "✅ 构建全部完成！"
echo "📍 本地编译产物: $BINARY_FULL_PATH"
echo "📤 发布制品目录: $OUTPUT_DIR"
echo "🚀 本地运行示例:"
echo "   ./$OUTPUT_DIR/$BINARY_TARGET_NAME"
echo "============================================="