/*
 * gpu_dma_copy.cu - Minimal host-side GPU DMA copy client
 *
 * Supported flows:
 *   1. push: host GPU -> DPU file
 *   2. pull: DPU file -> host GPU
 */

#include <cuda_runtime.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_mmap.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern "C" {
#include "ctrl_channel.h"
#include "dma_transfer.h"
#include "doca_device_utils.h"
}

#define DEFAULT_SERVICE_NAME "gpu_dpu_dma_copy"

#define CUDA_CHECK(call)                                                                  \
	do {                                                                                  \
		cudaError_t err__ = (call);                                                       \
		if (err__ != cudaSuccess) {                                                       \
			fprintf(stderr, "CUDA error at %s:%d: %s\n", __FILE__, __LINE__,              \
				cudaGetErrorString(err__));                                               \
			exit(EXIT_FAILURE);                                                            \
		}                                                                                 \
	} while (0)

static double elapsed_seconds(const struct timespec *start, const struct timespec *end)
{
	return (double)(end->tv_sec - start->tv_sec) +
	       (double)(end->tv_nsec - start->tv_nsec) / 1e9;
}

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

	result = doca_mmap_add_dev(mmap, dev);
	if (result != DOCA_SUCCESS)
		goto err;

	result = doca_mmap_set_memrange(mmap, gpu_ptr, size);
	if (result != DOCA_SUCCESS)
		goto err;

	result = doca_mmap_set_permissions(mmap, permissions);
	if (result != DOCA_SUCCESS)
		goto err;

	result = doca_mmap_start(mmap);
	if (result != DOCA_SUCCESS)
		goto err;

	result = doca_mmap_export_pci(mmap, dev, &desc, &desc_len);
	if (result != DOCA_SUCCESS)
		goto err;
	if (desc_len > DMA_EXPORT_DESC_MAX) {
		result = DOCA_ERROR_NO_MEMORY;
		goto err;
	}

	memcpy(export_desc, desc, desc_len);
	*export_desc_len = (uint32_t)desc_len;
	*mmap_out = mmap;
	return DOCA_SUCCESS;

err:
	if (mmap != NULL)
		(void)doca_mmap_destroy(mmap);
	return result;
}

static int read_file_to_host(const char *path, uint8_t **data_out, size_t *len_out)
{
	FILE *fp;
	long size;
	uint8_t *buf;

	fp = fopen(path, "rb");
	if (fp == NULL)
		return -1;
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return -1;
	}
	size = ftell(fp);
	if (size < 0) {
		fclose(fp);
		return -1;
	}
	if (fseek(fp, 0, SEEK_SET) != 0) {
		fclose(fp);
		return -1;
	}

	buf = (uint8_t *)malloc((size_t)size);
	if (buf == NULL) {
		fclose(fp);
		return -1;
	}
	if (fread(buf, 1, (size_t)size, fp) != (size_t)size) {
		free(buf);
		fclose(fp);
		return -1;
	}

	fclose(fp);
	*data_out = buf;
	*len_out = (size_t)size;
	return 0;
}

static int write_file_from_gpu(const char *path, void *gpu_ptr, uint64_t size)
{
	FILE *fp;
	uint8_t *host_buf;

	host_buf = (uint8_t *)malloc((size_t)size);
	if (host_buf == NULL)
		return -1;

	CUDA_CHECK(cudaMemcpy(host_buf, gpu_ptr, size, cudaMemcpyDeviceToHost));
	CUDA_CHECK(cudaDeviceSynchronize());

	fp = fopen(path, "wb");
	if (fp == NULL) {
		free(host_buf);
		return -1;
	}
	if (fwrite(host_buf, 1, (size_t)size, fp) != (size_t)size) {
		fclose(fp);
		free(host_buf);
		return -1;
	}

	fclose(fp);
	free(host_buf);
	return 0;
}

static void print_gpu_sample(void *gpu_ptr, uint64_t size)
{
	uint8_t sample[16] = {0};
	size_t sample_len = size < sizeof(sample) ? (size_t)size : sizeof(sample);

	if (sample_len == 0)
		return;

	CUDA_CHECK(cudaMemcpy(sample, gpu_ptr, sample_len, cudaMemcpyDeviceToHost));
	CUDA_CHECK(cudaDeviceSynchronize());

	printf("[HOST] GPU buffer sample:");
	for (size_t i = 0; i < sample_len; ++i)
		printf(" %02x", sample[i]);
	printf("\n");
}

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
	if (resp->magic != DMA_TRANSFER_MAGIC || resp->version != DMA_TRANSFER_VERSION)
		return -1;
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage (COMCH mode, default):\n"
		"  push: %s -o push -p <HOST_BF_PCI> -g <GPU_ID> -f <DPU_FILE> -s <SIZE_MiB> [-b fill] [-S service]\n"
		"  pull: %s -o pull -p <HOST_BF_PCI> -g <GPU_ID> -f <DPU_FILE> [-O LOCAL_OUT] [-S service]\n"
		"\n"
		"Usage (TCP mode, add -T <DPU_IP>):\n"
		"  push: %s -o push -p <HOST_BF_PCI> -g <GPU_ID> -f <DPU_FILE> -s <SIZE_MiB> -T <DPU_IP>\n"
		"  pull: %s -o pull -p <HOST_BF_PCI> -g <GPU_ID> -f <DPU_FILE> -T <DPU_IP> [-O LOCAL_OUT]\n"
		"\n"
		"  -T <DPU_IP>  Use TCP instead of COMCH (DPU must be started with -T flag, port %u)\n",
		prog, prog, prog, prog, DMA_TRANSFER_PORT);
}

