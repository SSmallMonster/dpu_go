"""DPU异常定义"""

class DPUError(Exception):
    """DPU基础异常"""
    pass

class DPUConnectionError(DPUError):
    """DPU连接错误"""
    pass

class DPUOperationError(DPUError):
    """DPU操作错误"""
    pass