# Plan: Implement FFT Audio Analyzer

## Context
The current implementation of the `AudioAnalyzer` is a placeholder. It uses a naive $O(N^2)$ Discrete Fourier Transform (DFT) and incorrectly retrieves audio data by always grabbing the beginning of the first buffer, ignoring the current playback position and channel interleaving.

## Implementation Strategy

### 1. Upgrade FFT Algorithm
Replace the current $O(N^2)$ DFT in `AudioAnalyzer::PerformFFT` with a much more efficient $O(N \log N)$ **Cooley-Tukey FFT** algorithm.
- Implement an in-place Cooley-Tukey radix-2 FFT.
- Precompute "twiddle factors" (sine and cosine values) and store them in a lookup table within the `AudioAnalyzer` class to avoid expensive trigonometric calls during the `Update()` loop.

### 2. Improve Audio Data Retrieval
Rewrite `AudioAnalyzer::GetChannelData` to provide a accurate sample buffer for analysis.
- **Use Play Cursor:** Use the `GetPlayCursor()` method from `idSoundVoice_XAudio2` to determine the current playback position within the audio stream.
- **Handle Buffer Wrap-around:** Since audio data is distributed across multiple queued buffers in XAudio2, the function must be able to wrap around from the end of one buffer to the beginning of the next to provide a contiguous block of `FFT_SIZE` samples.
- **Mono Conversion:** Convert the interleaved multi-channel audio data (e.g., stereo) into a mono stream of `float` samples. This is typically done by averaging the samples from all channels at each frame.
- **Data Conversion:** Ensure the conversion from the engine's 16-bit signed PCM (`short`) format to `float` is correct and handles the current playback position accurately.

### 3. Enhance Analysis Scope
Update `AudioAnalyzer::Update` to perform analysis on the entire soundscape rather than just the first channel of the first emitter.
- Iterate through all active emitters and channels in the `idSoundWorldLocal`.
- Aggregate the results (e.g., by taking the maximum or average magnitude across all channels/emitters) to provide a holistic view of the audio intensity.

## Critical Files to Modify
- `neo/sound/audio_analyzer.h`
- `neo/sound/audio_analyzer.cpp`

## Verification Plan
- **Unit Test (Conceptual):** Verify the Cooley-Tukey FFT implementation against a known sine wave signal.
- **Runtime Verification:** Run the game and observe the `AudioAnalyzer`'s output (via debug logs or by hooking into the results) to ensure it responds dynamically to changes in audio volume and frequency (e.g., bass hits, high-pitched sounds).
- **Performance Check:** Ensure the new FFT implementation does not significantly impact the game's frame rate.