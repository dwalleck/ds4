"""Native Windows ARM64 build; leaves the Unix Makefile untouched."""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
LOCAL = Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "ds4-tools"
CORE = ["ds4.c", "ds4_image.c", "ds4_distributed.c", "ds4_tp.c", "ds4_ssd.c",
        "ds4_layer_pack.c", "ds4_engram.c"]
PROGRAMS = {
    "ds4": ["ds4_cli.c", "ds4_help.c", "ds4_prompt_prefix.c", "linenoise.c", "ds4_gpu_args.c"],
    "ds4-server": ["ds4_server.c", "ds4_help.c", "ds4_kvstore.c", "rax.c", "ds4_gpu_args.c"],
    "ds4-bench": ["ds4_bench.c", "ds4_help.c", "ds4_gpu_args.c"],
    "ds4-eval": ["ds4_eval.c", "ds4_eval_cases.c", "ds4_help.c", "linenoise.c"],
    "ds4-agent": ["ds4_agent.c", "ds4_help.c", "ds4_prompt_prefix.c", "ds4_web.c", "ds4_kvstore.c", "linenoise.c", "ds4_gpu_args.c"],
}
TESTS = {
    "ds4_test": (["tests/ds4_test.c", "ds4_help.c", "ds4_kvstore.c", "rax.c", "ds4_gpu_args.c"], False),
    "ds4_agent_test": (["tests/ds4_agent_test.c", "ds4_help.c", "ds4_prompt_prefix.c", "ds4_web.c", "ds4_kvstore.c", "linenoise.c", "ds4_gpu_args.c"], False),
    "test_session_state": (["tests/test_session_state.c"], True),
    "test_windows_unsupported": (["tests/test_windows_unsupported.c", "ds4_gpu_args.c"], False),
}
CUDA_TESTS = ("test_cuda_tokentile", "test_cuda_ssd_cache",
              "test_cuda_q8_rows", "test_cuda_compressor")


def run(args, env, cwd):
    print(subprocess.list2cmdline([str(x) for x in args]), flush=True)
    subprocess.run([str(x) for x in args], env=env, cwd=cwd, check=True)


def environment(vs):
    if os.name != "nt":
        raise RuntimeError("This entry point requires Windows; use make on Unix")
    if not vs:
        vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
        vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-property", "installationPath"], text=True).strip()
    vcvars = Path(vs) / "VC/Auxiliary/Build/vcvarsall.bat"
    command = f'cmd.exe /d /s /c ""{vcvars}" arm64 >nul && set"'
    result = subprocess.run(command, capture_output=True, text=True, check=True)
    env = dict(os.environ)
    for line in result.stdout.splitlines():
        if "=" in line and not line.startswith("="):
            key, value = line.split("=", 1)
            env[key.upper()] = value
    if env.get("VSCMD_ARG_TGT_ARCH", "").lower() != "arm64":
        raise RuntimeError("Visual Studio did not select ARM64")
    return env


