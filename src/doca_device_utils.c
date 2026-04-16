#include <stdio.h>
#include <string.h>

#include <doca_dev.h>
#include <doca_comch.h>
#include <doca_dma.h>
#include <doca_error.h>

#include "doca_device_utils.h"

static doca_error_t check_dma_capable(struct doca_devinfo *devinfo)
{
	return doca_dma_cap_task_memcpy_is_supported(devinfo);
}

doca_error_t open_dma_device_by_pci(const char *pci_addr, struct doca_dev **dev)
{
	struct doca_devinfo **dev_list = NULL;
	uint32_t nb_devs = 0;
	doca_error_t result;

	*dev = NULL;

	result = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (result != DOCA_SUCCESS)
		return result;

	for (uint32_t i = 0; i < nb_devs; ++i) {
		uint8_t is_equal = 0;

		if (check_dma_capable(dev_list[i]) != DOCA_SUCCESS)
			continue;

		result = doca_devinfo_is_equal_pci_addr(dev_list[i], pci_addr, &is_equal);
		if (result != DOCA_SUCCESS || is_equal == 0)
			continue;

		result = doca_dev_open(dev_list[i], dev);
		doca_devinfo_destroy_list(dev_list);
		return result;
	}

	doca_devinfo_destroy_list(dev_list);
	return DOCA_ERROR_NOT_FOUND;
}

doca_error_t open_dma_device_with_net_representor_by_pci(const char *pci_addr, struct doca_dev **dev)
{
	struct doca_devinfo **dev_list = NULL;
	uint32_t nb_devs = 0;
	doca_error_t result;

	*dev = NULL;

	result = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (result != DOCA_SUCCESS)
		return result;

	for (uint32_t i = 0; i < nb_devs; ++i) {
		struct doca_dev *candidate = NULL;
		struct doca_devinfo_rep **rep_list = NULL;
		uint32_t nb_reps = 0;
		uint8_t is_equal = 0;

		if (check_dma_capable(dev_list[i]) != DOCA_SUCCESS)
			continue;

		result = doca_devinfo_is_equal_pci_addr(dev_list[i], pci_addr, &is_equal);
		if (result != DOCA_SUCCESS || is_equal == 0)
			continue;

		result = doca_dev_open(dev_list[i], &candidate);
		if (result != DOCA_SUCCESS)
			continue;

		result = doca_devinfo_rep_create_list(candidate, DOCA_DEVINFO_REP_FILTER_NET, &rep_list, &nb_reps);
		if (result == DOCA_SUCCESS && nb_reps > 0) {
			doca_devinfo_rep_destroy_list(rep_list);
			doca_devinfo_destroy_list(dev_list);
			*dev = candidate;
			return DOCA_SUCCESS;
		}

		if (result == DOCA_SUCCESS)
			doca_devinfo_rep_destroy_list(rep_list);
		(void)doca_dev_close(candidate);
	}

	doca_devinfo_destroy_list(dev_list);
	return DOCA_ERROR_NOT_FOUND;
}

doca_error_t open_comch_device_by_pci(const char *pci_addr, bool server_mode, struct doca_dev **dev)
{
	struct doca_devinfo **dev_list = NULL;
	uint32_t nb_devs = 0;
	doca_error_t result;
	const char *mode_name = server_mode ? "server" : "client";

	*dev = NULL;

	result = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (result != DOCA_SUCCESS)
		return result;

	for (uint32_t i = 0; i < nb_devs; ++i) {
		struct doca_dev *candidate = NULL;
		uint8_t is_equal = 0;
		char dev_pci[DOCA_DEVINFO_PCI_ADDR_SIZE] = {0};

		(void)doca_devinfo_get_pci_addr_str(dev_list[i], dev_pci);
		result = doca_devinfo_is_equal_pci_addr(dev_list[i], pci_addr, &is_equal);
		if (result != DOCA_SUCCESS || is_equal == 0)
			continue;

		if (server_mode)
			result = doca_comch_cap_server_is_supported(dev_list[i]);
		else
			result = doca_comch_cap_client_is_supported(dev_list[i]);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr,
				"[COMCH] Skip device %s: comch %s not supported: %s\n",
				dev_pci[0] != '\0' ? dev_pci : "<unknown>",
				mode_name,
				doca_error_get_descr(result));
			continue;
		}

		result = doca_dev_open(dev_list[i], &candidate);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr,
				"[COMCH] Skip device %s: doca_dev_open failed: %s\n",
				dev_pci[0] != '\0' ? dev_pci : "<unknown>",
				doca_error_get_descr(result));
			continue;
		}

		if (server_mode) {
			struct doca_devinfo_rep **rep_list = NULL;
			uint32_t nb_reps = 0;

			result = doca_devinfo_rep_create_list(candidate, DOCA_DEVINFO_REP_FILTER_NET, &rep_list, &nb_reps);
			if (result == DOCA_SUCCESS && nb_reps > 0) {
				fprintf(stderr,
					"[COMCH] Using device %s for comch server (%u net representors)\n",
					dev_pci[0] != '\0' ? dev_pci : "<unknown>",
					nb_reps);
				doca_devinfo_rep_destroy_list(rep_list);
				doca_devinfo_destroy_list(dev_list);
				*dev = candidate;
				return DOCA_SUCCESS;
			}
			if (result == DOCA_SUCCESS) {
				fprintf(stderr,
					"[COMCH] Skip device %s: no net representors\n",
					dev_pci[0] != '\0' ? dev_pci : "<unknown>");
			} else {
				fprintf(stderr,
					"[COMCH] Skip device %s: rep enumeration failed: %s\n",
					dev_pci[0] != '\0' ? dev_pci : "<unknown>",
					doca_error_get_descr(result));
			}
			if (result == DOCA_SUCCESS)
				doca_devinfo_rep_destroy_list(rep_list);
			(void)doca_dev_close(candidate);
			continue;
		}

		fprintf(stderr,
			"[COMCH] Using device %s for comch client\n",
			dev_pci[0] != '\0' ? dev_pci : "<unknown>");
		doca_devinfo_destroy_list(dev_list);
		*dev = candidate;
		return DOCA_SUCCESS;
	}

	doca_devinfo_destroy_list(dev_list);
	return DOCA_ERROR_NOT_FOUND;
}

void print_doca_dma_devices(void)
{
	struct doca_devinfo **dev_list = NULL;
	uint32_t nb_devs = 0;
	doca_error_t result;

	result = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to enumerate DOCA devices: %s\n", doca_error_get_descr(result));
		return;
	}

	printf("DOCA DMA-capable devices:\n");
	for (uint32_t i = 0; i < nb_devs; ++i) {
		char pci_addr[DOCA_DEVINFO_PCI_ADDR_SIZE] = {0};

		if (check_dma_capable(dev_list[i]) != DOCA_SUCCESS)
			continue;

		if (doca_devinfo_get_pci_addr_str(dev_list[i], pci_addr) != DOCA_SUCCESS)
			snprintf(pci_addr, sizeof(pci_addr), "<unknown>");

		printf("  [%u] %s\n", i, pci_addr);
	}

	doca_devinfo_destroy_list(dev_list);
}
