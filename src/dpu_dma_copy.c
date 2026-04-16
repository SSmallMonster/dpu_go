/*
 * dpu_dma_copy.c - Minimal DPU-side DMA copy service
 *
 * Control plane: DOCA Comch
 * Data plane: DOCA DMA with remote mmap import
 *
 * Supported flows:
 *   1. Host GPU -> DPU file
 *   2. DPU file -> Host GPU
 */

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_comch.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_dma.h>
#include <doca_error.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ctrl_channel.h"
#include "dma_transfer.h"
#include "doca_device_utils.h"

#define DEFAULT_STAGE_MIB 256ULL
#define DEFAULT_QUEUE_DEPTH 4U
#define DEFAULT_SERVICE_NAME "gpu_dpu_dma_copy"

struct transfer_stats {
	double dma_seconds;
	double io_seconds;
	double total_seconds;
	double dma_bandwidth_gbps;
};

struct dma_runtime {
	struct doca_dev *dev;
	struct doca_dma *dma;
	struct doca_ctx *ctx;
	struct doca_pe *pe;
	struct doca_buf_inventory *buf_inv;
	struct doca_mmap *local_mmap;
	void *stage_buffer;
	uint64_t stage_size;
	uint64_t max_dma_size;
	uint64_t chunk_size;
	uint32_t queue_depth;
	size_t num_remaining_tasks;
};

struct dma_slot {
	struct doca_buf *src_buf;
	struct doca_buf *dst_buf;
	struct doca_dma_task_memcpy *task;
	doca_error_t task_result;
	size_t size;
	void *local_addr;
};

static void destroy_runtime(struct dma_runtime *runtime);

static double elapsed_seconds(const struct timespec *start, const struct timespec *end)
{
	return (double)(end->tv_sec - start->tv_sec) +
	       (double)(end->tv_nsec - start->tv_nsec) / 1e9;
}

static void dma_task_completed_cb(struct doca_dma_task_memcpy *task,
				  union doca_data task_user_data,
				  union doca_data ctx_user_data)
{
	struct dma_runtime *runtime = ctx_user_data.ptr;
	doca_error_t *slot_result = task_user_data.ptr;

	(void)task;
	*slot_result = DOCA_SUCCESS;
	--runtime->num_remaining_tasks;
}

static void dma_task_error_cb(struct doca_dma_task_memcpy *task,
			      union doca_data task_user_data,
			      union doca_data ctx_user_data)
{
	struct dma_runtime *runtime = ctx_user_data.ptr;
	doca_error_t *slot_result = task_user_data.ptr;

	*slot_result = doca_task_get_status(doca_dma_task_memcpy_as_task(task));
	--runtime->num_remaining_tasks;
}

static void init_response(dma_transfer_response_t *resp,
			  const dma_transfer_request_t *req,
			  doca_error_t result)
{
	memset(resp, 0, sizeof(*resp));
	resp->magic = DMA_TRANSFER_MAGIC;
	resp->version = DMA_TRANSFER_VERSION;
	resp->request_id = req->request_id;
	resp->status = result == DOCA_SUCCESS ? 0U : 1U;
	resp->error_code = (uint32_t)result;
}

