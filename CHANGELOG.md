# Changelog

Notable changes to ffgl-datamosh. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), versioning follows
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- **The `Corrupt` preset rendered a pixel-exact passthrough.** `Quantise` was
  applied *after* the temporal blend in `FlowPost`, so the rounded field became
  the next frame's blend target, inertia dragged each new estimate most of the
  way back to it, and the result rounded to the grid point it started on.
  Starting at zero it stayed at zero forever, which made **any non-zero Quantise
  inert at the default Motion Smoothing of 0.3** — and Corrupt sets Quantise 0.7.
  Quantising before the blend puts both operands on the grid, so steady motion
  holds a grid point instead of collapsing to the origin.

  Found by building a randomiser: a dice that rolls the damage controls has to
  know which combinations produce nothing, and measuring that turned up a
  combination the shipped presets already used.

- **A held vector field could never be acquired, and `Reset` could not recover
  it.** At Motion Smoothing 1.0 the blend discards each new estimate in favour of
  the previous field. Applied when there is no previous field — a cold start, or
  the frame after a Reset drops the history — that holds the zero it started
  with, permanently, with no way back except moving the slider. The inertia is
  now skipped on any frame with no history, so the frame after a Reset acquires a
  real field and the hold resumes from that.

  Note this does not change Bloom from a cold start: motion needs two frames to
  estimate, so a field held before any has played is still empty. That is correct
  — you cannot hold what does not exist — and the recipes page already says to
  let movement play first.

- **`Quantise` silenced the effect entirely on ordinary footage.** It was
  applied to the stored vector field, upstream of the motion gate, so any
  motion below half a grid step rounded to zero — and zero motion closes the
  gate, and a closed gate refreshes the block from the live frame. The result
  was a pixel-exact passthrough exactly where the plugin had been asked to
  damage the image hardest.

  The step reaches half a macroblock, so at the default Block Size of 16 the
  dead zone ran to 3 px/frame, against this codebase's own note that per-frame
  motion on ordinary footage is mostly 0.5–3 px. The shipped `Corrupt` preset
  sets Quantise 0.7 and sat inside it. Fixing the latch in the entry above was
  necessary but not sufficient: it made Quantise reach the field, and this is
  what the field then did with it.

  Quantise now applies to the **displacement**, downstream of the gate, so a
  vector that rounds away can no longer take the gate with it. A block whose
  motion rounds to zero holds where it is without moving — which is what a
  skipped block does in a real decoder — rather than refreshing from a keyframe
  it never received. `Quantise` and `Motion Threshold` are now independent: one
  decides how precisely motion is described, the other what counts as moving.
  Moving it out of `FlowPost` also removes the feedback path that caused the
  latch, so that bug cannot return in another form.

### Added

- **`Spread` — damage that creeps into blocks which never moved.** Everything
  else this plugin does is affine: a per-pixel decision computed from this
  frame's numbers alone, so a block with no motion of its own could never mosh.
  This is the one pass with a spatial memory. Corruption starts where the motion
  is strongest and spreads outward from there; when the level falls the frontier
  is always the faintest part of the field, so it goes out first and the damage
  retreats the way it came. It reads as a gesture rather than a setting.

  Two rates, both per *second*, and the second is the one that would have been
  missed. The attenuation is a half-life, like Decay. The kernel **radius** is
  `speed × dt` — a fixed per-frame radius is a per-frame rate wearing a spatial
  disguise, moving the front twice as fast at 120 fps as at 60. And the spread
  is a **dilation**, not a blur: a mean blur is diffusion, whose front advances
  as the square root of elapsed time with no coefficient that recovers
  frame-rate independence, while a max dilation composes radii by addition
  exactly. Measured over the same half-second, the front reached 0.500 of the
  frame at 30 fps against 0.531 at 60 — one block of 32 apart.

  The field's magnitude encodes *reach*, not intensity, so it is shaped into a
  gate opening rather than used raw. Raw, it conflated the two and did nothing
  visible: persistence is a per-frame rate, so a damage of 0.4 survives as
  `0.4ⁿ` and is indistinguishable from no hold within three frames — a field
  averaging 0.39 across a still region moved the image by 0.0000. A block the
  corruption has reached is corrupted, not 40% corrupted.

  It `max`es into the spatial term rather than multiplying, and replaces the
  *gate* rather than the level. Where a block genuinely moves the damage is at
  most what the gate already is, so `max` returns the gate and nothing changes;
  where a block is still, the gate is ~0 and the damage supplies the opening.
  Substituting it for the mosh level instead would put 0.02 through the hold-map
  and come out at 0.135 — seven times too sticky — and Motion Threshold would
  stop meaning anything.

  Style-managed, unlike the mask, and the difference is the point: `Spread`
  describes what the damage *does*, which is character, while a mask is where an
  instance sits in a composition. `Reset` clears the field, since a damage front
  grown before a cut surviving it is what `Reset` exists to abolish. Appended
  rather than inserted next to `Decay` where it belongs by topic, for the same
  index-stability reason as the mask. Costs one `GL_R16F` target at block
  resolution — 32 KB at 1080p and the default Block Size.

