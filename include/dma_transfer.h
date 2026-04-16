#ifndef DMA_TRANSFER_H
#define DMA_TRANSFER_H

#include <stdint.h>

#define DMA_TRANSFER_PORT 18517
#define DMA_TRANSFER_MAGIC 0x44545246U /* "DTRF" */
#define DMA_TRANSFER_VERSION 1U

#define DMA_PCI_ADDR_LEN 32
#define DMA_PATH_LEN 256
#define DMA_EXPORT_DESC_MAX 512

typedef enum {
	DMA_REMOTE_MEM_GPU = 1,
	DMA_REMOTE_MEM_CPU = 2,
} dma_remote_mem_t;

typedef enum {
	DMA_REQ_PUSH_TO_DPU = 1,
	DMA_REQ_PULL_INFO = 2,
	DMA_REQ_PULL_TO_HOST = 3,
} dma_request_type_t;

typedef struct {
	uint32_t magic;
	uint32_t version;
	uint32_t type;
	uint32_t remote_mem_type;
	uint64_t request_id;
	char host_pci_addr[DMA_PCI_ADDR_LEN];
	char dpu_path[DMA_PATH_LEN];
	uint64_t remote_addr;
	uint64_t transfer_size_bytes;
	uint32_t export_desc_len;
	uint8_t export_desc[DMA_EXPORT_DESC_MAX];
} dma_transfer_request_t;

typedef struct {
	uint32_t magic;
	uint32_t version;
	uint32_t status;
	uint32_t error_code;
	uint64_t request_id;
	uint64_t transfer_size_bytes;
	double dma_seconds;
	double io_seconds;
	double total_seconds;
	double dma_bandwidth_gbps;
} dma_transfer_response_t;

#endif
