#include "dpu_cache_api.h"
#include "ctrl_channel.h"
#include "dma_transfer.h"
#include "doca_device_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <time.h>
#include <cuda_runtime.h>
#include <doca_buf.h>
#include <doca_dev.h>
#include <doca_mmap.h>
#include <doca_error.h>

// 导出GPU buffer函数（从gpu_dma_copy.cu提取）
static doca_error_t export_gpu_buffer(struct doca_dev *dev,
                                      void *gpu_ptr,
                                      uint64_t size,
                                      uint32_t permissions,
                                      struct doca_mmap **mmap_out,
                                      uint8_t *export_desc,
                                      uint32_t *export_desc_len)
{
    struct doca_mmap *mmap = NULL;
    const void *desc = NULL;
    size_t desc_len = 0;
    doca_error_t result;

    result = doca_mmap_create(&mmap);
    if (result != DOCA_SUCCESS)
        return result;

    result = doca_mmap_set_dev(mmap, dev);
    if (result != DOCA_SUCCESS) {
        doca_mmap_destroy(mmap);
        return result;
    }

    result = doca_mmap_add_dev_remote_pci(mmap, dev);
    if (result != DOCA_SUCCESS) {
        doca_mmap_destroy(mmap);
        return result;
    }

    result = doca_mmap_populate(mmap, gpu_ptr, size, PAGE_SIZE, permissions, DOCA_MMAP_POPULATE_FLAGS_NONE);
    if (result != DOCA_SUCCESS) {
        doca_mmap_destroy(mmap);
        return result;
    }

    result = doca_mmap_start(mmap);
    if (result != DOCA_SUCCESS) {
        doca_mmap_destroy(mmap);
        return result;
    }

    result = doca_mmap_export_pci(mmap, dev, &desc, &desc_len);
    if (result != DOCA_SUCCESS) {
        doca_mmap_stop(mmap);
        doca_mmap_destroy(mmap);
        return result;
    }

    if (desc_len > DMA_EXPORT_DESC_MAX) {
        printf("Export descriptor too large: %zu > %d\n", desc_len, DMA_EXPORT_DESC_MAX);
        doca_mmap_stop(mmap);
        doca_mmap_destroy(mmap);
        return DOCA_ERROR_INVALID_VALUE;
    }

    memcpy(export_desc, desc, desc_len);
    *export_desc_len = desc_len;
    *mmap_out = mmap;

    return DOCA_SUCCESS;
}

// 发送请求并接收响应函数（从gpu_dma_copy.cu提取）
static int send_request_and_recv_response(struct ctrl_channel *ch,
                                         const dma_transfer_request_t *req,
                                         dma_transfer_response_t *resp)
{
    uint32_t msg_len = 0;

    if (ctrl_channel_send(ch, req, sizeof(*req)) != DOCA_SUCCESS)
        return -1;
    if (ctrl_channel_wait_for_message(ch, resp, sizeof(*resp), &msg_len) != DOCA_SUCCESS)
        return -1;
    if (msg_len != sizeof(*resp))
        return -1;

    return 0;
}

// 执行真正的DMA push操作
int perform_real_dma_push(struct doca_dev *dev, struct ctrl_channel *ch,
                         void* gpu_data, size_t total_size, const char* dpu_path,
                         const char* host_pci_addr)
{
    dma_transfer_request_t req = {0};
    dma_transfer_response_t resp = {0};
    struct doca_mmap *gpu_mmap = NULL;
    doca_error_t result;
    struct timespec start_ts, end_ts;

    // 设置请求基本信息
    req.magic = DMA_TRANSFER_MAGIC;
    req.version = DMA_TRANSFER_VERSION;
    req.remote_mem_type = DMA_REMOTE_MEM_GPU;
    req.request_id = rand();

    // 复制PCI地址和DPU路径
    strncpy(req.host_pci_addr, host_pci_addr, sizeof(req.host_pci_addr) - 1);
    strncpy(req.dpu_path, dpu_path, sizeof(req.dpu_path) - 1);

    // 导出GPU内存
    result = export_gpu_buffer(dev,
                              gpu_data,
                              total_size,
                              DOCA_ACCESS_FLAG_PCI_READ_ONLY,
                              &gpu_mmap,
                              req.export_desc,
                              &req.export_desc_len);
    if (result != DOCA_SUCCESS) {
        printf("Failed to export GPU memory: %s\n", doca_error_get_descr(result));
        return -1;
    }

    // 设置传输参数
    req.type = DMA_REQ_PUSH_TO_DPU;
    req.remote_addr = (uint64_t)(uintptr_t)gpu_data;
    req.transfer_size_bytes = total_size;

    printf("Performing real DMA push: %zu bytes to %s\n", total_size, dpu_path);

    // 发送请求并等待响应
    clock_gettime(CLOCK_MONOTONIC, &start_ts);
    if (send_request_and_recv_response(ch, &req, &resp) != 0) {
        printf("Failed to send DMA request\n");
        if (gpu_mmap) {
            doca_mmap_stop(gpu_mmap);
            doca_mmap_destroy(gpu_mmap);
        }
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &end_ts);

    // 清理GPU mmap
    if (gpu_mmap) {
        doca_mmap_stop(gpu_mmap);
        doca_mmap_destroy(gpu_mmap);
    }

    // 检查响应状态
    if (resp.status != 0) {
        printf("DPU push failed: error_code=%u\n", resp.error_code);
        return -1;
    }

    // 计算性能统计
    double end_to_end_seconds = (end_ts.tv_sec - start_ts.tv_sec) +
                               (end_ts.tv_nsec - start_ts.tv_nsec) / 1e9;

    printf("DMA push complete: %lu bytes\n", resp.transfer_size_bytes);
    printf("End-to-end: %.6f sec, %.2f GB/s\n",
           end_to_end_seconds,
           ((double)resp.transfer_size_bytes / end_to_end_seconds) / 1e9);
    printf("DPU DMA only: %.6f sec, %.2f GB/s\n",
           resp.dma_seconds,
           resp.dma_bandwidth_gbps);

    return 0;
}

