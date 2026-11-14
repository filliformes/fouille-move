# Fouille — Claude Code context

## Status (2026-11-14): moving to v3
**Read `fouille-design-v3.md` first.** It replaces the v2 design for architecture, surface and
sources: Fouille becomes an **Overtake** module (16 voices on the left pads, 16 gestures on
the right), anchored to an atlas of Quebec places (Montréal first, then Saguenay, Kamouraska),
fed by field recordings (Aporee, Freesound by GPS), live MP3 web radios, local sound maps and
the Move's input, with a web map (github.io + served by the Move). Everything below describes
the v0.2 sound_generator that is still in `src/`; it has never been verified on a Move.
Step 0 tools: `tools/fouille_probe.c` (static ARM64, built by `scripts/build.sh`) and
`scripts/netcheck.sh` (runs `scripts/device-netcheck.sh` on the Move, brings the log back).

## What this is
Network microsound excavator for Ableton Move. Fetches sounds from Freesound/Internet Archive
via wifi, sculpts them with vactrol LPG envelopes. Inspired by Emiliano Pennisi's Envion/Endogen/Interfera.
Schwung sound generator. API: plugin_api_v2_t. Language: C (with pthreads).
Voice architecture: 8-voice polyphonic, round-robin stealing.

## Repo structure (v0.3, Overtake — supersedes the v0.2 notes below)
- `engine/` — portable C99 engine (fe.h API): voices, DSP, harvest worker (Aporee + Freesound by GPS), built-in atlas
- `move/` — Move host: `fouille_move.c` (plugin_api_v2 + string-param bridge), `ui.js` (Overtake UI), `module.json` (version lives here)
- `tests/` — `render_test.c` (offline relation tests, run by build.sh), `net_test.c` (live harvest, run ON the Move)
- `legacy/` — the v0.2 sound_generator, kept for reference, not built
- Install path is now `modules/overtake/fouille/`; install by atomic rename (`scripts/install.sh`)

## Repo structure (v0.2, legacy)
- `src/dsp/fouille.c` — all DSP logic (vactrol LPG, fetch thread, corpus navigator, ternary envelope, render_block)
- `src/module.json` — module metadata and version (must match git tag on release)
- `scripts/build.sh` — Docker ARM64 cross-compile (always use this)
- `scripts/install.sh` — deploys to Move via scp + fixes ownership
- `.github/workflows/release.yml` — CI: verifies version, builds, releases, updates release.json

## Design document
See `fouille-design-v2.md` at repo root for the full architecture design, including:
- Dual Seeder (Freesound + Internet Archive) architecture
- Vactrol LPG model (coupled amplitude + filter)
- Corpus Navigator (2D descriptor-space pad mapping)
- Echo Pool (Phase Garden-style feedback re-injection)
- Spectral Lock, Pulse Engine, Erosion filter
- Organic drift (Brownian LFO) modulation

## Parameter pages (2 pages × 8 knobs, jog-wheel navigation)
### Page 1: Excavation
| Knob | Key | Description |
|------|-----|-------------|
| 1 | terrain | Sound category (Nature/Machine/Voice/Abstract/Material/Urban/Organic/Cosmic) |
| 2 | depth | Spectral brightness filter for fetch queries |
| 3 | rise | Ternary envelope attack time (0.5ms–2s) → LPG strike |
| 4 | hold | Sustain behavior: Freeze / Drift / Granulate |
| 5 | fall | Ternary envelope decay time (1ms–5s) + LPG tail |
| 6 | strike | Vactrol nonlinearity (gentle bloom → bright snap) |
| 7 | lock | Spectral Lock resonant tracker Q (0=bypass) |
| 8 | erode | Erosion degradation rate |

### Page 2: Currents
| Knob | Key | Description |
|------|-----|-------------|
| 1 | flow | Auto-rotation interval (5s–120s) |
| 2 | scatter | Velocity-to-position randomness |
| 3 | stretch | Playback speed (0.25x–4x) |
| 4 | grain | Pulse Engine micro-event size (1ms–200ms) |
| 5 | drift | Organic LFO modulation depth |
| 6 | pool | Echo Pool feedback blend |
| 7 | spread | Stereo voice panning width |
| 8 | volume | Master output (LOW default — lowercase headroom) |

## Architecture: Fetch Thread
- Background pthread handles all network I/O (never in render_block)
- Lock-free communication via stdatomic.h
- v0.1: generates synthetic micro-textures as placeholder
- v0.2 target: popen("curl") → Freesound API + Internet Archive API → minimp3 decode
- Descriptors (centroid, loudness, noisiness) computed in fetch thread after decode
- Double-buffered slot swap: write to shadow buffer, atomic pointer swap

## Architecture: Vactrol LPG
- Per-voice coupled amplitude + filter (one-pole LPF)
- Asymmetric rise/fall rates (fast-ish attack, slow exponential decay)
- Nonlinearity: harder strikes = brighter + louder response
- Ternary envelope output → vactrol CV → state follower → cutoff + amp

## Architecture: Corpus Navigator
- Pad grid (4×8) maps to 2D descriptor space
- X axis (columns): loudness (quiet → loud)
- Y axis (rows): centroid (dark → bright)
- Each pad press finds the nearest slot by Euclidean distance in descriptor space
- Velocity controls LPG strike intensity + position scatter

## Critical constraints
- NEVER write to `/tmp` from render_block — fetch thread can use /tmp for temp files
- NEVER allocate memory in `render_block` — all state lives in the instance struct
- NEVER call printf/log/mutex in `render_block`
- Output path: `modules/sound_generators/fouille/` (not audio_fx!)
- Files on Move must be owned by `ableton:users` — `scripts/install.sh` handles this
- `release.json` is auto-updated by CI — never edit manually
- Git tag `vX.Y.Z` must match `version` in `src/module.json` exactly
- The fetch thread MUST be joined in destroy_instance (set running=0, pthread_join)
- Freesound API key: `/data/UserData/schwung/modules/sound_generators/fouille/freesound.key`
- Internet Archive API: no key needed (public)
- `get_param` MUST return -1 for unknown keys (not 0)

## Build & deploy
```bash
./scripts/build.sh          # Docker ARM64 cross-compile
./scripts/install.sh        # Deploy to move.local
```

## Release
Use the `/move-schwung-release` skill.

## Artistic credit
Directly inspired by Emiliano Pennisi (Avenir):
- Envion (MIT, Pure Data/Max) — Found Net Sound, Dynatext, de-authoring
- Endogen (Max/SuperCollider) — vactrol LPG, lowercase, corpus nav, Phase Garden
- Interfera (Max) — geosonic seeder, Internet Archive, Spectral Lock, Erosion
No code ported. Conceptual lineage acknowledged in README.

## License
MIT
