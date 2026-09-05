# Agent runbook: validating Datamosh in a live Resolume

You are running on the machine that has Resolume. This session is the only place
this work can happen — the plugins have never been loaded into the host, and no
amount of test-suite green says anything about whether they work there.

Read [CLAUDE.md](CLAUDE.md) first for the codebase traps. This document is about
driving the host.

---

## What you can do that a person cannot

Resolume exposes an HTTP API on `localhost:8080/api/v1` (Arena **and** Avenue,
since 7.8). Three capabilities together turn most of the validation from a
matter of opinion into a matter of measurement:

| | |
| --- | --- |
| `PUT /parameter/by-id/{id}` | Set a parameter **through the host's own parameter path** — the same path a slider drag uses |
| `GET /composition/monitors/{id}/snapshot.png` | Capture a render target as a PNG (**Resolume 7.26+ only**) |
| `GET /composition/layers/{n}` | Read back every effect's parameters: names, types, values, dropdown options |

So instead of "drag the slider and see if it looks moshed", you can set a value,
capture a frame, and compare pixels. `tools/snapshot-diff.py` does the comparison
and returns an exit code.

That matters more here than in most projects, because of the thing you must not
forget:

> When the pipeline cannot run, `Passthrough` emits a **pixel-exact copy of the
> input**. A completely dead plugin is indistinguishable from a correctly
> bypassing effect.

Every check below is therefore built to tell *working* from *inert*, never
*working* from *black*.

---

## Before you touch anything

**You will be modifying the user's Resolume composition.** `PUT` and `POST`
change live state, and Resolume has no undo you can drive. Before the first write:

1. **Ask the user for permission**, and ask them to save their composition first.
2. Ask them to open a **new, empty composition**, or tell you which deck and
   layers are safe to use. Do not add effects to layers you did not create.
3. Record what you change so you can put it back.

Never `PUT` to a parameter you have not first `GET`'d and recorded the value of.

If the user cannot give you a scratch composition, stop and hand them
[VALIDATING.md](VALIDATING.md) instead. A validation pass is not worth
overwriting someone's show file.

---

## Step 0 — Ground truth about the install

Run the collector before anything else. It is strictly read-only.

```sh
bash tools/collect-datamosh-diagnostics.sh | tee /tmp/datamosh-diag.txt   # macOS
powershell -ExecutionPolicy Bypass -File tools\collect-datamosh-diagnostics.ps1   # Windows
```

Read the whole thing. The findings marked `>>>` are the ones that matter. In
particular: quarantine or Mark-of-the-Web still set, no `arm64` slice, `plugMain`
not exported, or more than one Extra Effects folder holding a Datamosh binary —
if there are two, **you cannot know which one Resolume loaded**, and every
observation after that is meaningless until the user empties one.

The Windows collector has never been executed on Windows. If it fails to parse,
say so and gather the same sections by hand; do not silently skip them.

## Step 1 — Establish the control channel

Ask the user to enable **Preferences → Webserver**. Then:

```sh
curl -sS --max-time 3 http://localhost:8080/api/v1/product
```

Expect `{"name":"Arena"|"Avenue","major":7,"minor":…}`. **Record the version** —
it decides what is even testable:

| Needs | Version |
| --- | --- |
| Parameter groups (collapsible sections) | 7.3.0 |
| Our `datamosh:` lines reaching the host log | 7.3.1 |
| REST and WebSocket API | 7.8 |
| Native Apple Silicon | 7.11 |
| Snapshot endpoints, bundled MCP server | 7.26 |

Below 7.26 you have no snapshots, so the image comparisons below are unavailable
and those checks go to the human. Say so explicitly rather than skipping them.

A `402` on any `/composition/layergroups/…` path means Avenue, not a fault.

## Step 2 — Did the effect actually register? (**GATE A**)

```sh
curl -sS http://localhost:8080/api/v1/effects | python3 -m json.tool | grep -i -B2 -A2 datamosh
```

This is the machine-readable answer to "is the plugin there at all". Match on the
**name** as well as `idstring` — it is not documented that a third-party FFGL
plugin's four-character ID (`DMSH`) is surfaced verbatim, so do not assert it.

If absent, the plugin never loaded, and Resolume's UI shows nothing at all when
that happens — no notification, no badge, no dialog. Go to triage below.

**The mixer will not appear here.** FFGL mixers are blend modes, not effects.
`GET /composition/layers/1` and read what is under `video.mixer` — read the
actual JSON rather than assuming a key path; the spec models it as an
unstructured map and does not name its keys.

## Step 3 — Instantiate it, and find its parameters

```sh
# text/plain body, not JSON
curl -sS -X POST http://localhost:8080/api/v1/composition/layers/1/effects/video/add \
     -H 'Content-Type: text/plain' --data 'effect:///video/Datamosh'

curl -sS http://localhost:8080/api/v1/composition/layers/1 > /tmp/layer1.json
```

In `video.effects[]` find ours, then map every parameter name to its `id` from
the `params` map. Keep that map; everything below uses it.

