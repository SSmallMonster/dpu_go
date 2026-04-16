#ifndef DPU_DMA_REAL_H
#define DPU_DMA_REAL_H

#include <doca_dev.h>
#include "ctrl_channel.h"

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

#ifdef __cplusplus
}
#endif

#endif // DPU_DMA_REAL_H