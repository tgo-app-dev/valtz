# Valtz

A native macOS app for generative creation and composition.


Let the pictures tell the story.

<p align="center">
<img src="docs/images/0_clean_start.png"
     alt="Valtz starts with a clean workspace"
     width="900">
</p>

<p align="center">
<img src="docs/images/1a_write_prompt.png"
     alt="Write a prompt for a clip: a fleet of Elven galleons in a storm"
     width="900">
</p>

<p align="center">
<img src="docs/images/1b_enhance_prompt.png"
     alt="Enhance it in the Prompt Editor: the assistant rewrites it in the model's own form, its sound and music included"
     width="900">
</p>

<p align="center">
<img src="docs/images/1c_preview.png"
     alt="Generate: the clip previews live as it is made, with the time left"
     width="900">
</p>

<p align="center">
<img src="docs/images/1d_continuation.png"
     alt="Continue the clip from its last 2 to 5 seconds"
     width="900">
</p>

<p align="center">
<img src="docs/images/1e_continuation.png"
     alt="The continuation's prompt: the clip's last seconds in the row, named Video 1 in the text"
     width="900">
</p>

<p align="center">
<img src="docs/images/1f_monitor.png"
     alt="The continuation generating, the status bar showing the GPU, memory and TFLOP/s at work"
     width="900">
</p>

<p align="center">
<img src="docs/images/1g_review.png"
     alt="Review the two clips on the timeline, the continuation laid after the first"
     width="900">
</p>

<p align="center">
<img src="docs/images/1h_seamless.png"
     alt="Across the seam: the continuation picks up where the clip ended"
     width="900">
</p>

<p align="center">
<img src="docs/images/1i_assets.png"
     alt="Everything made is an asset: each generation, its prompt and the guide"
     width="900">
</p>

---

<p align="center">
<img src="docs/images/2a_plan_stick_figure.png"
     alt="Plan a scene by drawing it: two stick figures on markup layers, keyed to move on a timeline"
     width="900">
</p>

<p align="center">
<img src="docs/images/2b_write_simple_prompt.png"
     alt="The plan in the prompt as a reference, the scene told around it in plain words"
     width="900">
</p>

<p align="center">
<img src="docs/images/2c_local_prompt_enhancement.png"
     alt="Enhance it on this Mac: the assistant turns plan and words into the model's subjects, what each keeps of the plan, and the shot"
     width="900">
</p>

<p align="center">
<img src="docs/images/2d_accepted_prompt.png"
     alt="The enhanced prompt accepted, the plan named Video 1 in it"
     width="900">
</p>

<p align="center">
<img src="docs/images/2e_rendering_step_2.png"
     alt="Rendering, step 2 of 8: the two people already where the plan put them"
     width="900">
</p>

<p align="center">
<img src="docs/images/2f_rendering_step_7.png"
     alt="Rendering, step 7 of 8"
     width="900">
</p>

<p align="center">
<img src="docs/images/2g_result_check_1.png"
     alt="Check the result against the plan: the stick figures over the clip, each person where their figure stands"
     width="900">
</p>

<p align="center">
<img src="docs/images/2h_result_check_2.png"
     alt="Later in the clip: the figures still on the people as they meet"
     width="900">
</p>

<p align="center">
<img src="docs/images/2i_result_check_3.png"
     alt="The planned moment: the cup of water poured where the figures meet"
     width="900">
</p>

<p align="center">
<img src="docs/images/2j_video_result.png"
     alt="The finished clip"
     width="900">
</p>

---

<p align="center">
<img src="docs/images/3a_agentic_prompt.png"
     alt="Ask the agent: a four-slide deck on the four seasons, for pre-school children"
     width="900">
</p>

<p align="center">
<img src="docs/images/3b_make_images.png"
     alt="The agent works through the editor's tools step by step, each step waiting for you to run it"
     width="900">
</p>

<p align="center">
<img src="docs/images/3c_generate.png"
     alt="Its pictures generate in the background, landing in Assets as it goes on"
     width="900">
</p>

<p align="center">
<img src="docs/images/3d_feedback.png"
     alt="It looks at each page as drawn before placing the words on it"
     width="900">
</p>

<p align="center">
<img src="docs/images/3e_result_spring.png"
     alt="The result: a four-page deck, spring on the first page"
     width="900">
</p>

<p align="center">
<img src="docs/images/3f_result_winter.png"
     alt="Winter, on the last page"
     width="900">
</p>

---

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
