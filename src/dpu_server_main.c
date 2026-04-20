// DPU服务器主程序
#include "dma_transfer.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>

// 声明run_dma_server函数
int run_dma_server(const char *pci_addr, const char *rep_pci_addr,
                   const char *service_name, bool use_tcp);

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s -p <DPU_DMA_PCI> [-r rep_pci] [-S service] [-T]\n"
		"  -p PCI   DPU DMA device PCI address\n"
		"  -r PCI   Representor PCI address (for COMCH mode)\n"
		"  -S NAME  Service name (default: dpu_copy)\n"
		"  -T       Use TCP control channel instead of COMCH (port %u)\n"
		"  -h       Show this help\n",
		prog, DMA_TRANSFER_PORT);
}

int main(int argc, char **argv)
{
	const char *pci_addr = NULL;
	const char *rep_pci_addr = NULL;
	const char *service_name = "dpu_copy";
	bool use_tcp = false;
	int opt;

	// 解析命令行参数
	while ((opt = getopt(argc, argv, "p:r:S:Th")) != -1) {
		switch (opt) {
		case 'p':
			pci_addr = optarg;
			break;
		case 'r':
			rep_pci_addr = optarg;
			break;
		case 'S':
			service_name = optarg;
			break;
		case 'T':
			use_tcp = true;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (pci_addr == NULL) {
		fprintf(stderr, "Error: PCI address (-p) is required\n\n");
		usage(argv[0]);
		return 1;
	}

	printf("Starting DPU DMA server:\n");
	printf("  PCI Address: %s\n", pci_addr);
	printf("  Mode: %s\n", use_tcp ? "TCP" : "COMCH");
	if (use_tcp) {
		printf("  TCP Port: %u\n", DMA_TRANSFER_PORT);
	} else {
		printf("  Service: %s\n", service_name);
		if (rep_pci_addr) {
			printf("  Rep PCI: %s\n", rep_pci_addr);
		}
	}
	printf("\n");

	// 调用DMA服务器
	return run_dma_server(pci_addr, rep_pci_addr, service_name, use_tcp);
}