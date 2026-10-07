# Valtz

Image, video, music and speech generation and editing for Apple Silicon
Macs, running entirely on the Mac.

- **SwiftUI app** (`app/macos`) ↔ **C++20 model-controller layer**
  (`core`, via the `ValtzBridge` Swift/C++ interop facade) ↔
  **[vpipe](https://github.com/tgo-app-dev/vpipe)** as the execution
  engine (models and Metal kernels, linked in-process as an installed
  package).
- **Projects** are `.valtz` documents: their records and a
  content-addressed media store, edited in a working copy with save,
  revert and undo. Derived assets form a make-like dependency graph --
  every version records its recipe and the exact input content it was
  built from, so staleness is decided by content. Video is linked in
  place.
- **Models** are offered per memory tier and downloaded on demand; an
  on-device language model helps write prompts.
- **One window, progressive detail**: a single prompt box with inline
  media; live previews stream into the stage with A/B compare; layers,
  timelines, markup and the inspector open as they are needed.
- **`valtzctl`** drives the same controller from the command line;
  anything the app can do, it can.
- **Extensions** (`docs/EXTENSIONS.md`) bring models to Valtz as data,
  with a vpipe plugin as their backend.

## Build

Needs macOS 26 or later on Apple Silicon, Xcode (Swift 6), CMake ≥ 3.29,
Ninja (`brew install ninja`) and an **installed vpipe**:

```sh
# vpipe, once (see its README): build it, then install it
cmake --install <vpipe-build> --prefix ~/dump/vpipe-install

git submodule update --init
cmake --preset dev && cmake --build --preset dev   # → ~/dump/valtz-build
~/dump/valtz-build/tests/valtz_test
open ~/dump/valtz-build/app/macos/Valtz.app
```

`VALTZ_VPIPE_PREFIX` points at a vpipe installed elsewhere; `cmake
--preset no-engine` builds without vpipe (the UI and the store only).

The `dev` build runs in place: it finds libvpipe where it was installed.
A **relocatable** app -- libvpipe and the plugin inside the bundle, every
reference rewritten, signed ad hoc -- is the `valtz-dist` target:

```sh
cmake --preset release && cmake --build --preset release --target valtz-dist
# → ~/dump/valtz-build-rel/dist/Valtz.app
```

### Auto-update (optional)

Valtz updates itself with [Sparkle](https://sparkle-project.org/) when
it is built with it. Sparkle is not vendored:

```sh
app/macos/fetch-sparkle.sh            # → ~/dump/sparkle/Sparkle-<version>
cmake --preset release -DVALTZ_SPARKLE_DIR=~/dump/sparkle/Sparkle-<version>
```

Without it the app builds and runs, and simply never offers an update.

## License

Valtz is licensed under the [Mozilla Public License 2.0](LICENSE); see
[`NOTICE`](NOTICE). The Valtz Extension SDK, when it is published, will
be licensed under the Apache License 2.0, as vpipe is; the example
extensions (`examples/extensions/`) already are.

Third-party components -- vpipe, LMDB, nlohmann/json, Sparkle and
MiniMax H3's prompt-writing guide, each under its own license -- are
listed in [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md). Model
weights are not part of Valtz, nor is anything licensed for
non-commercial use only (such as QwenLM's Qwen-Image prompt rewriters):
each is downloaded from its publisher, on request, under the publisher's
license.
