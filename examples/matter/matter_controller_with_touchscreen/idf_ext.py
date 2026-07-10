import importlib.util
import sys
from pathlib import Path


def _bmgr_callback(target_name: str, ctx, args, **kwargs) -> None:
    raise RuntimeError("ESP Board Manager component is not available yet")


_FALLBACK_ACTIONS = {
    "actions": {
        "bmgr": {
            "callback": _bmgr_callback,
            "options": [],
            "short_help": "Run ESP Board Manager",
        },
    }
}


def _load_module(file_path: Path, module_name: str):
    spec = importlib.util.spec_from_file_location(module_name, file_path)
    if spec is None or spec.loader is None:
        raise ImportError(f"Failed to create spec for {file_path}")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = mod
    spec.loader.exec_module(mod)
    return mod


def action_extensions(base_actions: dict, project_path: str) -> dict:
    bmgr_ext = (
        Path(project_path)
        / "managed_components"
        / "espressif__esp_board_manager"
        / "idf_ext.py"
    )
    if not bmgr_ext.exists():
        return _FALLBACK_ACTIONS

    bmgr = _load_module(bmgr_ext, "matter_controller_m5tab_bmgr_idf_ext")
    actions = bmgr.action_extensions(base_actions, project_path)
    bmgr_action = actions.get("actions", {}).get("bmgr")
    if isinstance(bmgr_action, dict) and "callback" in bmgr_action:
        callback = bmgr_action["callback"]

        def wrapped_bmgr(target_name: str, ctx, args, **kwargs) -> None:
            callback(target_name, ctx, args, **kwargs)
            project_dir = Path(project_path)
            sdkconfig = project_dir / "sdkconfig"
            if sdkconfig.exists():
                sdkconfig.unlink()
                sdkconfig_json = project_dir / "build" / "config" / "sdkconfig.json"
                if sdkconfig_json.exists():
                    sdkconfig_json.unlink()
                print(
                    "[Board Manager] Removed sdkconfig; next build will apply board_manager.defaults"
                )

        bmgr_action["callback"] = wrapped_bmgr
    actions.get("actions", {}).pop("gen-bmgr-config", None)
    return actions
