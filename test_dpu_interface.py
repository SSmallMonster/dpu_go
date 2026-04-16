#!/usr/bin/env python3
"""
DPU Cache 接口测试

专门测试 store_kv 接口功能
"""

import sys
import torch
import traceback

def test_store_kv():
    """测试 store_kv 接口"""
    print("=" * 50)
    print("DPU Cache store_kv 接口测试")
    print("=" * 50)

    try:
        # 1. 导入和初始化
        print("\n1. 初始化 DPU Cache...")
        from dpu_cache import DPUConfig, DPUAgent

        config = DPUConfig(
            dpu_ip="192.168.100.2",
            host_pci_addr="0000:ba:00.0",
            gpu_id=0
        )
        print(f"✅ 配置: {config}")

        agent = DPUAgent(config)
        print("✅ DPU Agent 初始化成功")

        # 2. 检查CUDA环境
        print("\n2. 检查CUDA环境...")
        if torch.cuda.is_available():
            device = torch.device("cuda:0")
            print(f"✅ 使用CUDA设备: {torch.cuda.get_device_name()}")
        else:
            device = torch.device("cpu")
            print("⚠️  CUDA不可用，使用CPU")

        # 3. 基本功能测试
        print("\n3. 基本功能测试...")

        # 创建测试张量
        k_tensor = torch.randn(4, 8, 16, device=device, dtype=torch.float32)
        v_tensor = torch.randn(4, 8, 16, device=device, dtype=torch.float32)

        print(f"K tensor: {k_tensor.shape}, {k_tensor.dtype}, {k_tensor.device}")
        print(f"V tensor: {v_tensor.shape}, {v_tensor.dtype}, {v_tensor.device}")
        print(f"数据大小: K={k_tensor.nbytes}字节, V={v_tensor.nbytes}字节")

        # 显示样本数据
        k_sample = k_tensor.flatten()[:3].cpu()
        v_sample = v_tensor.flatten()[:3].cpu()
        print(f"K 样本: {k_sample.tolist()}")
        print(f"V 样本: {v_sample.tolist()}")

        # 调用 store_kv
        test_key = "test_basic_kv"
        print(f"\n调用 store_kv('{test_key}')...")
        result = agent.store_kv(test_key, k_tensor, v_tensor)

        if result:
            print("✅ store_kv 调用成功")
        else:
            print("❌ store_kv 调用失败")
            return False

        # 4. 错误处理测试
        print("\n4. 错误处理测试...")

        # 设备不匹配
        if torch.cuda.is_available():
            try:
                k_gpu = torch.randn(2, 4, device="cuda")
                v_cpu = torch.randn(2, 4, device="cpu")
                agent.store_kv("test_device_error", k_gpu, v_cpu)
                print("❌ 设备不匹配应该失败")
            except Exception as e:
                print(f"✅ 设备不匹配正确抛出异常: {type(e).__name__}")

        # 数据类型不匹配
        try:
            k_float = torch.randn(2, 4, device=device, dtype=torch.float32)
            v_int = torch.randint(0, 10, (2, 4), device=device, dtype=torch.int32)
            agent.store_kv("test_dtype_error", k_float, v_int)
            print("❌ 数据类型不匹配应该失败")
        except Exception as e:
            print(f"✅ 数据类型不匹配正确抛出异常: {type(e).__name__}")

        print("\n" + "=" * 50)
        print("✅ store_kv 接口测试通过")
        print("=" * 50)
        return True

    except Exception as e:
        print(f"\n❌ 测试失败: {e}")
        print(f"详细错误:\n{traceback.format_exc()}")
        return False

def main():
    """主函数"""
    print("开始 DPU Cache 接口测试...\n")

    success = test_store_kv()

    if success:
        print("\n🎉 所有测试通过！")
        print("ℹ️  注意: 实际DMA传输需要DOCA环境")
    else:
        print("\n❌ 测试失败")

    return 0 if success else 1

if __name__ == "__main__":
    sys.exit(main())