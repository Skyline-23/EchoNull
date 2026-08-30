# EchoNull

EchoNull is an independent Windows 10/11 audio bridge that performs one job:
NVIDIA NvAFX Acoustic Echo Cancellation (AEC). It captures a physical microphone
and a WASAPI render-loopback reference, aligns both streams by timestamp, runs
NvAFX AEC, and renders the result to a virtual cable such as VB-CABLE.

Noise removal is intentionally downstream. A typical graph is:

```text
physical microphone ---- near end ----\
                                      NvAFX AEC -> CABLE Input -> NVIDIA Broadcast
speaker loopback ------- far end -----/
```

EchoNull does not include WebRTC AEC3, noise suppression, AGC, a noise gate,
dereverb, or an NvAFX denoiser.

## Independent implementation

This repository was created from scratch and is not a fork of another AEC
application. The only implementation reference is NVIDIA's public API,
documentation, and MIT-licensed sample repository:

- [NVIDIA AFX SDK user guide](https://docs.nvidia.com/maxine/afx/latest/index.html)
- [NVIDIA AFX SDK samples](https://github.com/NVIDIA-Maxine/AFX-SDK-Samples)

No NVIDIA runtime, model, or sample asset is committed to this repository.

## Architecture

The code follows Clean Architecture dependency boundaries:

```text
domain
  audio entities and status models
      ^
application
  ports, pipeline use case, timestamp alignment, delay estimation, ERLE
      ^
infrastructure
  WASAPI capture/render, NvAFX adapter, config and WAV adapters
      ^
presentation
  CLI today; native Windows control UI is the next adapter
```

Application code has no dependency on Win32, COM, WASAPI, or NVIDIA headers.
Those details remain in infrastructure adapters and are wired in the composition
root.

## Requirements

- Windows 10 or 11 x64
- NVIDIA RTX GPU with Tensor Cores
- NVIDIA graphics driver supported by the installed AFX SDK
- NVIDIA Audio Effects SDK 2.1 or newer
- Visual Studio C++ Build Tools and CMake
- VB-CABLE or another virtual playback endpoint for bridge output

An RTX 50-series GPU uses the SDK's `blackwell/aec_48k.trtpkg` model. EchoNull
searches that path first when `model` is blank and `AFX_SDK_ROOT` is set.

## Build

Install or extract the NVIDIA AFX SDK, then configure an x64 Visual Studio build:

```powershell
$env:AFX_SDK_ROOT = "C:\path\to\AFX-SDK"
cmake -S . -B build -A x64 -DAFX_SDK_ROOT="$env:AFX_SDK_ROOT"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Without the SDK, the project still builds device listing and the unit tests. The
`run` and `validate` commands then fail with an explicit SDK-not-available error.

## Configure and run

List endpoint names and stable IDs:

```powershell
.\build\Release\echonull.exe devices
```

Edit `config/echonull.ini`. Device selectors may be `default`, an endpoint ID,
or a unique part of a friendly name. The default output selector is
`CABLE Input`.

Run with SDK runtime paths prepared:

```powershell
.\scripts\run.ps1
```

The audio clients use 48 kHz mono float in WASAPI shared mode with Windows'
high-quality format conversion enabled. Capture packet QPC timestamps form the
common timeline, so long-running device-clock drift does not accumulate as a
fixed sample offset. An envelope correlation estimator refines the acoustic
speaker-to-microphone delay without growing an arbitrary buffer.

## ERLE validation

Use a 48 kHz PCM or float WAV containing far-end speech:

```powershell
.\build\Release\echonull.exe validate `
  --config config\echonull.ini `
  --test-wav C:\audio\far-end-speech.wav `
  --output out\validation
```

EchoNull plays the file through the configured reference speaker, captures its
loopback and the physical microphone, runs AEC, then records:

- `near-before.wav`
- `far-reference.wav`
- `aec-output.wav`

It reports ERLE over active far-end intervals. The initial acceptance target is
20 dB steady-state. Remain silent for an ERLE-only test; repeat while speaking to
audit double-talk preservation by listening to `aec-output.wav`.

## Runtime and redistribution

The AFX SDK installer/package supplies its DLLs, dependencies, and model files.
They are governed by NVIDIA's SDK terms and are not redistributable merely under
this repository's license. Keep application packaging separate until those terms
and the [NVIDIA Maxine branding guidelines](https://www.nvidia.com/maxine-sdk-guidelines)
have been reviewed for the intended distribution.