// 执行真正的DMA pull操作
int perform_real_dma_pull(struct doca_dev *dev, struct ctrl_channel *ch,
                         const char* dpu_path, void* gpu_data, size_t total_size,
                         const char* host_pci_addr)
{
    dma_transfer_request_t req = {0};
    dma_transfer_response_t resp = {0};
    struct doca_mmap *gpu_mmap = NULL;
    doca_error_t result;
    struct timespec start_ts, end_ts;

    // 设置请求基本信息
    req.magic = DMA_TRANSFER_MAGIC;
    req.version = DMA_TRANSFER_VERSION;
    req.remote_mem_type = DMA_REMOTE_MEM_GPU;
    req.request_id = rand();

    // 复制PCI地址和DPU路径
    strncpy(req.host_pci_addr, host_pci_addr, sizeof(req.host_pci_addr) - 1);
    strncpy(req.dpu_path, dpu_path, sizeof(req.dpu_path) - 1);

    // 首先获取DPU文件信息
    req.type = DMA_REQ_PULL_INFO;
    if (send_request_and_recv_response(ch, &req, &resp) != 0) {
        printf("Failed to get DPU file info\n");
        return -1;
    }

    if (resp.status != 0) {
        printf("DPU pull-info failed: error_code=%u\n", resp.error_code);
        return -1;
    }

    if (resp.transfer_size_bytes == 0) {
        printf("DPU file is empty\n");
        return -1;
    }

    if (resp.transfer_size_bytes != total_size) {
        printf("Size mismatch: expected %zu, got %lu\n", total_size, resp.transfer_size_bytes);
        return -1;
    }

    // 导出GPU内存（用于接收数据）
    result = export_gpu_buffer(dev,
                              gpu_data,
                              total_size,
                              DOCA_ACCESS_FLAG_PCI_READ_WRITE,
                              &gpu_mmap,
                              req.export_desc,
                              &req.export_desc_len);
    if (result != DOCA_SUCCESS) {
        printf("Failed to export GPU memory: %s\n", doca_error_get_descr(result));
        return -1;
    }

    // 设置传输参数
    req.type = DMA_REQ_PULL_TO_HOST;
    req.remote_addr = (uint64_t)(uintptr_t)gpu_data;
    req.transfer_size_bytes = total_size;

    printf("Performing real DMA pull: %zu bytes from %s\n", total_size, dpu_path);

    // 发送请求并等待响应
    clock_gettime(CLOCK_MONOTONIC, &start_ts);
    if (send_request_and_recv_response(ch, &req, &resp) != 0) {
        printf("Failed to send DMA pull request\n");
        if (gpu_mmap) {
            doca_mmap_stop(gpu_mmap);
            doca_mmap_destroy(gpu_mmap);
        }
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &end_ts);

    // 清理GPU mmap
    if (gpu_mmap) {
        doca_mmap_stop(gpu_mmap);
        doca_mmap_destroy(gpu_mmap);
    }

    // 检查响应状态
    if (resp.status != 0) {
        printf("DPU pull failed: error_code=%u\n", resp.error_code);
        return -1;
    }

    // 计算性能统计
    double end_to_end_seconds = (end_ts.tv_sec - start_ts.tv_sec) +
                               (end_ts.tv_nsec - start_ts.tv_nsec) / 1e9;

    printf("DMA pull complete: %lu bytes\n", resp.transfer_size_bytes);
    printf("End-to-end: %.6f sec, %.2f GB/s\n",
           end_to_end_seconds,
           ((double)resp.transfer_size_bytes / end_to_end_seconds) / 1e9);
    printf("DPU DMA only: %.6f sec, %.2f GB/s\n",
           resp.dma_seconds,
           resp.dma_bandwidth_gbps);

    return 0;
}