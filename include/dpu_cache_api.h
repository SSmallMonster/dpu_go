#ifndef DPU_CACHE_API_H
#define DPU_CACHE_API_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DPU_CACHE_SUCCESS 0
#define DPU_CACHE_ERROR -1
#define DPU_CACHE_KEY_NOT_FOUND -2

typedef struct {
    char dpu_ip[16];
    char host_pci_addr[32];
    int gpu_id;
    int max_concurrent_ops;
    int initialized;
} dpu_config_t;

// 初始化和清理
int dpu_cache_init(dpu_config_t* config);
int dpu_cache_cleanup(void);

// KV操作
int dpu_cache_store(const char* key_id,
                   void* k_data, size_t k_size, int k_dtype, int* k_shape, int k_ndim,
                   void* v_data, size_t v_size, int v_dtype, int* v_shape, int v_ndim);

int dpu_cache_retrieve(const char* key_id,
                      void** k_data, size_t* k_size, int* k_dtype, int* k_shape, int* k_ndim,
                      void** v_data, size_t* v_size, int* v_dtype, int* v_shape, int* v_ndim);

int dpu_cache_remove(const char* key_id);
int dpu_cache_contains(const char* key_id);

#ifdef __cplusplus
}
#endif

#endif // DPU_CACHE_API_H