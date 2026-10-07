# Third-Party Licenses

Valtz itself is licensed under the Mozilla Public License 2.0; see the
root `LICENSE` and `NOTICE` files.

Two groups appear below. The first covers software **redistributed in
binary form inside the macOS application bundle** (`Valtz.app`): its
obligations travel with every copy of the app, and its license texts
ship inside the bundle under `Contents/Resources/Licenses` (and beside
the app in the disk image). The second covers source vendored in this
repository or compiled into Valtz.

Each component's license text is in the file named beside it; this
document is the index, not a substitute for those texts.

---

# Redistributed in the macOS application bundle

## vpipe

The execution engine, bundled as `Contents/Frameworks/libvpipe.0.dylib`.
Home page: <https://github.com/tgo-app-dev/vpipe>.

**License: Apache License, Version 2.0.** Its `LICENSE`, `NOTICE` and
`THIRD_PARTY_LICENSES.md` (the software vpipe itself bundles or links,
LMDB among it) ship in `Contents/Resources/Licenses/vpipe/`, copied from
the vpipe installation the app was built against (`share/doc/vpipe`).

## Sparkle

The auto-update framework, bundled as `Contents/Frameworks/Sparkle.framework`
when the app is built with `-DVALTZ_SPARKLE_DIR` (see
`app/macos/fetch-sparkle.sh`). Home page: <https://sparkle-project.org/>.

**License: MIT.** Text: `app/macos/licenses/MIT-Sparkle.txt`.

## FFmpeg (optional)

Bundled as shared libraries in `Contents/Frameworks` only when the
release is packaged with an FFmpeg directory (`--ffmpeg-dir`); otherwise
vpipe loads it from the host, if present. Home page: <https://ffmpeg.org/>.

**License: GNU Lesser General Public License, version 2.1 or later**, for
an FFmpeg configured without GPL or non-free components. Text:
`app/macos/licenses/LGPL-2.1.txt`. A bundled copy must be one built that
way (vpipe's `apps/macos-app/build-lgpl-ffmpeg.sh` builds one); its
source is available from the FFmpeg project at the version recorded in
the libraries.

---

# Vendored in, or compiled into, Valtz

## LMDB

`extern/lmdb` (git submodule, LMDB 0.9.35), compiled into the core.
Home page: <https://www.symas.com/lmdb>.

**License: OpenLDAP Public License, version 2.8.** Texts:
`extern/lmdb/libraries/liblmdb/LICENSE` and `.../COPYRIGHT`.

## JSON for Modern C++ (nlohmann/json)

`3rd-party/nlohmann/json.hpp`, a vendored single header.
Home page: <https://github.com/nlohmann/json>.

**License: MIT.** Text: `3rd-party/nlohmann/LICENSE.MIT`.

## MiniMax H3 prompt-writing skill

`3rd-party/minimax-h3/` (SKILL.md and its guides), unmodified, compiled
into the core.

**License: MiniMax H3 Community License.** Texts:
`3rd-party/minimax-h3/LICENSE` and `NOTICE`, which also ship in the app
(`Contents/Resources/Licenses/MiniMax-H3`).

---

# Downloaded on request

## Qwen-Image 2.1 prompt rewriters

QwenLM's instructions for writing Qwen-Image 2.1 prompts and edit
instructions (`prompt_rewrite/prompts` in
<https://github.com/QwenLM/Qwen-Image-2.1>, at a pinned commit). Neither
this repository nor the app contains them: Settings › Capabilities offers
them as a download under Qwen-Image 2.1, and Valtz's prompt enhancer
follows them once downloaded (its own Qwen-Image guide otherwise).

**License: Qwen Research License -- NON-COMMERCIAL use only.** Its text
is downloaded with them.

# Model weights

Valtz ships no model weights. Each model it offers is downloaded by the
person using it, from its publisher, under the publisher's own license
-- several of which restrict commercial use (YuE2's are CC BY-NC 4.0, for
one). The catalog names each model's source, and its license where it
records one (`core/resources/model-catalog.json`).
