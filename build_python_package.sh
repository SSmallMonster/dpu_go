#!/bin/bash

# DPU Cache构建脚本

set -e

echo "Building DPU Cache Python Package..."

# 构建C共享库
echo "Step 1: Building C shared library..."
mkdir -p build
cd build
cmake ..
make dpu_cache -j$(nproc)
cd ..

# 检查共享库是否生成
if [ ! -f "build/libdpu_cache.so" ]; then
    echo "Error: libdpu_cache.so not found!"
    exit 1
fi

echo "Step 2: Setting up Python package..."
cd python

# 创建符号链接到共享库
mkdir -p dpu_cache/native
ln -sf ../../build/libdpu_cache.so dpu_cache/native/
ln -sf ../../include/dpu_cache_api.h dpu_cache/native/

# 安装Python包（开发模式）
pip install -e .

echo "Build completed successfully!"
echo ""
echo "Usage:"
echo "  import dpu_cache"
echo "  config = dpu_cache.DPUConfig(dpu_ip='192.168.100.2')"
echo "  agent = dpu_cache.DPUAgent(config)"
echo ""