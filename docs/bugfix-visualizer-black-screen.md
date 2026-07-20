# Bugfix: Visualizer Goes Black the Instant Console/Menu Closes

**Date:** July 2026
**Status:** Fixed
**Files touched:** `neo/renderer/tr_backend_draw.cpp` (`RB_VisFboEnd`)

## Symptom

The visualizer rendered correctly whenever the in-game console or the picker
menu was open (visible behind/through it), but the screen went solid black
the instant either was closed. Audio was confirmed flowing the whole time
(`vis_status` showed live band data), and `vis_show` was on.

## Why it was hard to find

The engine only redirects the visualizer's draws through an offscreen
ping-pong FBO (`visualizer/feedbackrtA`/`feedbackrtB`) when the console/menu
are **closed** (`pingPongActive = feedback && pingPongReady && !overlayOpenNow`,
in `neo/sound/visualizer_manager.cpp`). With console/menu open, content draws
go straight to the backbuffer, which is why that path always looked fine.

RenderDoc captures of the broken state kept showing almost no draw calls at
all for the visualizer — just the standard 3D-scene tonemap blit. This looked
like the content was never being drawn. It turned out RenderDoc's OpenGL hook
doesn't reliably capture this engine's manually-resolved framebuffer
extension functions (`qglGenFramebuffers`, `qglFramebufferTexture2D`,
`qglCheckFramebufferStatus`, resolved via `GLimp_ExtensionPointer` instead of
going through the normal loader RenderDoc hooks into), so the entire
offscreen-FBO region of the frame was invisible to the capture. Several hours
were spent chasing that red herring before realizing it.

## How it was actually diagnosed

Direct in-process instrumentation (not RenderDoc) settled it:

1. Added a thread-safe diagnostic queue in `idVisualizerManager::Draw2D()` /
   `Frame()` to get console output out of the SMP draw-worker thread (where
   `idLib::Printf` silently redirects to `OutputDebugString` instead of the
   console — `idCommonLocal::VPrintf` checks `idLib::IsMainThread()`).
2. Confirmed `BeginOffscreenRender`/`EndOffscreenRender` fire every frame with
   the FBO correctly alternating between `feedbackrtA`/`feedbackrtB`.
3. Added the same style of diagnostic directly in `RB_VisFboBegin`/
   `RB_VisFboEnd` (the render backend thread) and did a raw `glReadPixels`
   scan of the FBO texture right before the blit: **it had real, bright,
   animated content** (hundreds of non-black samples, moving each frame).
4. Did the same scan on the backbuffer immediately after the blit's draw
   call: **completely black, zero non-black samples.** The blit itself was
   the failure point, not content generation.

## Root cause

`RB_VisFboEnd()` blits the FBO's contents onto the backbuffer with a
full-screen quad (`backEnd.unitSquareSurface`, defined in
`R_MakeFullScreenTris()` with vertices already in clip-space NDC, -1..1).
The `BindShader_Texture()` shader used for that blit still needs the MVP
matrix uniform to be set to identity so those NDC vertices pass through
untouched — but `RB_VisFboEnd` never set one.

Without an explicit identity MVP, the shader used whatever MVP the last
real 2D/3D view had left behind (the 3D scene's perspective projection),
transforming the quad's corners to somewhere entirely outside the clip
volume. The draw call executed, but rasterized zero fragments — hence a
GPU-level black screen despite correct content upstream.

`RB_PostProcess` and `RB_MotionBlur` use the same `unitSquareSurface` and
don't have this problem: `RB_PostProcess` runs immediately after a real view
already set a valid MVP, and `RB_MotionBlur` explicitly calls `RB_SetMVP(...)`
itself. `RB_VisFboEnd` is the odd one out — it can run at an arbitrary point
with no guarantee of what MVP is currently active, and had no explicit setup
of its own.

## The fix

In `RB_VisFboEnd`, right before binding the texture and shader:

```cpp
idRenderMatrix identityMVP;
identityMVP.Identity();
RB_SetMVP( identityMVP );
```

See `RB_DrawTestImage` (`tr_backend_rendertools.cpp`) for the same pattern
used elsewhere in this codebase (it builds a full orthographic matrix via
`SetRenderParms( RENDERPARM_MVPMATRIX_X, ..., 4 )` before its own
`BindShader_Texture()` draw).

## Lessons for next time

- If a RenderDoc capture shows suspiciously few draw calls for a code path
  you *know* is executing (confirmed via other means), check whether the
  missing calls involve manually-resolved GL extension function pointers.
  RenderDoc's hook may not see them.
- `idLib::Printf`/`idCommon::Printf` silently do nothing useful off the main
  thread (redirect to `OutputDebugString`). Any diagnostic logging from a
  worker/backend thread needs to be queued and flushed from a known
  main-thread call site.
- A full-screen blit quad with NDC-range vertices needs an explicit identity
  MVP set immediately before its draw call — never assume "no transform
  needed" without checking what state the shader's vertex stage actually
  reads.
