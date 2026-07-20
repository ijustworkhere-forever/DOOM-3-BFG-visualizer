# Bugfix: MilkDrop Shader Transpiler Could Hang the Whole Game

**Date:** July 2026
**Status:** Contained (root cause not fully isolated; see below)
**Files touched:** `neo/sound/MilkShaderTranspiler.cpp`,
`neo/external/hlsl2glslfork/hlslang/MachineIndependent/Initialize.cpp`

## Symptom

`vis_shaderTranspileTest` (PRD M3's HLSL->GLSL transpiler self-test, which
runs a real preset's `warp_`/`comp_` shader body through the vendored
`hlsl2glslfork` library) froze the entire game process indefinitely. No
crash, no error, no timeout -- just a permanently stuck process.

## Why it matters

This isn't just a test-command bug. The shader body that triggers it
(`clamp(tan(v.yx), -12, 12)`, a vector clamped against bare scalar bounds)
is an extremely common, completely idiomatic pattern in real MilkDrop
presets. Any future code path that transpiles a preset's custom shaders at
load time (the PRD M3 "wire into the live pipeline" work, not yet done)
would inherit this hang for a meaningful fraction of real-world presets.

## Diagnosis

Bisected the real preset's shader body down to a minimal 2-line repro:

```hlsl
float2 h1 = clamp(tan(zz.yx), -12, 12);
```

The first hypothesis was a missing-overload gap: `Initialize.cpp` (the
vendored library's built-in function declarations) only declares
same-vector-type overloads for `min`/`max`/`clamp`
(e.g. `float2 clamp(float2, float2, float2)`), not the scalar-broadcast
form (`float2 clamp(float2, float, float)`) that real HLSL and this exact
idiom rely on. That's a real, worth-fixing gap on its own -- exact-type
matches should win over implicit-cast resolution -- so those overloads were
added. **It did not fix the hang.** Re-testing the identical minimal repro
against a rebuilt binary reproduced the exact same freeze.

Further diagnosis, since this ruled out the most obvious cause:

- **Not a busy-spin loop.** `Get-Process` on the hung PID showed CPU time
  essentially flat (a couple seconds of accumulated CPU across several
  minutes of wall-clock time) -- a tight infinite loop would peg a core and
  accumulate CPU time roughly 1:1 with wall time. This process was genuinely
  blocked/waiting, not spinning.
- **Not a modal assert dialog.** MSVC Debug builds show a blocking
  Abort/Retry/Ignore dialog on a failed `assert()`, which would perfectly
  explain near-zero CPU. Ruled out directly: `Get-Process`'s
  `MainWindowHandle` was `0` (no visible top-level window at all, so no
  dialog was showing), and the process still reported `Responding = True`
  (its Windows message pump was still alive).
- **Not in the parts of the parser we could reasonably hand-trace.** Traced
  the call chain a scalar-to-vector argument promotion would take --
  `TParseContext::findFunction` -> `promoteFunctionArguments` ->
  `constructBuiltInAllowUpwardVectorPromote` -> `constructBuiltIn` ->
  `ir_add_unary_math` -> `TIntermUnary::promote` (all in `ParseHelper.cpp` /
  `Intermediate.cpp`) -- every failure path in that chain degrades cleanly
  to a "can't convert"/"no matching overloaded function" error and a dummy
  recovery node, not a loop. `TSymbolTableLevel::findCompatible`
  (`SymbolTable.cpp`), the overload-resolution routine itself, is also
  straight-line and bounded (no unbounded loop).

The actual infinite loop is somewhere deeper than this -- most likely in the
6000+ line bison-generated `yyparse` driver (`Gen_hlslang_tab.cpp`) or its
custom lexer hooks (the `hlmojo` preprocessor / `PaParseComment`), neither of
which was fully isolated despite real effort. `hlsl2glslfork` is effectively
unmaintained upstream, so continuing to hunt for the exact loop inside a
15,000+ line vendored fork had steeply diminishing returns.

## The fix: containment, not a root-cause patch

Given a real, third-party parser bug that real preset content can trigger,
and no practical way to fully eliminate it, `idMilkShaderTranspiler::Transpile`
now runs the actual parse/translate work on an isolated worker thread
(`idSysThread`) and waits on it with a **bounded timeout**
(`MILK_TRANSPILE_TIMEOUT_MS`, currently 4 seconds) instead of calling
straight into `hlsl2glslfork` on the caller's thread:

- If the worker finishes in time, its result is used normally.
- If it times out, `Transpile()` returns failure (matching this function's
  existing documented contract -- callers already tolerate failure and skip
  the shader, NFR-3) and the transpiler is **permanently disabled for the
  rest of the process's lifetime**. `hlsl2glslfork` keeps extensive mutable
  global/static state (`GlobalPoolAllocator`, per-language `SymbolTables`,
  a TLS init flag) with no thread-safety story; a call abandoned mid-flight
  could leave that state partially mutated, so retrying later risks a crash
  or a second hang instead of a clean failure. Better to lose the feature
  for the rest of the session than risk corrupting shared state.
- The timed-out worker thread and its job data are deliberately **leaked**,
  not force-terminated. `TerminateThread`-ing a thread stuck mid-allocation
  inside a custom pool allocator risks corrupting the CRT heap far worse
  than leaking one idle thread and a few hundred bytes -- and since the
  transpiler is disabled for good after this happens, it can only ever leak
  once per process lifetime.

## Lessons for next time

- CPU time (not just "is it still running") is the fast way to tell a busy
  spin loop from a genuine block/wait -- `Get-Process`'s `CPU` field over a
  known wall-clock interval settles it in one check.
- `MainWindowHandle == 0` + `Responding == True` rules out "the whole
  process is wedged" and "there's a blocking dialog nobody's there to
  click" in one shot, without needing a live screenshot.
- Not every hang is worth root-causing to completion, especially in an
  unmaintained vendored dependency with unclear internals. A bounded,
  fail-safe containment wrapper that matches the caller's existing
  failure-tolerance contract is a legitimate, permanent fix in its own
  right, not just a stopgap.
