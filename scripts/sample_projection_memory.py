"""Sample private RAM and WDDM memory of the D3D12 game process named by SaveCheck."""

import argparse
import csv
import ctypes as ct
from ctypes import wintypes as win
from pathlib import Path
import re
import time


MIB = 1024 ** 2
PERIOD = 0.2
IDENTITY = re.compile(
    r'LogSaveCheckProjectionMemory: Display: identity pid=(\d+) luid=(\w+)'
    r' ism=([01]) version=(\S+) adapter="([^"]+)" dll="([^"]+)"'
)
REGION_MARK = re.compile(r"event=window_begin phase=(\w+) step=(\d+) cycle=(\d+) mode=(\w+) qpc=(\d+)")
VIEW_MARK = re.compile(r"views step=(\d+) cohort=current")


class ProcessMemory(ct.Structure):
    # PROCESS_MEMORY_COUNTERS_EX2, available on Windows 11 22H2 and newer.
    _fields_ = [
        ("size", win.DWORD),
        ("page_faults", win.DWORD),
        *[(field, ct.c_size_t) for field in (
            "peak_working_set", "working_set", "peak_paged_pool", "paged_pool",
            "peak_nonpaged_pool", "nonpaged_pool", "pagefile", "peak_pagefile",
            "private_bytes", "private_working_set",
        )],
        ("shared_commit", ct.c_uint64),
    ]


class MemoryRegion(ct.Structure):
    # 64-bit MEMORY_BASIC_INFORMATION.
    _fields_ = [
        ("base", ct.c_void_p), ("allocation_base", ct.c_void_p), ("allocation_protection", win.DWORD),
        ("partition", win.WORD), ("size", ct.c_size_t), ("state", win.DWORD),
        ("protection", win.DWORD), ("type", win.DWORD),
    ]


class CounterValue(ct.Structure):
    _fields_ = [("status", win.DWORD), ("value", ct.c_int64)]


class CounterItem(ct.Structure):
    _fields_ = [("name", win.LPWSTR), ("sample", CounterValue)]


def bind(library, name, result, *args):
    function = getattr(library, name)
    function.argtypes = args
    function.restype = result
    return function


def require_pdh(status, name):
    if status:
        raise OSError(f"{name}: PDH 0x{status & 0xFFFFFFFF:08x}")


