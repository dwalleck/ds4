"""Print native toolchain, GPU and Windows commit information; changes no settings."""
import ctypes
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess

from build import LOCAL, environment, require_arm64


class MemoryStatus(ctypes.Structure):
    _fields_ = [("length", ctypes.c_ulong), ("load_percent", ctypes.c_ulong),
                ("physical_total", ctypes.c_ulonglong), ("physical_available", ctypes.c_ulonglong),
                ("commit_limit", ctypes.c_ulonglong), ("commit_available", ctypes.c_ulonglong),
                ("virtual_total", ctypes.c_ulonglong), ("virtual_available", ctypes.c_ulonglong),
                ("extended_available", ctypes.c_ulonglong)]


def main():
    if os.name != "nt":
        raise SystemExit("Windows only")
    status = MemoryStatus()
    status.length = ctypes.sizeof(status)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
        raise ctypes.WinError()
    report = {"host": platform.machine(), "windows": platform.version(),
              "memory_bytes": {name: getattr(status, name) for name, _ in status._fields_[2:]},
              "memory_load_percent": status.load_percent}
    try:
        env = environment(None)
        report["msvc"] = env.get("VCTOOLSVERSION")
        report["sdk"] = env.get("WINDOWSSDKVERSION")
        report["host_arch"] = env.get("VSCMD_ARG_HOST_ARCH")
        report["target_arch"] = env.get("VSCMD_ARG_TGT_ARCH")
    except (OSError, subprocess.CalledProcessError, RuntimeError) as error:
        env = os.environ.copy()
        report["visual_studio_error"] = str(error)
    tools = {
        "clang": LOCAL / "clang+llvm-23.1.4-aarch64-pc-windows-msvc/bin/clang-cl.exe",
        "nvcc": Path(os.environ.get("CUDA_PATH", str(LOCAL / "cuda-13.4.92/nvidia/cu13"))) / "bin/nvcc.exe",
    }
    report["tools"] = {}
    for name, path in tools.items():
        if path.is_file():
            try:
                require_arm64(path)
                result = subprocess.run([str(path), "--version"], env=env, capture_output=True, text=True)
                report["tools"][name] = {"path": str(path), "status": result.returncode,
                                         "version": (result.stdout + result.stderr).strip()}
            except (OSError, RuntimeError) as error:
                report["tools"][name] = {"path": str(path), "error": str(error)}
        else:
            report["tools"][name] = {"path": str(path), "missing": True}
    nvidia_smi = shutil.which("nvidia-smi")
    if nvidia_smi:
        result = subprocess.run([nvidia_smi, "--query-gpu=name,driver_version,memory.total,memory.free",
                                 "--format=csv"], capture_output=True, text=True)
        report["gpu"] = {"status": result.returncode, "output": (result.stdout + result.stderr).strip()}
    else:
        report["gpu"] = {"missing": True}
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
