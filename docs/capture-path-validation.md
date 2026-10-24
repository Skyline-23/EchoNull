# Capture routing and AEC validation (2026-10-25)

## Failures addressed

- The old setup generated redundant `If/EndIf` wrappers. Updating repository
  source alone did not replace the installed 0.1.7 DLL or old setup executable.
- A stream-effect (SFX/LFX) installation did not cover every shared capture path.
  The affected Discord input explicitly requested RAW capture.
- Envelope-based delay estimation latched onto a 170-185 ms offset with poor
  confidence and shifted the reference behind the acoustic echo. A successful
  GPU call did not prove meaningful cancellation.

The installer now uses the existing Equalizer APO COM class at the microphone's
endpoint-effect location, preserves the original vendor SFX, and records the
exact registration values required for uninstall. It refuses an occupied foreign
EFX slot. This is an interoperability extension, not a modification of Equalizer
APO or a driver certification claim. Device Selector can overwrite that routing;
rerun EchoNull Setup if the panel reports that its endpoint hook is missing.

The processor uses timestamp-aligned reference audio without the heuristic
delay shift. It allows up to two frames for loopback delivery within the existing
80 ms playout lead. No CPU fallback or automatic effect shedding was added.

## Checks performed

On Windows with an RTX 5070 Ti and a 96 kHz stereo USB microphone:

| Check | Result |
| --- | --- |
| SDK and no-SDK unit/host/architecture tests | All four tests passed in each build |
| Registry migration | 20 updates idempotent; undo restores original values; foreign EFX preserved; SFX and LFX covered |
| Direct NVIDIA AEC, generated aligned echo | 39.7 dB input/output energy reduction |
| Direct AEC, generated 20/40/80/160 ms delayed echo | 18.0 / 16.3 / 23.4 / 39.5 dB |
| Complete production processor, generated 37 ms echo, AEC only, 96 kHz stereo | 47.3 dB; zero protected misses/overruns/underruns |
| Same processor at 48 kHz | 49.2 dB; zero protected misses/overruns/underruns |
| Installed shared-mode RAW capture | GPU processing advanced; no AEC/noise error |
| Installed ordinary shared capture | GPU processing advanced; no AEC/noise error |
| Installed DLL identity | SHA-256 matched the locally built DLL |
| User playback interval after installation | Approximately 51.7 dB aggregate AEC input/output energy ratio; reference present; no protected misses or queue overruns |
| User's Discord listening check after the final installation | User confirmed normal voice transmission and no audible speaker echo |

The live energy ratio is an observed interval, not a standardized ERLE test or a
guarantee for all rooms, content, microphone gain, or simultaneous speech. No
microphone audio file was recorded. Diagnostic probes discard samples and retain
only levels/counters. ASIO and exclusive-mode paths remain outside the shared
Windows audio engine.

Reproduce the generated-signal tests with a matching packaged DLL:

```powershell
.\build-gpu\Release\echonull_aec_signal_tests.exe .\EchoNullPlugin.dll 40
.\build-gpu\Release\echonull_pipeline_benchmark.exe .\EchoNullPlugin.dll 0 96000 500 37
.\build-gpu\Release\echonull_pipeline_benchmark.exe .\EchoNullPlugin.dll 0 48000 500 37
```

The explicit local device probe (not part of CTest and not shipped to users) is:

```powershell
.\build-gpu\Release\echonull_capture_path_probe.exe raw
.\build-gpu\Release\echonull_capture_path_probe.exe default
```

Runtime diagnostics are in `%ProgramData%\EchoNull\Logs\EchoNull.log`;
installation stages and failures are in `Setup.log` in the same directory.
Telemetry tests use isolated mappings and cannot overwrite a live audio-engine
snapshot.

Microsoft documents the difference between stream and endpoint effects in
[Audio Processing Object Architecture](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/audio-processing-object-architecture)
and the EFX registration property in
[PKEY_FX_EndpointEffectClsid](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/pkey-fx-endpointeffectclsid).
