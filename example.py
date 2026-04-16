#!/usr/bin/env python3
"""DPU Cache使用示例"""

import torch
import sys
import os

# 添加Python包路径
sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'python'))

try:
    import dpu_cache
    print("✓ DPU Cache package imported successfully")
except ImportError as e:
    print(f"✗ Failed to import dpu_cache: {e}")
    print("Please run ./build_python_package.sh first")
    sys.exit(1)

def main():
    print("\nDPU Cache Python API Demo")
    print("=" * 40)

    # 创建配置
    config = dpu_cache.DPUConfig(
        dpu_ip="192.168.100.2",
        host_pci_addr="0000:ba:00.0",
        gpu_id=0
    )
    print(f"Config: {config}")

    try:
        # 创建DPU Agent
        print("\nInitializing DPU Agent...")
        agent = dpu_cache.DPUAgent(config)
        print("✓ DPU Agent initialized")

        # 创建测试张量（如果有CUDA设备）
        if torch.cuda.is_available():
            print(f"\nUsing CUDA device: {torch.cuda.get_device_name()}")

            # 创建测试数据
            device = torch.device(f"cuda:{config.gpu_id}")
            k_tensor = torch.randn(4, 8, 64, device=device, dtype=torch.float32)
            v_tensor = torch.randn(4, 8, 64, device=device, dtype=torch.float32)

            print(f"K tensor shape: {k_tensor.shape}, device: {k_tensor.device}")
            print(f"V tensor shape: {v_tensor.shape}, device: {v_tensor.device}")

            # 测试存储操作
            test_key = "test_key_001"
            print(f"\nStoring KV cache with key: {test_key}")

            result = agent.store_kv(test_key, k_tensor, v_tensor)
            if result:
                print("✓ Store operation completed")
            else:
                print("✗ Store operation failed")

            # 测试包含检查
            print(f"\nChecking if key exists: {test_key}")
            exists = agent.contains_key(test_key)
            print(f"Key exists: {exists}")

            # 测试检索操作
            print(f"\nRetrieving KV cache for key: {test_key}")
            retrieved = agent.retrieve_kv(test_key)
            if retrieved:
                k_ret, v_ret = retrieved
                print(f"✓ Retrieved K shape: {k_ret.shape}, V shape: {v_ret.shape}")
            else:
                print("✗ Retrieve operation failed or not implemented")

            # 测试删除操作
            print(f"\nRemoving KV cache for key: {test_key}")
            removed = agent.remove_kv(test_key)
            if removed:
                print("✓ Remove operation completed")
            else:
                print("✗ Remove operation failed")

        else:
            print("\n⚠ No CUDA device available, skipping tensor operations")

    except Exception as e:
        print(f"\n✗ Error during DPU operations: {e}")
        return 1

    print("\nDemo completed!")
    return 0

if __name__ == "__main__":
    sys.exit(main())