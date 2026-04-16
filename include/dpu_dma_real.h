#ifndef DPU_DMA_REAL_H
#define DPU_DMA_REAL_H

#include <stddef.h>
#include <stdbool.h>

// 前向声明以避免头文件依赖问题
struct doca_dev;
struct ctrl_channel;

#ifdef __cplusplus
extern "C" {
#endif

// 执行真正的DMA push操作
int perform_real_dma_push(struct doca_dev *dev, struct ctrl_channel *ch,
                         void* gpu_data, size_t total_size, const char* dpu_path,
                         const char* host_pci_addr);

// 执行真正的DMA pull操作
int perform_real_dma_pull(struct doca_dev *dev, struct ctrl_channel *ch,
                         const char* dpu_path, void* gpu_data, size_t total_size,
                         const char* host_pci_addr);

// 运行完整的DMA服务器 - 新增的服务器端功能
int run_dma_server(const char *pci_addr, const char *rep_pci_addr,
                   const char *service_name, bool use_tcp);

#ifdef __cplusplus
}
#endif

#endif // DPU_DMA_REAL_H