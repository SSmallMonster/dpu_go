cmake_minimum_required(VERSION 3.18)
project(gpu_dpu_dma_copy LANGUAGES C)

set(CMAKE_C_STANDARD 11)

set(DOCA_DIR "/opt/mellanox/doca" CACHE PATH "Path to DOCA SDK installation")

find_path(DOCA_INCLUDE_DIR
    NAMES doca_dma.h
    PATHS ${DOCA_DIR}/include /opt/mellanox/doca/include /usr/include
    REQUIRED
)

find_library(DOCA_DMA_LIB doca_dma PATHS ${DOCA_DIR}/lib/x86_64-linux-gnu ${DOCA_DIR}/lib/aarch64-linux-gnu ${DOCA_DIR}/lib64 REQUIRED)
find_library(DOCA_COMMON_LIB doca_common PATHS ${DOCA_DIR}/lib/x86_64-linux-gnu ${DOCA_DIR}/lib/aarch64-linux-gnu ${DOCA_DIR}/lib64 REQUIRED)
find_library(DOCA_COMCH_LIB doca_comch PATHS ${DOCA_DIR}/lib/x86_64-linux-gnu ${DOCA_DIR}/lib/aarch64-linux-gnu ${DOCA_DIR}/lib64 REQUIRED)

include_directories(${PROJECT_SOURCE_DIR}/include ${DOCA_INCLUDE_DIR})

add_executable(dpu_dma_copy
    src/dpu_server_main.c
    src/dpu_dma_real.c
    src/ctrl_channel.c
    src/comch_ctrl.c
    src/doca_device_utils.c
)
target_link_libraries(dpu_dma_copy
    ${DOCA_DMA_LIB}
    ${DOCA_COMMON_LIB}
    ${DOCA_COMCH_LIB}
    pthread
)
target_compile_options(dpu_dma_copy PRIVATE -Wall -Wextra)

# 添加DPU Cache共享库 (带条件CUDA支持)
add_library(dpu_cache SHARED
    src/dpu_cache_api.c
    src/dpu_dma_real.c
    src/ctrl_channel.c
    src/comch_ctrl.c
    src/doca_device_utils.c
)
target_link_libraries(dpu_cache
    ${DOCA_DMA_LIB}
    ${DOCA_COMMON_LIB}
    ${DOCA_COMCH_LIB}
    pthread
)
target_compile_options(dpu_cache PRIVATE -Wall -Wextra -fPIC)

include(CheckLanguage)
check_language(CUDA)

if(CMAKE_CUDA_COMPILER AND NOT CMAKE_CROSSCOMPILING)
    enable_language(CUDA)
    set(CMAKE_CUDA_STANDARD 11)
    find_package(CUDAToolkit REQUIRED)

    # 添加CUDA支持到共享库
    target_link_libraries(dpu_cache CUDA::cudart)

    add_executable(gpu_dma_copy
        src/gpu_dma_copy.cu
        src/ctrl_channel.c
        src/comch_ctrl.c
        src/doca_device_utils.c
    )
    target_link_libraries(gpu_dma_copy
        CUDA::cudart
        ${DOCA_DMA_LIB}
        ${DOCA_COMMON_LIB}
        ${DOCA_COMCH_LIB}
    )
    target_compile_options(gpu_dma_copy PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler -Wall>
    )
endif()
