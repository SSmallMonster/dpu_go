# GPU <-> DPU DMA Copy

这个仓库现在只保留一套最小、可维护的双向实现：

- `gpu_dma_copy`: 运行在 host，负责导出 GPU buffer 并向 DPU 发控制请求
- `dpu_dma_copy`: 运行在 BlueField DPU，负责导入 remote mmap 并执行 DOCA DMA

支持两种方向：

- `push`: Host GPU -> DPU file
- `pull`: DPU file -> Host GPU

控制面支持两种模式（可通过 `-T` 参数切换）：

- **COMCH 模式**（默认）：使用 DOCA Comch，无需 DPU 有可达 IP
- **TCP 模式**（`-T`）：使用普通 TCP socket，COMCH 不可用时的备选方案

数据面使用 DOCA DMA + `doca_mmap_export_pci()` / `doca_mmap_create_from_export()`。

## Design

这次重写刻意只保留核心功能：

- 不再依赖官方 sample 源码
- 控制层通过 `ctrl_channel` 抽象，支持 COMCH 和 TCP 两种后端
- 不再保留旧的 KV cache demo、带宽测试程序和多套协议
- DPU 侧只维护一个可复用的 staging buffer，避免每次传输都重新注册大块本地内存

实际链路如下：

1. Host 分配 GPU buffer
2. Host 用 BF/BlueField 对应的 host-side PCI function 导出 GPU mmap
3. Host 通过 DOCA Comch 发送请求、目标 DPU 文件路径、remote address 和 export descriptor
4. DPU 导入 remote mmap
5. DPU 按 chunk 流式执行 DMA
6. `push` 时把 chunk 写入 DPU 本地文件
7. `pull` 时从 DPU 本地文件读入 staging buffer，再 DMA 写回 Host GPU

## Repository Layout

```text
.
├── CMakeLists.txt
├── README.md
├── include/
│   ├── ctrl_channel.h
│   ├── comch_ctrl.h
│   ├── dma_transfer.h
│   └── doca_device_utils.h
├── src/
│   ├── dpu_dma_copy.c
│   ├── gpu_dma_copy.cu
│   ├── ctrl_channel.c
│   ├── comch_ctrl.c
│   └── doca_device_utils.c
└── scripts/
    ├── build_host.sh
    ├── build_dpu.sh
    └── preflight_check.sh
```

## Requirements

### Host

- x86 host
- NVIDIA GPU with GPUDirect RDMA support
- CUDA toolkit
- DOCA host package
- `nvidia-peermem` loaded
- 能看到 host-side BF/BlueField PCI function，例如 `0000:ba:00.0`

### DPU

- BlueField DPU
- DOCA dpu package
- 能看到 DPU-side DMA PCI function，例如 `0000:03:00.0`

## Build

### Host

```bash
chmod +x scripts/build_host.sh
./scripts/build_host.sh
```

产物：

```bash
build-host/gpu_dma_copy
```

### DPU

```bash
chmod +x scripts/build_dpu.sh
./scripts/build_dpu.sh
```

产物：

```bash
build-dpu/dpu_dma_copy
```

## Run

### 1. 启动 DPU 服务

**COMCH 模式（默认）：**

```bash
./build-dpu/dpu_dma_copy -p 0000:03:00.0 -m 256 -q 4
```

**TCP 模式（COMCH 不可用时）：**

```bash
./build-dpu/dpu_dma_copy -p 0000:03:00.0 -m 256 -q 4 -T
```

TCP 模式监听 `0.0.0.0:18517`，host 通过 DPU 的管理 IP 连接。

DPU 侧参数：

- `-p`: DPU 侧 DMA PCI 地址
- `-m`: DPU staging buffer 大小，单位 MiB
- `-c`: 单个 DMA chunk 大小，单位 MiB，默认自动选择
- `-q`: DPU 侧并发 DMA task 数，默认 `4`
- `-r`: DPU side representor PCI，默认自动选择（仅 COMCH 模式）
- `-S`: Comch service name，默认 `gpu_dpu_dma_copy`（仅 COMCH 模式）
- `-T`: 使用 TCP 控制面，替代 COMCH

启动后会打印：

- 可用 DMA 设备
- staging buffer 大小
- 设备允许的单 task 最大 size
- 实际采用的 chunk size 和 queue depth

### 2. Host GPU -> DPU file

**COMCH 模式：**

```bash
./build-host/gpu_dma_copy -o push -p 0000:ba:00.0 -g 3 -f /tmp/on_dpu.bin -s 256
```

**TCP 模式：**

```bash
./build-host/gpu_dma_copy -o push -p 0000:ba:00.0 -g 3 -f /tmp/on_dpu.bin -s 256 -T <DPU_IP>
```

Host 侧参数：

