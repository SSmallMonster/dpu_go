#include "dpu_cache_api.h"
#include "ctrl_channel.h"
#include "dma_transfer.h"
#include "doca_device_utils.h"
#include "dpu_dma_real.h"

// 包含完整的DOCA头文件（参考dpu_dma_copy.c）
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_dma.h>
#include <doca_error.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <cuda_runtime.h>

// 全局配置
static dpu_config_t g_config = {0};
static struct ctrl_channel *g_ctrl_channel = NULL;
static struct doca_dev *g_doca_dev = NULL;

// KV文件头格式
typedef struct {
    char magic[4];           // "KVCH"
    uint32_t version;        // 版本号
    int32_t k_dtype;         // K tensor数据类型
    int32_t v_dtype;         // V tensor数据类型
    int32_t k_ndim;          // K tensor维度数
    int32_t v_ndim;          // V tensor维度数
    int32_t k_shape[4];      // K tensor形状
    int32_t v_shape[4];      // V tensor形状
    uint64_t k_size;         // K tensor字节数
    uint64_t v_size;         // V tensor字节数
    char reserved[8];        // 预留
} kv_header_t;

int dpu_cache_init(dpu_config_t* config) {
    if (!config) {
        return DPU_CACHE_ERROR;
    }
    printf("  - DPU IP address: %s\n", config->dpu_ip);
    memcpy(&g_config, config, sizeof(dpu_config_t));

    printf("===========================================\n");
    printf("  DPU Cache API Configuration              \n");
    printf("===========================================\n");
    printf("Configuration details:\n");
    printf("  - DPU IP address: %s\n", g_config.dpu_ip);
    printf("  - Host PCI address: %s\n", g_config.host_pci_addr);
    printf("  - GPU device ID: %d\n", g_config.gpu_id);
    printf("  - Max concurrent operations: %d\n", g_config.max_concurrent_ops);
    printf("  - Initialization status: %s\n", g_config.initialized ? "SUCCESS" : "PENDING");
    printf("-------------------------------------------\n");

    doca_error_t result;
	if (g_config.dpu_ip != NULL) {
		result = ctrl_channel_tcp_client_create(g_config.dpu_ip, DMA_TRANSFER_PORT, &g_ctrl_channel);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "Failed to connect TCP to %s:%u: %s\n",
				g_config.dpu_ip, DMA_TRANSFER_PORT, doca_error_get_descr(result));
			return EXIT_FAILURE;
		}
	} else {
		result = ctrl_channel_comch_client_create("dpu_copy", g_config.host_pci_addr, &g_ctrl_channel);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "Failed to create Comch client: %s\n", doca_error_get_descr(result));
			return EXIT_FAILURE;
		}
	}
	(void)ctrl_channel_wait_for_connection(g_ctrl_channel);
    
    // 初始化 DOCA 设备
    result = open_dma_device_by_pci(g_config.host_pci_addr, &g_doca_dev);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to open DOCA device %s: %s\n", g_config.host_pci_addr, doca_error_get_descr(result));
		return EXIT_FAILURE;
	}
    g_config.initialized = 1;

    printf("DPU Cache initialized successfully (simplified mode):\n");
    printf("  - DPU IP: %s\n", g_config.dpu_ip);
    printf("  - Host PCI: %s\n", g_config.host_pci_addr);
    printf("  - GPU: %d\n", g_config.gpu_id);
    printf("  - DOCA device: %s\n", g_doca_dev ? "initialized" : "not initialized");
    printf("  - Control channel: %s\n", g_ctrl_channel ? "initialized" : "not initialized");

    return DPU_CACHE_SUCCESS;
}

int dpu_cache_cleanup(void) {
    if (g_ctrl_channel) {
        // TODO: 添加控制通道清理
        g_ctrl_channel = NULL;
    }
    if (g_doca_dev) {
        // TODO: 添加DOCA设备清理
        g_doca_dev = NULL;
    }
    memset(&g_config, 0, sizeof(dpu_config_t));
    return DPU_CACHE_SUCCESS;
}

// 生成DPU端文件路径
static void generate_dpu_path(const char* key_id, char* dpu_path, size_t path_size) {
    snprintf(dpu_path, path_size, "/tmp/kv_cache_%s.bin", key_id);
}

// 创建KV头部
static void create_kv_header(kv_header_t* header,
                            int k_dtype, int* k_shape, int k_ndim, size_t k_size,
                            int v_dtype, int* v_shape, int v_ndim, size_t v_size) {
    memset(header, 0, sizeof(kv_header_t));
    memcpy(header->magic, "KVCH", 4);
    header->version = 1;
    header->k_dtype = k_dtype;
    header->v_dtype = v_dtype;
    header->k_ndim = k_ndim;
    header->v_ndim = v_ndim;
    header->k_size = k_size;
    header->v_size = v_size;

    // 复制shape，最多4维
    for (int i = 0; i < k_ndim && i < 4; i++) {
        header->k_shape[i] = k_shape[i];
    }
    for (int i = 0; i < v_ndim && i < 4; i++) {
        header->v_shape[i] = v_shape[i];
    }
}

