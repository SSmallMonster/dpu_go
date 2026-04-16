#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build-host"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "=== Building host-side GPU DMA client ==="

cmake "$PROJECT_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DDOCA_DIR="${DOCA_DIR:-/opt/mellanox/doca}"

make -j"$(nproc)" gpu_dma_copy

echo ""
echo "Build complete:"
echo "  ${BUILD_DIR}/gpu_dma_copy"
echo ""
echo "Push GPU -> DPU file:"
echo "  ./gpu_dma_copy -o push -p <HOST_BF_PCI> -g <GPU_ID> -f <DPU_FILE> -s <SIZE_MiB> [-S service]"
echo ""
echo "Pull DPU file -> GPU:"
echo "  ./gpu_dma_copy -o pull -p <HOST_BF_PCI> -g <GPU_ID> -f <DPU_FILE> [-O LOCAL_OUT] [-S service]"
