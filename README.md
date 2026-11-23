<p align="center">
  <img src="https://github.com/user-attachments/assets/8796a273-0707-4866-b816-6339247f72f5" alt="valtz" width="200">
</p>

# <b><i style="font-family:'Bookman Old Style',Bookman,serif">valtz</i></b>: local AI video, pictures and sound on a Mac

**<b><i style="font-family:'Bookman Old Style',Bookman,serif">valtz</i></b> is a native Mac app that makes AI video with sound, still pictures, speech and songs on Apple silicon, with an agent that works inside it.** The models run on the machine in front of you, through [Vpipe](https://github.com/tgo-app-dev/vpipe), and rendering on a Mac is fast ([vpipe.ai/benchmarks](https://vpipe.ai/benchmarks)). You open the app and render. macOS 26.2 or later.

The official site is [vpipe.ai/valtz](https://vpipe.ai/valtz).

<p align="center">
  <a href="https://vpipe.ai/valtz"><strong>Official site</strong></a>
  &nbsp;·&nbsp;
  <a href="https://discord.com/invite/zG8a8BMKe">Discord</a>
  &nbsp;·&nbsp;
  <a href="https://www.reddit.com/r/vpipe">reddit.com/r/vpipe</a>
</p>

<p align="center">
  <a href="https://github.com/tgo-app-dev/valtz/releases/latest"><strong>Download for Mac</strong></a>
  &nbsp;·&nbsp;
  <a href="#made-on-a-mac">Films</a>
  &nbsp;·&nbsp;
  <a href="#the-app">The app</a>
  &nbsp;·&nbsp;
  <a href="#several-macs">Several Macs</a>
  &nbsp;·&nbsp;
  <a href="#questions">Questions</a>
  &nbsp;·&nbsp;
  <a href="#build">Build</a>
</p>

<p align="center">
  <img src="https://raw.githubusercontent.com/tgo-app-dev/valtz-media/main/elven-galleons-storm.webp" alt="Elven galleons sailing a storm, an AI video generated locally in valtz on a Mac" width="720">
</p>

<p align="center">
  Galleons in a storm. Picture and sound, made in <b><i style="font-family:'Bookman Old Style',Bookman,serif">valtz</i></b>.
  <a href="https://github.com/user-attachments/assets/68771dcc-3820-4a4c-ba10-433a9d5be188">Play it with sound</a>
</p>

The loops on this page are silent, so the page stays quiet. Every generated clip has a link to the same frames with their sound. Each one was generated in <b><i style="font-family:'Bookman Old Style',Bookman,serif">valtz</i></b>.

## At a glance

- **Video with sound.** MiniMax H3 turns a prompt, or a picture, into a clip. Give it a first frame and a last frame. The picture takes shape while you watch, step by step.
- **Pictures.** MiniMax-H3-Image, Qwen-Image 2.1, FLUX.2 klein, FLUX.3 Image Klein, Krea 2, Z-Image, MOSS-Image, P-H3-Image, and Fibo 2.0 for edits. Mark a region and say what should change.
- **Sound.** Songs with YuE2 and MiniMax-Music3. Speech with MOSS-TTS and Fish Audio Drama 3. Lift a vocal off a track, or a lead vocal off the backing. Mark beats and words on the timeline.
- **Editing.** One-stop, in the app. Grade, cut, titles, sound and the timeline, then export the finished film from the same window.
- **Longer work.** A long video is shots joined with no seam. Give a picture a voice or a song and the person speaks or sings it. The agent (experimental) cuts a timeline from clips you already have, watches a clip and describes it, and builds a scene shot by shot.
- **Several Macs.** In fleet mode, this Mac sends a job to another Mac that can run it. Two Macs on Thunderbolt can also share one generation. A paired Mac can lend this one memory.
- **On your Mac, and fast.** Rendering happens on the Apple silicon in the machine ([vpipe.ai/benchmarks](https://vpipe.ai/benchmarks)). The app is open source, [MPL-2.0](LICENSE). The interface is in English and Simplified Chinese.

## Made on a Mac

Short excerpts. The link under each film is the same shot with its sound. The last loop has no sound: it is the stick-figure plan, picture by picture.

<p align="center">
  <img src="https://raw.githubusercontent.com/tgo-app-dev/valtz-media/main/forest-water-fox.webp" alt="A blue-haired elf reaching toward a translucent water fox in a forest pool, generated in valtz" width="640">
</p>

<p align="center">
  A water fox, and the hand that called it.
  <a href="https://github.com/user-attachments/assets/56b52e82-c2a2-49c9-9fde-d872c3fb579b">With sound</a>
</p>

<p align="center">
  <img src="https://raw.githubusercontent.com/tgo-app-dev/valtz-media/main/tokyo-arrival.webp" alt="A woman filming herself in front of Tokyo Tower, an AI video with a spoken Japanese line, made in valtz" width="640">
</p>

<p align="center">
  She arrives and says so: 「やっと東京に着いたよ」.
  <a href="https://github.com/user-attachments/assets/f8de2fd2-3904-4be8-9442-acab0927b89b">With sound</a>
</p>

<p align="center">
  <img src="https://raw.githubusercontent.com/tgo-app-dev/valtz-media/main/kyoto-matcha.webp" alt="A woman with a matcha drink on a Japanese street, from a 4K AI video made in valtz" width="640">
</p>

<p align="center">
  A slow street, from a 4K generation, and the line 「今日は自分の時間」.
  <a href="https://github.com/user-attachments/assets/71546cad-1bc4-476b-ad59-b5c4421831c3">With sound</a>
</p>

<p align="center">
  <img src="https://raw.githubusercontent.com/tgo-app-dev/valtz-media/main/war-photographer.webp" alt="A photographer running down a burning street beside a tram, a cinematic AI shot made in valtz" width="640">
</p>

<p align="center">
  A photographer, a tram, a burning street.
  <a href="https://github.com/user-attachments/assets/ca94ec2a-1991-4091-b23e-32cb65b99e1f">With sound</a>
</p>

<p align="center">
  <img src="https://raw.githubusercontent.com/tgo-app-dev/valtz-media/main/stick-figures.webp" alt="Two stick figures drawn on a valtz timeline, then the generated shot of the two people where the figures stood" width="820">
</p>

<p align="center">
  Stick figures drawn first. The shot follows them.
</p>

## The app

One window for the prompt, the picture, the grade, the cut, the sound and the timeline. Editing is one-stop: the finished film is exported from this app. What you make is kept: the generation, the grade, the upscale, each with the prompt and the seed.

### Watch it take shape

A draft is a few steps on purpose. You see the fleet, the lightning and the waves before the clip is finished, and you stop early when the idea is wrong.

<p align="center">
  <a href="https://github.com/user-attachments/assets/6fe3b3d2-fcae-4682-a407-a6c90febf301">
    <img src="https://github.com/user-attachments/assets/2bb539d5-7cdf-48d3-a480-eca153e8ecb4" alt="valtz generating a storm clip on a Mac, step 1 of 4, the ships already visible" width="820">
  </a>
</p>

<p align="center">
  MiniMax H3 FL2VA, Favor Fast, four steps.
  <a href="https://github.com/user-attachments/assets/6fe3b3d2-fcae-4682-a407-a6c90febf301">Play, with English narration</a>
</p>

Favor is Fast, Med or Fine. Tune is there when you want the steps, the adapter and the attention yourself, the seed beside it in the card. Turn on Skip kernel autotuning (Settings → General → Reproducibility) when the same seed should come back the same.

<p align="center">
  <img src="https://github.com/user-attachments/assets/81a3806b-b77e-4921-9101-4aa293bb7a04" alt="valtz Tune panel for MiniMax H3, with Favor Fast selected and TaoMate, attention and seed controls" width="820">
</p>

A long video is a chain of shots. Each one continues from the last shot’s own last seconds. Keep a shot's latents (the card's Latents row, off by default) and the next one continues through them, for a seamless join; without them it reads the tail as a reference. Give it a picture of someone and a voice or a song, and they speak or sing that sound. You can also hand it a few reference frames, or a [RefMod in ComfyUI’s format](https://github.com/Luisacaotica/ComfyUI-MiniMaxH3Mod).

### Grade it where it sits

Exposure, contrast, highlights, shadows, temperature. Then a color picked off the picture itself. Then the three-way wheels: shadows, midtones and highlights, each with its own tint. Bypass compares before and after. Baking the grade writes a new clip and leaves the original alone.

<p align="center">
  <a href="https://github.com/user-attachments/assets/779f7512-1194-4cc6-ae97-37073ddb9838">
    <img src="https://github.com/user-attachments/assets/e2d36486-7fd3-4e54-80d1-39e433a9b306" alt="valtz three-way color wheels grading a storm clip, shadows pushed toward magenta" width="820">
  </a>
</p>

<p align="center">
  <a href="https://github.com/user-attachments/assets/779f7512-1194-4cc6-ae97-37073ddb9838">Play the grade, with English narration</a>
</p>

### A larger frame, from the same shot

The draft above is 832×480. Double the canvas and a super-resolution model fills the new pixels. RelayVSR does this for clips and for stills, including a 2× pass. Open-H3-Regenerate-2K-IC is the pass that keeps a face, the lips, the text and the layout of the draft. RIFE changes the frame rate, or makes the motion slow.

<p align="center">
  <a href="https://github.com/user-attachments/assets/fe98a910-0ab1-4da4-bba3-ef2260c78daa">
    <img src="https://github.com/user-attachments/assets/03e203c9-c5d5-49e6-aa18-e70258e5e11e" alt="valtz canvas at 832 by 480, ready to double to 1664 by 960 for a super-resolution pass" width="820">
  </a>
</p>

<p align="center">
  <a href="https://github.com/user-attachments/assets/fe98a910-0ab1-4da4-bba3-ef2260c78daa">Play the upscale, with English narration</a>
</p>

The timeline holds the finished clip as a layer: trim it, cut a keyed clip with the scissors, snap to a bookmark. Light and dark follow the system, or stay where you left them. An export can carry the prompt and the seed that made it.

### Pictures, in words

MiniMax-H3-Image draws the still, then edits it. Circle the thing you mean, drop it in as a reference, and say what should change. You can compare the result with the original. On a Mac with 32 GB or more, MiniMax's own prompt expander, a download of its own, rewrites the prompt. Without it, the Agentic Helper's Enhance does that rewrite. Pictures can go past 4K×4K. New pictures can land on a new page, or on a new layer of the page you are on.

<p align="center">
  <a href="https://github.com/user-attachments/assets/04db6ab9-1a1d-48a0-a893-df98801ba018">
    <img src="https://github.com/user-attachments/assets/f0242681-75df-4bdc-a585-b8b1d67fd415" alt="valtz comparing a MiniMax-H3-Image edit of a hotpot photograph, with the marked bowl on one side" width="820">
  </a>
</p>

<p align="center">
  Circle the bowl. Ask for the change. Compare.
  <a href="https://github.com/user-attachments/assets/04db6ab9-1a1d-48a0-a893-df98801ba018">Play, with Chinese narration</a>
</p>

### Sound, beats and an agent

The audio panel is a card on the clip: level, pitch in semitones, EQ, reverb, a spatial field. A song can be split into vocal and accompaniment, or a lead vocal and the backing. Transcribe the words, lay them on the timeline as bookmarks, and other clips snap to those marks. Beats can be marked the same way.

The agent in the app drives the tools. It is experimental: turn it on in Settings → Experimental. It can build a scene shot by shot, carry a long video through its continuations, and cut a timeline from clips you already have, to the beats and between spoken lines. It can watch a clip and describe what is in it, and place words on a picture after it has looked at the picture. Its helpers run locally, GGUF weights included. A small vision model names assets and does a light rewrite of a prompt.

Draw the shot first if you want the camera to obey a plan. Stick figures on a markup layer, a plain sentence around them, and the generation follows the figures.

## Several Macs

A fleet is the Macs on one network, joined in Settings → Fleet. This Mac can send a job to another member that is free and can run it: a clip, a picture, a song, speech, a transcript, a video summary, an upscale. A job no member takes comes back to this Mac’s queue. Models can copy across, already compressed.

Two of those Macs can also do one generation together. Pair them over Thunderbolt. A clip whose model can be split runs half on each Mac, the partial sums going over the cable: TCP, or RDMA when the ports are Thunderbolt 5. The same split covers a wide set of picture models. A paired Mac can lend this one memory for the expert cache.

<p align="center">
  <a href="https://github.com/user-attachments/assets/c166d7dd-3033-4843-8032-7a9962cc75a4">
    <img src="https://github.com/user-attachments/assets/6b5c1948-4087-437e-90d2-54d153b39ff0" alt="Two valtz windows on two Macs sharing one MiniMax H3 generation over Thunderbolt, each near 22 TFLOP/s" width="820">
  </a>
</p>

<p align="center">
  Two M6 Macs, one with 16 GB and one with 24 GB, one Thunderbolt 4 cable, the job split between them.
  <a href="https://github.com/user-attachments/assets/c166d7dd-3033-4843-8032-7a9962cc75a4">Play, with Chinese narration</a>
</p>

On a published run, an M5 Ultra paired with an M5 Max over two Thunderbolt 5 cables passed 200 TFLOP/s and finished an 832×480, 124-frame Favor Fast clip in 22 seconds. That figure is from the [v0.1.0.33 release](https://github.com/tgo-app-dev/valtz/releases/tag/v0.1.0.33). The full list of what each version added is on the [releases page](https://github.com/tgo-app-dev/valtz/releases).

## What you need

| | |
| --- | --- |
| **Mac** | Apple silicon. MacBook Air, MacBook Pro, iMac, Mac mini, Mac Studio. |
| **System** | macOS 26.2 or later. |
| **Memory** | MiniMax H3 video is offered from 16 GB, streamed off disk. MiniMax-H3-Image’s own prompt expander, a separate download, is offered from 32 GB. Without it, the Agentic Helper expands the prompt. |
| **Disk** | The app is the program. Each model is its own download, from its publisher, the first time you use it. |
| **More Macs** | Optional. A fleet sends jobs to other Macs on the network. Two Macs on Thunderbolt can share one generation. RDMA wants Thunderbolt 5. |

A new generation is made at the size in the generation card. When that differs from the canvas, Settings → General → Generations decides how it lands: centred at its own size, fitted inside, or with the canvas resized to it.

<details>
<summary>Models the app knows how to run</summary>

Video is MiniMax H3: FL2VA (first and last frame), Ref2VA, RefMod, the TaoMate and Turbo adapters, and the Open-H3 regenerate refiners.

Pictures are MiniMax-H3-Image, P-H3-Image, Qwen-Image 2.1, Krea 2, FLUX.2 klein, FLUX.3 Image Klein, Z-Image and MOSS-Image; Fibo 2.0 Edit Turbo edits them.

Songs are YuE2 and MiniMax-Music3. Speech is MOSS-TTS and Fish Audio Drama 3. Listening is Qwen3-ASR. Separating a song is BS-RoFormer. Upscaling is RelayVSR and FlashVSR for clips, VOSR 2.0 and RelayVSR for pictures. Frame rate is RIFE.

The agent can use Qwen, DeepSeek, Ternary-Bonsai and LFM3-VL, including GGUF weights.

Weights are not in this repository. <b><i style="font-family:'Bookman Old Style',Bookman,serif">valtz</i></b> downloads a model when you ask for it, under that publisher’s licence.

</details>

## Questions

**What is <b><i style="font-family:'Bookman Old Style',Bookman,serif">valtz</i></b>?**
A native Mac app for local AI video with sound, still pictures, speech and songs. It is built by T-Go LLC on [Vpipe](https://github.com/tgo-app-dev/vpipe), and the source is in this repository.

**Can <b><i style="font-family:'Bookman Old Style',Bookman,serif">valtz</i></b> run MiniMax H3 on a Mac?**
Yes. MiniMax H3 is the video engine: text to video, a picture to video, a first frame and a last frame, references, and continuations. It is offered from 16 GB of memory.

**Does generation stay on the Mac?**
Yes. The render uses the Mac’s GPU. Download a model once, from its publisher, and generating with it happens on the Mac. The app can still check for its own updates.

**Does <b><i style="font-family:'Bookman Old Style',Bookman,serif">valtz</i></b> need an API key, an account, or an NVIDIA card?**
No. Rendering needs none of those. There is no account to create, generation does not call an API, and the GPU is the Apple silicon in the Mac.

A gated model is the exception on download: its publisher asks you to accept the licence on Hugging Face, then paste an access token for that download. The token is not stored. The agent can also think through a cloud provider you choose; that path is optional, and it is where a provider key is used. The local helper does not use one.

**Can several Macs share the work?**
Yes. In Settings → Fleet, this Mac can send a job to another member. Two Macs on Thunderbolt can also split one generation between them. See [Several Macs](#several-macs).

**Can I change a picture after it is generated?**
Yes. Circle a region, or hand the model reference pictures, and describe the edit. You can also grade a clip, bake that grade, and upscale, all in the same project.

**Can I keep working while a clip renders?**
Yes. Generations run as jobs. Assets accumulate while you write the next prompt, and an agent-started shot can play as it is made.

**Is <b><i style="font-family:'Bookman Old Style',Bookman,serif">valtz</i></b> free to use?**
The source is [Mozilla Public License 2.0](LICENSE). The Mac build is published on the [releases page](https://github.com/tgo-app-dev/valtz/releases). Model weights are separate downloads, each under its publisher’s licence.

**Will my clips look the same if I use the same seed?**
Turn on Skip kernel autotuning, in Settings → General → Reproducibility, when you need a seed to repeat. An export can be stamped with the prompt and the seed it was made from. The stamp holds only those two; the model and its settings are kept with the generation in the project.

**Which Macs does it run on?**
Apple silicon, macOS 26.2 or later.

**Can I use what I make commercially?**
<b><i style="font-family:'Bookman Old Style',Bookman,serif">valtz</i></b>’s own code is MPL-2.0. Each model has its publisher’s licence, and that licence is what covers the pictures and clips the model makes. Check it before you sell the output. QwenLM’s Qwen-Image prompt rewriters are non-commercial, and they are not part of this repository.

## Build

Building from source needs macOS 26.2 or later on Apple silicon, Xcode (Swift 6), CMake 3.29 or newer, Ninja (`brew install ninja`) and an installed Vpipe.

```sh
# vpipe, once (see its README): build it, then install it
cmake --install <vpipe-build> --prefix ~/dump/vpipe-install

git submodule update --init
cmake --preset dev && cmake --build --preset dev   # → ~/dump/valtz-build
~/dump/valtz-build/tests/valtz_test
open ~/dump/valtz-build/app/macos/Valtz.app
```

`VALTZ_VPIPE_PREFIX` points at a Vpipe installed somewhere else. `cmake --preset no-engine` builds the app without Vpipe, which is the interface and the store only.

The `dev` build runs in place and finds libvpipe where it was installed. A relocatable app, with libvpipe and the plugin inside the bundle, every reference rewritten, signed ad hoc, is the `valtz-dist` target:

```sh
cmake --preset release && cmake --build --preset release --target valtz-dist
# → ~/dump/valtz-build-rel/dist/Valtz.app
```

### Updates

The shipped app updates itself with [Sparkle](https://sparkle-project.org/). Sparkle is not vendored. To build that in:

```sh
app/macos/fetch-sparkle.sh            # → ~/dump/sparkle/Sparkle-<version>
cmake --preset release -DVALTZ_SPARKLE_DIR=~/dump/sparkle/Sparkle-<version>
```

A build without Sparkle runs, and simply never offers an update. If you only want the app, [download it](https://github.com/tgo-app-dev/valtz/releases/latest).

## License

<b><i style="font-family:'Bookman Old Style',Bookman,serif">valtz</i></b> is licensed under the [Mozilla Public License 2.0](LICENSE). See [`NOTICE`](NOTICE). The <b><i style="font-family:'Bookman Old Style',Bookman,serif">valtz</i></b> Extension SDK, when it is published, will be Apache License 2.0, as Vpipe is. The example extensions in `examples/extensions/` already are. How an extension is put together is in [`docs/EXTENSIONS.md`](docs/EXTENSIONS.md).

Third-party components, each under its own license, are listed in [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md): among them Vpipe, FFmpeg (in the release), Sparkle, LMDB, nlohmann/json, three.js, Babylon.js, MiniMax H3’s prompt-writing guide, MOSS-Image’s prompt enhancer and Fish Audio’s Drama 3 guide.

Model weights are not part of <b><i style="font-family:'Bookman Old Style',Bookman,serif">valtz</i></b>. Anything licensed for non-commercial use only, such as QwenLM’s Qwen-Image prompt rewriters, is not shipped here. Each model is downloaded from its publisher, when you ask for it, under the publisher’s license.

<p align="center">
  <a href="https://vpipe.ai/valtz"><strong>vpipe.ai/valtz</strong></a>
  &nbsp;·&nbsp;
  <a href="https://discord.com/invite/zG8a8BMKe">Discord</a>
  &nbsp;·&nbsp;
  <a href="https://www.reddit.com/r/vpipe">reddit.com/r/vpipe</a>
</p>

<p align="center">
  <a href="https://github.com/tgo-app-dev/valtz/releases/latest"><strong>Download for Mac</strong></a>
  &nbsp;·&nbsp;
  <a href="https://github.com/tgo-app-dev/valtz/releases">Releases</a>
  &nbsp;·&nbsp;
  <a href="https://github.com/tgo-app-dev/vpipe">Vpipe</a>
  &nbsp;·&nbsp;
  <a href="docs/EXTENSIONS.md">Extensions</a>
</p>
