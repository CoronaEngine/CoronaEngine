import base64
import mimetypes
import os

import requests
import logging

from config.paths_config import get_project_media_dir
from Quasar.ai_service.entrance import ai_entrance


def base64_to_image_file(base64_string: str, output_path: str = None) -> str:
    """
    将Base64字符串转换为图片文件

    Args:
        base64_string: Base64编码的图片字符串
        output_path: 输出文件路径（可选）

    Returns:
        保存的文件路径
    """
    try:
        # 移除可能的data:前缀
        if "," in base64_string:
            # 格式: data:image/png;base64,iVBORw0KGgoAAA...
            header, base64_data = base64_string.split(",", 1)
        else:
            base64_data = base64_string

        # 解码Base64
        image_data = base64.b64decode(base64_data)

        # 如果没有指定输出路径，生成一个
        if not output_path:
            # 尝试从Base64头部获取扩展名
            if "," in base64_string:
                mime_type = base64_string.split(";")[0].split(":")[1]
                ext = mimetypes.guess_extension(mime_type) or ".png"
            else:
                ext = ".png"

            filename = f"image_temp{ext}"
            output_path = str(get_project_media_dir() / filename)

        # 确保目录存在
        os.makedirs(os.path.dirname(output_path), exist_ok=True)

        # 保存为文件
        with open(output_path, "wb") as f:
            f.write(image_data)

        print(f"图片已保存到: {output_path}")
        return output_path

    except base64.binascii.Error as e:
        print(f"Base64解码错误: {e}")
        raise ValueError("无效的Base64字符串")
    except Exception as e:
        print(f"保存图片时出错: {e}")
        raise


def upload_file_to_server(path):
    oss_setting = ai_entrance.collector.AI_SETTINGS.get("oss", None)
    if oss_setting:
        server_ip = oss_setting.get("server_ip")
        host = oss_setting.get("host")
        file = open(path, "rb")
        result = requests.post(
            f"http://{server_ip}:{host}/file_system/upload", files={"file": file}
        ).json()
        return f"http://{server_ip}:{host}/{result.get('data')}"
    else:
        return path