static doca_error_t create_runtime(struct dma_runtime *runtime,
				   const char *pci_addr,
				   uint64_t stage_size_bytes,
				   uint64_t requested_chunk_size,
				   uint32_t queue_depth)
{
	union doca_data ctx_data = {0};
	uint64_t stage_chunk_limit;
	doca_error_t result;

	memset(runtime, 0, sizeof(*runtime));

	result = open_dma_device_by_pci(pci_addr, &runtime->dev);
	if (result != DOCA_SUCCESS)
		goto fail;

	runtime->queue_depth = queue_depth;
	runtime->stage_size = stage_size_bytes;

	if (posix_memalign(&runtime->stage_buffer, 4096, runtime->stage_size) != 0) {
		result = DOCA_ERROR_NO_MEMORY;
		goto fail;
	}
	memset(runtime->stage_buffer, 0, runtime->stage_size);

	result = doca_pe_create(&runtime->pe);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_dma_create(runtime->dev, &runtime->dma);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_dma_cap_task_memcpy_get_max_buf_size(doca_dev_as_devinfo(runtime->dev), &runtime->max_dma_size);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_dma_task_memcpy_set_conf(runtime->dma,
					       dma_task_completed_cb,
					       dma_task_error_cb,
					       runtime->queue_depth);
	if (result != DOCA_SUCCESS)
		goto fail;

	runtime->ctx = doca_dma_as_ctx(runtime->dma);
	result = doca_pe_connect_ctx(runtime->pe, runtime->ctx);
	if (result != DOCA_SUCCESS)
		goto fail;

	ctx_data.ptr = runtime;
	result = doca_ctx_set_user_data(runtime->ctx, ctx_data);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_ctx_start(runtime->ctx);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_buf_inventory_create(runtime->queue_depth * 2, &runtime->buf_inv);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_buf_inventory_start(runtime->buf_inv);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_mmap_create(&runtime->local_mmap);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_mmap_add_dev(runtime->local_mmap, runtime->dev);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_mmap_set_memrange(runtime->local_mmap, runtime->stage_buffer, runtime->stage_size);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_mmap_set_permissions(runtime->local_mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_mmap_start(runtime->local_mmap);
	if (result != DOCA_SUCCESS)
		goto fail;

	stage_chunk_limit = runtime->stage_size / runtime->queue_depth;
	if (stage_chunk_limit == 0)
		goto fail_invalid;

	runtime->chunk_size = requested_chunk_size == 0 ? runtime->max_dma_size : requested_chunk_size;
	if (runtime->chunk_size > runtime->max_dma_size)
		runtime->chunk_size = runtime->max_dma_size;
	if (runtime->chunk_size > stage_chunk_limit)
		runtime->chunk_size = stage_chunk_limit;
	if (runtime->chunk_size == 0)
		goto fail_invalid;

	return DOCA_SUCCESS;

fail_invalid:
	result = DOCA_ERROR_INVALID_VALUE;
fail:
	destroy_runtime(runtime);
	return result;
}

static void destroy_runtime(struct dma_runtime *runtime)
{
	if (runtime->local_mmap != NULL)
		(void)doca_mmap_destroy(runtime->local_mmap);
	if (runtime->buf_inv != NULL) {
		(void)doca_buf_inventory_stop(runtime->buf_inv);
		(void)doca_buf_inventory_destroy(runtime->buf_inv);
	}
	if (runtime->ctx != NULL) {
		(void)doca_ctx_stop(runtime->ctx);
	}
	if (runtime->dma != NULL)
		(void)doca_dma_destroy(runtime->dma);
	if (runtime->pe != NULL)
		(void)doca_pe_destroy(runtime->pe);
	if (runtime->dev != NULL)
		(void)doca_dev_close(runtime->dev);
	free(runtime->stage_buffer);
	memset(runtime, 0, sizeof(*runtime));
}

static void release_slot(struct dma_slot *slot)
{
	if (slot->task != NULL) {
		doca_task_free(doca_dma_task_memcpy_as_task(slot->task));
		slot->task = NULL;
	}
	if (slot->src_buf != NULL) {
		(void)doca_buf_dec_refcount(slot->src_buf, NULL);
		slot->src_buf = NULL;
	}
	if (slot->dst_buf != NULL) {
		(void)doca_buf_dec_refcount(slot->dst_buf, NULL);
		slot->dst_buf = NULL;
	}
}

static doca_error_t submit_dma_batch(struct dma_runtime *runtime,
				     struct doca_mmap *remote_mmap,
				     uint64_t remote_addr_base,
				     uint64_t file_offset,
				     uint64_t bytes_left,
				     bool pull_from_remote,
				     FILE *fp,
				     size_t *processed_bytes,
				     double *dma_seconds,
				     double *io_seconds)
{
	struct dma_slot *slots = NULL;
	struct timespec dma_start = {0}, dma_end = {0};
	struct timespec io_start = {0}, io_end = {0};
	size_t slot_count = 0;
	doca_error_t result = DOCA_SUCCESS;

	slots = calloc(runtime->queue_depth, sizeof(*slots));
	if (slots == NULL)
		return DOCA_ERROR_NO_MEMORY;

	while (slot_count < runtime->queue_depth && bytes_left > 0) {
		struct dma_slot *slot = &slots[slot_count];
		size_t curr_size = (size_t)(bytes_left > runtime->chunk_size ? runtime->chunk_size : bytes_left);
		uint64_t remote_addr = remote_addr_base + file_offset + *processed_bytes;
		union doca_data task_data = {0};
		struct doca_buf *src_buf;
		struct doca_buf *dst_buf;

		slot->local_addr = (uint8_t *)runtime->stage_buffer + slot_count * runtime->chunk_size;
		slot->size = curr_size;
		slot->task_result = DOCA_ERROR_UNKNOWN;

		if (!pull_from_remote) {
			clock_gettime(CLOCK_MONOTONIC, &io_start);
			if (fread(slot->local_addr, 1, curr_size, fp) != curr_size) {
				result = DOCA_ERROR_IO_FAILED;
				goto cleanup;
			}
			clock_gettime(CLOCK_MONOTONIC, &io_end);
			*io_seconds += elapsed_seconds(&io_start, &io_end);
		}

		result = doca_buf_inventory_buf_get_by_addr(runtime->buf_inv,
							    runtime->local_mmap,
							    slot->local_addr,
							    curr_size,
							    &slot->src_buf);
		if (result != DOCA_SUCCESS)
			goto cleanup;

		result = doca_buf_inventory_buf_get_by_addr(runtime->buf_inv,
							    remote_mmap,
							    (void *)(uintptr_t)remote_addr,
							    curr_size,
							    &slot->dst_buf);
		if (result != DOCA_SUCCESS)
			goto cleanup;

		if (pull_from_remote) {
			src_buf = slot->dst_buf;
			dst_buf = slot->src_buf;
			result = doca_buf_set_data(src_buf, (void *)(uintptr_t)remote_addr, curr_size);
		} else {
			src_buf = slot->src_buf;
			dst_buf = slot->dst_buf;
			result = doca_buf_set_data(src_buf, slot->local_addr, curr_size);
		}
		if (result != DOCA_SUCCESS)
			goto cleanup;

		task_data.ptr = &slot->task_result;
		result = doca_dma_task_memcpy_alloc_init(runtime->dma, src_buf, dst_buf, task_data, &slot->task);
		if (result != DOCA_SUCCESS)
			goto cleanup;

		result = doca_task_submit(doca_dma_task_memcpy_as_task(slot->task));
		if (result != DOCA_SUCCESS)
			goto cleanup;

		runtime->num_remaining_tasks++;
		slot_count++;
		bytes_left -= curr_size;
		*processed_bytes += curr_size;
	}

	clock_gettime(CLOCK_MONOTONIC, &dma_start);
	while (runtime->num_remaining_tasks > 0) {
		if (doca_pe_progress(runtime->pe) == 0) {
			struct timespec ts = {.tv_sec = 0, .tv_nsec = 10000};
			nanosleep(&ts, &ts);
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &dma_end);
	*dma_seconds += elapsed_seconds(&dma_start, &dma_end);

	for (size_t i = 0; i < slot_count; ++i) {
		if (slots[i].task_result != DOCA_SUCCESS) {
			result = slots[i].task_result;
			goto cleanup;
		}
	}

	if (pull_from_remote) {
		for (size_t i = 0; i < slot_count; ++i) {
			clock_gettime(CLOCK_MONOTONIC, &io_start);
			if (fwrite(slots[i].local_addr, 1, slots[i].size, fp) != slots[i].size) {
				result = DOCA_ERROR_IO_FAILED;
				goto cleanup;
			}
			clock_gettime(CLOCK_MONOTONIC, &io_end);
			*io_seconds += elapsed_seconds(&io_start, &io_end);
		}
	}

cleanup:
	while (runtime->num_remaining_tasks > 0) {
		if (doca_pe_progress(runtime->pe) == 0) {
			struct timespec ts = {.tv_sec = 0, .tv_nsec = 10000};
			nanosleep(&ts, &ts);
		}
	}
	for (size_t i = 0; i < runtime->queue_depth; ++i)
		release_slot(&slots[i]);
	free(slots);
	return result;
}

static doca_error_t stream_remote_and_file(struct dma_runtime *runtime,
					   struct doca_mmap *remote_mmap,
					   uint64_t remote_addr,
					   uint64_t total_size,
					   bool pull_from_remote,
					   FILE *fp,
					   struct transfer_stats *stats)
{
	struct timespec total_start = {0}, total_end = {0};
	size_t processed = 0;
	doca_error_t result = DOCA_SUCCESS;

	memset(stats, 0, sizeof(*stats));
	clock_gettime(CLOCK_MONOTONIC, &total_start);

	while (processed < total_size) {
		result = submit_dma_batch(runtime,
					  remote_mmap,
					  remote_addr,
					  0,
					  total_size - processed,
					  pull_from_remote,
					  fp,
					  &processed,
					  &stats->dma_seconds,
					  &stats->io_seconds);
		if (result != DOCA_SUCCESS)
			break;
	}

	clock_gettime(CLOCK_MONOTONIC, &total_end);
	stats->total_seconds = elapsed_seconds(&total_start, &total_end);
	if (stats->dma_seconds > 0.0)
		stats->dma_bandwidth_gbps = ((double)total_size / stats->dma_seconds) / 1e9;

	return result;
}

static doca_error_t handle_push_to_dpu(struct dma_runtime *runtime,
				       const dma_transfer_request_t *req,
				       dma_transfer_response_t *resp)
{
	struct doca_mmap *remote_mmap = NULL;
	struct transfer_stats stats;
	FILE *fp = NULL;
	doca_error_t result;

	fp = fopen(req->dpu_path, "wb");
	if (fp == NULL)
		return DOCA_ERROR_IO_FAILED;

	result = doca_mmap_create_from_export(NULL,
					      req->export_desc,
					      req->export_desc_len,
					      runtime->dev,
					      &remote_mmap);
	if (result != DOCA_SUCCESS)
		goto out;

	result = stream_remote_and_file(runtime,
					remote_mmap,
					req->remote_addr,
					req->transfer_size_bytes,
					true,
					fp,
					&stats);
	if (result == DOCA_SUCCESS) {
		resp->transfer_size_bytes = req->transfer_size_bytes;
		resp->dma_seconds = stats.dma_seconds;
		resp->io_seconds = stats.io_seconds;
		resp->total_seconds = stats.total_seconds;
		resp->dma_bandwidth_gbps = stats.dma_bandwidth_gbps;
		printf("[DPU] PUSH %s: %" PRIu64 " bytes, dma=%.6f sec, io=%.6f sec, %.2f GB/s\n",
		       req->dpu_path,
		       req->transfer_size_bytes,
		       stats.dma_seconds,
		       stats.io_seconds,
		       stats.dma_bandwidth_gbps);
	}

out:
	if (remote_mmap != NULL)
		(void)doca_mmap_destroy(remote_mmap);
	if (fp != NULL)
		fclose(fp);
	return result;
}

static doca_error_t handle_pull_info(const dma_transfer_request_t *req, dma_transfer_response_t *resp)
{
	struct stat st;

	if (stat(req->dpu_path, &st) != 0)
		return DOCA_ERROR_NOT_FOUND;
	if (!S_ISREG(st.st_mode))
		return DOCA_ERROR_INVALID_VALUE;

	resp->transfer_size_bytes = (uint64_t)st.st_size;
	return DOCA_SUCCESS;
}

static doca_error_t handle_pull_to_host(struct dma_runtime *runtime,
					const dma_transfer_request_t *req,
					dma_transfer_response_t *resp)
{
	struct doca_mmap *remote_mmap = NULL;
	struct transfer_stats stats;
	FILE *fp = NULL;
	struct stat st;
	doca_error_t result;

	if (stat(req->dpu_path, &st) != 0)
		return DOCA_ERROR_NOT_FOUND;
	if ((uint64_t)st.st_size != req->transfer_size_bytes)
		return DOCA_ERROR_INVALID_VALUE;

	fp = fopen(req->dpu_path, "rb");
	if (fp == NULL)
		return DOCA_ERROR_IO_FAILED;

	result = doca_mmap_create_from_export(NULL,
					      req->export_desc,
					      req->export_desc_len,
					      runtime->dev,
					      &remote_mmap);
	if (result != DOCA_SUCCESS)
		goto out;

	result = stream_remote_and_file(runtime,
					remote_mmap,
					req->remote_addr,
					req->transfer_size_bytes,
					false,
					fp,
					&stats);
	if (result == DOCA_SUCCESS) {
		resp->transfer_size_bytes = req->transfer_size_bytes;
		resp->dma_seconds = stats.dma_seconds;
		resp->io_seconds = stats.io_seconds;
		resp->total_seconds = stats.total_seconds;
		resp->dma_bandwidth_gbps = stats.dma_bandwidth_gbps;
		printf("[DPU] PULL %s: %" PRIu64 " bytes, dma=%.6f sec, io=%.6f sec, %.2f GB/s\n",
		       req->dpu_path,
		       req->transfer_size_bytes,
		       stats.dma_seconds,
		       stats.io_seconds,
		       stats.dma_bandwidth_gbps);
	}

out:
	if (remote_mmap != NULL)
		(void)doca_mmap_destroy(remote_mmap);
	if (fp != NULL)
		fclose(fp);
	return result;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s -p <DPU_DMA_PCI> [-r rep_pci] [-m stage_mib] [-c chunk_mib] [-q queue_depth] [-S service] [-T]\n"
		"  -T   Use TCP control channel instead of COMCH (listens on port %u)\n",
		prog, DMA_TRANSFER_PORT);
}

int main(int argc, char **argv)
{
	struct dma_runtime runtime;
	const char *pci_addr = NULL;
	const char *rep_pci_addr = NULL;
	const char *service_name = DEFAULT_SERVICE_NAME;
	uint64_t stage_mib = DEFAULT_STAGE_MIB;
	uint64_t chunk_mib = 0;
	uint32_t queue_depth = DEFAULT_QUEUE_DEPTH;
	int use_tcp = 0;
	struct ctrl_channel *ch = NULL;
	int opt;
	doca_error_t result;

	while ((opt = getopt(argc, argv, "p:r:m:c:q:S:Th")) != -1) {
		switch (opt) {
		case 'p':
			pci_addr = optarg;
			break;
		case 'r':
			rep_pci_addr = optarg;
			break;
		case 'm':
			stage_mib = strtoull(optarg, NULL, 10);
			break;
		case 'c':
			chunk_mib = strtoull(optarg, NULL, 10);
			break;
		case 'q':
			queue_depth = (uint32_t)strtoul(optarg, NULL, 10);
			break;
		case 'S':
			service_name = optarg;
			break;
		case 'T':
			use_tcp = 1;
			break;
		case 'h':
		default:
			usage(argv[0]);
			return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	if (pci_addr == NULL || stage_mib == 0 || queue_depth == 0) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	print_doca_dma_devices();
	result = create_runtime(&runtime,
				pci_addr,
				stage_mib * 1024ULL * 1024ULL,
				chunk_mib * 1024ULL * 1024ULL,
				queue_depth);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to initialize DMA runtime: %s\n", doca_error_get_descr(result));
		return EXIT_FAILURE;
	}

	printf("===========================================\n");
	printf("  Minimal DPU DMA Copy Server             \n");
	printf("===========================================\n");
	printf("[DPU] PCI: %s\n", pci_addr);
	printf("[DPU] Stage buffer: %" PRIu64 " MiB\n", stage_mib);
	printf("[DPU] Max DMA chunk supported by device: %" PRIu64 " bytes\n", runtime.max_dma_size);
	printf("[DPU] Using chunk size: %" PRIu64 " bytes, queue depth: %u\n", runtime.chunk_size, runtime.queue_depth);

	if (use_tcp) {
		result = ctrl_channel_tcp_server_create(DMA_TRANSFER_PORT, &ch);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "Failed to create TCP server: %s\n", doca_error_get_descr(result));
			destroy_runtime(&runtime);
			return EXIT_FAILURE;
		}
		printf("[DPU] Waiting for TCP client on port %u\n", DMA_TRANSFER_PORT);
	} else {
		result = ctrl_channel_comch_server_create(service_name, pci_addr, rep_pci_addr, &ch);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "Failed to create Comch server: %s\n", doca_error_get_descr(result));
			destroy_runtime(&runtime);
			return EXIT_FAILURE;
		}
		printf("[DPU] Waiting for Comch client on service '%s'\n", service_name);
	}
	(void)ctrl_channel_wait_for_connection(ch);
	printf("[DPU] Client connected\n");

	for (;;) {
		dma_transfer_request_t req;
		dma_transfer_response_t resp;
		uint32_t msg_len = 0;

		result = ctrl_channel_wait_for_message(ch, &req, sizeof(req), &msg_len);
		if (result != DOCA_SUCCESS)
			break;
		if (msg_len != sizeof(req) || req.magic != DMA_TRANSFER_MAGIC || req.version != DMA_TRANSFER_VERSION)
			break;

		init_response(&resp, &req, DOCA_SUCCESS);
		switch (req.type) {
		case DMA_REQ_PUSH_TO_DPU:
			result = handle_push_to_dpu(&runtime, &req, &resp);
			break;
		case DMA_REQ_PULL_INFO:
			result = handle_pull_info(&req, &resp);
			break;
		case DMA_REQ_PULL_TO_HOST:
			result = handle_pull_to_host(&runtime, &req, &resp);
			break;
		default:
			result = DOCA_ERROR_INVALID_VALUE;
			break;
		}

		resp.status = result == DOCA_SUCCESS ? 0U : 1U;
		resp.error_code = (uint32_t)result;
		result = ctrl_channel_send(ch, &resp, sizeof(resp));
		if (result != DOCA_SUCCESS)
			break;
		(void)ctrl_channel_progress(ch);
	}

	ctrl_channel_destroy(ch);
	destroy_runtime(&runtime);
	return EXIT_SUCCESS;
}
