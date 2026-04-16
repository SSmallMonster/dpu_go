# DPU Python集成设计文档

## 项目概述

将现有的GPU-DPU DMA传输C项目包装成Python可调用的包，实现LMCache项目中agent_wrapper.py所需的KV缓存存储接口。这是一个快速原型项目，用于GPU到GPU的数据传输测试，无需持久化存储。

## 系统架构

### 整体架构
```
Python应用 → agent_wrapper.py → ctypes → libdpu_cache.so → DOCA DMA → DPU
                    ↓
               KV Cache接口 → 文件传输映射 → GPU内存DMA → 临时文件存储
```

### 技术选型
- **集成方式**: ctypes包装 + 共享库
- **控制协议**: TCP模式（`-T`参数），避免COMCH配置复杂性
- **存储映射**: KV操作映射到DPU临时文件操作
- **数据格式**: 自定义二进制格式，包含tensor元数据和原始数据

## 组件设计

### 1. C共享库层 (libdpu_cache.so)

#### 核心API设计
```c
// dpu_cache_api.h
typedef struct {
    char dpu_ip[16];              // DPU管理IP地址
    char host_pci_addr[32];       // Host-side BF PCI地址
    int gpu_id;                   // GPU设备ID
    int max_concurrent_ops;       // 最大并发操作数
    int initialized;              // 初始化状态
} dpu_config_t;

// 生命周期管理
int dpu_cache_init(dpu_config_t* config);
int dpu_cache_cleanup(void);

// KV缓存操作
int dpu_cache_store(const char* key_id,
                   void* k_data, size_t k_size, int k_dtype, int* k_shape, int k_ndim,
                   void* v_data, size_t v_size, int v_dtype, int* v_shape, int v_ndim);

int dpu_cache_retrieve(const char* key_id,
                      void** k_data, size_t* k_size, int* k_dtype, int* k_shape, int* k_ndim,
                      void** v_data, size_t* v_size, int* v_dtype, int* v_shape, int* v_ndim);

int dpu_cache_remove(const char* key_id);
int dpu_cache_contains(const char* key_id);
```

#### 内部实现策略
1. **复用现有DMA核心**: 保持`gpu_dma_copy.cu`和相关DMA逻辑不变
2. **重构为库函数**: 将main函数逻辑提取为可调用函数
3. **文件路径映射**: `key_id` → `/tmp/kv_cache_{key_id}.bin`
4. **元数据管理**: 在文件头部存储tensor shape、dtype等信息

#### 文件格式定义
```
KV Cache文件格式 (/tmp/kv_cache_{key_id}.bin):

[Header: 64 bytes]
- magic: "KVCH" (4 bytes)
- version: 1 (4 bytes)
- k_dtype: torch.dtype映射 (4 bytes)
- v_dtype: torch.dtype映射 (4 bytes)
- k_ndim: K tensor维度数 (4 bytes)
- v_ndim: V tensor维度数 (4 bytes)
- k_shape: [4 x int32] K tensor形状 (16 bytes)
- v_shape: [4 x int32] V tensor形状 (16 bytes)
- k_size: K tensor字节数 (8 bytes)
- v_size: V tensor字节数 (8 bytes)
- reserved: 预留字段 (8 bytes)

[Data Section: k_size + v_size bytes]
- K tensor原始数据
- V tensor原始数据
```

### 2. Python ctypes包装层

#### 包目录结构
```
dpu_cache/
├── __init__.py              # 包初始化，导出主要接口
├── _api.py                  # ctypes包装实现
├── config.py                # 配置类定义
├── exceptions.py            # 异常定义
├── native/                  # C库文件
│   ├── libdpu_cache.so     # 编译后的共享库
│   ├── dpu_cache_api.h     # C API头文件
│   └── README.md           # 构建说明
└── setup.py                # 安装脚本
```

#### 核心ctypes包装实现
```python
# _api.py
import ctypes
import torch
import numpy as np
from typing import Tuple, Optional

class DPUAgent:
    def __init__(self, config):
        """初始化DPU代理"""
        self.lib = ctypes.CDLL('./native/libdpu_cache.so')
        self._setup_function_signatures()
        self._init_dpu_config(config)

    def _setup_function_signatures(self):
        """设置ctypes函数签名"""
        # dpu_cache_store签名
        self.lib.dpu_cache_store.argtypes = [
            ctypes.c_char_p,           # key_id
            ctypes.c_void_p,           # k_data
            ctypes.c_size_t,           # k_size
            ctypes.c_int,              # k_dtype
            ctypes.POINTER(ctypes.c_int), # k_shape
            ctypes.c_int,              # k_ndim
            ctypes.c_void_p,           # v_data
            ctypes.c_size_t,           # v_size
            ctypes.c_int,              # v_dtype
            ctypes.POINTER(ctypes.c_int), # v_shape
            ctypes.c_int               # v_ndim
        ]
        self.lib.dpu_cache_store.restype = ctypes.c_int

    def store_kv(self, key_id: str, k_tensor: torch.Tensor, v_tensor: torch.Tensor) -> bool:
        """存储KV张量到DPU"""
        # 提取tensor元数据
        k_ptr = k_tensor.data_ptr()
        v_ptr = v_tensor.data_ptr()
        k_shape_array = (ctypes.c_int * len(k_tensor.shape))(*k_tensor.shape)
        v_shape_array = (ctypes.c_int * len(v_tensor.shape))(*v_tensor.shape)

        # 调用C函数
        result = self.lib.dpu_cache_store(
            key_id.encode('utf-8'),
            k_ptr,
            k_tensor.nbytes,
            self._torch_dtype_to_int(k_tensor.dtype),
            k_shape_array,
            len(k_tensor.shape),
            v_ptr,
            v_tensor.nbytes,
            self._torch_dtype_to_int(v_tensor.dtype),
            v_shape_array,
            len(v_tensor.shape)
        )

        return result == 0

    def retrieve_kv(self, key_id: str, target_device: str) -> Optional[Tuple[torch.Tensor, torch.Tensor]]:
        """从DPU检索KV张量"""
        # C函数调用获取元数据
        # GPU内存分配
        # 数据传输
        # 构建torch.Tensor对象
        pass
```