def require_arm64(path):
    with path.open("rb") as f:
        if f.read(2) != b"MZ":
            raise RuntimeError(f"Not a PE executable: {path}")
        f.seek(0x3c)
        offset = struct.unpack("<I", f.read(4))[0]
        f.seek(offset)
        if f.read(4) != b"PE\0\0" or struct.unpack("<H", f.read(2))[0] != 0xaa64:
            raise RuntimeError(f"Not ARM64 (0xAA64): {path}")
    print(f"PE ARM64 0xAA64: {path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("target", choices=("smoke", "cuda-smoke", "compat", "cpu", "cuda"))
    ap.add_argument("--vs", help="Visual Studio installation root (otherwise vswhere)")
    ap.add_argument("--clang", type=Path, default=LOCAL / "clang+llvm-23.1.4-aarch64-pc-windows-msvc/bin/clang-cl.exe")
    ap.add_argument("--cuda", type=Path, default=Path(os.environ.get("CUDA_PATH", str(LOCAL / "cuda-13.4.92/nvidia/cu13"))))
    ap.add_argument("--out", type=Path, default=ROOT / "build/windows-arm64")
    ap.add_argument("--program", choices=tuple(PROGRAMS), action="append")
    ap.add_argument("--test", choices=(*TESTS, *CUDA_TESTS), action="append",
                    help="Build a test without running model-dependent cases")
    args = ap.parse_args()
    if args.target != "cuda" and any(t in CUDA_TESTS for t in args.test or ()):
        ap.error("CUDA numerical tests require the cuda target")
    env = environment(args.vs)
    out = args.out.resolve() / args.target
    out.mkdir(parents=True, exist_ok=True)
    clang = args.clang.resolve()
    if not clang.is_file():
        raise RuntimeError("clang-cl missing; pass --clang with the portable LLVM path")
    cc = [str(clang), "--target=aarch64-pc-windows-msvc", "/nologo", "/MD", "/O2", "/W3",
          "/D_CRT_SECURE_NO_WARNINGS", "/D_CRT_NONSTDC_NO_WARNINGS", "/D_CRT_DECLARE_NONSTDC_NAMES=0",
          "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/I" + str(ROOT / "compat/win32/include"), "/I" + str(ROOT)]
    cc.append("/I" + str(ROOT / "third_party/tre/include"))
    libs = ["ws2_32.lib", "psapi.lib", "advapi32.lib", "bcrypt.lib"]
    built = {}

    def compile_source(source, extra=()):
        key = str(source)
        if key in built:
            return built[key]
        src = ROOT / source
        obj = out / (str(source).replace("/", "_").replace("\\", "_") + ".obj")
        language = ["/TP", "/std:c++17", "/EHsc"] if src.suffix == ".cpp" else ["/TC", "/std:c11"]
        run([*cc, *language, *extra, "/c", src, "/Fo" + str(obj)], env, out)
        built[key] = obj
        return obj

    def link(name, objects, extra=()):
        exe = out / (name + ".exe")
        run([clang, "--target=aarch64-pc-windows-msvc", "/nologo", "/MD", *objects,
             "/Fe" + str(exe), "/link", "/MACHINE:ARM64", "/STACK:8388608", *libs, *extra], env, out)
        require_arm64(exe)
        return exe

    if args.target == "smoke":
        for compiler, name in [(shutil.which("cl.exe", path=env["PATH"]), "msvc-smoke"), (clang, "clang-smoke")]:
            exe = out / (name + ".exe")
            run([compiler, "/nologo", "/TC", "/std:c11", "/W4", "/WX", ROOT / "tools/windows/smoke.c",
                 "/Fe" + str(exe), "/Fo" + str(out / (name + ".obj"))], env, out)
            require_arm64(exe)
            run([exe], env, out)
        return
    if args.target == "cuda-smoke":
        nvcc = args.cuda.resolve() / "bin/nvcc.exe"
        exe = out / "cuda-smoke.exe"
        env["PATH"] = str(nvcc.parent) + os.pathsep + env["PATH"]
        run([nvcc, "-arch=sm_121a", "-target-dir=arm64", "--cudart=shared", "-ccbin",
             str(Path(shutil.which("cl.exe", path=env["PATH"])).parent),
             ROOT / "tools/windows/smoke.cu", "-o", exe], env, out)
        require_arm64(exe)
        run([exe], env, out)
        return
    compatibility = [compile_source(p.relative_to(ROOT)) for p in sorted((ROOT / "compat/win32").glob("*.cpp"))]
    compatibility += [compile_source(p.relative_to(ROOT)) for p in sorted((ROOT / "compat/win32").glob("*.c"))]
    compatibility += [compile_source(Path("third_party/tre/lib") / (name + ".c"),
                                      ["/FI" + str(ROOT / "third_party/tre/config.h")])
                      for name in ("regcomp", "regexec", "regerror", "tre-ast", "tre-compile",
                                   "tre-match-backtrack", "tre-match-parallel", "tre-mem", "tre-parse", "tre-stack")]
    if args.target == "compat":
        for test in sorted((ROOT / "tests").glob("test_win32_*.c")):
            extra = [compile_source("linenoise.c")] if test.stem == "test_win32_linenoise" else []
            exe = link(test.stem, [compile_source(test.relative_to(ROOT)), *compatibility, *extra])
            run([exe], env, out)
        return
    if args.target == "cpu":
        cc.append("/DDS4_NO_GPU")
    core = [compile_source(p) for p in CORE]
    if args.target == "cuda":
        nvcc = args.cuda.resolve() / "bin/nvcc.exe"
        env["PATH"] = str(nvcc.parent) + os.pathsep + env["PATH"]
        # cl.exe has no #include_next. Shadow headers use these generated
        # forwards only with MSVC; never hard-code the installed SDK revision.
        forwarding = out / "crt-forward"
        for name in ("fcntl.h", "limits.h", "signal.h", "stdio.h", "string.h",
                     "time.h", "sys/stat.h", "sys/types.h"):
            original = next((Path(folder) / name for folder in env["INCLUDE"].split(";")
                             if (Path(folder) / name).is_file()), None)
            if original is None:
                raise RuntimeError(f"Cannot find CRT header {name}")
            wrapper = forwarding / "ds4_ucrt" / name
            wrapper.parent.mkdir(parents=True, exist_ok=True)
            wrapper.write_text('#pragma once\n#include "' + original.as_posix() + '"\n')
        cuda_sources = [ROOT / "ds4_cuda.cu", *(ROOT / "cuda/mmq").glob("*.cu")]
        for source in cuda_sources:
            obj = out / (source.stem + ".cuda.obj")
            run([nvcc, "-O3", "--use_fast_math", "-std=c++17",
                 "-gencode", "arch=compute_121a,code=sm_121a", "-target-dir=arm64",
                 "-DDS4_CUDA_HAVE_MXF4=1", "-D_CRT_DECLARE_NONSTDC_NAMES=0", "-DNOMINMAX",
                 "-DWIN32_LEAN_AND_MEAN", "-I" + str(forwarding), "-Xcompiler=/MD,/Zc:preprocessor",
                 "-I" + str(ROOT / "compat/win32/include"), "-I" + str(ROOT / "cuda/mmq"),
                 "-c", source, "-o", obj], env, out)
            core.append(obj)
        libs.append(str(args.cuda.resolve() / "lib/arm64/cudart.lib"))
        # NVIDIA's ARM64 wheel ships cuBLAS exports but no import library.
        # Generate that link-only artifact from the verified native DLL.
        dll = args.cuda.resolve() / "bin/arm64/cublas64_13.dll"
        require_arm64(dll)
        exports = subprocess.check_output([str(clang.parent / "llvm-readobj.exe"),
                                           "--coff-exports", str(dll)], text=True)
        names = [line.split("Name: ", 1)[1].strip() for line in exports.splitlines()
                 if line.strip().startswith("Name: ")]
        if not names:
            raise RuntimeError("cuBLAS DLL has no named exports")
        definition = out / "cublas.def"
        definition.write_text("LIBRARY cublas64_13.dll\nEXPORTS\n" + "\n".join(names) + "\n")
        import_lib = out / "cublas.lib"
        run([shutil.which("lib.exe", path=env["PATH"]), "/nologo", "/MACHINE:ARM64",
             "/DEF:" + str(definition), "/OUT:" + str(import_lib)], env, out)
        libs.append(str(import_lib))
        for name in ("cudart64_13.dll", "cublas64_13.dll", "cublasLt64_13.dll"):
            source = args.cuda.resolve() / "bin/arm64" / name
            require_arm64(source)
            shutil.copy2(source, out / name)
    for name in args.program or ([] if args.test else PROGRAMS):
        link(name, [*[compile_source(p) for p in PROGRAMS[name]], *core, *compatibility])
    for name in args.test or []:
        if name in CUDA_TESTS:
            sources = ["tests/" + name + ".c"]
            test_core = [built["ds4_image.c"], *core[len(CORE):]]
        else:
            sources, includes_engine = TESTS[name]
            test_core = [obj for source, obj in zip(CORE, core)
                         if not includes_engine or source != "ds4.c"]
            test_core += core[len(CORE):]
        link(name, [*[compile_source(p) for p in sources], *test_core, *compatibility])
    (out / "build.json").write_text(json.dumps({"target": args.target, "clang": str(clang),
        "cuda": str(args.cuda.resolve()), "sdk": env.get("WINDOWSSDKVERSION"),
        "msvc": env.get("VCTOOLSVERSION")}, indent=2) + "\n")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError, OSError) as error:
        print(f"Windows build failed: {error}", file=sys.stderr)
        sys.exit(1)
