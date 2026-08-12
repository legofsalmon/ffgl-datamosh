# ffgl-datamosh

Datamosh effects for Resolume, as FFGL plugins for Windows and macOS.

Two builds share one render core:

| Plugin | Type | Where it shows up |
| --- | --- | --- |
| **Datamosh** | `FF_EFFECT`, `DMSH` | Effects list. Drop it on a layer, a group, or the composition. |
| **Datamosh Transplant** | `FF_MIXER`, `DMMX` | A layer's blend-mode dropdown. Applies that layer's motion to the layers below. |

---

## Why this is simulated rather than real datamoshing

Datamoshing traditionally works by corrupting inter-frame compression — deleting
I-frames so pixels persist, or duplicating P-frames so motion vectors keep
applying to an increasingly wrong image.

**None of that is available inside Resolume.** Its playback path uses HAP and
DXV, which are all-intra codecs: every frame is an independent DXT-compressed
still, and there are no motion vectors anywhere to corrupt. Even with an H.264
source, an FFGL plugin receives a decoded RGBA texture, never the bitstream.

So this estimates the motion itself and then reproduces what a broken decoder
does with it. For live use that turns out to be the better deal: it works on any
source including cameras and NDI, it cannot desync or fall apart mid-set, and
every stage of the process becomes a parameter instead of an encoder setting.

## What it does

All three classic looks come out of the same pipeline:

- **Melt** — stop refreshing pixels from the input while motion keeps flowing.
  With **Auto Mode → On Cut**, the plugin detects clip changes underneath it and
  melts through them on its own. Put it on the composition and every cut moshes
  with nothing to trigger.
- **Bloom** — freeze the vector field and keep applying it, so motion piles up
  and the image explodes into flowing trails. This is P-frame duplication.
- **Pixel drag** — raise the threshold so only fast-moving blocks smear.

**Softness** takes the look from hard macroblock tearing to smooth liquid flow;
the estimator is the same either way.

## Building

Requires CMake 3.20+ and a C++17 compiler. The FFGL SDK is vendored as a
submodule, and GLEW ships prebuilt inside it for Windows, so there is no package
manager involved.

```sh
git clone --recursive https://github.com/legofsalmon/ffgl-datamosh.git
cd ffgl-datamosh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

macOS builds a universal binary by default (`arm64;x86_64`). Resolume 7.11+ runs
natively on Apple Silicon and will not load an x86_64-only plugin.

To install straight into Resolume:

```sh
cmake -S . -B build -DDATAMOSH_INSTALL_TO_RESOLUME=ON
cmake --build build --target install
```

Otherwise copy the built `.dll` (Windows) or `.bundle` (macOS) into
`Documents/Resolume Arena/Extra Effects` and restart Resolume.

## Testing

```sh
ctest --test-dir build --output-on-failure   # 17 tests
./build/tests/datamosh_tests --profile        # per-pass GPU timing
```

The tests create a real headless OpenGL 4.1 core context and drive the pipeline
exactly as Resolume does, against synthetic clips with **known ground truth** —
a pattern translating at a known velocity must produce the matching vectors.
That is the only way to regression-test a motion estimator; "does it look right"
is not a test. They also cover cut detection, freeze, resize storms, GL object
lifetime, and that no NaN can reach the feedback buffers.

The most pointed test is `MotionCompensationReconstructsPureTranslation`: if the
vector sign, warp direction, pel snapping and block-centre lookup are all
correct, displacing the previous frame by the estimated motion must reproduce
the current one. Get any single one backwards and the error doubles instead of
cancelling.

## How it works

Everything is fragment shaders and ping-pong framebuffers, because FFGL 2 needs
OpenGL 4.1 core and **macOS caps at exactly 4.1** — no compute shaders, no
SSBOs, no image load/store.

Per frame:

1. **Ingest** — normalise the host texture into an exact-size RGBA16F buffer,
   resolving the `MaxUV` sub-rectangle and un-premultiplying alpha once, so no
   later pass has to think about either.
2. **Luma + pyramid** — extract luma and generate mips. The mip chain is the
   search pyramid.
3. **Motion search** — hierarchical block matching, coarse to fine, one fragment
   per macroblock. Candidates are the coarser level's vector, the four
   neighbours' vectors, zero, and a small local window. Costs carry a
   regularisation term charged in pixels, which is what keeps the field coherent
   rather than confetti.
4. **Flow post** — spatial blur, temporal inertia, freeze, and hard limits
   including NaN rejection.
5. **Control** — a 1×1 state machine deciding the current mosh level from the
   triggers, the beat clock, the audio level, and the cut detector.
6. **Mosh** — displace the accumulation buffer along the field and decide per
   pixel whether it may refresh.
7. **Composite** — wet/dry, re-premultiply, draw to the host.

Roughly 61 MB of buffers per instance at 1080p, most passes at block resolution.

### Notes for anyone extending this

- **No `glReadPixels` anywhere in the render path.** The cut detector needs a
  whole-frame statistic, and reading one back would stall the GPU mid-frame. It
  is computed by mip reduction and consumed on the GPU instead.
- **Whole-pixel snapping matters.** The accumulation buffer is resampled into
  itself every frame; with plain bilinear filtering it blurs to mush within a
  second or two. `Pel Snap` is on by default for that reason, and it is also
  what real codecs do.
- **`ffglex::FFGLFBO` is not used.** Its `Release()` never deletes the colour
  texture — the second guard re-tests `depthBufferID` — so it leaks a
  full-resolution texture on every resize. `RenderTarget` replaces it, also
  dropping the depth buffer none of these passes need.
- **`ScopedFBOBinding` does not restore the viewport.** `ScopedViewport` in
  `src/core/GL.h` covers that gap; without it the host draws into a corner of
  its own framebuffer.
- **One plugin per binary.** `FFGL.cpp` dispatches through a single global
  `g_CurrPluginInfo`, so the effect and mixer cannot share a library.

## Parameters

**Mosh** — Amount · Trigger · Reset · Auto Mode (Manual / On Cut / On Beat) ·
Cut Sensitivity · Burst Length · Beat Divisor

**Motion** — Gain · Freeze · Smoothing · Threshold · Block Size (4/8/16/32) ·
Softness · Pel Snap · Invert

**Damage** — Decay · Corruption · Chroma Drift · Mix

**Sync** — Audio (FFT) · Audio Amount · Audio Band

**Cost** — Quality (Low / Medium / High / Ultra)

The mixer adds **Motion Source**, choosing whether motion comes from its own
layer or the one below, and **Blend** — the parameter FFGL's mixer base always
registers, folded into the wet/dry so it is not a dead slider.

## Status

Working and tested, not yet validated inside Resolume itself — that needs a
machine with Resolume on it. The manual checklist to run there:

- [ ] Both plugins appear and load
- [ ] Composition resolution change while running
- [ ] Clip cut, layer bypass, rapid parameter scrubbing
- [ ] Two instances stacked
- [ ] Mixer appears in the blend-mode dropdown
- [ ] No VRAM growth over a 30-minute soak
- [ ] Confirm Resolume delivers `SetTime` to effects (see the frame-advance gate
      in `src/plugins/DatamoshPlugin.h`; it falls back to advancing every call)