- **`Mask Amount` and `Mask Invert` — where the mosh is allowed to land.** The
  mask comes from the brightness of the *motion* input, so in the effect that
  is the clip's own brightness and in **Mosh Transplant** it is the other
  layer's: a layer of scrolling text or a generator becomes a stencil that also
  drives the movement, and `Motion Source` flips both together. Alpha comes
  along for free, because the mask is computed before premultiplication is
  undone, so a logo on a transparent background masks without a keying step.

  It multiplies the motion gate rather than replacing it, so a region moshes
  only where it is *both* bright and moving — which means a still mask layer in
  the mixer does nothing, as a still motion layer always has. And it sits
  outside the hold-time map, like the gate: folded inside, a half-lit pixel
  would hold for 0.45 s against a fully lit one's 4 s, both reading simply as
  "held", and the mask would collapse into a hard key with no midtones.
  Outside, half depth is half the cross-fade weight, which is what makes it
  paintable.

  Both parameters are **appended**, never inserted, so no existing composition
  reloads wrong — at the cost of a `Mask` panel region below `Output`, since
  groups only collapse when consecutive. Neither is style-managed: a style
  describes a character, a mask is where the instance sits in a composition,
  and a managed mask would be wiped by picking any look.

  Note the hazard, because it is this codebase's headline trap wearing a new
  hat: dark footage at full Mask Amount produces no mosh anywhere, correctly,
  and a plugin producing no mosh emits a pixel-exact copy of its input.

- `GateIsIndependentOfFrameRate` — the same wall-clock interval delivered as
  four steps or as eight must leave the same picture, measured at a part-open
  gate. `DecayIsIndependentOfFrameRate` could not have caught this: it runs at
  Motion Threshold 0 on 3 px/frame motion, where the gate is exactly 1 and the
  normalisation is the identity.
- `DamageSpreadsIntoBlocksThatNeverMoved`, `DamageCreepIsIndependentOfFrameRate`
  and `ResetClearsTheDamageField`. All three passed on their first draft **and
  all three still passed with the thing they covered reintroduced** — the
  clearest demonstration yet of why this repo re-breaks every fix. The first
  measured the damage field, which a mosh pass ignoring it entirely leaves
  perfectly populated; it now measures the picture. The second compared two
  fronts already saturated against the frame edge, which agree however wrong the
  arithmetic is; it now runs on a frame wide enough to catch them partway. The
  third measured a whole-field mean, which stays high because the frame after a
  Reset re-seeds immediately; it now measures beyond the seeding band, where only
  the creep can have put anything.
- `MakeMovingBandPattern` in the harness — a band that moves beside a region that
  provably does not. Its still region can also be made to brighten, because a
  region whose content never changes cannot show whether it moshed: holding a
  pixel and refreshing it give the same colour when the pixel never changes.
- `LumaMaskLocalisesTheMoshAndInvertSwapsIt`, and
  `LumaMaskUsesThisFramesLumaNotLastFrames` — the mask must read the luma
  `PassLuma` just wrote, not the previous frame's. Binding the wrong side is a
  one-word mistake that behaves perfectly on any footage where the mask does
  not move, so the second test flips the split for a single frame; it is the
  only one of the two that fails when the wiring is wrong.
- `NoStyleCanWipeTheMask` — every style, not just one, since a baseline written
  by any of them would do it.
- `MakeSplitPattern` in the harness: both halves carry the same contrast and
  differ only in DC offset, so the estimator sees identical structure on each
  side and anything that differs across the split can only be the mask.