- `-o push/pull`: 传输方向
- `-p`: host-side BF PCI 地址（DMA 导出 GPU mmap 需要）
- `-g`: GPU ID
- `-f`: DPU 上的目标/源文件路径
- `-s`: 生成一块指定大小的 GPU buffer，单位 MiB
- `-i`: 把本地文件先拷进 GPU 再发到 DPU（push 模式）
- `-O`: 把拉回的 GPU 内容落盘（pull 模式）
- `-b`: 填充值，默认 `0x5a`
- `-S`: Comch service name（仅 COMCH 模式）
- `-T <DPU_IP>`: 使用 TCP 控制面，指定 DPU 管理 IP

### 3. DPU file -> Host GPU

**COMCH 模式：**

```bash
./build-host/gpu_dma_copy -o pull -p 0000:ba:00.0 -g 3 -f /tmp/on_dpu.bin -O /tmp/from_gpu.bin
```

**TCP 模式：**

```bash
./build-host/gpu_dma_copy -o pull -p 0000:ba:00.0 -g 3 -f /tmp/on_dpu.bin -T <DPU_IP> -O /tmp/from_gpu.bin
```

## Performance

DPU 端和 Host 端打印的吞吐含义不同：

- DPU 打印的是 DMA 时间和 DMA 吞吐
- Host 打印的是端到端时间和端到端吞吐

这两者一般不会相同，因为 Host 端包含：

- 控制消息往返（COMCH 或 TCP）
- DPU 文件读写
- DPU staging buffer 上的 chunk 流水

而 DPU 端的 `dma_bandwidth_gbps` 更接近真实 PCIe DMA 带宽。

## Tuning

### Chunk Size

单次 DMA task 大小不能超过设备能力上限。程序会自动读取：

```c
doca_dma_cap_task_memcpy_get_max_buf_size(...)
```

如果不显式传 `-c`，就自动取：

- 设备允许的最大单 task size
- `stage_buffer / queue_depth`

两者中的较小值。

### Queue Depth

`-q` 控制 DPU 侧同一批次会提交多少个 DMA task。

建议从这些值开始试：

- `-q 1`
- `-q 4`
- `-q 8`

不是越大越快。很多平台在 `4` 或 `8` 已经接近上限。

### Staging Buffer

`-m` 需要至少满足：

```text
stage_size >= queue_depth * chunk_size
```

如果你要更大的 chunk 或更高并发，先把 `-m` 提上去。

## Notes

### 控制面：COMCH vs TCP

控制面只传少量消息：操作方向、文件路径、remote address、export descriptor、状态回执。

- **COMCH 模式**：通过 DOCA Comch 传递，无需 DPU 有可达 IP，但依赖 DPU 上的 net representor 正确配置
- **TCP 模式**：通过普通 TCP socket（端口 18517）传递，需要 host 能通过管理网络访问 DPU IP；当 COMCH 因 DPU 配置问题不可用时使用

大块数据无论哪种模式都走 DOCA DMA，不走控制面。

### 为什么 DPU 侧预分配 staging buffer

如果每次传输都在 DPU 上重新分配和注册一大块本地内存，端到端时间会被 setup 成本放大。现在的实现只在启动时注册一次 staging buffer，后续传输重复使用。

### 对齐告警

如果看到：

```text
Memory range isn't aligned to 64B
```

这通常是性能告警，不一定导致失败。当前实现已经把 DPU staging buffer 做了 `4096` 对齐；GPU buffer 由 `cudaMalloc()` 分配。

## Quick Test

### COMCH 模式

1. DPU:
```bash
./build-dpu/dpu_dma_copy -p 0000:03:00.0 -m 256 -q 4
```
2. Host push:
```bash
./build-host/gpu_dma_copy -o push -p 0000:ba:00.0 -g 3 -f /tmp/test.bin -s 64
```
3. Host pull:
```bash
./build-host/gpu_dma_copy -o pull -p 0000:ba:00.0 -g 3 -f /tmp/test.bin -O /tmp/test.out
```

### TCP 模式（COMCH 不可用时）

1. DPU（加 `-T`）:
```bash
./build-dpu/dpu_dma_copy -p 0000:03:00.0 -m 256 -q 4 -T
```
2. Host push（加 `-T <DPU_IP>`）:
```bash
./build-host/gpu_dma_copy -o push -p 0000:ba:00.0 -g 3 -f /tmp/test.bin -s 64 -T 192.168.100.2
```
3. Host pull:
```bash
./build-host/gpu_dma_copy -o pull -p 0000:ba:00.0 -g 3 -f /tmp/test.bin -T 192.168.100.2 -O /tmp/test.out
```

4. 如果 push 用的是 `-i /tmp/local_input.bin`，比较：

```bash
cmp /tmp/test.out /tmp/local_input.bin
```

如果 push 用的是 synthetic `-s` 而不是 `-i`，直接看 Host 打印出来的 sample bytes 是否和填充值一致即可。
