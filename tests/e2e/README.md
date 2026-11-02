# End-to-end tests

These tests drive the real vpipe engine with the models installed on this Mac: the folder Settings › Storage names, kept in `locations.json`. There are three layers, and each one checks its own interface:

| Layer | Driver | What it checks |
|---|---|---|
| C++ | `tests/e2e-test.cc` in `valtz_test`, gated by `VALTZ_TEST_E2E` | Controller results: sizes, frame counts, soundtrack, pixels, seed determinism, previews, cancel, memory refusal, export, undo |
| CLI | `valtzctl-smoke.sh` | Each `valtzctl` command's exit code, what the project holds, and the files it writes (`valtzctl probe`) |
| App | `app-run.sh` | `Valtz.app` driven by its `VALTZ_SNAPSHOT_TASKS` / `_AGENT` hooks. Each scenario's final `snapshot:` line must show no `lastError`, no `memoryFailure`, and the assets it should have made. Window captures are kept |

## Run

Build with the engine first (see the dev preset):

```bash
cmake -S . -B ~/dump/valtz-build-e2e -G Ninja -DCMAKE_BUILD_TYPE=Debug -DVALTZ_VPIPE_PREFIX=$HOME/dump/vpipe-install -DCMAKE_OSX_DEPLOYMENT_TARGET=27.0
cmake --build ~/dump/valtz-build-e2e
```

Then run either tier:

```bash
tests/e2e/run-all.sh --tier quick
```

```bash
tests/e2e/run-all.sh --tier full
```

- `quick` runs a picture, a 2-second clip, enhance and the Capabilities page, in about 10 minutes. `full` runs everything.
- `--only cpp|ctl|app` runs one layer.
- Each run writes to `~/dump/valtz-e2e/<time>/`: `report.md`, `results.tsv`, `cpp.log`, `ctl/` and `app/<scenario>/` (logs and PNGs).
- The script exits non-zero when any check fails.
- The layers run one after another because they share the GPU. Don't start a run while the app generates.
- Run it from a terminal that has Screen Recording permission, in the foreground, so the app's captures use ScreenCaptureKit.

To run a single C++ test:

```bash
VALTZ_TEST_E2E=1 ~/dump/valtz-build-e2e/tests/valtz_test --filter e2e.a_clip_from_words
```

`VALTZ_TEST_E2E_COMPRESS=1` adds the compress test. It writes an 8-bit Krea 2 beside the original (about 16 GB), makes a picture from it, then deletes the copy.

## Skips

A test whose model isn't installed is **skipped**, not passed. The skip names the missing model, for example `needs flashvsr-plus-turbo`.

| Feature | Models |
|---|---|
| Text-to-image, cancel, export, undo | `krea2-turbo` (+ `taew2_1` preview) |
| Text-to-video, first/last frame, continuation, memory refusal | `minimax-h3-fl2va` (+ `taeh3`, `minimax-h3-turbo-4step`) |
| Favor › Custom adapters | `minimax-h3-pika-flex`, `minimax-h3-turbo-3step` |
| Enhance, intent, summarize, agent | `qwen3.5-9b` |
| Image edit | `qwen-image-2.1-turbo` |
| Image upscale | `vosr`, `dinov2-large` |
| Video upscale | `flashvsr-plus-turbo` |
| Reference-to-video, video edit | `minimax-h3-ref2va` (Speed with `minimax-h3-ref2va-turbo-4step`, else Quality) |
| Past 768P | `open-h3-regenerate-2k`, `open-h3-latent-upscaler`, `open-h3-fast-vae` |

These are not covered:
- Audio-only features (songs, speech, transcribe, separate), left out to save disk.
- The 8-step Turbo LoRA behind the Balanced preset, also left out.
- `minimax-h3-image-vae`: no Valtz request decodes a one-frame H3 latent, so it can't be reached end to end.
- Inpaint, ControlNet, face restore and frame interpolation: Valtz has no model kind for them.
