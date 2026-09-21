# GPU-only contention validation (v0.1.8)

Local validation on 2026-09-22: Windows x64, RTX 5070 Ti, NVIDIA driver
616.92, AFX SDK 2.1.0.9 Blackwell AEC and 48 kHz Denoiser models.

These are synthetic timing tests, **not microphone recordings or a listening
test in the user's game**. Both real NVIDIA effects run on nonzero input. No
audio devices, CPU effect backend, or raw-audio overload bypass are used.

## Changes exercised

- Remove the 8/10 ms effect-shedding thresholds and two-second dry bypass.
- Use one worker-owned CUDA context for AEC and Noise Removal.
- Request WDDM process priority HIGH; all probes observed class 4, status 0.
- Warm up both effects before accepting live frames. The original first live
  Run took 103-110 ms even after successful model loading.
- Select GPU results at the actual output deadline, with an 80 ms output lead
  (20 ms more than v0.1.7, including frame assembly/resampler lookahead).
- Reject incomplete effect chains and contain unrecoverable misses without
  exposing raw input. This containment is an audible failure, not a successful
  substitute for processing on time.

## Full processor under contention

Three consecutive 15-second runs, 1,500 input blocks each. The load uses a
separate CUDA process and a bounded matrix-multiplication graph. Observed GPU
utilization was predominantly 95-97%, with samples from 94-98%; peak temperature
was 68 C. The probe used the production processor source, worker, queues,
resamplers and output policy, with test-only synthetic-reference injection.

| Input | AEC / Noise frames completed | Inference >10 ms | Maximum inference | Maximum audio callback | Misses / queue overruns / underruns |
| --- | ---: | ---: | ---: | ---: | --- |
| 96 kHz stereo | 1498 / 1498 | 103 | 33.848 ms | 0.239 ms | 0 / 0 / 0 |
| 48 kHz stereo | 1499 / 1499 | 56 | 30.709 ms | 0.131 ms | 0 / 0 / 0 |
| 96 kHz stereo | 1497 / 1497 | 121 | 40.993 ms | 0.199 ms | 0 / 0 / 0 |

Counts exclude warm-up and frames still pending at probe shutdown. A separate
45 ms injected worker stall at 96 kHz also completed with zero protected misses,
queue overruns and output underruns. No effect is disabled merely because an
inference call exceeds its 10 ms input period.
A deliberate 80 ms worker stall produced three protected frames, then recovered
with both effects still enabled. This explicitly verifies the failure boundary;
it is not included in the zero-miss contention results above.

## Reproduce

Configure with `AFX_SDK_ROOT`, build Release, and supply a bundled EchoNull DLL.
The manual probes are not part of ordinary CI because they need an RTX GPU and
licensed runtime assets. The test hooks are compiled out of the shipping DLL.

```powershell
cmake --build build-gpu --config Release --parallel
ctest --test-dir build-gpu -C Release --output-on-failure
build-gpu/Release/echonull_pipeline_benchmark.exe EchoNullPlugin.dll 45 96000
python tools/benchmark_gpu_contention.py `
  build-gpu/Release/echonull_pipeline_benchmark.exe EchoNullPlugin.dll `
  --pipeline --frames 1500
```

The Python runner requires CUDA-enabled PyTorch. It stops loading at 80 C or
90 seconds, and aborts a failed probe. Unit tests cover missing and partial GPU
results, raw-input exclusion, recovery, telemetry fields and persisted logs;
plug-in smoke tests cover bypass and variable-size 96 kHz stereo blocks.

## Limits

WDDM HIGH is a scheduling preference, not a reservation that can preempt every
game kernel immediately. This synthetic CUDA load cannot certify all game,
graphics-driver, DPC, or CPU contention scenarios. If a GPU stall exceeds the
playout budget, the affected frame fades to silence and reports a protected
miss; it does not leak raw noise/echo or silently disable either effect.
