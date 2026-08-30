# EchoNull

EchoNull adds NVIDIA NvAFX Acoustic Echo Cancellation (AEC) to an existing
Windows microphone through Equalizer APO. It does not create another microphone,
install an audio driver, or require a virtual audio cable.

The only file users import is `EchoNullPlugin.dll`. The playback
selector, AEC and Noise Removal controls, output meter, and runtime status are
embedded in the plug-in editor; there is no EchoNull CLI or separate setup
application. Release builds also carry the NvAFX runtime, selected GPU model,
and applicable NVIDIA license documents inside that DLL.

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

## Install

- Windows 10 or 11 x64
- NVIDIA RTX GPU supported by the bundled AFX model
- Equalizer APO with post-mix enabled on the reference playback endpoint and
  capture processing enabled on the existing microphone

1. Download `EchoNullPlugin.dll` from the latest GitHub Release.
2. Put it in `C:\Program Files\EqualizerAPO\VSTPlugins`.
3. Open Equalizer APO Device Selector. Enable capture processing on the existing
   microphone and post-mix processing on playback devices you want to use.
4. In Configuration Editor, scope a section to the microphone and import
   `EchoNullPlugin.dll` with the VST plug-in command.
5. Open the embedded panel, choose the playback reference, and press
   **APPLY REFERENCE**.

On first activation, the DLL extracts its embedded vendor payload to an
`EchoNull/<bundle-id>` directory below the host process's Windows temporary
directory. The cache is reused on later starts and a new DLL version receives a
new cache directory. The current AFX runtime is about 1.6 GB, so leave enough
disk space for the audio-service cache.

EchoNull accepts the endpoint's native format. A 96 kHz stereo playback or
capture stream is downmixed and resampled internally to the 48 kHz mono frames
required by NvAFX AEC, then converted back to the capture pipeline's rate.

Noise Removal defaults to off. It becomes available when the release was built
with NVIDIA's 48 kHz Denoiser feature; otherwise enabling it produces the
explicit `NOISE MODEL MISSING` status while AEC continues to work.

## Build

```powershell
$env:AFX_SDK_ROOT = "C:\path\to\AFX-SDK"
cmake -S . -B build -A x64 -DAFX_SDK_ROOT="$env:AFX_SDK_ROOT" -DECHONULL_NVAFX_ARCHITECTURE=blackwell
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The packer appends the NvAFX DLLs, architecture-specific AEC model, optional
Denoiser feature, and license files to the PE image. The ready-to-import result
is copied to the repository root as `EchoNullPlugin.dll`; it is intentionally
gitignored. Test executables remain in the build directory only and are never
part of a release.

Builds without the proprietary SDK still compile and run the tests. The AEC
instance then reports a GPU/model error and safely bypasses processing.

## CI and releases

`.github/workflows/ci.yml` builds and tests every push and pull request without
proprietary assets. `.github/workflows/release.yml` runs for `v*` tags or manual
dispatch, downloads AFX SDK 2.1.0 from NGC, builds the self-contained DLL, runs
the tests, writes a SHA-256 file, and uploads both files to GitHub Releases.

Release builds require the repository Actions secret `NGC_CLI_API_KEY`. Keep it
only in GitHub Secrets; never place it in a workflow, local config committed to
Git, or command output.

## Generated Equalizer APO reference

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

Release DLLs embed the selected AFX runtime, models, dependencies, and license
documents; the source repository does not commit those vendor binaries. They
remain governed by NVIDIA's terms. Review those terms and the
[NVIDIA Maxine branding guidelines](https://www.nvidia.com/maxine-sdk-guidelines)
before redistributing a binary package.
