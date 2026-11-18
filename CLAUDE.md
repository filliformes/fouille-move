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

## Right pads: gestures (v0.5.1)
- Grid by idea, top row first: Transformer (Bongo, Pulsars, Accroche, Corps), Déplacer (Étirement,
  Contagion, Voisin de timbre, Dérive géosonique), Se souvenir (Gel, Ressac, Passé, Réécriture),
  Deviner (Terne, Double hasard, Hésitation, Futur). Ids = `FE_X_*` in `engine/fe.h`.
- All 16 built. Terne lines use five families of our own (Rebond, Rafale, Bégaiement, Vague, Pluie,
  `t_line_step`); Double hasard draws lines/families from them; Hésitation is one shared pulse.
- Memory: each voice keeps its previous fragment (Passé) and a pool of FE_NFUT=6 tapes holds prepared
  next fragments (Futur): 39 tapes of 30 s, about 206 MB at 44.1 kHz (was 17 tapes, 90 MB).
  Shift + Futur = adopt (`fe_adopt`, key `xa`), not latch. A renewal (refetch or flux) adopts a
  prepared Futur instead of harvesting.
- Voisin de timbre picks among the 16 fragments in RAM (no disk corpus yet). Réécriture writes into
  the tape: the wear stays after release, until the fragment is renewed.
- A gesture has an amount (0 = off) and 4 settings (knobs 5-8). Global amount (v = -1) + per-voice
  amount; a voice hears the larger. Onsets are detected in the engine (`gestures_block`).
- UI: hold = plays; velocity then pressure = amount; Shift = latch/unlatch; Undo + pad = default
  settings; hold left pad(s) then right pads = those sounds only (4 gestures max per sound).
- Left pads: tap opens at Départ exactly (no scatter); re-tap closes (after the 300 ms double-tap
  window); double tap = figer; Shift + tap = fouiller encore; reopening in a tail dips then restarts at Départ.
- Names are Fouille's own (no module names from Interfera/Endogen): the knob 7 "Lock" stays a voice param.
- v0.5.1 (Vincent played v0.5.0): LEDs like Loopex. Right pads = one colour per row (AzureBlue,
  NeonPink, Purple, VividYellow), White when touched or latched. Left pads dark unless the sound
  plays (`op` key = held open, not the tail); green, White = frozen, row colour = its own latched
  gesture, BrightOrange = a Futur is ready. LEDs set at once on the hand's action (not queued).
  A close goes dark at the release; the sound still closes after the 300 ms double-tap window.
- Screen: holding a gesture shows `fe_gesture_status` (key `xst:g,v`) on the bottom two lines;
  Dérive and Futur linger 8 s after release so the wait is visible.
- Contagion also borrows from closed sounds (their tape read in place, `vghost`), full stereo by map
  direction. Dérive: a tap is enough, it stays where it went (Retour off), Undo + Dérive = home.
  Ressac: louder, in waves (period from Temps), the line stretches as the wave rises (slower, lower).
  Réécriture: a sweep wears the region ahead of the head (heard within a second). Terne: make-up x2
  and a fast vactrol on strikes (strikes land at the held level). Hésitation no longer resets Gel.
  Futur less veiled (filter default 0.75). Saved states keep old gesture settings: Undo + pad resets.
- v0.5.2: Lieu page knobs 5-8 = Coupe bas (6 dB/oct, 10 Hz-2 kHz, `FE_P_LOCUT`), Coupe haut (6 dB/oct,
  20 kHz-2.1 kHz, `FE_P_HICUT`), Vitesse (same param as Matiere k1, x0.25-x4, x1 default), Volume.
  A held right pad still takes knobs 5-8 for its settings. New params sit at the end of the voice state.

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
