#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build-dpu"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "=== Building DPU-side DMA server ==="

cmake "$PROJECT_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CROSSCOMPILING=ON \
    -DDOCA_DIR="${DOCA_DIR:-/opt/mellanox/doca}"

make -j"$(nproc)" dpu_dma_copy

echo ""
echo "Build complete:"
echo "  ${BUILD_DIR}/dpu_dma_copy"
echo ""
echo "Run:"
echo "  ./dpu_dma_copy -p <DPU_DMA_PCI> [-r rep_pci] [-m stage_mib] [-c chunk_mib] [-q queue_depth] [-S service]"
