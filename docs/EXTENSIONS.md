# Writing a Valtz extension

An extension brings new models to Valtz, and everything Valtz needs to
offer them: what they make, where to download them, the graph they run
in, their tuning options, how to recognize their weight files, how to
word their prompts, and how the on-device assistant should improve a
request for them. The backend, the code that runs the model, is a
**vpipe plugin** inside the package. Everything else is data.

This is the author's reference. A complete, runnable example is in
`examples/extensions/watercolor/`.

## The package

A package is a folder whose name ends in `.valtzext`:

```
acme-video.valtzext/
  manifest.cbor          the declaration: one CBOR map
  plugins/acme.so        vpipe plugins (optional): the backend
  skills/acme-t2v.md     prompt-enhancement instructions (optional)
  licenses/...           anything else the manifest names
```

Valtz looks for packages, in order, in:

1. `Valtz.app/Contents/Extensions/` (shipped with the app);
2. `~/Library/Application Support/com.tgous.valtz/extensions/`;
3. each path in `$VALTZ_EXTENSIONS` (`:`-separated), which may be a
   folder of packages or a package itself (development).

Packages are read once, at launch. A vpipe plugin loads into the
engine's session once per process and cannot be unloaded, so a package
that is added, changed or turned off takes effect at the next launch.

## Authoring

Write the manifest as JSON, then pack it into the package as CBOR:

```
valtzctl ext pack manifest.json acme-video.valtzext   # writes manifest.cbor
valtzctl ext show acme-video.valtzext                 # the CBOR, as JSON
valtzctl ext check acme-video.valtzext                # does this Valtz take it?
VALTZ_EXTENSIONS=$PWD/acme-video.valtzext valtzctl ext   # as a launch takes it
valtzctl ext disable com.acme.video                   # | enable
```

`check` prints the verdict and the manifest as Valtz reads it: upgraded
to the current interface, with models withheld. `ext` (or `ext list`)
also starts the engine, so it reports whether your plugin loaded.

### Rules for keys

* Keys are `lower_snake`. Valtz never renames a key or changes what it
  means. A key that is absent means "the behaviour from before it
  existed", so every field is optional unless this document says
  otherwise.
* A key Valtz does not know is skipped, so a manifest can carry what a
  newer Valtz reads without breaking an older one. When skipping it
  would be WRONG, name the feature that introduced it in
  `required_features` (below).
* Text a person reads (`name`, `description`, a model's `name` and
  `notes`, a family's `name`, a member's `label`, an option's `label`,
  `note` and `help`) is either a string or a map of BCP-47 language to
  string: `{"en": "Watercolor", "zh-Hans": "水彩"}`. Valtz picks the UI
  language, then the language alone (`zh` for `zh-Hans`), then `en`,
  then any.

## Versioning

Valtz versions the extension interface like vpipe versions its plugin
ABI: **one integer, plus features, plus a window**.

| key | meaning |
|---|---|
| `interface` | **Required.** The interface you wrote the manifest for. This Valtz is interface **1**. |
| `interface_min` | The oldest interface that reads your manifest correctly. Defaults to `interface`. Set it lower to promise that what you use from later interfaces is optional. |
| `required_features` | Features this package cannot work without, e.g. `["catalog/1", "vpipe-backend/1"]`. A Valtz that lacks one refuses the whole package. |

A Valtz at interface N:

* reads manifests written for its oldest interface up to N, and
  rewrites older ones into the current schema step by step (each
  interface change ships with that rewrite, tested against a frozen old
  manifest);
* reads a manifest written for a later interface **as it is** when that
  manifest's `interface_min` is N or lower, and refuses it otherwise
  (`too-new`);
* refuses a manifest older than its oldest interface (`too-old`). Valtz
  raises the oldest interface only in a major release, and always reads
  at least the last three.

The integer changes only when an older manifest would be misread, for
example a key's meaning or default changing. Everything additive is a new
optional key, or, when ignoring it would be wrong, a new **feature**.

Interface 1's features:

| feature | what it covers |
|---|---|
| `catalog/1` | `models`, `families`, `auto` |
| `tuning-options/1` | `engine.vpipe.options` |
| `recognize/1` | `recognize` |
| `skills/1` | `skills`, `prompting.enhance` naming one |
| `prompt-template/1` | `prompting.template` |
| `vpipe-backend/1` | `vpipe.plugins` |