class WindowsMemory:
    def __init__(self, pid, luid, ram_only=False):
        self.prefix = f"pid_{pid}_{luid}_".lower()
        kernel = ct.WinDLL("kernel32", use_last_error=True)
        pdh = ct.WinDLL("pdh")
        number = ct.POINTER(ct.c_int64)
        self.query_clock = bind(kernel, "QueryPerformanceCounter", win.BOOL, number)
        clock_frequency = bind(kernel, "QueryPerformanceFrequency", win.BOOL, number)
        self.frequency = ct.c_int64()
        if not clock_frequency(ct.byref(self.frequency)):
            raise ct.WinError(ct.get_last_error())
        self.get_memory = bind(kernel, "K32GetProcessMemoryInfo", win.BOOL, win.HANDLE, ct.c_void_p, win.DWORD)
        self.wait = bind(kernel, "WaitForSingleObject", win.DWORD, win.HANDLE, win.DWORD)
        self.close_handle = bind(kernel, "CloseHandle", win.BOOL, win.HANDLE)
        self.open_query = bind(pdh, "PdhOpenQueryW", ct.c_long, win.LPCWSTR, ct.c_size_t, ct.POINTER(win.HANDLE))
        self.close_query = bind(pdh, "PdhCloseQuery", ct.c_long, win.HANDLE)
        self.add_counter = bind(pdh, "PdhAddEnglishCounterW", ct.c_long, win.HANDLE, win.LPCWSTR,
                                ct.c_size_t, ct.POINTER(win.HANDLE))
        self.collect = bind(pdh, "PdhCollectQueryData", ct.c_long, win.HANDLE)
        self.array = bind(pdh, "PdhGetFormattedCounterArrayW", ct.c_long, win.HANDLE, win.DWORD,
                          ct.POINTER(win.DWORD), ct.POINTER(win.DWORD), ct.c_void_p)
        open_process = bind(kernel, "OpenProcess", win.HANDLE, win.DWORD, win.BOOL, win.DWORD)
        self.process = open_process(0x00100410, False, pid)  # synchronize, query information, read VM
        if not self.process:
            raise ct.WinError(ct.get_last_error())
        self.query = win.HANDLE()
        self.counters = {}
        if ram_only:
            return
        try:
            require_pdh(self.open_query(None, 0, ct.byref(self.query)), "PdhOpenQueryW")
            for kind in ("Dedicated Usage", "Shared Usage"):
                handle = win.HANDLE()
                require_pdh(self.add_counter(self.query, rf"\GPU Process Memory(*)\{kind}",
                                             0, ct.byref(handle)), kind)
                self.counters[kind] = handle
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.query:
            self.close_query(self.query)
        self.close_handle(self.process)

    def running(self):
        result = self.wait(self.process, 0)
        if result not in (0, 258):
            raise OSError(f"Unexpected process wait status {result}")
        return result == 258

    def qpc(self):
        result = ct.c_int64()
        if not self.query_clock(ct.byref(result)):
            raise ct.WinError(ct.get_last_error())
        return result.value

    def gpu_values(self, counter):
        size, count = win.DWORD(), win.DWORD()
        format_value = 0x400 | 0x1000  # PDH_FMT_LARGE | PDH_FMT_NOSCALE
        status = self.array(counter, format_value, ct.byref(size), ct.byref(count), None)
        if not status and not count.value:
            return {}
        if status & 0xFFFFFFFF != 0x800007D2:  # PDH_MORE_DATA
            require_pdh(status, "PdhGetFormattedCounterArrayW (size)")
        buffer = ct.create_string_buffer(size.value)
        require_pdh(self.array(counter, format_value, ct.byref(size), ct.byref(count), buffer),
                    "PdhGetFormattedCounterArrayW (values)")
        items = ct.cast(buffer, ct.POINTER(CounterItem))
        return {
            item.name.lower(): item.sample.value
            for item in items[:count.value]
            if item.sample.status in (0, 1) and item.name.lower().startswith(self.prefix)
        }

    def regions(self):
        query = bind(ct.WinDLL("kernel32", use_last_error=True), "VirtualQueryEx", ct.c_size_t,
                     win.HANDLE, ct.c_void_p, ct.POINTER(MemoryRegion), ct.c_size_t)
        totals = dict.fromkeys(("private_commit", "mapped_commit", "image_commit", "reserve"), 0)
        commit_types = {0x20000: "private_commit", 0x40000: "mapped_commit", 0x1000000: "image_commit"}
        address = 0
        region = MemoryRegion()
        start = self.qpc()
        while query(self.process, address, ct.byref(region), ct.sizeof(region)):
            if region.state == 0x1000:  # MEM_COMMIT
                totals[commit_types[region.type]] += region.size
            elif region.state == 0x2000:  # MEM_RESERVE
                totals["reserve"] += region.size
            address = (region.base or 0) + region.size
        if ct.get_last_error() != 87:  # ERROR_INVALID_PARAMETER at end of address space
            raise ct.WinError(ct.get_last_error())
        end = self.qpc()
        return {"query_begin_qpc": start, "query_end_qpc": end,
                **{f"{key}_mib": value / MIB for key, value in totals.items()}}

    def measure(self):
        start = self.qpc()
        ram = ProcessMemory()
        ram.size = ct.sizeof(ram)
        if not self.get_memory(self.process, ct.byref(ram), ram.size):
            raise ct.WinError(ct.get_last_error())
        gpu = {}
        if self.query:
            require_pdh(self.collect(self.query), "PdhCollectQueryData")
            dedicated = self.gpu_values(self.counters["Dedicated Usage"])
            shared = self.gpu_values(self.counters["Shared Usage"])
            if not dedicated or dedicated.keys() != shared.keys():
                return None
            gpu = {
                "dedicated_gpu_mib": sum(dedicated.values()) / MIB,
                "shared_gpu_mib": sum(shared.values()) / MIB,
                "gpu_instances": "|".join(sorted(dedicated)),
            }
        end = self.qpc()
        return {
            "qpc": (start + end) // 2,
            "qpc_hz": self.frequency.value,
            "read_span_ms": (end - start) * 1000 / self.frequency.value,
            "private_working_set_mib": ram.private_working_set / MIB,
            "private_bytes_mib": ram.private_bytes / MIB,
            **gpu,
        }


