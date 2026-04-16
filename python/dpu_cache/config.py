"""DPU配置类"""

class DPUConfig:
    """DPU配置参数"""

    def __init__(self,
                 dpu_ip: str = "192.168.100.2",
                 host_pci_addr: str = "0000:ba:00.0",
                 gpu_id: int = 0,
                 max_concurrent_ops: int = 4):
        self.dpu_ip = dpu_ip
        self.host_pci_addr = host_pci_addr
        self.gpu_id = gpu_id
        self.max_concurrent_ops = max_concurrent_ops

    def __repr__(self):
        return (f"DPUConfig(dpu_ip='{self.dpu_ip}', "
                f"host_pci_addr='{self.host_pci_addr}', "
                f"gpu_id={self.gpu_id}, "
                f"max_concurrent_ops={self.max_concurrent_ops})")