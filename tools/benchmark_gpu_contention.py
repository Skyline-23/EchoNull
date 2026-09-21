"""Bounded synthetic GPU contention test. Does not open or record audio devices.

Requires a CUDA-enabled PyTorch installation. Runs the manual NvAFX probe with
normal and high WDDM priority. Stops loading at 80 C or after 90 seconds.
"""
import argparse
import json
import subprocess
import threading
import time

import torch


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("benchmark")
    parser.add_argument("plugin")
    parser.add_argument("--pipeline", action="store_true",
                        help="benchmark the full plug-in processor instead of raw SDK calls")
    parser.add_argument("--frames", type=int, default=600,
                        help="pipeline frames per case, 200-1800 at 100 frames/s")
    args = parser.parse_args()
    if not 200 <= args.frames <= 1800:
        parser.error("--frames must be between 200 and 1800")
    stop = threading.Event()
    ready = threading.Event()
    failures = []
    samples = []

    def load():
        try:
            torch.set_num_threads(1)
            a = torch.randn((4096, 4096), device="cuda", dtype=torch.float16)
            b = torch.randn_like(a)
            result = torch.empty_like(a)
            torch.mm(a, b, out=result)
            torch.cuda.synchronize()
            graph = torch.cuda.CUDAGraph()
            with torch.cuda.graph(graph):
                for _ in range(32):
                    torch.mm(a, b, out=result)
            until = time.monotonic() + 90
            ready.set()
            while not stop.is_set() and time.monotonic() < until:
                begin = time.perf_counter()
                graph.replay()
                torch.cuda.synchronize()
                # Leave about 3% host submission headroom, measured separately
                # by nvidia-smi; this is not a claim of exact GPU utilization.
                time.sleep((time.perf_counter() - begin) * .03 / .97)
        except Exception as error:
            failures.append(str(error))
        finally:
            stop.set()
            ready.set()

    def monitor():
        try:
            while not stop.wait(.5):
                result = subprocess.check_output(
                    ["nvidia-smi", "--query-gpu=utilization.gpu,temperature.gpu",
                     "--format=csv,noheader,nounits"], text=True, timeout=5)
                util, temp = map(int, result.strip().splitlines()[0].split(","))
                samples.append((util, temp))
                if temp >= 80:
                    failures.append("thermal cutoff")
                    stop.set()
        except Exception as error:
            failures.append(f"GPU monitoring failed: {error}")
            stop.set()

    workers = [threading.Thread(target=load), threading.Thread(target=monitor)]
    for worker in workers:
        worker.start()
    try:
        if not ready.wait(20) or failures:
            raise RuntimeError(f"GPU load startup failed: {failures}")
        cases = [("private", "normal"), ("shared", "normal"),
                 ("shared", "high"), ("shared", "high")]
        if args.pipeline:
            cases = [("0", "96000"), ("0", "48000"), ("0", "96000")]
        for mode, priority in cases:
            if stop.is_set():
                raise RuntimeError(f"Load stopped: {failures}")
            begin = len(samples)
            command = [args.benchmark, args.plugin, mode, priority]
            if args.pipeline:
                command.append(str(args.frames))
            run = subprocess.run(command,
                                 capture_output=True, text=True, timeout=25)
            window = samples[begin:]
            print(json.dumps({"mode": mode, "priority": priority,
                              "exit": run.returncode, "probe": run.stdout,
                              "error": run.stderr, "gpu_samples": window}), flush=True)
            if run.returncode != 0:
                raise RuntimeError("NvAFX benchmark failed")
    finally:
        stop.set()
        for worker in workers:
            worker.join(timeout=10)
    if failures:
        raise RuntimeError(str(failures))


if __name__ == "__main__":
    main()
