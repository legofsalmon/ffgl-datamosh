# Notes for Claude

Two FFGL 2.x plugins for Resolume that simulate datamoshing by estimating motion
on the GPU and withholding pixel refresh while continuing to apply the motion
field. Windows and macOS.

| Binary | Type, ID | Where it shows up |
| --- | --- | --- |
| `Datamosh` | `FF_EFFECT`, `DMSH` | Effects list |
| `DatamoshTransplant` | `FF_MIXER`, `DMMX` | A layer's blend-mode dropdown, as **Mosh Transplant** |

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel
ctest --test-dir build --output-on-failure     # 57 tests, headless EGL + llvmpipe
./build/tests/datamosh_tests --profile          # per-pass GPU timing
```

## If you are here to validate a build inside Resolume

**[AGENT-RUNBOOK.md](AGENT-RUNBOOK.md)** — written for you. Resolume's REST API
lets you set parameters and capture frames, so most of the pass is objective
rather than eyeballed. Read it before touching anything.

**[VALIDATING.md](VALIDATING.md)** — the same ground for a human at the machine.
Hand them the parts you cannot do.

## Traps in this codebase

Every one of these has already cost a release or a day. They are here because
none of them is visible from the code that contains them.

**A dead plugin looks exactly like a working bypass.** When the pipeline cannot
run — shader compile failure, failed allocation, stuck frame gate, first frame —
`ProcessOpenGL` falls through to `Passthrough`, which is `texture(InputTexture,
uv)` with the same `MaxUV`. It emits a pixel-exact copy of its input. Never
accept "it looks fine" or "it bypasses correctly" as evidence that anything ran.

**The FFGL SDK must be built as a CMake `OBJECT` library.** `plugMain` lives in
`FFGL.cpp`, whose only other symbols are unreferenced, so out of a static archive
the linker discards the whole object and you get plugins with no entry point that
load nowhere, silently. `cmake/FFGLPlugin.cmake` does this deliberately; CI
checks the export on all three platforms. Do not "simplify" it back.

**`SetParamInfo` unconditionally `push_back`s.** It does not update in place, so
"renaming" an inherited parameter appends a phantom record. `GetNumParams` then
overreports, the host's default-initialisation walk hits the phantom, gets
`FF_FAIL`, and destroys the instance with nothing logged anywhere. This once made
the mixer unloadable in every FFGL host. The inherited `mixVal` is therefore left
alone; `BothPluginsSurviveHostInstantiation` replays the host's walk.

That trap is specific to `SetParamInfo`. `SetParamGroup`, `SetParamDisplayName`,
`SetParamVisibility` and `SetParamElements` are different: they `FindParamInfo`
and assign in place, so they cannot append a phantom, and the SDK's own comment
on visibility says Resolume honours the event. `mixVal` is grouped through the
first of these. Whether it can be *hidden* waits on one check in the host — that
it does not special-case a mixer's index 0 as the blend amount.

**`PluginInfoStruct::PluginName` is `char[16]` and is not null-terminated when
full**, so a name of 16 characters or more runs into the adjacent `PluginType`.
Fifteen is the limit. Both plugins carry a `static_assert`.

**OpenGL 4.1 core is a hard ceiling.** FFGL 2 on macOS caps there: no compute
shaders, no SSBOs, no image load/store. Everything is fragment shaders and
ping-pong FBOs, and it has to stay that way.

**No `glReadPixels` in the render path.** The cut detector's whole-frame
statistic is reduced through a mip chain and consumed on the GPU. A readback
would stall the frame.

**`ffglex::FFGLFBO` is not used** — its `Release()` never deletes the colour
texture, leaking a full-resolution texture on every resize. `RenderTarget`
replaces it. `ScopedFBOBinding` does not restore the viewport either; use
`ScopedViewport`.

**The SDK's audio analyser starts at zero gain.** `ffglqs::Audio` computes each
bin as `fft[i] * fft[i] * gain` with `gain = 0`, and nothing in the SDK sets it.
`DatamoshPlugin.h` calls `SetGain(0.0f)` — decibels, so that is unity. Without
it every band reads zero forever and the audio parameters are inert with no
symptom. This shipped broken in three releases.

## Testing

Tests that only exercise our own code prove very little here. The two bugs above
that reached users were both invisible to a test that called our getter and
compared it to what our setter had stored.

- Values that reach the shader via `ReadParams` are covered by
  `ParametersMapToTheRightFields`. Anything read at render time instead — the
  audio level was the only one — needs its own method and its own test.
- Drive the host's path, not ours: `SetParamElementValue`, `SetFloatParameter`,
  the default-initialisation walk.
- When you fix a silent bug, **reintroduce it and confirm the new test fails.** A
  test written against a bug you cannot re-break is a test you have not verified.

## Conventions

Work on a branch, open a PR, let CI go green on all three jobs, then merge. Do
not push to `main`. Release by pushing a `v*` tag, or from Actions → release with
a version — the workflow normalises a missing `v` and rejects a malformed
version.

Verify release archives by **downloading and opening them**. Every CI step passed
on the v0.1.0 archives; only unzipping one caught that everything was nested
under `staging/`.
