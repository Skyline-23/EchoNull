# EchoNull

EchoNull adds NVIDIA NvAFX Acoustic Echo Cancellation (AEC) to an existing
Windows microphone through Equalizer APO. It does not create another microphone,
install an audio driver, or require a virtual audio cable.

The only EchoNull binary users import is `EchoNullPlugin.dll`. The playback
selector, AEC and Noise Removal controls, output meter, and runtime status are
embedded in the plug-in editor; there is no EchoNull CLI or separate setup
application.

```text
selected playback endpoint
  -> Equalizer APO post-mix
  -> EchoNullPlugin.dll (Reference mode, pass-through)
  -> timestamped shared-memory reference bus

existing microphone endpoint
  -> Equalizer APO capture stage
  -> EchoNullPlugin.dll (AEC mode)
  -> same microphone endpoint, processed in place
```

EchoNull intentionally implements playback echo cancellation followed by
optional NVIDIA Background Noise Removal. Room dereverberation, AGC, gates, and
Studio Voice are outside the project scope.

## Plug-in interface

The embedded interface uses a dark GPU-audio-tool visual language with green
status accents. It contains only controls that belong to the AEC pipeline:

- playback-reference endpoint selector
- AEC on/off switch and reference-strength slider
- Noise Removal on/off switch and strength slider
- live microphone output meter
- reference, GPU/model, bypass, and processing status
- endpoint refresh action

The selector deliberately contains no microphone list. The microphone is the
capture endpoint section where the plug-in was loaded. Playback choices are
filtered to active endpoints that have Equalizer APO's post-mix processing
registered and audio enhancements enabled.

Applying a reference writes an endpoint-GUID-scoped Equalizer APO configuration
for a second instance of the same DLL in transparent Reference mode. Audio
continues through that playback instance unchanged.

The live capture instance in `audiodg.exe` publishes its processed output level
and runtime state through a read-only telemetry mapping. The editor reads that
mapping, so the meter represents the active microphone pipeline rather than the
Configuration Editor's preview instance. Stale telemetry is shown as
`AUDIO ENGINE OFFLINE`.

## Architecture

The project follows Clean Architecture boundaries:

```text
domain
  audio endpoint and status models
      ^
application
  resampling, timestamp buffers, delay estimation
      ^
infrastructure
  NvAFX, endpoint discovery, Equalizer APO config, shared reference bus
      ^
adapter
  one Equalizer APO-compatible plug-in DLL with an embedded Win32 editor
```

Application code has no dependency on Win32, COM, WASAPI, Equalizer APO, or
NVIDIA headers.

## Requirements

- Windows 10 or 11 x64
- NVIDIA RTX GPU supported by the installed AFX SDK
- NVIDIA Audio Effects SDK 2.1 or newer, including the 48 kHz AEC model
- the NVIDIA 48 kHz Denoiser model when Noise Removal is enabled
- Equalizer APO with post-mix enabled on the reference playback endpoint and
  capture processing enabled on the existing microphone
- Visual Studio C++ Build Tools and CMake 3.24 or newer

EchoNull accepts the endpoint's native format. A 96 kHz stereo playback or
capture stream is downmixed and resampled internally to the 48 kHz mono frames
required by NvAFX AEC, then converted back to the capture pipeline's rate.

The proprietary NVIDIA runtime and model are not committed to this repository.
Download them from the
[NVIDIA Maxine Windows Audio Effects SDK page](https://catalog.ngc.nvidia.com/orgs/nvidia/maxine/resources/maxine_windows_audio_effects_sdk).

## Build

```powershell
$env:AFX_SDK_ROOT = "C:\path\to\AFX-SDK"
cmake -S . -B build -A x64 -DAFX_SDK_ROOT="$env:AFX_SDK_ROOT"
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The product output is `build\Release\EchoNullPlugin.dll`. The build also places
`echonull.ini` and the required NVIDIA runtime dependencies beside it. Those are
runtime data and vendor dependencies, not additional EchoNull applications.
Test executables are created only when `ECHONULL_BUILD_TESTS=ON`; neither a CLI
nor a setup executable is defined by the project.

Builds without the proprietary SDK still compile and run the tests. The AEC
instance then reports a GPU/model error and safely bypasses processing.

## Configure

1. Open Equalizer APO Device Selector.
2. Enable post-mix processing on any playback endpoint that may be used as the
   echo reference.
3. Enable Equalizer APO capture processing on the existing microphone endpoint.
4. Put `EchoNullPlugin.dll`, `echonull.ini`, the NvAFX runtime DLLs, and the models
   in a directory readable by `LOCAL SERVICE`.
5. Set `model` in `echonull.ini` to the absolute path of `aec_48k.trtpkg`.
   Set `noise_model` to `denoiser_48k.trtpkg` when using Noise Removal.
6. In Equalizer APO Configuration Editor, scope a section to the microphone and
   import `EchoNullPlugin.dll` through its VST plug-in command.
7. Open the plug-in panel, select a playback reference, and press
   **APPLY REFERENCE**.

The panel creates `EchoNull-reference.txt` in Equalizer APO's configuration
directory and maintains this marked include in `config.txt`:

```text
# EchoNull managed reference begin
Device: all
Include: EchoNull-reference.txt
# EchoNull managed reference end
```

The generated file is equivalent to:

```text
Device: {SELECTED-PLAYBACK-ENDPOINT-GUID}
Stage: post-mix
VSTPlugin: Library "C:\path\to\EchoNullPlugin.dll" Mode 0
```

The microphone instance uses the default `Mode 1` (AEC). If the playback
reference is stopped, stale, unavailable, or the NVIDIA model cannot load, the
capture instance bypasses AEC rather than muting the microphone.

## Limitations

- Applications using ASIO, WASAPI exclusive mode, RAW mode, or another path
  that bypasses Windows system effects also bypass EchoNull.
- Equalizer APO must be active on both the selected playback endpoint and the
  microphone capture endpoint.
- The first implementation targets normal mono/stereo endpoint layouts. A
  multichannel endpoint needs additional host-instance validation.
- Equalizer APO loads third-party processing code under `audiodg.exe`; follow
  its documented protected-audio and compatibility tradeoffs.

## Independent implementation and licensing

EchoNull is an independent implementation and is not a fork of Equalizer APO.
It uses NVIDIA's public AFX API and the clean-room, BSD-3-Clause
[`Xaymar/vst2sdk`](https://github.com/Xaymar/vst2sdk) headers at a pinned revision
for interoperability with Equalizer APO's legacy plug-in host. See
`THIRD_PARTY_NOTICES.md`.

The AFX SDK runtime, model packages, and dependencies remain governed by
NVIDIA's terms. Review those terms and the
[NVIDIA Maxine branding guidelines](https://www.nvidia.com/maxine-sdk-guidelines)
before redistributing a binary package.
