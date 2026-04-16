#ifndef DOCA_DEVICE_UTILS_H
#define DOCA_DEVICE_UTILS_H

#include <stdbool.h>

#include <doca_dev.h>
#include <doca_error.h>

#ifdef __cplusplus
extern "C" {
#endif

doca_error_t open_dma_device_by_pci(const char *pci_addr, struct doca_dev **dev);
doca_error_t open_dma_device_with_net_representor_by_pci(const char *pci_addr, struct doca_dev **dev);
doca_error_t open_comch_device_by_pci(const char *pci_addr, bool server_mode, struct doca_dev **dev);
void print_doca_dma_devices(void);

#ifdef __cplusplus
}
#endif

#endif
