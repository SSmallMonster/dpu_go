#!/bin/bash
set -e

echo "========================================"
echo " GPU <-> DPU DMA Copy Preflight Check"
echo "========================================"
echo ""

ARCH="$(uname -m)"
echo "Architecture: ${ARCH}"
echo ""

if [ -d "/opt/mellanox/doca" ]; then
    echo "[OK]   DOCA SDK found at /opt/mellanox/doca"
else
    echo "[FAIL] DOCA SDK not found at /opt/mellanox/doca"
fi

if [ -f "/opt/mellanox/doca/include/doca_dma.h" ]; then
    echo "[OK]   doca_dma.h found"
else
    echo "[FAIL] doca_dma.h not found"
fi

if command -v lspci >/dev/null 2>&1; then
    echo ""
    echo "Mellanox / BlueField devices:"
    lspci | grep -i "Mellanox\|BlueField\|ConnectX" || true
fi

if [ "${ARCH}" = "x86_64" ]; then
    echo ""
    echo "Host-side checks:"
    if command -v nvidia-smi >/dev/null 2>&1; then
        echo "[OK]   NVIDIA GPU detected"
        nvidia-smi --query-gpu=name,pci.bus_id --format=csv,noheader || true
    else
        echo "[FAIL] nvidia-smi not found"
    fi

    if command -v nvcc >/dev/null 2>&1; then
        echo "[OK]   nvcc found"
    else
        echo "[FAIL] nvcc not found"
    fi

    if lsmod 2>/dev/null | grep -q nvidia_peermem; then
        echo "[OK]   nvidia-peermem loaded"
    else
        echo "[WARN] nvidia-peermem not loaded"
    fi
fi

if [ "${ARCH}" = "aarch64" ]; then
    echo ""
    echo "DPU-side checks:"
    free -m || true
fi

echo ""
echo "Done."