- `QuantiseDoesNotSilenceSlowMotion` — every Quantise setting must still visibly
  depart from passthrough at 1.5 px/frame. `EveryStylePresetChangesTheImage`
  moved from 3 px/frame to the same 1.5, because at 3 it passed on the exact
  edge of the dead zone: one notch slower and `Corrupt` rendered nothing while
  the test stayed green. Presets have to be proved at the slow end, not the fast
  end where every setting works.
- `EveryStylePresetChangesTheImage` — each shipped preset must visibly depart
  from the live image. Written because both bugs above were combinations: every
  control was individually fine, and a per-parameter test with everything else at
  default would have passed all five presets while two of them rendered nothing.
- `ResetReseedsAFrozenVectorField`, and `MeanInteriorDifference` moves into the
  shared harness so both test files ask the question the same way.

- **The motion gate meant something different at every frame rate.** It is a
  spatial term multiplied into `persistence`, and `persistence` is not an
  output weight: the mosh pass writes its result into the accumulation buffer
  and reads it back the next frame, so it is the pole of a feedback loop and a
  raw multiplier on it is a *rate*. Mosh Amount and Decay are built as
  `exp2(-dt/T)` for exactly that reason; the gate was a plain multiply, so
  survival over a wall-clock interval was `gate⁴` where the same interval at
  double the frame rate gave `gate⁸`.

  This is the class of bug 0.2.0 existed to remove, still present in the one
  term nobody looked at — a raw multiply is invisible standing between two that
  look just like it. It survived because the gate is a `smoothstep` over half a
  pixel of motion, so intermediate values are genuinely hard to reach: the
  error was real everywhere and visible almost nowhere. Measured at a
  part-open gate, 30 fps against 60 differed by **0.108**; normalised, by
  **0.0005**.

  The spatial terms are now collapsed into one value normalised once, in
  `MoshCommon.glsl`. Endpoints are exact and at 60 fps the exponent is 1, so
  this is the identity there and changes nothing already measured. It lands
  before the features that need it because anything with a gradient across the
  frame — unlike a hairline smoothstep — would have shown this immediately, and
  three separate normalisations with three separate comments is how divergence
  starts.

### Changed

- **The mosh pass and the composite pass now share one shader source.** The
  field sampling, the motion gate and the two time curves move into a new
  `MoshCommon.glsl`, which is prepended to both `Mosh.glsl` and
  `Composite.glsl` at compile time and carries the `#version` line for them.

  Groundwork, with no behaviour change: the composite pass is about to grow
  views that redraw the warp's decision, and a second copy of a dozen lines of
  gate arithmetic would agree the day it was written and drift afterwards. A
  view that draws a different field than the warp used is worse than no view.

  Verified as a refactor rather than assumed to be one — the accumulation
  buffer was checksummed over 24 frames across six parameter sets spanning
  every damage control, before and after, and every digest is identical. The
  suite passing is not evidence here: a shader that fails to compile falls
  through to `Passthrough`, which looks exactly like a working bypass.

## [0.2.0] — 2026-09-02

**Saved compositions change appearance.** Several sliders now mean something
different at the same position. FFGL has no migration hook, so this release
takes every such change at once rather than breaking compositions five times
across five releases. The endpoints of every remapped control are preserved:
0 still means what it meant and 1 still means what it meant. It is the middle
that moved, and it moved because the middle was where nothing happened.

### Changed

- **Mosh Amount is a hold time on a log scale, not a per-frame retention
  fraction.** The old slider spent most of its travel inside the first few
  frames of hold and crushed the entire usable range into its top tenth —
  measured: 0.5 bought 0.3% of the effect, 0.9 bought 14%, half of it sat
  above 0.95. It was also frame-rate dependent, three lines below a comment
  in the same shader condemning exactly that: a composition authored at 60 fps
  moshed visibly harder at 30. Now 0.5 is a 0.45 s hold at any frame rate.
  The gate stays outside the map as a linear multiplier — folded into the
  exponent it would lift a barely-moving pixel to a near-full hold and erase
  Motion Threshold. This also fixes the burst release, which was nominally a
  250 ms ramp and perceptually a 25 ms snap, and it restores usable MIDI
  resolution to a control that previously had about three bits of it.
- **Decay is geometric.** `mix(8, 0.05, d)` put 0.5 at a 4 s half-life —
  longer than any burst — so nothing happened for four fifths of the slider
  and then it collapsed. Same endpoints; 0.5 is now 0.63 s.