def wait_for_game(log, timeout):
    deadline = time.monotonic() + timeout
    while not log.exists() and time.monotonic() < deadline:
        time.sleep(0.1)
    with log.open(encoding="utf-8-sig", errors="replace") as stream:
        while time.monotonic() < deadline:
            for line in stream:
                match = IDENTITY.search(line)
                if match:
                    return match.groups()
                if any(marker in line for marker in ("LogSaveCheckProjectionMemory: Display: pass=0 ",
                                                     "LogExit: Exiting.", "LogWindows: Error: appError called:")):
                    raise ValueError(f"Game exited before providing its PID and adapter: {log}")
            time.sleep(0.1)
    raise TimeoutError(f"No PID or D3D12 adapter in {log}")


class RegionSnapshots:
    def __init__(self, log, output):
        self.log = log.open(encoding="utf-8-sig", errors="replace")
        try:
            self.output = output.open("x", encoding="utf-8", newline="")
        except BaseException:
            self.log.close()
            raise
        self.steps = set()
        self.writer = None

    def close(self):
        self.log.close()
        self.output.close()

    def collect(self, memory, pid):
        for line in self.log:
            view = VIEW_MARK.search(line)
            if view:
                self.steps.add(view[1])
            mark = REGION_MARK.search(line)
            if mark and mark[2] in self.steps:
                phase, step, cycle, mode, qpc = mark.groups()
                row = dict(pid=pid, phase=phase, step=step, cycle=cycle, mode=mode, qpc=qpc, **memory.regions())
                if self.writer is None:
                    self.writer = csv.DictWriter(self.output, row, lineterminator="\n")
                    self.writer.writeheader()
                self.writer.writerow(row)
                self.output.flush()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--dll", type=Path, required=True)
    parser.add_argument("--ism", type=int, choices=(0, 1), required=True)
    parser.add_argument("--adapter", default="NVIDIA GeForce RTX 5080")
    parser.add_argument("--ram-only", action="store_true")
    parser.add_argument("--timeout", type=float, default=220, help="Maximum seconds after game PID discovery")
    parser.add_argument("--regions-output", type=Path, help="Record OS address space categories at object checkpoints")
    args = parser.parse_args()
    pid, luid, ism, version, adapter, dll = wait_for_game(args.log, 90)
    if int(ism) != args.ism or adapter != args.adapter or not Path(dll).samefile(args.dll):
        raise ValueError(f"Unexpected game identity: ISM={ism}, adapter={adapter}, DLL={dll}")
    memory = WindowsMemory(int(pid), luid, args.ram_only)
    regions = None
    count = 0
    deadline = time.monotonic() + args.timeout
    try:
        if args.regions_output:
            regions = RegionSnapshots(args.log, args.regions_output)
        with args.output.open("x", encoding="utf-8", newline="") as stream:
            writer = None
            next_sample = time.perf_counter()
            while memory.running():
                if time.monotonic() > deadline:
                    raise TimeoutError(f"Game did not finish the memory route: {args.log}")
                try:
                    row = memory.measure()
                    if regions:
                        regions.collect(memory, pid)
                except OSError:
                    if not memory.running():
                        break
                    raise
                if row is not None:
                    tagged = {"pid": pid, "adapter": adapter, "luid": luid, "ism": ism, "version": version, **row}
                    if writer is None:
                        writer = csv.DictWriter(stream, tagged, lineterminator="\n")
                        writer.writeheader()
                    writer.writerow(tagged)
                    stream.flush()
                    count += 1
                next_sample += PERIOD
                delay = next_sample - time.perf_counter()
                if delay > 0:
                    time.sleep(delay)
                else:
                    next_sample = time.perf_counter()
    finally:
        if regions:
            regions.close()
        memory.close()
    if count == 0 or "LogSaveCheckProjectionMemory: Display: pass=1 " not in args.log.read_text(encoding="utf-8-sig"):
        raise ValueError(f"Memory route did not complete with samples: {args.log}")
    print(f"pid={pid} adapter={adapter} luid={luid} ISM={ism} samples={count}")


if __name__ == "__main__":
    main()