#### 配置管理
```python
# config.py
class DPUAgentConfig:
    def __init__(self,
                 dpu_ip: str = "192.168.100.2",
                 host_pci_addr: str = "0000:ba:00.0",
                 gpu_id: int = 0,
                 max_concurrent_ops: int = 4):
        self.dpu_ip = dpu_ip
        self.host_pci_addr = host_pci_addr
        self.gpu_id = gpu_id
        self.max_concurrent_ops = max_concurrent_ops
```

### 3. agent_wrapper.py集成

#### 集成策略
最小化修改现有agent_wrapper.py代码，只需要修改导入部分：

```python
# agent_wrapper.py 修改
try:
    # 导入新的DPU C API实现
    from dpu_cache._api import DPUAgent as dpu_agent
    from dpu_cache.config import DPUAgentConfig as dpu_agent_config
    DPU_API_AVAILABLE = True
except ImportError as e:
    logger.warning(f"DPU API not available: {e}")
    # 保持现有的mock fallback逻辑
    dpu_agent = None
    dpu_agent_config = None
    DPU_API_AVAILABLE = False
```

其余agent_wrapper.py的逻辑保持不变，包括：
- 元数据注册表管理
- 错误处理
- 日志记录
- Mock实现fallback

## 数据流设计

### 存储操作流程
1. **Python调用**: `store_kv(key_id, k_tensor, v_tensor)`
2. **元数据提取**: 获取tensor的GPU内存地址、shape、dtype
3. **ctypes调用**: 传递指针和元数据给C函数
4. **C层处理**:
   - 生成DPU文件路径 `/tmp/kv_cache_{key_id}.bin`
   - 准备元数据头部
   - 调用现有GPU DMA push逻辑
   - TCP控制消息发送到DPU
5. **DPU端**: 接收DMA数据，写入本地文件

### 检索操作流程
1. **Python调用**: `retrieve_kv(key_id, target_device)`
2. **ctypes调用**: 请求C层检索数据
3. **C层处理**:
   - 从DPU读取文件头部，解析元数据
   - 在GPU分配目标内存
   - 调用现有GPU DMA pull逻辑
4. **Python层**: 根据元数据重建torch.Tensor对象

### 错误处理策略
- **C层错误**: 返回错误码，Python层转换为相应异常
- **网络错误**: DPU连接失败时抛出DPUConnectionError
- **内存错误**: GPU内存分配失败时抛出DPUOperationError
- **数据一致性**: 检索时验证元数据匹配性

## 性能考量

### 优化策略
1. **零拷贝设计**: 直接操作GPU内存指针，避免主机内存中转
2. **批量操作**: 支持同时传输K和V tensor，减少DMA往返
3. **内存对齐**: 确保GPU内存4KB对齐，优化DMA性能
4. **连接复用**: 维持到DPU的TCP连接，避免重复连接开销

### 性能预期
- **延迟**: 单次KV存储/检索操作 < 10ms（取决于tensor大小）
- **吞吐**: 接近PCIe Gen3/4理论带宽（受DPU网络限制）
- **并发**: 支持4个并发DMA操作（可配置）

## 构建和部署

### 构建流程
1. **C库构建**: 修改现有CMakeLists.txt，生成libdpu_cache.so
2. **Python包构建**: 使用setup.py打包，包含原生库
3. **依赖管理**: requirements.txt包含torch, numpy等依赖

### 部署要求
- **Host环境**: x86主机，NVIDIA GPU，CUDA toolkit，DOCA host package
- **DPU环境**: BlueField DPU，DOCA dpu package，可访问管理IP
- **网络要求**: Host能通过TCP连接DPU管理网络
- **Python环境**: Python 3.7+，PyTorch 1.8+

### 测试策略
1. **单元测试**: Python包装层功能测试
2. **集成测试**: 端到端GPU-DPU-GPU数据路径验证
3. **性能测试**: 不同tensor大小的吞吐和延迟基准测试
4. **错误处理测试**: 网络断连、内存不足等异常情况测试

## 风险和限制

### 技术风险
- **DOCA API变更**: 依赖特定版本DOCA库，升级可能需要适配
- **内存管理**: GPU内存泄漏风险，需要仔细管理tensor生命周期
- **并发安全**: 多线程访问时的数据竞争问题

### 功能限制
- **临时存储**: DPU重启后数据丢失，不支持持久化
- **单DPU支持**: 当前设计不支持多DPU负载均衡
- **TCP协议**: 相比COMCH性能稍低，但配置简单

### 缓解策略
- 版本锁定和兼容性测试
- 严格的内存管理和泄漏检测
- 线程安全的API设计
- 详细的错误日志和监控

## 总结

本设计提供了一个最小化、快速原型的GPU-DPU集成方案，通过ctypes包装现有C代码实现Python接口。重点是简单性和快速验证，适合GPU到GPU数据传输的概念验证需求。

实现后的系统将提供agent_wrapper.py所需的完整KV缓存接口，同时保持与现有LMCache架构的兼容性。