**Read the parameter block carefully — it settles a whole class of bug on its
own.** Check that the names, types, dropdown `options[]` and defaults are exactly
what `src/plugins/DatamoshPlugin.h` registers, that there are no extra entries,
and that nothing is unnamed. A phantom record from `SetParamInfo` shows up here
as an entry that should not exist. That bug once made the mixer unloadable in
every host with nothing logged anywhere.

Also check `Motion Gain` (range 0..4), `Burst Length` (0.05..8) and `Motion Lag`
(0..15). If the host reports them as `0..1`, it is normalising, and every value
you set afterwards will be compressed to the bottom of its range — nothing will
look broken, it just never gets as extreme as the labels promise. Confirm it
behaviourally in Step 5 before trusting any sweep.

## Step 4 — Capture setup

```sh
curl -sS http://localhost:8080/api/v1/composition/monitors
```

Pick the render target for the layer or composition under test and note its `id`.

```sh
snap() { curl -sS -o "$1" "http://localhost:8080/api/v1/composition/monitors/$RTID/snapshot.png"; }
```

**Snapshots are not frame-synchronised.** They are captures of whatever the
target holds when the request lands, so:

- Never conclude anything from a single pair. Capture 3–5 and take the median.
- The source clip must be **playing** for divergence checks and **paused on a
  still** for identity checks — otherwise the clip's own motion is the difference
  you are measuring, not the plugin's.
- Allow a settling delay after each `PUT` (~500 ms) before capturing.

## Step 5 — The objective checks

Set up: one clip with strong continuous motion, playing. `Style = Custom`,
`Motion Threshold = 0.0`.

### 5a. Mosh Amount 0 must equal bypass — geometry and alpha

**Pause the clip on a still frame.** Capture with the effect bypassed, then with
it active at `Mosh Amount = 0`.

```sh
python3 tools/snapshot-diff.py bypass.png mosh0.png --expect identical
```

**PASS** — identical. **FAIL** — any difference at all means a `MaxUV`
sub-rectangle or premultiplied-alpha bug: we are resampling or shifting an image
we were supposed to pass through untouched. A shift or zoom versus bypass is the
signature.

This is the one check where identical is the *correct* answer. Everywhere else it
is the failure.

### 5b. Liveness — Mosh Amount 1 must diverge (**GATE B2**)

**Resume playback.** `PUT Mosh Amount = 1.0`, wait ~2 s, capture. Compare against
a frame captured from the live clip with the effect bypassed at the same moment.

```sh
python3 tools/snapshot-diff.py live.png mosh1.png --expect different --min-changed 5
```

**PASS** — grossly different; the picture should be an abstract dragged mess.
**FAIL** — the plugin is inert, or the host is not delivering parameter writes.

**Disambiguate a failure**, because these are different bugs:

`PUT Style = Melt` (a dropdown; find its option index from `options[]`) and
capture again. `ApplyStyle` writes parameters internally and never touches the
host parameter path.

- Melts on Style but not on `PUT Mosh Amount` → **the host is not delivering
  parameter writes.** Everything numeric below would be measuring defaults. Stop.
- Melts on neither → `InitGL` failed, or `SetTime` first delivered exactly `0.0`,
  matching `lastHostTime`'s initialiser, so `timeMoved` was false on the very
  first call, `hasHistory` never became true, and it renders pure passthrough
  forever. Go to triage.

### 5c. The estimator is running — the sharpest check available

Still at `Mosh Amount = 1.0`. `PUT Motion Gain = 0.0`. Capture two frames ~1 s
apart.

```sh
python3 tools/snapshot-diff.py gain0-t1.png gain0-t2.png
```

**PASS** — the moving regions freeze dead in place, so successive captures are
nearly identical in those regions while genuinely static regions keep refreshing.
Compare against the same pair taken at `Motion Gain = 1.0`, which should differ
much more.

This is binary and it separates "pixels are being held" from "motion is being
estimated and applied". It is the single most informative observation in the
whole pass. Report both numbers, not just the verdict.

### 5c². The Motion view is the estimator, drawn

`PUT View = 1` (Motion). Capture one frame.

**PASS** — the frame is coloured wherever the clip is moving: hue is direction,
brightness is speed. **FAIL** — it is black while 5c said the estimator is
running, which means the diagnostic itself is dead and every later observation
made with it is worthless.

Then `PUT View = 2` (Gate) and capture again. Read it as a legend:

| what you see | what it means |
| --- | --- |
| black | no motion here — nothing for the gate to decide |
| red | motion, rejected by **Motion Threshold**. Brighter = closer to clearing it |
| green | the gate is open |
| cyan | the gate is shut but **Spread**'s creep has reached here |
| washed toward grey | the **mask** is closing it |
| dim overall | the gate is open but little is being held — check **Mosh Amount** and **Decay** |

Both views replay what the *last rendered frame* actually used rather than
recomputing from the current parameters, so they cannot disagree with the effect.
That is also what makes them worth trusting in the checks below: if 5d or 5e
looks wrong, switch to Gate before assuming the parameter is broken.