int main(int argc, char **argv)
{
	const char *mode = NULL;
	const char *pci_addr = NULL;
	const char *dpu_file = NULL;
	const char *input_file = NULL;
	const char *output_file = NULL;
	const char *service_name = DEFAULT_SERVICE_NAME;
	const char *dpu_tcp_host = NULL;
	uint64_t size_mib = 0;
	unsigned long fill_byte = 0x5a;
	uint64_t request_id = 1;
	int gpu_id = 0;
	void *gpu_buffer = NULL;
	uint8_t *input_data = NULL;
	struct doca_dev *dev = NULL;
	struct doca_mmap *gpu_mmap = NULL;
	struct ctrl_channel *ch = NULL;
	dma_transfer_request_t req;
	dma_transfer_response_t resp;
	struct timespec start_ts = {0}, end_ts = {0};
	double end_to_end_seconds;
	doca_error_t result;
	int opt;

	while ((opt = getopt(argc, argv, "o:p:g:f:s:i:O:b:S:T:h")) != -1) {
		switch (opt) {
		case 'o':
			mode = optarg;
			break;
		case 'p':
			pci_addr = optarg;
			break;
		case 'g':
			gpu_id = atoi(optarg);
			break;
		case 'f':
			dpu_file = optarg;
			break;
		case 's':
			size_mib = strtoull(optarg, NULL, 10);
			break;
		case 'i':
			input_file = optarg;
			break;
		case 'O':
			output_file = optarg;
			break;
		case 'b':
			fill_byte = strtoul(optarg, NULL, 0);
			break;
		case 'S':
			service_name = optarg;
			break;
		case 'T':
			dpu_tcp_host = optarg;
			break;
		case 'h':
		default:
			usage(argv[0]);
			return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	if (mode == NULL || pci_addr == NULL || dpu_file == NULL) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	CUDA_CHECK(cudaSetDevice(gpu_id));
	{
		cudaDeviceProp prop;
		CUDA_CHECK(cudaGetDeviceProperties(&prop, gpu_id));
	printf("[HOST] Using GPU %d: %s\n", gpu_id, prop.name);
	}

	if (dpu_tcp_host != NULL) {
		result = ctrl_channel_tcp_client_create(dpu_tcp_host, DMA_TRANSFER_PORT, &ch);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "Failed to connect TCP to %s:%u: %s\n",
				dpu_tcp_host, DMA_TRANSFER_PORT, doca_error_get_descr(result));
			return EXIT_FAILURE;
		}
	} else {
		result = ctrl_channel_comch_client_create(service_name, pci_addr, &ch);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "Failed to create Comch client: %s\n", doca_error_get_descr(result));
			return EXIT_FAILURE;
		}
	}
	(void)ctrl_channel_wait_for_connection(ch);

	result = open_dma_device_by_pci(pci_addr, &dev);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to open DOCA device %s: %s\n", pci_addr, doca_error_get_descr(result));
		goto fail;
	}

	memset(&req, 0, sizeof(req));
	req.magic = DMA_TRANSFER_MAGIC;
	req.version = DMA_TRANSFER_VERSION;
	req.request_id = request_id++;
	snprintf(req.host_pci_addr, sizeof(req.host_pci_addr), "%s", pci_addr);
	snprintf(req.dpu_path, sizeof(req.dpu_path), "%s", dpu_file);
	req.remote_mem_type = DMA_REMOTE_MEM_GPU;

	if (strcmp(mode, "push") == 0) {
		uint64_t transfer_size;
		size_t input_len = 0;

		if (input_file != NULL) {
			if (read_file_to_host(input_file, &input_data, &input_len) != 0) {
				fprintf(stderr, "Failed to read %s\n", input_file);
				goto fail;
			}
			transfer_size = (uint64_t)input_len;
		} else if (size_mib > 0) {
			transfer_size = size_mib * 1024ULL * 1024ULL;
		} else {
			fprintf(stderr, "push mode requires -i or -s\n");
			goto fail;
		}

		CUDA_CHECK(cudaMalloc(&gpu_buffer, transfer_size));
		if (input_data != NULL) {
			CUDA_CHECK(cudaMemcpy(gpu_buffer, input_data, transfer_size, cudaMemcpyHostToDevice));
			free(input_data);
		} else {
			CUDA_CHECK(cudaMemset(gpu_buffer, (int)(fill_byte & 0xff), transfer_size));
		}
		CUDA_CHECK(cudaDeviceSynchronize());

		result = export_gpu_buffer(dev,
					   gpu_buffer,
					   transfer_size,
					   DOCA_ACCESS_FLAG_PCI_READ_ONLY,
					   &gpu_mmap,
					   req.export_desc,
					   &req.export_desc_len);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "Failed to export GPU memory: %s\n", doca_error_get_descr(result));
			goto fail;
		}

		req.type = DMA_REQ_PUSH_TO_DPU;
		req.remote_addr = (uint64_t)(uintptr_t)gpu_buffer;
		req.transfer_size_bytes = transfer_size;

		clock_gettime(CLOCK_MONOTONIC, &start_ts);
		if (send_request_and_recv_response(ch, &req, &resp) != 0)
			goto fail;
		clock_gettime(CLOCK_MONOTONIC, &end_ts);

		if (resp.status != 0) {
			fprintf(stderr, "DPU push failed: %u\n", resp.error_code);
			goto fail;
		}

		end_to_end_seconds = elapsed_seconds(&start_ts, &end_ts);
		printf("[HOST] Push complete: %" PRIu64 " bytes to %s\n", resp.transfer_size_bytes, dpu_file);
		printf("[HOST] End-to-end: %.6f sec, %.2f GB/s\n",
		       end_to_end_seconds,
		       ((double)resp.transfer_size_bytes / end_to_end_seconds) / 1e9);
		printf("[HOST] DPU DMA only: %.6f sec, %.2f GB/s\n",
		       resp.dma_seconds,
		       resp.dma_bandwidth_gbps);
		print_gpu_sample(gpu_buffer, resp.transfer_size_bytes);
	} else if (strcmp(mode, "pull") == 0) {
		req.type = DMA_REQ_PULL_INFO;
		if (send_request_and_recv_response(ch, &req, &resp) != 0)
			goto fail;
		if (resp.status != 0) {
			fprintf(stderr, "DPU pull-info failed: %u\n", resp.error_code);
			goto fail;
		}
		if (resp.transfer_size_bytes == 0) {
			fprintf(stderr, "DPU file is empty\n");
			goto fail;
		}

		CUDA_CHECK(cudaMalloc(&gpu_buffer, resp.transfer_size_bytes));
		CUDA_CHECK(cudaMemset(gpu_buffer, 0, resp.transfer_size_bytes));
		CUDA_CHECK(cudaDeviceSynchronize());

		result = export_gpu_buffer(dev,
					   gpu_buffer,
					   resp.transfer_size_bytes,
					   DOCA_ACCESS_FLAG_PCI_READ_WRITE,
					   &gpu_mmap,
					   req.export_desc,
					   &req.export_desc_len);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "Failed to export GPU memory: %s\n", doca_error_get_descr(result));
			goto fail;
		}

		req.type = DMA_REQ_PULL_TO_HOST;
		req.remote_addr = (uint64_t)(uintptr_t)gpu_buffer;
		req.transfer_size_bytes = resp.transfer_size_bytes;

		clock_gettime(CLOCK_MONOTONIC, &start_ts);
		if (send_request_and_recv_response(ch, &req, &resp) != 0)
			goto fail;
		clock_gettime(CLOCK_MONOTONIC, &end_ts);

		if (resp.status != 0) {
			fprintf(stderr, "DPU pull failed: %u\n", resp.error_code);
			goto fail;
		}

		end_to_end_seconds = elapsed_seconds(&start_ts, &end_ts);
		printf("[HOST] Pull complete: %" PRIu64 " bytes from %s\n", resp.transfer_size_bytes, dpu_file);
		printf("[HOST] End-to-end: %.6f sec, %.2f GB/s\n",
		       end_to_end_seconds,
		       ((double)resp.transfer_size_bytes / end_to_end_seconds) / 1e9);
		printf("[HOST] DPU DMA only: %.6f sec, %.2f GB/s\n",
		       resp.dma_seconds,
		       resp.dma_bandwidth_gbps);
		print_gpu_sample(gpu_buffer, resp.transfer_size_bytes);

		if (output_file != NULL) {
			if (write_file_from_gpu(output_file, gpu_buffer, resp.transfer_size_bytes) != 0) {
				fprintf(stderr, "Failed to write %s\n", output_file);
				goto fail;
			}
			printf("[HOST] Saved pulled data to %s\n", output_file);
		}
	} else {
		fprintf(stderr, "Unknown mode: %s\n", mode);
		goto fail;
	}

	if (gpu_mmap != NULL)
		(void)doca_mmap_destroy(gpu_mmap);
	if (gpu_buffer != NULL)
		(void)cudaFree(gpu_buffer);
	ctrl_channel_destroy(ch);
	(void)doca_dev_close(dev);
	return EXIT_SUCCESS;

fail:
	free(input_data);
	if (gpu_mmap != NULL)
		(void)doca_mmap_destroy(gpu_mmap);
	if (gpu_buffer != NULL)
		(void)cudaFree(gpu_buffer);
	ctrl_channel_destroy(ch);
	if (dev != NULL)
		(void)doca_dev_close(dev);
	return EXIT_FAILURE;
}
