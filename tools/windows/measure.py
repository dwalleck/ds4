"""Run one native workload and record Windows memory and logical I/O counters."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import os
from pathlib import Path
import subprocess
import time


class MemoryStatus(C.Structure):
    _fields_ = [("length", W.DWORD), ("load", W.DWORD),
                *[(name, C.c_ulonglong) for name in (
                    "physical_total", "physical_available", "commit_limit",
                    "commit_available", "virtual_total", "virtual_available", "extended")]]


class ProcessMemory(C.Structure):
    _fields_ = [("cb", W.DWORD), ("page_faults", W.DWORD),
                *[(name, C.c_size_t) for name in (
                    "peak_working_set", "working_set", "peak_paged_pool", "paged_pool",
                    "peak_nonpaged_pool", "nonpaged_pool", "pagefile", "peak_pagefile", "private")]]


class IO(C.Structure):
    _fields_ = [(name, C.c_ulonglong) for name in (
        "read_ops", "write_ops", "other_ops", "read_bytes", "write_bytes", "other_bytes")]


class GpuMemory(C.Structure):
    _fields_ = [(name, C.c_ulonglong) for name in ("total", "free", "used")]


class CounterValue(C.Structure):
    _fields_ = [("status", W.DWORD), ("value", C.c_double)]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, required=True, help="New results directory")
    ap.add_argument("--env", action="append", default=[], metavar="NAME=VALUE",
                    help="Child-only environment override, recorded in the result")
    ap.add_argument("command", nargs=argparse.REMAINDER)
    args = ap.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if os.name != "nt" or not command:
        ap.error("Windows and a workload command are required")
    overrides = {}
    for item in args.env:
        key, separator, value = item.partition("=")
        if not separator or not key:
            ap.error("--env requires NAME=VALUE")
        overrides[key] = value
    child_env = dict(os.environ, **overrides)
    args.out.mkdir(parents=True, exist_ok=False)
    kernel = C.WinDLL("kernel32", use_last_error=True)
    psapi = C.WinDLL("psapi", use_last_error=True)
    kernel.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
    kernel.OpenProcess.restype = W.HANDLE
    kernel.CloseHandle.argtypes = [W.HANDLE]
    kernel.CloseHandle.restype = W.BOOL
    kernel.GlobalMemoryStatusEx.argtypes = [C.POINTER(MemoryStatus)]
    kernel.GlobalMemoryStatusEx.restype = W.BOOL
    kernel.GetProcessIoCounters.argtypes = [W.HANDLE, C.POINTER(IO)]
    kernel.GetProcessIoCounters.restype = W.BOOL
    psapi.GetProcessMemoryInfo.argtypes = [W.HANDLE, C.POINTER(ProcessMemory), W.DWORD]
    psapi.GetProcessMemoryInfo.restype = W.BOOL
    monitor_errors = []
    nvml = None
    device = C.c_void_p()
    try:
        nvml = C.CDLL("nvml.dll")
        nvml.nvmlInit_v2.restype = C.c_int
        nvml.nvmlDeviceGetHandleByIndex_v2.argtypes = [C.c_uint, C.POINTER(C.c_void_p)]
        nvml.nvmlDeviceGetMemoryInfo.argtypes = [C.c_void_p, C.POINTER(GpuMemory)]
        if nvml.nvmlInit_v2() != 0:
            raise RuntimeError("NVML initialization failed")
        if nvml.nvmlDeviceGetHandleByIndex_v2(0, C.byref(device)) != 0:
            nvml.nvmlShutdown()
            raise RuntimeError("NVML GPU 0 lookup failed")
    except (OSError, RuntimeError) as error:
        monitor_errors.append(str(error))
        nvml = None

    pdh = C.WinDLL("pdh")
    query, counter = C.c_void_p(), C.c_void_p()
    pdh.PdhOpenQueryW.argtypes = [W.LPCWSTR, C.c_size_t, C.POINTER(C.c_void_p)]
    pdh.PdhAddEnglishCounterW.argtypes = [C.c_void_p, W.LPCWSTR, C.c_size_t, C.POINTER(C.c_void_p)]
    pdh.PdhCollectQueryData.argtypes = [C.c_void_p]
    pdh.PdhGetFormattedCounterValue.argtypes = [C.c_void_p, W.DWORD, C.c_void_p, C.POINTER(CounterValue)]
    pdh.PdhCloseQuery.argtypes = [C.c_void_p]
    status = pdh.PdhOpenQueryW(None, 0, C.byref(query))
    if status == 0:
        status = pdh.PdhAddEnglishCounterW(query, r"\PhysicalDisk(_Total)\Disk Read Bytes/sec", 0, C.byref(counter))
    if status == 0:
        status = pdh.PdhCollectQueryData(query)
    if status:
        monitor_errors.append("PDH disk counter unavailable: " + hex(status & 0xffffffff))
        counter = C.c_void_p()

    def gpu_snapshot():
        result = subprocess.run(["nvidia-smi", "--query-gpu=name,driver_version,memory.total,memory.used,memory.free",
                                 "--format=csv,noheader,nounits"], capture_output=True, text=True)
        return {"returncode": result.returncode, "output": result.stdout.strip(), "error": result.stderr.strip()}

    before = gpu_snapshot()
    started = time.monotonic()
    samples = []
    handle = None
    child = None
    try:
        with (args.out / "stdout.log").open("wb") as stdout, (args.out / "stderr.log").open("wb") as stderr:
            child = subprocess.Popen(command, stdout=stdout, stderr=stderr,
                                     creationflags=subprocess.CREATE_NEW_PROCESS_GROUP, env=child_env)
            handle = kernel.OpenProcess(0x0400 | 0x0010, False, child.pid)
            if not handle:
                raise C.WinError(C.get_last_error())
            with (args.out / "samples.jsonl").open("w", encoding="utf-8") as log:
                while True:
                    memory = MemoryStatus(length=C.sizeof(MemoryStatus))
                    process = ProcessMemory(cb=C.sizeof(ProcessMemory))
                    io = IO()
                    if not kernel.GlobalMemoryStatusEx(C.byref(memory)):
                        raise C.WinError(C.get_last_error())
                    sample = {"seconds": time.monotonic() - started,
                              "system_commit": memory.commit_limit - memory.commit_available,
                              "system_commit_limit": memory.commit_limit,
                              "physical_available": memory.physical_available}
                    if psapi.GetProcessMemoryInfo(handle, C.byref(process), C.sizeof(process)):
                        sample.update(private=process.private, working_set=process.working_set,
                                      peak_working_set=process.peak_working_set, page_faults=process.page_faults)
                    if kernel.GetProcessIoCounters(handle, C.byref(io)):
                        sample.update({name: getattr(io, name) for name, _ in IO._fields_})
                    if nvml:
                        gpu = GpuMemory()
                        if nvml.nvmlDeviceGetMemoryInfo(device, C.byref(gpu)) == 0:
                            sample.update(gpu_used=gpu.used, gpu_free=gpu.free, gpu_total=gpu.total)
                    if counter and pdh.PdhCollectQueryData(query) == 0:
                        disk = CounterValue()
                        if pdh.PdhGetFormattedCounterValue(counter, 0x200, None, C.byref(disk)) == 0 and disk.status in (0, 1):
                            sample["system_disk_read_bytes_per_second"] = disk.value
                    samples.append(sample)
                    log.write(json.dumps(sample) + "\n")
                    log.flush()
                    if child.poll() is not None:
                        break
                    time.sleep(1)
    finally:
        if child is not None and child.poll() is None:
            child.terminate()
            child.wait()
        if handle:
            kernel.CloseHandle(handle)
        if query:
            pdh.PdhCloseQuery(query)
        if nvml:
            nvml.nvmlShutdown()
    summary = {"command": command, "environment_overrides": overrides, "returncode": child.returncode,
               "elapsed_seconds": time.monotonic() - started,
               "gpu_before": before, "gpu_after": gpu_snapshot(),
               "peak_private": max((s.get("private", 0) for s in samples), default=0),
               "peak_working_set": max((s.get("peak_working_set", 0) for s in samples), default=0),
               "peak_system_commit": max(s["system_commit"] for s in samples),
               "peak_gpu_used": max((s["gpu_used"] for s in samples if "gpu_used" in s), default=None),
               "peak_system_disk_read_bytes_per_second": max((s["system_disk_read_bytes_per_second"] for s in samples
                                                             if "system_disk_read_bytes_per_second" in s), default=None),
               "monitor_errors": monitor_errors,
               "last_counters": samples[-1],
               "io_scope": "Process logical I/O is distinct from machine-wide physical disk read rate. GPU 0 memory is device-wide; other processes contribute to GPU, disk, and system commit counters."}
    (args.out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return child.returncode


if __name__ == "__main__":
    raise SystemExit(main())
