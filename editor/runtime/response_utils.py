import json
import time


def create_success_response(data):
    """创建成功响应"""
    return json.dumps({
        "success": True,
        "data": data,
        "timestamp": time.time()
    })

def create_error_response(message):
    """创建错误响应"""
    return json.dumps({
        "success": False,
        "error": message,
        "timestamp": time.time()
    })