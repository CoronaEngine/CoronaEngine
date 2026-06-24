import json
import os
import datetime
import logging
from CoronaCore.core.corona_editor import CoronaEditor
from CoronaPlugin.core.corona_plugin_base import PluginBase
from CoronaCore.utils.file_handler import FileHandler
from utils.settings import settings_manager
from .utils.project_copy import ProjectCopy
logger = logging.getLogger(__name__)


@PluginBase.register_web("ProjectLauncher")
class ProjectLauncher(PluginBase):

    @staticmethod
    def get_default_project_path() -> str:
        # 从配置文件读取
        return settings_manager.get_default_path()

    @staticmethod
    def get_app_version() -> str:
        # 从配置文件读取
        return settings_manager.get_version()

    @staticmethod
    def browse_folder(default_path) -> str:
        """弹出文件夹选择对话框"""
        # 假设 FileHandler 有选择目录的方法，若没有可调用底层 QFileDialog
        path = FileHandler.open_directory(caption="选择项目保存位置",default_dir=default_path)
        return path if path else ""

    @staticmethod
    def get_recent_projects() -> list:
        """前端初始化时调用，获取历史记录"""
        return settings_manager.get_recent_projects()

    @staticmethod
    def create_project(project_data: dict) -> str:
        """创建项目目录及初始化文件"""
        name = project_data.get("name")
        base_dir = project_data.get("path")
        mode = project_data.get("mode", "3d")  # 获取前端传来的 mode
        target_full_path = os.path.join(base_dir, name)

        # 调用工具类处理物理复制和配置修改
        ProjectCopy.create_from_template(target_full_path, name, mode)
        settings_manager.set_default_path(base_dir)
        return target_full_path

    @staticmethod
    def create_world_project(world_data: dict) -> dict:
        """AI 世界创建专用：自动命名 + 保存到引擎 data 目录，无需用户指定 name/path。

        与 create_project 的区别：不接收 name/path，全部由后端决定：
        - 保存位置固定为引擎根目录下的 data/（core_path.repo_root/data）
        - 名称按"模式 + 递增编号"自动生成并防重名（创造世界_1 / 剧情世界_1 ...）
        - 把 worldPrompt 写入 project.ini 的 [Project] world_prompt，供引擎/AI 后续读取
        返回 {name, path}，与打开普通项目的返回结构一致。
        """
        import configparser
        from utils.settings import core_path

        mode = world_data.get("mode", "creative")
        prompt = world_data.get("prompt", "") or ""

        # 引擎 data 目录（不存在则创建）
        base_dir = os.path.join(str(core_path.repo_root), "data")
        os.makedirs(base_dir, exist_ok=True)

        # 模式 + 递增编号，防重名
        label = "剧情世界" if mode == "story" else "创造世界"
        index = 1
        while os.path.exists(os.path.join(base_dir, f"{label}_{index}")):
            index += 1
        final_name = f"{label}_{index}"
        target_full_path = os.path.join(base_dir, final_name)

        # 复制模板并初始化 project.ini
        project_ini = ProjectCopy.create_from_template(target_full_path, final_name, mode)

        # 把世界提示词持久化进 project.ini，供引擎/AI 后续读取
        try:
            cfg = configparser.ConfigParser()
            cfg.read(project_ini, encoding='utf-8')
            if 'Project' not in cfg:
                cfg['Project'] = {}
            cfg['Project']['world_prompt'] = prompt
            with open(project_ini, 'w', encoding='utf-8') as f:
                cfg.write(f)
        except Exception as e:
            logger.error(f"Failed to persist world_prompt: {e}")

        return {"name": final_name, "path": os.path.dirname(project_ini)}

    @staticmethod
    def open_project(project_path: str) -> bool:
        """执行打开项目的逻辑（加载资源、初始化环境等）"""
        return ProjectCopy.open_and_update(project_path)

    @staticmethod
    def set_project_mode(mode_data: dict) -> bool:
        """设置当前编辑器的工作模式 (2D/3D/Render)"""
        mode = mode_data.get("mode")
        settings = mode_data.get("settings")
        logger.info(f"Switching editor mode to: {mode} with settings: {settings}")
        # 这里可以根据模式调整渲染引擎参数
        return True

    @staticmethod
    def open_project_file() -> dict:
        """
        弹出文件选择框，可选 project.ini 项目，或 Vision 场景 .json。
        选中 .json 时即时新建一个轻量项目（external_live 模式）承载，返回其目录，
        前端按打开普通项目的流程处理即可；引擎启动后由 MainView 自动加载/导入。
        成功后返回项目信息并更新最近项目列表。
        """
        # 1. 弹出对话框：项目 .ini 或 Vision 场景 .json
        _, file_path = FileHandler.open_file(
            caption="打开项目或 Vision 场景",
            file_types="项目或 Vision 场景 (*.ini *.json)",
            default_dir=settings_manager.get_default_path(),
            read_content=False
        )

        if not file_path or not os.path.exists(file_path):
            return {}

        # 2a. Vision 场景：新建轻量项目承载
        if file_path.lower().endswith('.json'):
            return ProjectLauncher._create_project_from_vision(file_path)

        # 2b. 普通 .ini 项目：原有逻辑
        project_dir = os.path.dirname(file_path)
        try:
            import configparser
            proj_cfg = configparser.ConfigParser()
            proj_cfg.read(file_path, encoding='utf-8')
            project_name = proj_cfg.get('Project', 'name', fallback="default_project")

            # 3. 更新全局最近项目记录 (存储的是目录路径)
            settings_manager.add_recent_project(project_dir)

            # 4. 返回前端 handleSideAction 所需的结构
            return {
                "name": project_name,
                "path": project_dir
            }
        except Exception as e:
            logger.error(f"Failed to open project file: {e}")
            return {}

    @staticmethod
    def _create_project_from_vision(json_path: str) -> dict:
        """为一个 Vision 场景 .json 新建轻量项目（纯文件 IO，不依赖引擎）。

        复制项目模板，把 [vision] source_path + import_mode=external_live 写入模板
        入口场景的 .scene 文件；真正的代理 actor / 相机 / 绑定导入延迟到引擎启动后，
        由 MainView._apply_vision_source_for_scene 的 external_live 分支首次完成。
        返回 {name, path}，与打开普通项目的返回结构一致。
        """
        try:
            if not CoronaEditor.CoronaEngine.is_vision_available():
                logger.error("Vision backend is not available in this build")
                return {}
            abs_json = os.path.abspath(json_path)
            if not os.path.isfile(abs_json):
                logger.error("Vision scene file not found: %s", abs_json)
                return {}

            base_dir = settings_manager.get_default_path()
            project_name = os.path.splitext(os.path.basename(abs_json))[0]

            # 目标目录重名则加后缀
            target_path = os.path.join(base_dir, project_name)
            counter = 1
            while os.path.exists(target_path):
                target_path = os.path.join(base_dir, f"{project_name}_{counter}")
                counter += 1
            final_name = os.path.basename(target_path)

            # 1. 复制项目模板（写 project.ini，加入最近项目）
            project_ini = ProjectCopy.create_from_template(target_path, final_name, '3d')
            project_dir = os.path.dirname(project_ini)

            # 2. 定位模板入口场景文件
            import configparser
            cfg = configparser.ConfigParser()
            cfg.read(project_ini, encoding='utf-8')
            entrance = cfg.get('Project', 'entrance_scene', fallback='').strip()
            if not entrance:
                logger.error("Template project has no entrance_scene: %s", project_ini)
                return {}
            scene_file = os.path.join(project_dir, *entrance.split('/'))
            if not os.path.isfile(scene_file):
                logger.error("Entrance scene file not found: %s", scene_file)
                return {}

            # 3. 往入口 .scene 注入 [vision] 元数据（格式同 Scene.save_data）
            scene_cfg = configparser.ConfigParser()
            scene_cfg.read(scene_file, encoding='utf-8')
            if 'vision' not in scene_cfg:
                scene_cfg['vision'] = {}
            scene_cfg['vision']['source_path'] = abs_json
            scene_cfg['vision']['import_mode'] = 'external_live'
            with open(scene_file, 'w', encoding='utf-8') as f:
                scene_cfg.write(f)

            logger.info("Created lightweight project for Vision scene: %s -> %s", abs_json, project_dir)
            return {"name": final_name, "path": project_dir}
        except Exception as e:
            logger.exception("Failed to create project from Vision scene: %s", e)
            return {}