A **model** may also list `required_features`. A Valtz that lacks one
withholds that model and still offers the rest of the package.

Your vpipe plugin is versioned by vpipe (docs/PLUGINS.md in vpipe: ABI
integer, features, a two-version window). Declare what it was built for
(below); Valtz checks that against the vpipe it runs on before loading
it.

## The manifest

```json
{
  "format": "valtz-extension",
  "interface": 1,
  "id": "com.acme.video",
  "version": "1.2.0",
  "name": {"en": "Acme Video", "zh-Hans": "Acme 视频"},
  "description": "Acme's text-to-video model, with sound.",
  "vendor": "Acme Inc.",
  "license": "Commercial",
  "required_features": ["catalog/1", "vpipe-backend/1"],
  "vpipe": {"plugins": [...]},
  "models": [...],
  "families": [...],
  "auto": {...},
  "recognize": [...],
  "skills": [...]
}
```

| key | |
|---|---|
| `format` | **Required**, `"valtz-extension"`. |
| `id` | **Required.** Reverse-DNS, lower case (`a-z 0-9 . - _`, at least one dot). It names the package in recipes and in the user's choices: keep it forever. |
| `version` | Your version. Recorded with everything made with your models. |
| `vendor`, `license` | Shown with the package. |

### `vpipe.plugins`: the backend

```json
"vpipe": {"plugins": [
  {"file": "plugins/acme.so", "abi": 8,
   "required_features": ["stage-commands/1"],
   "stages": ["acme-model-config", "acme-generate"]}
]}
```

| key | |
|---|---|
| `file` | The plugin, inside the package. A path outside the package is refused. |
| `abi` | The vpipe plugin ABI it was built for (`VPIPE_PLUGIN_ABI_VERSION`). Outside the running vpipe's window, it is not loaded. |
| `required_features` | The vpipe host features it needs (`VPIPE_FEATURE_*`). |
| `stages` | The stage types it registers. Valtz asks vpipe for each after the session starts; one missing means the plugin did not load. |

If a plugin is missing, refused or did not load, **nothing in the
package is offered**: its models would have nothing to run on. Valtz and
its engine keep running, and the package shows `backend-failed` with the
reason. vpipe's log has vpipe's own.

### `models`

Each entry uses the schema of Valtz's built-in catalog
(`core/resources/model-catalog.json`), read by the same code:

| key | |
|---|---|
| `id` | **Required.** Unique across Valtz and every package: prefix it with your name. A taken id is withheld (`duplicate-id`): a package cannot replace a built-in model. |
| `hf_path` | **Required.** The download: `<org>/<repo>` on the Hugging Face hub, fetched by vpipe's model-fetch into `<models root>/<org>/<repo>`. |
| `subdir`, `file`, `fetch_variant` | One model of several in a repo: a folder in it, a single file, vpipe model-fetch's `model_variant`. |
| `gated`, `disk_gb`, `min_ram_gb` | A gated repo; its size; the smallest memory tier it is offered on. |
| `name`, `notes`, `license` | Shown in Settings › Capabilities. |
| `family`, `role` | Its family's id; `image`, `video`, `audio`, `assistant`, `lora`, `vae`, `encoder`, `upscale`, `preview`, `branch`. |
| `capabilities` | What it makes: `text-to-image`, `image-edit`, `text-to-video`, `image-to-video`, `reference-to-video`, `text-to-audio`, `prompt-enhance`, `intent`, … A capability this Valtz does not know withholds the model (`unknown-capability`). |
| `rank`, `rank_for` | Its rank against other models for the same capability. |
| `preview_with`, `requires` | Its TAE preview model, and models it needs installed (a decoder, an encoder). These may name built-in models (`taef2`) or your own. |
| `required_features` | Features this model needs (above). |
| `engine.vpipe` | Its graph, below. |
| `prompting` | How it reads a prompt, below. |

#### `engine.vpipe`: the graph

Valtz builds each job's vpipe graph itself, in one of a few **shapes**.
A model names its shape. The shape's parameters are the same keys the
built-in models use:

