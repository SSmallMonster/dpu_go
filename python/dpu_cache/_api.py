"""DPU Cache ctypes API包装"""

import ctypes
import os
import torch
import numpy as np
from typing import Tuple, Optional
import logging

from .config import DPUConfig
from .exceptions import DPUConnectionError, DPUOperationError

logger = logging.getLogger(__name__)

# Torch dtype到整数的映射
TORCH_DTYPE_MAP = {
    torch.float32: 0,
    torch.float16: 1,
    torch.bfloat16: 2,
    torch.int32: 3,
    torch.int64: 4,
    torch.uint8: 5,
}

REVERSE_TORCH_DTYPE_MAP = {v: k for k, v in TORCH_DTYPE_MAP.items()}

# C结构体定义
class DPUConfigStruct(ctypes.Structure):
    _fields_ = [
        ("dpu_ip", ctypes.c_char * 16),
        ("host_pci_addr", ctypes.c_char * 32),
        ("gpu_id", ctypes.c_int),
        ("max_concurrent_ops", ctypes.c_int),
        ("initialized", ctypes.c_int)
    ]

class DPUAgent:
    """DPU代理 - ctypes包装实现"""

    def __init__(self, config: DPUConfig):
        self.config = config
        self.lib = None
        self._load_library()
        self._setup_function_signatures()
        self._initialize_dpu(config)

    def _load_library(self):
        """加载共享库"""
        # 获取当前文件的目录，然后寻找共享库
        current_dir = os.path.dirname(os.path.abspath(__file__))
        project_root = os.path.abspath(os.path.join(current_dir, "../../.."))

        possible_paths = [
            # 相对于项目根目录的路径
            os.path.join(project_root, "build/libdpu_cache.so"),
            os.path.join(project_root, "build-host/libdpu_cache.so"),
            # 相对路径
            "../build/libdpu_cache.so",
            "../../build/libdpu_cache.so",
            "../../../build/libdpu_cache.so",
            "./libdpu_cache.so",
            # 绝对路径
            "/usr/local/lib/libdpu_cache.so"
        ]

        for path in possible_paths:
            if os.path.exists(path):
                try:
                    self.lib = ctypes.CDLL(path)
                    logger.info(f"Loaded DPU library from: {path}")
                    return
                except OSError as e:
                    logger.warning(f"Failed to load {path}: {e}")
                    continue

        raise DPUConnectionError("Could not find or load libdpu_cache.so")

    def _setup_function_signatures(self):
        """设置ctypes函数签名"""
        # dpu_cache_init
        self.lib.dpu_cache_init.argtypes = [ctypes.POINTER(DPUConfigStruct)]
        self.lib.dpu_cache_init.restype = ctypes.c_int

        # dpu_cache_store
        self.lib.dpu_cache_store.argtypes = [
            ctypes.c_char_p,                    # key_id
            ctypes.c_void_p,                    # k_data
            ctypes.c_size_t,                    # k_size
            ctypes.c_int,                       # k_dtype
            ctypes.POINTER(ctypes.c_int),       # k_shape
            ctypes.c_int,                       # k_ndim
            ctypes.c_void_p,                    # v_data
            ctypes.c_size_t,                    # v_size
            ctypes.c_int,                       # v_dtype
            ctypes.POINTER(ctypes.c_int),       # v_shape
            ctypes.c_int                        # v_ndim
        ]
        self.lib.dpu_cache_store.restype = ctypes.c_int

        # dpu_cache_retrieve
        self.lib.dpu_cache_retrieve.argtypes = [
            ctypes.c_char_p,                    # key_id
            ctypes.POINTER(ctypes.c_void_p),    # k_data
            ctypes.POINTER(ctypes.c_size_t),    # k_size
            ctypes.POINTER(ctypes.c_int),       # k_dtype
            ctypes.POINTER(ctypes.c_int),       # k_shape
            ctypes.POINTER(ctypes.c_int),       # k_ndim
            ctypes.POINTER(ctypes.c_void_p),    # v_data
            ctypes.POINTER(ctypes.c_size_t),    # v_size
            ctypes.POINTER(ctypes.c_int),       # v_dtype
            ctypes.POINTER(ctypes.c_int),       # v_shape
            ctypes.POINTER(ctypes.c_int)        # v_ndim
        ]
        self.lib.dpu_cache_retrieve.restype = ctypes.c_int

        # dpu_cache_remove
        self.lib.dpu_cache_remove.argtypes = [ctypes.c_char_p]
        self.lib.dpu_cache_remove.restype = ctypes.c_int

        # dpu_cache_contains
        self.lib.dpu_cache_contains.argtypes = [ctypes.c_char_p]
        self.lib.dpu_cache_contains.restype = ctypes.c_int

        # dpu_cache_cleanup
        self.lib.dpu_cache_cleanup.argtypes = []
        self.lib.dpu_cache_cleanup.restype = ctypes.c_int

    def _initialize_dpu(self, config: DPUConfig):
        """初始化DPU配置"""
        c_config = DPUConfigStruct()
        c_config.dpu_ip = config.dpu_ip.encode('utf-8')
        c_config.host_pci_addr = config.host_pci_addr.encode('utf-8')
        c_config.gpu_id = config.gpu_id
        c_config.max_concurrent_ops = config.max_concurrent_ops
        c_config.initialized = 0

        result = self.lib.dpu_cache_init(ctypes.byref(c_config))
        if result != 0:
            raise DPUConnectionError(f"Failed to initialize DPU: error code {result}")

        logger.info("DPU Agent initialized successfully")

    def _torch_dtype_to_int(self, dtype: torch.dtype) -> int:
        """转换torch dtype到整数"""
        return TORCH_DTYPE_MAP.get(dtype, 0)

    def _int_to_torch_dtype(self, dtype_int: int) -> torch.dtype:
        """转换整数到torch dtype"""
        return REVERSE_TORCH_DTYPE_MAP.get(dtype_int, torch.float32)

    def store_kv(self, key_id: str, k_tensor: torch.Tensor, v_tensor: torch.Tensor) -> bool:
        """存储KV张量到DPU"""
        try:
            # 验证tensor在GPU上
            # if not k_tensor.is_cuda or not v_tensor.is_cuda:
            #     raise ValueError("Tensors must be on CUDA device")

            # # 验证tensor在同一设备
            # if k_tensor.device != v_tensor.device:
            #     raise ValueError("K and V tensors must be on same device")

            # 验证tensor数据类型一致
            if k_tensor.dtype != v_tensor.dtype:
                raise ValueError("K and V tensors must have same dtype")

            # 验证tensor连续性
            if not k_tensor.is_contiguous():
                k_tensor = k_tensor.contiguous()
            if not v_tensor.is_contiguous():
                v_tensor = v_tensor.contiguous()

            # 获取tensor数据指针
            k_ptr = k_tensor.data_ptr()
            v_ptr = v_tensor.data_ptr()

            # 准备shape数组
            k_shape = list(k_tensor.shape) + [0] * (4 - len(k_tensor.shape))
            v_shape = list(v_tensor.shape) + [0] * (4 - len(v_tensor.shape))

            k_shape_array = (ctypes.c_int * 4)(*k_shape[:4])
            v_shape_array = (ctypes.c_int * 4)(*v_shape[:4])

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

            if result == 0:
                logger.debug(f"Successfully stored KV cache for key: {key_id}")
                return True
            else:
                logger.error(f"Failed to store KV cache for key {key_id}, error: {result}")
                return False

        except Exception as e:
            logger.error(f"Exception in store_kv for key {key_id}: {e}")
            raise DPUOperationError(f"Failed to store KV cache: {e}") from e

    def retrieve_kv(self, key_id: str, target_device: str = "cuda") -> Optional[Tuple[torch.Tensor, torch.Tensor]]:
        """从DPU检索KV张量"""
        try:
            # 准备输出参数
            k_data_ptr = ctypes.c_void_p()
            v_data_ptr = ctypes.c_void_p()
            k_size = ctypes.c_size_t()
            v_size = ctypes.c_size_t()
            k_dtype = ctypes.c_int()
            v_dtype = ctypes.c_int()
            k_shape = (ctypes.c_int * 4)()
            v_shape = (ctypes.c_int * 4)()
            k_ndim = ctypes.c_int()
            v_ndim = ctypes.c_int()

            # 调用C函数
            result = self.lib.dpu_cache_retrieve(
                key_id.encode('utf-8'),
                ctypes.byref(k_data_ptr),
                ctypes.byref(k_size),
                ctypes.byref(k_dtype),
                k_shape,
                ctypes.byref(k_ndim),
                ctypes.byref(v_data_ptr),
                ctypes.byref(v_size),
                ctypes.byref(v_dtype),
                v_shape,
                ctypes.byref(v_ndim)
            )

            if result == -2:  # KEY_NOT_FOUND
                logger.debug(f"Key not found: {key_id}")
                return None
            elif result != 0:
                logger.error(f"Failed to retrieve KV cache for key {key_id}, error: {result}")
                return None

            # TODO: 重建torch tensor对象
            # 这里需要根据返回的指针和元数据创建tensor
            logger.warning("retrieve_kv not fully implemented yet")
            return None

        except Exception as e:
            logger.error(f"Exception in retrieve_kv for key {key_id}: {e}")
            raise DPUOperationError(f"Failed to retrieve KV cache: {e}") from e

    def remove_kv(self, key_id: str) -> bool:
        """从DPU删除KV缓存"""
        try:
            result = self.lib.dpu_cache_remove(key_id.encode('utf-8'))
            if result == 0:
                logger.debug(f"Successfully removed KV cache for key: {key_id}")
                return True
            else:
                logger.warning(f"Failed to remove KV cache for key {key_id}, error: {result}")
                return False

        except Exception as e:
            logger.error(f"Exception in remove_kv for key {key_id}: {e}")
            return False

    def contains_key(self, key_id: str) -> bool:
        """检查DPU是否包含指定key"""
        try:
            result = self.lib.dpu_cache_contains(key_id.encode('utf-8'))
            return result == 0

        except Exception as e:
            logger.error(f"Exception in contains_key for key {key_id}: {e}")
            return False

    def __del__(self):
        """清理资源"""
        if self.lib:
            try:
                self.lib.dpu_cache_cleanup()
            except:
                pass


# 兼容agent_wrapper.py的接口
def dpu_agent(config):
    """创建DPU Agent实例"""
    return DPUAgent(config)

def dpu_agent_config(**kwargs):
    """创建DPU Agent配置"""
    return DPUConfig(**kwargs)