// 真正的DMA传输函数
static int perform_dma_push(void* gpu_data, size_t total_size, const char* dpu_path) {
    printf("[DPU_CACHE] perform_dma_push called: gpu_data=%p, size=%zu, path=%s\n",
           gpu_data, total_size, dpu_path);

    if (!g_config.initialized) {
        printf("[DPU_CACHE ERROR] DPU Cache not initialized\n");
        fprintf(stderr, "[DPU_CACHE ERROR] DPU Cache not initialized\n");
        return DPU_CACHE_ERROR;
    }

    if (!g_doca_dev || !g_ctrl_channel) {
        printf("[DPU_CACHE ERROR] DOCA device or control channel not initialized (dev=%p, ch=%p)\n",
               g_doca_dev, g_ctrl_channel);
        fprintf(stderr, "[DPU_CACHE ERROR] DOCA device or control channel not initialized\n");
        return DPU_CACHE_ERROR;
    }

    printf("[DPU_CACHE] Performing real DMA push: size=%zu bytes to %s\n", total_size, dpu_path);

    int result = perform_real_dma_push(g_doca_dev, g_ctrl_channel,
                                     gpu_data, total_size, dpu_path,
                                     g_config.host_pci_addr);

    if (result != 0) {
        printf("[DPU_CACHE ERROR] perform_real_dma_push failed with code: %d\n", result);
        fprintf(stderr, "[DPU_CACHE ERROR] DMA push failed with error code: %d\n", result);
    } else {
        printf("[DPU_CACHE SUCCESS] DMA push completed successfully\n");
    }

    return result;
}

static int perform_dma_pull(const char* dpu_path, void* gpu_data, size_t total_size) {
    if (!g_config.initialized) {
        printf("DPU Cache not initialized\n");
        return DPU_CACHE_ERROR;
    }

    if (!g_doca_dev || !g_ctrl_channel) {
        printf("DOCA device or control channel not initialized\n");
        return DPU_CACHE_ERROR;
    }

    printf("Performing real DMA pull: %s to GPU, size=%zu bytes\n", dpu_path, total_size);

    return perform_real_dma_pull(g_doca_dev, g_ctrl_channel,
                               dpu_path, gpu_data, total_size,
                               g_config.host_pci_addr);
}

