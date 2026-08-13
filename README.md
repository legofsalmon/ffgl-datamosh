# ffgl-datamosh

Datamosh effects for Resolume, as FFGL plugins for Windows and macOS.

**📖 [Documentation](https://datamosh.letissier.ie)** ·
[Parameters](https://datamosh.letissier.ie/parameters.html) ·
[Recipes](https://datamosh.letissier.ie/recipes.html) ·
[Architecture](https://datamosh.letissier.ie/architecture.html) ·
[Download](https://github.com/legofsalmon/ffgl-datamosh/releases)

The site's source is in [`site/`](site/) and deploys to Vercel from there.

| Plugin | Type | Where it shows up |
| --- | --- | --- |
| **Datamosh** | `FF_EFFECT`, `DMSH` | Effects list. Drop it on a layer, a group, or the composition. |
| **Mosh Transplant** | `FF_MIXER`, `DMMX` | A layer's blend-mode dropdown. Applies that layer's motion to the layers below. |

## Why it simulates rather than corrupts

Datamoshing works by breaking inter-frame compression — deleting I-frames so
pixels persist, or duplicating P-frames so motion vectors keep applying to an
increasingly wrong image.

**None of that is reachable inside Resolume.** Its playback path uses HAP and
DXV, which are all-intra codecs: every frame is an independent compressed still,
so there are no motion vectors anywhere to corrupt. Even with an H.264 source, an
FFGL plugin receives a decoded texture, never the bitstream.

So this estimates the motion itself and reproduces what a decoder that has lost
its keyframe does with it. For live use that trades up: it works on any source
including cameras and NDI, it cannot desync mid-set, and every stage becomes a
parameter instead of an encoder setting.

Melt, bloom and pixel drag all come out of one pipeline, and a **Damage** group
exists to get the motion wrong on purpose — because an accurate estimator
reconstructs the frame, which is correct and far too clean to read as a broken
codec. [The docs explain the rest.](https://datamosh.letissier.ie)

## Install

From [Releases](https://github.com/legofsalmon/ffgl-datamosh/releases):

**macOS** — download the `.pkg` and double-click it. Signed and notarised, so no
security warning, no Terminal and no password; it installs into your Documents
folder and offers Arena and Avenue as tickboxes.

**Windows** — unzip, copy both DLLs into `Documents\Resolume Arena\Extra
Effects`, restart Resolume.

The macOS zip is still published for anyone who prefers copying files by hand,
but it needs `xattr -dr com.apple.quarantine` run on each bundle afterwards.
That step is exactly what the installer exists to remove: a zip propagates
quarantine to everything it extracts, and Resolume then refuses to load the
plugins with no error anywhere. Package payloads are not quarantined, and
neither is a local build.

## Build

CMake 3.20+ and a C++17 compiler. The FFGL SDK is vendored as a submodule and
GLEW ships prebuilt inside it, so there is no package manager involved.

```sh
git clone --recursive https://github.com/legofsalmon/ffgl-datamosh.git
cd ffgl-datamosh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

macOS builds a universal binary by default (`arm64;x86_64`); Resolume 7.11+ runs
natively on Apple Silicon and will not load an x86_64-only plugin.

Install straight into Resolume with
`-DDATAMOSH_INSTALL_TO_RESOLUME=ON` and `cmake --build build --target install`.

## Test

```sh
ctest --test-dir build --output-on-failure   # 42 tests
./build/tests/datamosh_tests --profile        # per-pass GPU timing
```

The suite creates a real headless OpenGL 4.1 core context and drives the
pipeline exactly as Resolume does, against synthetic clips with **known ground
truth** — a pattern translating at a known velocity must produce the matching
vectors. That is the only way to regression-test a motion estimator; "does it
look right" is not a test.

The Linux path uses EGL on a software rasteriser, so it runs on a CI machine
with no GPU and no display server.

## Release

Push a tag. `.github/workflows/release.yml` builds both platforms, re-runs the
tests, verifies the entry point and the macOS architectures, and publishes a
GitHub Release with the archives attached.

```sh
git tag v0.2.0 && git push origin v0.2.0
```

A release can also be cut from **Actions → release → Run workflow** by entering
the version there; the Releases API creates the tag against the chosen commit.
Running it with the version left empty builds and packages without publishing,
which is the way to test a change to the packaging itself.

## Status — smoke test passed, full pass outstanding

It loads and runs in Resolume. The smoke test passes on a real host: both
plugins register and instantiate, the effect renders, Resolume delivers
parameter writes, the motion estimator is genuinely running rather than the
pipeline falling through to passthrough, and wide-range parameters arrive at
their real values rather than normalised 0..1.

Outstanding: the aesthetic sweeps, a 30-minute soak, GL-state hygiene in a
chain, and the mixer beyond instantiation. The tuning defaults have still never
been judged against real footage.

**[VALIDATING.md](VALIDATING.md)** is the procedure for closing that gap: a
ten-minute smoke test, a triage table for when it fails, and a full pass. It is
written around one property of this codebase — when the pipeline cannot run,
`Passthrough` emits a pixel-exact copy of the input, so **a completely dead
plugin looks exactly like a correctly-bypassing effect**. Every check there is
built to tell working apart from inert rather than from black.

Start with the read-only diagnostics collector, which answers "are the files
where Resolume looks, are they loadable, did Resolume look, did Resolume load
them, did the parameters register" without your having to know what matters:

```sh
bash tools/collect-datamosh-diagnostics.sh | tee ~/Desktop/datamosh-diag.txt   # macOS
powershell -ExecutionPolicy Bypass -File .\tools\collect-datamosh-diagnostics.ps1   # Windows
```

The open questions it exists to settle: whether Resolume delivers `SetTime` to
effects (the frame-advance gate in `src/plugins/DatamoshPlugin.h` assumes it does
and falls back to advancing every call if not), whether the host sends real
parameter ranges or normalised 0..1, what it does with the mixer's inherited
`mixVal`, and whether every shader survives a stricter GLSL compiler than Mesa's.
Then `--profile` on real hardware, and tune `SEARCH_LAMBDA`, `SEARCH_ZERO_BIAS`
and `THRESHOLD_PIXEL_RANGE` (`src/core/MoshPipeline.cpp`) plus the cut-sensitivity
curve (`src/core/shaders/Control.glsl`) against footage.

## Licence

[MIT](LICENSE). Use it, change it, ship it in something commercial — keep the
copyright notice with it and accept that it comes with no warranty.

The plugins link the FFGL SDK and (on Windows) GLEW, both BSD-style and both
compatible; their notices are generated into a `THIRD-PARTY.txt` inside every
release archive alongside a copy of `LICENSE`.
