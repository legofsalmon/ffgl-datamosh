# Changelog

Notable changes to ffgl-datamosh. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), versioning follows
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

Nothing yet.

## [0.1.0] — 2026-08-12

First release. **Pre-release**: everything is verified by an automated test
suite, but the plugins have not yet been run inside Resolume and the tuning
defaults have never seen real video.

### Added

**Two plugins over a shared render core.** `Datamosh` (`FF_EFFECT`, ID `DMSH`)
drops on a layer, group or composition. `Datamosh Transplant` (`FF_MIXER`, ID
`DMMX`) appears in a layer's blend-mode dropdown and applies that layer's motion
to the layers below. Separate binaries because FFGL dispatches through a single
global plugin record and cannot advertise two plugins from one library.

**Motion estimation** by hierarchical block matching, coarse to fine, one
fragment per macroblock. Candidates are the coarser level's vector, the four
neighbours' vectors, zero, and a local window; costs carry a regularisation term
charged in pixels so the tuning holds at any resolution. The whole pipeline is
fragment shaders and ping-pong framebuffers, because FFGL 2 needs OpenGL 4.1
core and macOS caps at exactly that.

**The three classic looks** from one pipeline — melt, bloom, pixel drag —
with `Softness` carrying the result from hard macroblock tearing to smooth
liquid flow.

**Automatic melt on clip cuts.** A GPU-resident cut detector compares each frame
against a running baseline, so putting the effect on the composition with
`Auto Mode → On Cut` melts through every clip change with nothing to trigger.

**A Damage group that gets the motion wrong on purpose** — `Motion Lag`
(applies a vector field from up to 15 frames ago), `Block Repeat`, `Quantise` —
alongside `Corruption` and `Chroma Drift`. These exist because the estimator is
accurate enough that motion compensation reconstructs the frame, which is
correct and far too clean to read as a broken codec.

**Beat and audio sync** via Resolume's clock and FFT input, with a selectable
frequency band.

**Style presets** — Melt, Bloom, Drag, Liquid, Corrupt — writing a coherent
group of parameters from one dropdown and reverting to Custom when the operator
departs from them.

**Collapsible parameter groups** via `SetParamGroup`, and CPU-generated
thumbnails for both plugins.

**Cross-platform build and CI.** CMake, the FFGL SDK vendored as a submodule,
GLEW prebuilt inside it — no package manager. macOS builds a universal binary.
CI builds Windows and macOS and runs the test suite on Linux.

**A headless test suite**, 32 tests, driving a real OpenGL 4.1 core context
against synthetic clips with known ground truth. Covers estimator accuracy,
motion compensation reconstructing a translating frame, cut detection, beat
divisors, freeze, motion lag, frame-rate-independent decay, resize storms, GL
object lifetime, NaN containment, and the plugin layer separately.

### Fixed

Two defects caught in review before release, both of which fail silently:

- **The mixer could not be instantiated by any FFGL host.** Renaming its
  inherited `mixVal` parameter with `SetParamInfo` appended a phantom record
  rather than updating index 0 — that call unconditionally `push_back`s.
  `GetNumParams` then overreported, and `instantiateGL`, which writes every
  index's default before handing the plugin over, hit the phantom, got
  `FF_FAIL` and destroyed the instance. Nothing was logged. `mixVal` is now left
  alone and a test replays the host's default-initialisation walk.
- **The plugin binaries were built with no entry point.** `plugMain` lives in a
  translation unit nothing else references, so out of a static archive the
  linker discarded it entirely. The SDK is now built as an `OBJECT` library and
  CI verifies the export on all three platforms.

### Notes

- `ffglex::FFGLFBO` is not used: its `Release()` never deletes the colour
  texture, leaking a full-resolution texture on every resize. `RenderTarget`
  replaces it and drops the depth buffer none of these passes need.
- `ScopedFBOBinding` does not restore the viewport; `ScopedViewport` covers it.
- No `glReadPixels` in the render path — the cut detector's whole-frame
  statistic is reduced through a mip chain and consumed on the GPU rather than
  stalling the frame.
- macOS bundles are unsigned and unnotarised. Gatekeeper quarantines them and
  Resolume then silently will not load them; the install notes carry the
  `xattr -dr com.apple.quarantine` fix.

[Unreleased]: https://github.com/legofsalmon/ffgl-datamosh/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/legofsalmon/ffgl-datamosh/releases/tag/v0.1.0