- **Motion Threshold is squared.** Per-frame motion on ordinary footage is
  mostly 0.5–3 px, so a linear 0–8 px slider kept the whole decision in its
  bottom quarter and made the rest a wall. 0.5 is 2 px instead of 4, which
  also turns the Drag preset from "gate shut frame-wide" into "only fast
  things smear" — the trap that caught the author.
- **Motion Threshold is clamped below the estimator's reach.** At Quality
  Low the pyramid reaches 8 px and so did the top of the slider, so Quality
  Low with Motion Threshold at 1.0 was a total bypass, indistinguishable from
  a plugin that failed to load. No Quality setting can make the gate
  unclearable now.
- **Motion Smoothing absorbs Freeze.** They were the same operation applied
  twice — two successive mixes toward the same target compose to one,
  `1-(1-S)(1-F)`, symmetric — and a sweep measured their response curves
  identical to every digit. One axis, two names. Smoothing is now a hold
  time on a log scale like Mosh Amount, and reaches the full hold at 1 that
  used to need Freeze. The Bloom preset sets Smoothing 1.0.
- **Presets no longer write Mosh Amount.** Every style pushed it to 1.0, which
  saturated `max(MoshAmount + audio, held)` and silently disabled Trigger,
  Hold, Auto Mode, Cut Sensitivity, Burst Length, Beat Divisor and Audio
  Amount in the same instant — picking a look was the gesture that disarmed
  everything you would play. A style describes character; the gate stays the
  operator's. Riding Mosh Amount on a preset no longer flips the dropdown to
  Custom either.
- **Mosh Amount defaults to 0.3, Motion Lag to 2.** A fresh instance that is
  a pixel-exact passthrough is indistinguishable from a plugin that failed to
  load — this codebase's first documented trap, made the default. A non-zero
  default is only survivable now that the slider is a hold time; under the old
  taper any non-zero value was either invisible or catastrophic.
- **Reset is a keyframe.** It used to zero one texel of control state, so a
  vector field frozen across a cut survived it permanently. It now discards
  the flow history and reseeds the accumulation buffer from the live frame —
  from live, not to black, because a zeroed buffer under a level still ramping
  down is a dark flash on a panic button.
- **Corruption persists.** The damaged-block set was re-rolled from the frame
  counter every frame, so Corruption read as 60 Hz shimmer. The point of
  datamosh is that errors persist. It now advances on eighth notes while the
  host runs a clock, and every six frames otherwise.
- **Cut Sensitivity and Beat Divisor hide when their mode is not armed**, via
  the SDK's in-place `SetParamVisibility`, which its own comment documents as
  honoured by Resolume. What is on screen is what is live. If a host ignores
  the event, nothing hides and nothing breaks.
- Option labels: `On Cut` → `On Cut (Comp)`, because the detector only sees
  cuts between layers when the effect is on the composition; Beat Divisor
  `1/2/4/8/16` → `1 Beat / 2 Beats / 1 Bar / 2 Bars / 4 Bars`, because a bare
  column of numbers next to "On Beat" reads to any musician as note values,
  where 2 means *longer*; Block Size gains `px`; Audio Band `Volume` →
  `Full Range`. Labels only; stored values are untouched.
- `Pel Snap` → **`Pixel Snap`**. Pel is 1993 MPEG-committee vocabulary.
- The mixer's `Motion Source` is grouped instead of floating above the
  panel, and the inherited `mixVal` is grouped with it. Hiding `mixVal`
  waits on one check in Resolume — whether the host special-cases index 0 of
  a mixer as the blend amount.

### Added

- **`Default` in the Style dropdown.** Selecting Custom restores nothing — it
  is a status, not a destination — so there was no way back from a preset
  that had left Motion Threshold high enough to shut the gate. Appended at
  the end of the list, never inserted: the value is the position and a saved
  composition stores the position.

## [0.1.5] — 2026-08-13

### Removed