`PUT View = 0` before continuing. Every check after this one is about the
picture, and leaving a diagnostic on would fail all of them.

### 5d. Are the ranges real? (**GATE C**)

Two parameters on two different FFGL type paths. Behaviour only — a value read
back from our own panel proves nothing, because it is our own getter returning
what our own setter stored.

**Burst Length** (`FF_TYPE_STANDARD`): `Mosh Amount = 0`, `Burst Length = 8.0`,
`POST /parameter/by-id/{trigger-id}/trigger`. Poll snapshots every 500 ms and
time how long until the picture returns to clean. You are distinguishing ~8 s
from ~1 s. **FAIL at ~1 s** — the host sent normalised 0..1.

**Motion Lag** (`FF_TYPE_INTEGER`, a different host path): `Mosh Amount = 1.0`,
`Motion Threshold = 0`, `Motion Smoothing = 0`, `Softness = 0`. Capture at
`Motion Lag = 1` and at `15`, compare.

```sh
python3 tools/snapshot-diff.py lag1.png lag15.png --expect different --min-changed 5
```

Either failing means every wide-range parameter is silently compressed to its
bottom eighth. Settle it before any sweep.

### 5e. The audio path

Fixed in 0.1.3 and never yet confirmed against a real host. `Audio Amount = 1.0`,
audio playing into Resolume with the meters moving. The mosh should pulse with
the level.

If it does nothing, check the host is routing audio to plugins at all before
assuming ours is broken — the FFT arrives as parameter *element* values, and a
host that never populates them looks identical to a plugin that ignores them.

## Step 6 — Anchor everything in the log

Resolume's log is cumulative, and `datamosh: 1920x1080, 120x68 blocks, 59 MB of
buffers` is the same line every session. A line from an earlier run satisfies a
naive search. Anchor it:

1. Note the log file's byte size.
2. Set the composition to **1366×768**, delete and re-add the effect.
3. Search only the bytes after that mark.

**PASS** — exactly one new line reading `datamosh: 1366x768, 86x48 blocks, …`.
The block counts must be `86x48` (1366/16 = 85.4 → 86, ceiled; 768/16 = 48).
Numbers no previous session can have written.

That proves **only** that every shader compiled and every render target
allocated. It does not mean anything rendered — `LogVramBudget()` runs before
`PassIngest` draws. Rendering is proved by 5b and 5c and by nothing else.

Watch for the line **repeating near once per frame**: that means the instance is
being asked for two sizes alternately (output and preview), so `EnsureResources`
reallocates and resets `hasHistory` every frame and accumulation can never build.
It presents as a faint stutter rather than an effect, and the repeating line is
the only way to see it.

Log: `Preferences → Feedback → View Log`, or macOS
`~/Library/Logs/Resolume Arena/`, Windows `%LOCALAPPDATA%\Resolume Arena\`.

## Triage — the plugin is not there

Search the log for `Scanning directory`. Grep case-insensitively for substrings;
the verbatim strings below were captured on Resolume 6 and the wording may have
moved.

| | |
| --- | --- |
| No line naming the folder | Resolume never looked there. Wrong folder, or it needs adding under Preferences → Video. **The only way to tell "never found" from "found and rejected".** |
| + `Library could not be loaded` / `Error: 193` | The OS refused the load: architecture mismatch, quarantine/MotW, or a missing VC++ runtime |
| + `plugin main function was unsuccessfull` | `plugMain` returned FF_FAIL — a plugin bug, and the signature of the phantom-parameter class |
| Scan line, no error | Check the collector's `plugMain` export probe |

---

## What you cannot determine, and must hand over

Be explicit about these rather than guessing. A confident wrong answer here costs
more than an admitted gap.

- **Whether it looks good.** Pixel divergence proves the plugin is doing
  something; it says nothing about whether Melt reads as a melt, whether Softness
  carries from tearing to liquid, or whether the tuning defaults are any good on
  real footage. Aesthetic verdicts are the human's, and they are the main reason
  this project needs a validation pass at all.
- **Whether Motion Threshold's 0.15 default is usable.** At 0.15 the gate is
  1.2 px/frame. Ask them to judge it on their own footage and recommend a value.
- **Whether parameters render as collapsible groups**, and whether the effect and
  mixer thumbnails draw. UI, not API.
- **Sustained performance and VRAM over a 30-minute soak.**
- **Anything on a host below 7.26**, where you have no snapshots. Score those
  UNAVAILABLE, not FAILED.

## When you are done

Put the composition back as you found it, then write up:

- The collector output, whole
- The `snapshot-diff.py` numbers for each check — the numbers, not just PASS/FAIL
- The anchored `datamosh:` log line, verbatim
- The parameter block from `GET /composition/layers/{n}` — this is the durable
  artefact, and nothing else in the project has ever seen it
- Exactly what you did not test, and why

The scorecard in [VALIDATING.md](VALIDATING.md) is the format. If you find a bug,
the fix belongs on a branch with a test that fails without it — and reintroduce
the bug to confirm the test catches it.
