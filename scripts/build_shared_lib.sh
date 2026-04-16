#!/bin/bash

# Build DPU Cache shared library
# This script builds libdpu_cache.so for Python ctypes integration

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"

echo "Building DPU Cache Shared Library"
echo "=================================="
echo "Project root: $PROJECT_ROOT"
echo "Build directory: $BUILD_DIR"
echo ""

# Create build directory
echo "Step 1: Creating build directory..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Run CMake configuration
echo "Step 2: Running CMake configuration..."
cmake "$PROJECT_ROOT"

# Check if CMake succeeded
if [ $? -ne 0 ]; then
    echo "Error: CMake configuration failed!"
    exit 1
fi

# Build only the shared library
echo "Step 3: Building libdpu_cache.so..."
make dpu_cache -j$(nproc)

# Check if build succeeded
if [ $? -ne 0 ]; then
    echo "Error: Build failed!"
    exit 1
fi

# Check if the shared library was created
if [ ! -f "$BUILD_DIR/libdpu_cache.so" ]; then
    echo "Error: libdpu_cache.so not found!"
    exit 1
fi

echo ""
echo "Build completed successfully!"
echo "Shared library: $BUILD_DIR/libdpu_cache.so"

# Show library info
echo ""
echo "Library information:"
file "$BUILD_DIR/libdpu_cache.so"
ls -lh "$BUILD_DIR/libdpu_cache.so"

# Show exported symbols (if nm is available)
if command -v nm >/dev/null 2>&1; then
    echo ""
    echo "Exported DPU Cache API functions:"
    nm -D "$BUILD_DIR/libdpu_cache.so" 2>/dev/null | grep -E "dpu_cache_" | grep " T " || echo "No symbols found (may be stripped)"
fi

echo ""
echo "Next steps:"
echo "1. Run Python integration: ./build_python_package.sh"
echo "2. Or manually copy to Python package: cp build/libdpu_cache.so python/dpu_cache/native/"
echo "3. Test with: python3 example.py"