- **`Audio Band` loses Mid and High.** They read zero. `ffglqs::Audio` splits the
  2048-bin FFT buffer into equal thirds, but Resolume packs its spectrum into
  roughly the bottom eighth — [resolume/ffgl#25](https://github.com/resolume/ffgl/issues/25),
  filed by the author of that audio class, measures every bin above ~64 of 512
  sitting at `0.0f`. Scaled up, all the energy lands inside the bass third, so
  both upper selections averaged over silence. They were controls that looked
  like they worked and could not.

  A composition saved against an older build falls back to Volume rather than
  reading zero forever. If Resolume ever fixes the distribution they are three
  lines to restore, but not without a test that feeds a realistic spectrum
  rather than a flat one — the existing tests pass a flat buffer, which is
  exactly why this was invisible.

### Changed

- The docs now say plainly that **Resolume's own audio reactivity is better than
  the plugin's** for almost everything. Its parameter FFT gives a draggable
  frequency range instead of two fixed bands, Gain and Fall controls, an
  inverted-response option, and a choice of Clip, Layer, Group, Composition or
  External audio — and it works on every parameter, where the plugin's drives
  only the mosh gate. `Audio Amount` is worth keeping for one reason: it *adds*
  to Mosh Amount rather than owning it, so the slider and `Hold` still work
  while audio pumps underneath. An animated parameter belongs to its animation.

## [0.1.4] — 2026-08-12

### Added

- **`Hold` — a played, piano-style mosh.** Full mosh for exactly as long as it is
  on, ignoring `Burst Length` entirely. Map it to a key or MIDI note and tick
  **Piano** in the shortcut; Piano is a property of the shortcut, not of the
  parameter, so the plugin's job is only to expose something momentary-mappable.

  It is a boolean rather than a `ParamTrigger` for two independent reasons. The
  SDK's `consumeAllTrigger()` zeroes every trigger after each rendered frame, so
  an event cannot express a state that outlives a frame. And Resolume's own
  object model treats events as instants — the REST `EventParameter` schema has
  no `value` field at all and is excluded from the `GET`/`PUT` lists — so a
  latched event could not be inspected, written off, or reset. A boolean can be
  read back, forced off, and clicked in the panel, which is the whole safety
  argument for a control that could conceivably stick.

  Two behaviours exist because a stuck effect mid-set is the worst outcome in
  this design space. A hold takes ownership of the level while it is down, so no
  automatic burst is left running underneath it — without that, releasing in On
  Beat does nothing visible, since at 128bpm the bursts refire every 469ms while
  `Burst Length` defaults to a second, and a release that changes nothing is
  indistinguishable from a control that has jammed. And `Reset` now ends a hold
  that is still reading on: the shader alone zeroes the level for one frame and
  the still-true hold puts it straight back, so on its own Reset was a blink
  rather than a way out. Hold is the only state here that cannot expire by
  itself, and a lost key-up — focus moved to another application mid-hold, a
  dropped MIDI note-off — has to have exactly one recovery gesture.

- **[AGENT-RUNBOOK.md](AGENT-RUNBOOK.md)** and **[CLAUDE.md](CLAUDE.md)** — the
  validation procedure written for a coding agent running on the machine that has
  Resolume, rather than for a person at the keyboard.

  It exists because an agent has capabilities a person does not: Resolume's REST
  API can set a parameter *through the host's own parameter path* and capture a
  render target as a PNG, so most of the smoke test becomes a measurement instead
  of an opinion. `CLAUDE.md` is picked up automatically by a local Claude Code
  session and carries the traps — chiefly that `Passthrough` emits a pixel-exact
  copy of its input, so a dead plugin cannot be told from a working bypass by
  looking.

  It is equally explicit about what an agent must hand back: whether the effect
  looks any good, whether the tuning defaults suit real footage, whether the
  parameter panel groups correctly. And it requires consent and a scratch
  composition before any `PUT`, because those writes change live state and
  Resolume has no undo an agent can drive.

- **`tools/snapshot-diff.py`** — compares two PNG snapshots and returns an exit
  code, so "Mosh Amount 0 must be pixel-identical to bypass" and "Mosh Amount 1
  must diverge" are assertions rather than impressions. Pure standard library.
  Verified by round-tripping all five PNG row filters across greyscale,
  truecolour and alpha at 8 and 16 bits.

### Changed

- The release workflow normalises a missing `v` on the version input and rejects
  anything that is not `vMAJOR.MINOR.PATCH`. Dispatching with `0.1.3` produced a
  release tagged `0.1.3` alongside three `v`-prefixed ones — nothing broke, but
  it made the newest release the odd one out.

## [0.1.3] — 2026-08-12

### Fixed

- **The audio parameters did nothing whatsoever.** `ffglqs::Audio` computes every
  FFT bin as `fft[i] * fft[i] * gain`, and `gain` initialises to `0`. Nothing in
  the SDK ever sets it; the plugin has to, and did not. So `Audio Amount` and
  `Audio Band` were inert in every release so far — with no symptom other than
  nothing happening, which is indistinguishable from a quiet room.

  The audio level is the one value that never passes through `ReadParams` — it is
  read off the analyser at render time — which is exactly why the parameter
  mapping test could not see it. It has its own method now, and two tests drive
  the real host path: one asserts every band responds to a full-scale spectrum,
  the other that energy confined to the bottom of the spectrum reads louder on
  Bass than on High.

- **The mixer's name was truncated to `Datamosh Transpl`.**
  `PluginInfoStruct::PluginName` is `char[16]` and the SDK's copy loop stops at
  16 bytes without writing a terminator, so a name of 16 or more runs straight
  into the adjacent `PluginType` field — a host calling `strlen` reads past it.
  "Datamosh Transplant" is 19 characters. The mixer is now **Mosh Transplant**
  (15), and both plugins carry a `static_assert` so the limit is found at compile
  time rather than in a dropdown.

### Added

- **[VALIDATING.md](VALIDATING.md)** — the procedure for validating a build
  inside a real Resolume. A ten-minute gated smoke test, a triage table, and a
  full pass. It is built around the fact that `Passthrough` emits a pixel-exact
  copy of its input, so a completely dead plugin is indistinguishable from a
  correctly-bypassing effect; every check is written to tell working apart from
  inert rather than from black.
- **`tools/collect-datamosh-diagnostics.sh` / `.ps1`** — strictly read-only
  diagnostics collectors. They report whether the files are where Resolume looks,
  whether they are loadable (quarantine, Mark-of-the-Web, architecture, `plugMain`
  export), whether Resolume scanned the folder, what the log says, and — if the
  webserver is enabled — whether the effect registered, via `GET /api/v1/effects`.
- **MIT licence.** Until now the project had none, which legally meant all
  rights reserved — nobody could redistribute or modify it. `LICENSE` is copied
  into every release archive by the release workflow, so the next version is the
  first whose downloads carry it.

### Changed

- The documentation site is dark, and no longer follows the operating system's
  colour scheme. One palette on `:root` instead of a light base with two dark
  overrides; `color-scheme` and a `theme-color` meta so the browser's own
  furniture matches. The plugin's output is judged against black, so showing it
  on white on half the machines that open the page was showing it wrong.
- Sidebar section labels were using a border colour as text and sat under 2:1
  contrast on the dark ground. They use `--muted` now.

## [0.1.2] — 2026-08-12

### Changed

- Documentation now lives at <https://datamosh.letissier.ie>, deployed to Vercel
  from `site/`. The `INSTALL.txt` inside each archive points there.

  This is the whole reason for the release: those files are copied into the
  archives and frozen the moment a version is cut, so a release is the only way
  to change the URL anyone downloading actually sees. 0.1.1 shipped pointing at
  the GitHub repo.

### Added

- The site is a real deployment rather than a folder of files: security headers
  and a Content-Security-Policy, a 404 page, `robots.txt`, `sitemap.xml`, an SVG
  favicon, and an Open Graph image generated by `site/tools/make-og.py` so
  shared links unfurl with a preview.

## [0.1.1] — 2026-08-12

### Fixed

- The macOS archive nested everything inside a `staging/` folder — the build's
  own working directory name, appearing in a user-facing download. Unzipping it
  produced a folder called `staging` rather than the two bundles.

### Added

- The release workflow now inspects the archives it has just produced, checking
  that the plugins, `INSTALL.txt` and `THIRD-PARTY.txt` are all at the root.
  Every build step passed on the 0.1.0 archives; only opening one caught it.
- A release can be cut from the Actions UI with a version input, not only by
  pushing a tag.

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

[Unreleased]: https://github.com/legofsalmon/ffgl-datamosh/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/legofsalmon/ffgl-datamosh/releases/tag/v0.2.0
[0.1.5]: https://github.com/legofsalmon/ffgl-datamosh/releases/tag/v0.1.5
[0.1.4]: https://github.com/legofsalmon/ffgl-datamosh/releases/tag/v0.1.4
[0.1.3]: https://github.com/legofsalmon/ffgl-datamosh/releases/tag/0.1.3
[0.1.2]: https://github.com/legofsalmon/ffgl-datamosh/releases/tag/v0.1.2
[0.1.1]: https://github.com/legofsalmon/ffgl-datamosh/releases/tag/v0.1.1
[0.1.0]: https://github.com/legofsalmon/ffgl-datamosh/releases/tag/v0.1.0
