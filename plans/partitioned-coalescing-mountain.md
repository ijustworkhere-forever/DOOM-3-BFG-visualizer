# Plan: Keep the modernization moving from a verified x64 baseline

## Context
The Debug|x64 solution now builds, but the broader modernization is not complete. The cloned `C:\RBDOOM-3-BFG` confirms that the durable path is architecture-aware build configuration first, then incremental subsystem integration rather than copying its NVRHI/CMake renderer wholesale. The current tree also contains experimental audio/visualizer changes that are not all project-integrated and some malformed edits that must not be allowed to destabilize the working build.

The immediate goal is to make x64 solution selection truthful for every project, preserve the successful Debug|x64 build, and establish a clean, repeatable next build gate before adding larger features.

## Recommended implementation

### 1. Correct solution-level x64 mappings
Modify `neo/doom3.sln` only for the projects whose x64 configurations currently point to Win32:

- `Doom3BFG` / `neo/doomexe.vcxproj`
- `amplitude` / `neo/amplitude/amplitude.vcxproj`
- `doomclassic` / `doomclassic/doomclassic.vcxproj`
- `timidity` / `doomclassic/timidity/timidity.vcxproj`

For Debug, Release, and Retail, make each `.ActiveCfg` and `.Build.0` resolve to the matching x64 project configuration. Do not alter mappings already correct for `idLib`, `Game-d3xp`, or `external`.

### 2. Make shared library lookup architecture-aware
Update `neo/_PCLibs.props` so Win32 uses `$(DXSDK_DIR)\Lib\x86\` and x64 uses `$(DXSDK_DIR)\Lib\x64\`, while leaving the common dependency list unchanged. Keep the existing project-specific x64 `LibraryPath` settings intact.

### 3. Verify all MSBuild configurations
Run XML/project evaluation checks and build at least:

- Debug|x64 (required regression gate)
- Release|x64 (next architecture gate)
- Debug|Win32 (ensure the x86 path was not broken)

Record the first real failure if Release or Win32 exposes legacy code not exercised by Debug|x64. Keep build outputs in the existing build directories and do not mix CMake artifacts with MSBuild outputs.

### 4. Stabilize experimental feature work before integration
Before adding new audio/visualizer files to project files, audit and fix only compile-safety blockers in tracked code:

- `neo/ui/UserInterface.cpp` malformed inserted member definitions
- `neo/sound/XAudio2/XA2_SoundSample.cpp` syntax/member-access corruption
- `neo/renderer/RenderBackend.h` invalid/incomplete declarations; keep it excluded until a concrete implementation exists
- `neo/sound/AudioCapture_WASAPI.cpp` invalid WASAPI format field and capture-loop contract

Do not port the full RBDOOM renderer yet. Its NVRHI/DX12/Vulkan stack is a subsystem replacement requiring `extern\nvrhi`, ShaderMake, shader assets, and device-manager code absent from this tree.

### 5. Integrate media/audio incrementally
Once the baseline projects are stable:

- First integrate the existing Ogg decoder through the current XAudio2 decoder boundary.
- Add FFmpeg behind an explicit optional build setting with architecture-specific include/lib/bin paths derived from `C:\ffmpeg`.
- Correct decoder buffer sizing, `nb_samples` handling, EOF flushing, and cleanup before enabling broad formats.
- Resolve playlist ownership and separate output-device selection from WASAPI loopback input.

### 6. Add CMake as a separate future build path
Use RBDOOM’s `neo/CMakeLists.txt` and `neo/idlib/CMakeLists.txt` as references, but start with an out-of-source `idlib` target using this repository’s paths. Do not replace or regenerate the working Visual Studio solution, and defer external libraries, shaders, FFmpeg, Vulkan, and DX12 until the idlib CMake slice is independently verified.

## Critical files

- `neo/doom3.sln`
- `neo/_PCLibs.props`
- `neo/doomexe.vcxproj`
- `neo/amplitude/amplitude.vcxproj`
- `doomclassic/doomclassic.vcxproj`
- `doomclassic/timidity/timidity.vcxproj`
- `neo/ui/UserInterface.cpp`
- `neo/sound/XAudio2/XA2_SoundSample.cpp`
- `neo/sound/AudioCapture_WASAPI.cpp`
- `neo/renderer/RenderBackend.h`
- `neo/sound/XAudio2/XA2_FFmpegDecoder.cpp/.h`
- `C:\RBDOOM-3-BFG\neo\CMakeLists.txt`
- `C:\RBDOOM-3-BFG\neo\idlib\CMakeLists.txt`

## Verification

1. Validate solution/project XML and configuration mappings.
2. Build Debug|x64 and confirm it remains successful.
3. Build Release|x64 and Debug|Win32; report any configuration-specific blockers.
4. Use targeted compilation after each source repair rather than adding all experimental files at once.
5. Confirm no x64 configuration resolves the x86 DirectX library directory.
6. Keep the final report explicit about successful builds, warnings, and deferred renderer/feature work.