| shape | what it builds | parameters |
|---|---|---|
| `diffusion-image` | text to image and image edit: model-select → text-prompt → diffusion-conditioner → generate-image → vae-decode → save-image | `config_stage` (your family's model-config stage), `scheduler`, `defaults` (`width`, `height`, `steps`), `align`, `edit` (`max_references`, `reference_area`, `reference_to_conditioner`, `defaults`, `config`) |
| `minimax-h3` | a clip with its soundtrack, optionally from references (MiniMax H3's contract) | `config_stage`, `config`, `generate`, `defaults` (`fps`, `frames`, `steps`), `frame_grid`, `max_frames`, `references` |
| `yue2` | a song from a style and lyrics | `decoder`, `defaults`, `max_seconds`, `sample_rate` |
| `chat` | the assistant (text-chat) | |

Without `shape`, the shape follows the capabilities. A shape this Valtz
does not build withholds the model (`needs-shape`). A shape whose
contract changes gets a new name, never a new meaning.

The most common extension is a new image family. Your plugin registers
the family with vpipe (`register_family_profile`, its model exec and its
`<family>-model-config` stage), so vpipe's generic `generate-image`
runs it, and `diffusion-image` builds the graph.

Common to every shape:

| key | |
|---|---|
| `accel` | The acceleration tiers vpipe applies to it (`sol_attn`, `sage_attn`, `i8_gemm`, `ane_ffn`, `ane_qkv`, `motion_cache`): Favor's Custom offers these. |
| `ane_qkv_min_ram_gb` | Optional. Before M5 (no GPU matrix cores, at most 20 GPU cores), Fast and Med turn on `ane_ffn`, and `ane_qkv` for a picture model; with this, `ane_qkv` instead goes on for any role, only on a Mac with at least this much memory (MiniMax H3: 24). |
| `steps` | Steps per preference: `{"speed": 4, "balanced": 8, "quality": 12}`. |
| `turbo` | A few-step LoRA it runs with: `{"lora": <model id>, "steps": {...}, "scale", "config"}`. |
| `options` | Your own tuning options, below. |

#### `engine.vpipe.options`: tuning options

Options for your own stages, shown in Favor's Custom panel after
Valtz's own:

```json
{"key": "acme_cache", "type": "bool",
 "presets": {"speed": true},
 "label": {"en": "Acme cache", "zh-Hans": "Acme 缓存"},
 "note": "Reuses steps it can predict",
 "help": "Longer explanation, shown on hover.",
 "group": "compute", "stage": "generate"}
```

| key | |
|---|---|
| `key` | **Required.** `lower_snake`: the config key your stage reads. A key Valtz already offers (`steps`, `sage_attn`, `shift`, …) is not taken. |
| `type` | `bool`, `int` or `real`. |
| `min`, `max`, `step` | The range of a number. Values are held to it. |
| `default`, `presets` | The value, and the value under each preference (`speed`, `balanced`, `quality`). |
| `needs` | The option it refines: disabled while that one is off. |
| `excludes` | Options it turns off while it is on. |
| `requires` | `"matrix-cores"`: only on an M5 or later. |
| `group` | `steps`, `attention`, `compute` or `schedule` (the default). |
| `stage` | `generate` (the generating stage's config, the default) or `config` (your family's config stage). |
| `label`, `note`, `help` | Text. |

### `prompting`

| key | |
|---|---|
| `language` | `"english"` if the model reads English only. The assistant then writes its prompts in English and translates other languages. Without it, the assistant keeps the request's language. |
| `template` | Words around every prompt the model is given; `{prompt}` marks where the prompt goes: `"{prompt}. Loose watercolor illustration..."`. |
| `reference_tag` | What the prompt calls an edit's n-th picture: `"<image{n}>"`. |
| `reference_tags` | Per kind, for references: `{"image": "<Picture {n}>", "video": "<Video {n}>", "audio": "<Audio {n}>"}`. |
| `continuation` | How a continuation's prompt opens. |
| `enhance` | The skill the assistant uses to improve a request: `{"generate": <skill id>, "edit": <skill id>}`. |
| `outline` | The prompt template an empty prompt box starts from (the assistant's button with nothing to enhance): the form your model's prompts were trained in, with a short hint in each part. A string, its lines as an array, or either by language (`{"en": [...], "zh-Hans": [...]}`). Written for what the prompt's row holds: a line with `{image}`, `{video}` or `{audio}` is written once per staged medium of that kind, the marker made its tag; a line opening with `{each image}` (video, audio) is too, the marker dropped; `{subject}` is the next subject number, from 1 in each section (lines between blank lines); a line opening with `{if image}` (video, audio, `none`) is kept only when the row holds that kind (nothing at all). Optional: a Valtz without it ignores it. |

Valtz keeps prompts in its own form, with references as positional tags
(`<valtz_ref_img_0>`), and turns each tag into your model's names
(`reference_tag`, `reference_tags`) when it sends the prompt.

### `families` and `auto`

```json
"families": [{"id": "acme", "name": "Acme",
              "features": ["video-gen"],
              "members": [{"model": "acme-v1", "label": "v1"}, "acme-vae"]}],
"auto": {"video": {"generate": {"add": ["acme-v1"],
                                "before": "minimax-h3-fl2va"}}}
```

A family groups your models in Settings › Capabilities. Its features are
`video-gen`, `video-edit`, `image-gen`, `image-edit`, `audio-gen`,
`helper`, `video-upscale` or `image-upscale`. Naming a family that
already exists (a built-in one) adds your members to it: a LoRA pack for
MiniMax H3 joins the `minimax-h3` family. A model is in one family at
most.

`auto` adds your models to what the model field's Auto tries, per
modality (`image`, `video`, `audio`) and op (`generate`, `edit`). A list
is appended. `{"add": [...], "before": <id>}` inserts ahead of a model.
Appending is the polite default: installing a package should not change
what Auto already picks unless you mean it to.

### `recognize`: weight files

When someone drops weights on Favor's Custom panel, Valtz decides what
they are. Your rules run before its own heuristics:

```json
"recognize": [
  {"kind": "lora", "family": "acme",
   "keys_any": ["acme_blocks."],
   "metadata": {"modelspec.architecture": "acme-v1*"}},
  {"kind": "dit", "family": "acme",
   "class_name": ["AcmeTransformer3DModel"]}
]
```

| key | |
|---|---|
| `kind` | **Required.** `lora`, `dit` or `vae`. |
| `family` | The family it belongs to. |
| `class_name` | A diffusers folder's `config.json` `_class_name`, any of these. |
| `keys_any` | Some tensor name contains one of these. |
| `keys_all` | Each of these is in some tensor name. |
| `metadata` | Values in the safetensors header's `__metadata__`, matched as globs (`*` is any run of characters). |

Every condition a rule states must hold. A rule that states none never
matches. The first rule that matches wins. A folder is judged by its
`config.json` and its weights file (`adapter_model.safetensors`, or the
largest `.safetensors` in it).

### `skills`: prompt enhancement

```json
"skills": [{"id": "acme-t2v", "file": "skills/acme-t2v.md"}]
```

A skill is the instructions the on-device assistant (Qwen3.5) follows to
improve a request for your model, at most 256 KiB of text, named by a
model's `prompting.enhance`. Ask for one JSON object in reply:
`{"rewritten_prompt": "..."}`. You may add `"wh_ratio"` (the aspect
ratio the description is written for) and `"ratio_follow"` (the
picture whose shape the result follows). Valtz adds what the request
says about language, size and pictures. Skill ids are unique across
Valtz and every package.

## How Valtz takes a package

| state | |
|---|---|
| `ready` | Everything it brings is offered. |
| `partial` | Offered, except what `withheld` lists. |
| `disabled` | Turned off by the user (`valtzctl ext disable`); listed, nothing offered. |
| `refused` | Not read: `why` says why. |
| `backend-failed` | Its plugin was missing, refused or did not load; nothing offered. |

Why a package is refused: `no-manifest`, `too-large` (a manifest over 4
MiB), `unreadable` (not a CBOR map: JSON must be packed), `not-an-extension`,
`bad-id`, `no-interface`, `too-old`, `too-new`, `needs-feature`,
`duplicate-id` (another package took the id first), `upgrade-failed`.
A backend: `bad-path`, `missing-file`, `abi`, `feature`, `stage`.

Why an entry is withheld: `duplicate-id`, `unknown-capability`,
`unknown-reference` (a model it names is not there), `unknown-feature`
(a family feature), `two-families`, `needs-feature`, `needs-shape`,
`bad-entry`, `bad-path`, `missing-file`, `too-large`.

What a model of yours makes records your package's id and version
(recipe `params.extension`), so a project opened where your package is
missing can say what made it.