int dpu_cache_store(const char* key_id,
                   void* k_data, size_t k_size, int k_dtype, int* k_shape, int k_ndim,
                   void* v_data, size_t v_size, int v_dtype, int* v_shape, int v_ndim) {

    if (!g_config.initialized || !key_id || !k_data || !v_data) {
        return DPU_CACHE_ERROR;
    }

    // 生成DPU文件路径
    char dpu_path[256];
    generate_dpu_path(key_id, dpu_path, sizeof(dpu_path));

    // 创建KV头部
    kv_header_t header;
    create_kv_header(&header, k_dtype, k_shape, k_ndim, k_size,
                     v_dtype, v_shape, v_ndim, v_size);

    // 分配临时GPU内存用于组合头部和数据
    size_t total_size = sizeof(kv_header_t) + k_size + v_size;
    void* gpu_buffer;
    cudaError_t cuda_result = cudaMalloc(&gpu_buffer, total_size);
    if (cuda_result != cudaSuccess) {
        printf("Failed to allocate GPU memory: %s\n", cudaGetErrorString(cuda_result));
        return DPU_CACHE_ERROR;
    }

    // 检测k_data和v_data的内存位置
    struct cudaPointerAttributes k_attrs, v_attrs;
    bool k_is_gpu = (cudaPointerGetAttributes(&k_attrs, k_data) == cudaSuccess && 
                     k_attrs.type == cudaMemoryTypeDevice);
    bool v_is_gpu = (cudaPointerGetAttributes(&v_attrs, v_data) == cudaSuccess && 
                     v_attrs.type == cudaMemoryTypeDevice);

    // 复制数据到GPU buffer
    cudaError_t cuda_err;
    cuda_err = cudaMemcpy(gpu_buffer, &header, sizeof(kv_header_t), cudaMemcpyHostToDevice);
    if (cuda_err != cudaSuccess) {
        printf("[DPU_CACHE ERROR] Failed to copy header to GPU: %s\n", cudaGetErrorString(cuda_err));
        fprintf(stderr, "[DPU_CACHE ERROR] Header copy failed: %s\n", cudaGetErrorString(cuda_err));
        cudaFree(gpu_buffer);
        return DPU_CACHE_ERROR;
    }

    // 复制K张量（根据源位置选择拷贝类型）
    enum cudaMemcpyKind k_copy_kind = k_is_gpu ? cudaMemcpyDeviceToDevice : cudaMemcpyHostToDevice;
    cuda_err = cudaMemcpy((char*)gpu_buffer + sizeof(kv_header_t), k_data, k_size, k_copy_kind);
    if (cuda_err != cudaSuccess) {
        printf("[DPU_CACHE ERROR] Failed to copy K tensor: %s\n", cudaGetErrorString(cuda_err));
        fprintf(stderr, "[DPU_CACHE ERROR] K tensor copy failed: %s (size=%zu, kind=%s)\n",
                cudaGetErrorString(cuda_err), k_size, k_is_gpu ? "D2D" : "H2D");
        cudaFree(gpu_buffer);
        return DPU_CACHE_ERROR;
    }
    printf("K tensor copy: %s -> GPU (%s)\n",
       k_is_gpu ? "GPU" : "CPU",
       k_is_gpu ? "Device-to-Device" : "Host-to-Device");

    // 复制V张量（根据源位置选择拷贝类型）
    enum cudaMemcpyKind v_copy_kind = v_is_gpu ? cudaMemcpyDeviceToDevice : cudaMemcpyHostToDevice;
    cuda_err = cudaMemcpy((char*)gpu_buffer + sizeof(kv_header_t) + k_size, v_data, v_size, v_copy_kind);
    if (cuda_err != cudaSuccess) {
        printf("[DPU_CACHE ERROR] Failed to copy V tensor: %s\n", cudaGetErrorString(cuda_err));
        fprintf(stderr, "[DPU_CACHE ERROR] V tensor copy failed: %s (size=%zu, kind=%s)\n",
                cudaGetErrorString(cuda_err), v_size, v_is_gpu ? "D2D" : "H2D");
        cudaFree(gpu_buffer);
        return DPU_CACHE_ERROR;
    }
    printf("V tensor copy: %s -> GPU (%s)\n",
       v_is_gpu ? "GPU" : "CPU",
       v_is_gpu ? "Device-to-Device" : "Host-to-Device");    
    // cudaMemcpy((char*)gpu_buffer + sizeof(kv_header_t), k_data, k_size, cudaMemcpyDeviceToDevice);
    // cudaMemcpy((char*)gpu_buffer + sizeof(kv_header_t) + k_size, v_data, v_size, cudaMemcpyDeviceToDevice);

    // 执行DMA传输
    int result = perform_dma_push(gpu_buffer, total_size, dpu_path);

    // 清理GPU内存
    cudaFree(gpu_buffer);

    if (result == DPU_CACHE_SUCCESS) {
        printf("Successfully stored KV cache for key: %s\n", key_id);
    } else {
        printf("Failed to store KV cache for key: %s\n", key_id);
    }

    return result;
}

int dpu_cache_retrieve(const char* key_id,
                      void** k_data, size_t* k_size, int* k_dtype, int* k_shape, int* k_ndim,
                      void** v_data, size_t* v_size, int* v_dtype, int* v_shape, int* v_ndim) {

    if (!g_config.initialized || !key_id) {
        return DPU_CACHE_ERROR;
    }

    // 生成DPU文件路径
    char dpu_path[256];
    generate_dpu_path(key_id, dpu_path, sizeof(dpu_path));

    // TODO: 实现完整的检索逻辑
    printf("Retrieve operation for key: %s (not implemented yet)\n", key_id);

    return DPU_CACHE_KEY_NOT_FOUND;
}

int dpu_cache_remove(const char* key_id) {
    if (!g_config.initialized || !key_id) {
        return DPU_CACHE_ERROR;
    }

    // 生成DPU文件路径
    char dpu_path[256];
    generate_dpu_path(key_id, dpu_path, sizeof(dpu_path));

    printf("Remove operation for key: %s -> %s\n", key_id, dpu_path);

    // TODO: 发送删除文件的命令到DPU

    return DPU_CACHE_SUCCESS;
}

int dpu_cache_contains(const char* key_id) {
    if (!g_config.initialized || !key_id) {
        return DPU_CACHE_ERROR;
    }

    // 生成DPU文件路径
    char dpu_path[256];
    generate_dpu_path(key_id, dpu_path, sizeof(dpu_path));

    printf("Contains check for key: %s -> %s\n", key_id, dpu_path);

    // TODO: 检查DPU上文件是否存在

    return DPU_CACHE_KEY_NOT_FOUND;  // 默认返回不存在
}