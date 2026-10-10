// valtzctl -- drive the Valtz controller from a terminal.
//
//   valtzctl hw                               machine + tier
//   valtzctl caps                             capability matrix
//   valtzctl models                           catalog + install state
//   valtzctl probe <file>                     media probe
//   valtzctl new <project.valtz> [name]       create a project
//   valtzctl info <project.valtz>             assets, versions, staleness
//   valtzctl paths                            where Valtz keeps things
//   valtzctl import <project.valtz> [--copy|--link] <file>...
//                                             import (default: link video
//                                             in place, copy the rest)
//   valtzctl links <project.valtz>            re-check linked originals
//   valtzctl thumb <project.valtz> <asset-id> [px]
//   valtzctl generate <project.valtz> <prompt> [--model id] [--steps n]
//                     [--seed n] [--size WxH] [--no-preview]
//   valtzctl video <project.valtz> <prompt> [--size WxH] [--frames n]
//                  [--steps n] [--no-turbo] [--first <picture>]
//                  [--tune k=v,...]
//   valtzctl tuning <model-id> [--speed|--quality] [--edit]
//                   [--tune k=v,...]          Favor's options
//   valtzctl enhance <prompt> [--video [--frames n] | --audio
//                    [--seconds s]] [--partial]   the suggestion as it
//                    is written
//   valtzctl intent <text> [--image name]...
//   valtzctl download <model-id>
//   valtzctl ext [list] | enable <id> | disable <id>
//   valtzctl ext pack <manifest.json> <package.valtzext> | show <pkg>
//                | check <pkg>      extension packages (DESIGN §8a)

#include "valtz/base/text.h"
#include "valtz/controller/controller.h"
#include "valtz/media/camera.h"
#include "valtz/ext/extension.h"
#include "valtz/media/model-input.h"
#include "valtz/media/probe.h"

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <readpassphrase.h>
#include <sstream>
#include <set>
#include <string>
#include <vector>

using namespace valtz;
namespace fs = std::filesystem;

namespace {

int
usage()
{
  std::fputs(
      "usage: valtzctl <command> [args]\n"
      "  hw | caps | models\n"
      "  families             (Settings > Capabilities: each model family,\n"
      "         its features -- [x] made here -- and every resource it\n"
      "         runs with, where it is: valtz, vpipe or a link)\n"
      "  storage [--sort size|created|name] [--asc|--desc]\n"
      "         (Settings > Storage: what Valtz keeps on the internal\n"
      "         SSD -- models, projects, cache -- each sized, with when it\n"
      "         was made; each list in the order asked, largest first by\n"
      "         default)\n"
      "  link <model-id> <file|folder> | unlink <model-id>\n"
      "         (a model kept somewhere else -- ComfyUI's, Draw Things',\n"
      "         anywhere -- used from there)\n"
      "  assistant [<model-id>|auto] [--keep <seconds>]\n"
      "            [--drafter mtp|dflash] [--bits 8|4]\n"
      "            [--video-every <seconds>|auto]\n"
      "         (Settings > Agentic Helper: the helpers, which is chosen\n"
      "         and which runs, each one's sampler; with an id, that one\n"
      "         chosen -- kept for the app and every later run; --keep:\n"
      "         how long it stays loaded after a request, 0 not at all;\n"
      "         --drafter: what it drafts with -- its MTP head, or a\n"
      "         DFlash 2 drafter held at --bits; --video-every: a video\n"
      "         summary's seconds a frame, auto: 1, or 2 where memory\n"
      "         moves under 200 GB/s)\n"
      "  status [--thermal]   (the machine's load, as the app's status bar\n"
      "         shows it: ANE, GPU, memory; --thermal adds whether the GPU\n"
      "         is held back, from a 600 ms sample)\n"
      "  probe <file>\n"
      "  new <project.valtz> [name]\n"
      "  info <project.valtz>\n"
      "  paths\n"
      "  import <project.valtz> [--copy|--link] <file>...\n"
      "  links <project.valtz>\n"
      "  thumb <project.valtz> <asset-id> [px]\n"
      "  recipe <project.valtz> <asset-id>\n"
      "         (what made a derived asset; its timing)\n"
      "  folders <project.valtz>  (the asset list's folders)\n"
      "  folder <project.valtz> add <name> | rename <id> <name> | rm <id>\n"
      "  mv <project.valtz> <asset-id> <folder-id|top>\n"
      "  rename <project.valtz> <asset-id> <name>  (its name in the list;\n"
      "         a prompt's is then kept as its words change, \"\" names it\n"
      "         from its words again)\n"
      "  save <project.valtz> [--as <new.valtz>] | revert <project.valtz>\n"
      "         (the working copy written to its package, or read back\n"
      "         from it; every command saves as it ends, but --unsaved)\n"
      "  undo <project.valtz> | redo <project.valtz>  (its history of\n"
      "         changes, kept in its working copy between runs)\n"
      "  changes <project.valtz>  (dirty, the history, what the open\n"
      "         found: resumed changes, missing extensions)\n"
      "  rm <project.valtz> <asset-id>  (refused while it is in use)\n"
      "  modify <project.valtz> <asset-id> [name]  (an edited copy to\n"
      "         change instead: a composition of one layer showing it; a\n"
      "         composition or a markup copied)\n"
      "  capture <project.valtz> <asset-id> [--layers 1,2] [name]\n"
      "         (its layers and looks, frozen in a composition)\n"
      "  guide <project.valtz> <clip-id> <seconds> [--model id] [name]\n"
      "         (a continuation's guide: the clip's last seconds, rounded\n"
      "         up to a length the model takes -- 17n + 5 frames for H3 --\n"
      "         on a timeline of their own; --continue it)\n"
      "  grab <project.valtz> <clip-id> <frame> [name]\n"
      "         (a still of the clip's frame: a still composition showing\n"
      "         it from that mark-in)\n"
      "  place <project.valtz> <asset-id> [layer]  (into the project's\n"
      "         composition: layer 0, a new take -- or a layer; another\n"
      "         kind than the project's is not placed)\n"
      "  instantiate <project.valtz> <asset-id> [onto-asset-id] [--at L]\n"
      "         [--offset F]  (a NEW layer showing it, on onto -- else the\n"
      "         project's composition -- above L, or in L when blank; at\n"
      "         timeline frame F; refused into itself)\n"
      "  land <project.valtz> <asset-id> [onto-comp-id] [--at L]\n"
      "         [--offset F]\n"
      "         (a result landing, as the app places a generation's: a new\n"
      "         layer of onto -- the composition active at Start -- else\n"
      "         a composition of its own, the project's when it has none)\n"
      "  compose <project.valtz> still|timeline <W>x<H> [--rate N[/D]]\n"
      "         [--project] [name]  (a new composition -- 0x0: sound\n"
      "         alone; --project, or none yet: the project's)\n"
      "  composition <project.valtz> [<comp-id>]  (the project's\n"
      "         composition, or that one made it)\n"
      "  transition <project.valtz> <comp-id> <from> <to>\n"
      "         cut|dissolve|none  (where two layers overlap in time)\n"
      "  flatten <project.valtz> <asset-id> --asset [name]  (a flat asset\n"
      "         of it, as drawn: a picture, a movie, a sound)\n"
      "  history <project.valtz>  (the generation history: each edit's\n"
      "          base as the model got it, each result, oldest first)\n"
      "  auto   (what the model field's Auto picks, per modality and op)\n"
      "  ext [list]           (extension packages found at launch: each\n"
      "         one's state, what it brings, what was withheld and why)\n"
      "  ext enable <id> | disable <id>  (taken at the next launch)\n"
      "  ext pack <manifest.json> <package.valtzext>  (the manifest, as\n"
      "         authored, written into the package as manifest.cbor)\n"
      "  ext show <package.valtzext|manifest.cbor>  (the manifest as JSON)\n"
      "  ext check <package.valtzext>  (whether this Valtz takes it, and\n"
      "         the manifest as it reads it: upgraded, models withheld)\n"
      "  adjust <project.valtz> <comp-id> [k=v,...]\n"
      "         (the Adjust panel's values on a composition's layer 0 --\n"
      "         applied where it is drawn; none clears them)\n"
      "  crop <project.valtz> <comp-id> [k=v,...] [--layer L]\n"
      "         (the Crop panel: scale, offset_x, offset_y -- canvas widths\n"
      "         / heights, + right / down --, rotate -- degrees, +\n"
      "         clockwise --, pad_r, pad_g, pad_b, pad_a; on the picture's\n"
      "         own canvas -- with --layer, that layer placed on the\n"
      "         stack's frame (0 is layer 0). Applied when a model reads\n"
      "         it or it is exported; none clears it)\n"
      "  keys <project.valtz> <comp-id> adjust|crop|rotate|speed|audio\n"
      "       [--layer L] [--follow-speed on|off] [<frame>:k=v,...]...\n"
      "         (a layer's track: keyframes from its own start, linear\n"
      "         between; adjust keys as for adjust; crop keys offset_x,\n"
      "         offset_y, scale_x, scale_y (pad_* in any is its\n"
      "         background); rotate keys rotate=degrees; speed keys\n"
      "         rate=1.5; audio keys volume=0..4,pitch=semitones. None\n"
      "         clears the track; layer 0 without --layer.\n"
      "         audio --follow-speed on|off: its pitch follows its\n"
      "         speed, as a tape's, or is held -- with keys or alone)\n"
      "  trim <project.valtz> <comp-id> [--layer L] [<in> <out>|none]\n"
      "       [--offset F] [--duration F]\n"
      "         (a layer's marks in its source -- frames, a sound's\n"
      "         milliseconds; in a still, the frame shown -- where it\n"
      "         starts on the timeline, how long it runs; 0: its span)\n"
      "  canvas <project.valtz> <asset-id> <W>x<H>|own [--anchor A]\n"
      "          (Canvas Size: the stack on a canvas W x H, anchored at\n"
      "          tl t tr l c r bl b br -- c by default; own: its own)\n"
      "  timeline <project.valtz> <comp-id> <frames>  (0: its layers')\n"
      "  generate <project.valtz> <prompt> [--model id] [--steps n]\n"
      "           [--seed n] [--size WxH] [--no-preview]\n"
      "           [--speed|--quality] [--tune k=v,...]\n"
      "           [--base <file|asset-id>] [--ref <file|asset-id>]...\n"
      "           [--mode auto|edit|generate] [--adjust k=v,...]\n"
      "           (files are imported first; --base is the picture to\n"
      "           edit, resampled to the output; --ref pictures are\n"
      "           drawn from as they are -- with only refs, the model\n"
      "           composes a new picture from them; --adjust lays the\n"
      "           Adjust panel on the base first: exposure (stops),\n"
      "           contrast, highlights, shadows, vibrance, saturation,\n"
      "           temperature, tint (-1..1), on a RAW's development)\n"
      "  video <project.valtz> <prompt> [--model id] [--size WxH]\n"
      "        [--frames n | --seconds s] [--fps n] [--steps n]\n"
      "        [--speed|--quality] [--seed n] [--no-turbo]\n"
      "        [--first <file|asset-id>] [--tune k=v,...]\n"
      "        [--ref <file|asset-id>]... [--continue <clip>]\n"
      "        [--tail-seconds s] [--song-sound]\n"
      "        (a clip with its soundtrack, MiniMax H3; the length goes\n"
      "        up to one the model makes -- 17n+5 frames -- and the Turbo\n"
      "        LoRA, when installed, runs 4/6/8 steps; --first is a picture\n"
      "        the clip opens on, cropped to fill the frame. --ref: a\n"
      "        picture, clip or sound to draw on (Ref2VA: a subject, a\n"
      "        motion, a voice, a song; name them <Picture 1>, <Video 1>,\n"
      "        <Audio 1> in the prompt, numbered as the job lists them);\n"
      "        --continue: a clip carried on from its tail (3.75 s, or\n"
      "        --tail-seconds); --song-sound: the first sound reference is\n"
      "        the clip's soundtrack, as it is -- a music video)\n"
      "  audio <project.valtz> <prompt> [--lyrics <file>] [--model id]\n"
      "        [--plan full|melody|off] [--score <file.abc>]\n"
      "        [--max-seconds s] [--steps n] [--speed|--quality]\n"
      "        [--seed n] [--tune k=v,...] [--vae <folder>]\n"
      "        (a song, YuE2: the prompt is its style -- genre, mood,\n"
      "        instruments, voice -- then its lyrics under [Verse] /\n"
      "        [Chorus] headers; none, and the description makes the\n"
      "        music. --lyrics reads them from a file instead. The model\n"
      "        plans a score first (--plan; --score gives one to follow:\n"
      "        a cover) and decides the length, --max-seconds at most;\n"
      "        --vae decodes with another (YuE2-Vae-legacy). A WAV)\n"
      "  audio <project.valtz> <prompt> --model moss-tts-v1.5[-w8g64]\n"
      "        [--voice <file|asset>] [--seconds s] [--seed n]\n"
      "        (SPEECH, MOSS-TTS: the prompt is its fields -- lines\n"
      "        \"Instruction: ...\", \"Quality: ...\", \"Sound event: ...\",\n"
      "        \"Ambient sound: ...\", \"Language: ...\" -- then a blank\n"
      "        line and the words, \"[pause 1.5s]\" a pause. --voice: a\n"
      "        sound (or a clip) whose voice it speaks in, cloned from up\n"
      "        to 12 s of it -- with one, Auto picks a speech model.\n"
      "        --seconds: about how long. A 24 kHz mono WAV)\n"
      "  record --sources | record <project.valtz> [<sound-comp-id>]\n"
      "        [--source id] [--seconds s]  (a microphone -- the default\n"
      "        one without --source -- or the system's audio, \"system\",\n"
      "        recorded into a composition of sound alone: a flat sound,\n"
      "        on its blank layer or a new one; with none, a flat sound\n"
      "        alone)\n"
      "  output <project.valtz> [color=rec709|srgb|display-p3|rec2020|\n"
      "        rec2100-pq|rec2100-hlg] [fps=24|30000/1001] [channels=1|2]\n"
      "        [rate=48000]  (the project's output: what exporting it\n"
      "        writes; fps is its timeline's, set while that is empty)\n"
      "  camera --sources | camera <project.valtz> [--seconds s]\n"
      "        [--camera id]  (a still -- or, with --seconds, a clip with\n"
      "        the microphone's sound -- taken with a camera: a flat asset)\n"
      "  quantize <model-id>  (a quantized variant made from its source,\n"
      "        as Settings > Capabilities' Quantize: moss-tts-v1.5-w8g64\n"
      "        from moss-tts-v1.5, w8 group-64, into the models folder)\n"
      "  song <project.valtz> <text>  (the prompt as generate_audio reads\n"
      "        it: its style and lyrics)\n"
      "  outline <model> [--row image,video,audio] [--lang zh-Hans]\n"
      "        (the model's prompt template -- what an empty prompt box\n"
      "        starts from -- written for a reference row holding those\n"
      "        kinds, in order: each named by its tag where it belongs)\n"
      "  prompts <project.valtz>  (the prompts: each made from, how much)\n"
      "  prompt <project.valtz> add <text> [--name N] | show <id>\n"
      "         | set <id> <text>\n"
      "         (a prompt as an asset: its media named by POSITION in\n"
      "         the reference row -- <valtz_ref_img_0> the first picture,\n"
      "         _vid_ a clip, _aud_ a sound -- and each model told its\n"
      "         own names for them; set is refused once something was\n"
      "         made from it. @<id> in place of any prompt uses it;\n"
      "         --prompt-name N on generate, video and audio names the\n"
      "         prompt captured, as --name here: the person's name, kept\n"
      "         as its words change)\n"
      "  classify <file|folder>...  (lora, dit or vae: how a drop is filed)\n"
      "  transcribe <project.valtz> <sound|clip-id> [--model ID]\n"
      "         [--language L]  (its speech line by line, the sound\n"
      "         events heard: a text asset made from it, printed)\n"
      "  summarize <project.valtz> <clip-id> [--model ID] [--every S]\n"
      "         (watched by the helper -- a frame every S seconds, the\n"
      "         setting's by default -- scene by scene, then whole: a\n"
      "         text asset made from it, printed)\n"
      "  layers <project.valtz> <picture-id> [add [above] | move <layer> <by>\n"
      "         | show|hide <layer> | source <layer> <picture-id>\n"
      "         | rename <layer> <name> | remove <layer>\n"
      "         | markup [selected...] | merge <layer> <layer>\n"
      "         | mask <layer> on|off\n"
      "         | stroke <layer> <x,y;x,y;...>\n"
      "                 [r=8,soft=0.5,color=#rrggbbaa,erase]\n"
      "         | shape <layer> line|rect|ellipse <x0,y0,x1,y1>\n"
      "                 [stroke=#..,fill=#..,width=4]\n"
      "         | text <layer> <x,y|x0,y0,x1,y1> <text> [font=Name,size=48,\n"
      "                 bold,italic,underline,color=#..]\n"
      "                 (a box: wrapped in it, the lines that fit whole)\n"
      "         | materialize <layer> [object-id...]\n"
      "         | duplicate <layer> [name]  (a copy right above it: a\n"
      "                 markup's drawing and objects its own)\n"
      "         | drawing <layer>  (the file its painted pixels are in)\n"
      "         | paste-drawing <layer> <png> [dx dy] | clear-drawing <layer>\n"
      "         | split <layer> <frame> | slide <layer> <frame>\n"
      "         | stretch <layer> <frames>\n"
      "         | group <layer>... [--name N] | ungroup <folder>\n"
      "         | place <layer> <above-layer|bottom> [folder]\n"
      "         | pages <layer> <first> [count]] [--page P]\n"
      "         (a picture's layer stack, top first; \"\" is the bottom one.\n"
      "         markup: the layer the toolbar draws on, made when needed.\n"
      "         On a still with pages: --page P (from 1) puts a new layer\n"
      "         -- add, markup -- on that page alone; pages: the layer on\n"
      "         <count> pages from <first>, no count to the last. On a\n"
      "         timeline: markup --frame F, drawn at frame F (a new layer\n"
      "         from there, a second long, as any still put on one).\n"
      "         split: a raw clip or sound cut at a timeline frame in two\n"
      "         parts, each a composition, on the layer and one above it;\n"
      "         slide: where the layer starts, a timeline's length grown;\n"
      "         stretch: a still's -- picture, markup -- length, frames)\n"
      "  pages <project.valtz> <still-id> [add [--after P] [--count N]\n"
      "         | remove <P>]  (a still's pages, from 1: drawn a page at a\n"
      "         time; a new page continues what runs across it, its keys\n"
      "         -- keys <still> crop 0:... count a layer's pages from its\n"
      "         first, as frames -- where their pages went; export writes\n"
      "         one file a page, name-1.png, name-2.png ...)\n"
      "  flatten <project.valtz> <picture-id> -o <file.png> [--page P]\n"
      "  tuning <model-id> [--speed|--quality] [--edit] [--tune k=v,...]\n"
      "         [--turbo none|<id>|<file>] [--lora <file>[:scale]]...\n"
      "         [--dit <file|folder>] [--vae <file|folder>]\n"
      "         (Favor's options for the model -- their values at the\n"
      "         preference, with --tune's laid on as Custom does, their\n"
      "         ranges and what this Mac can have. --tune on generate and\n"
      "         video sets them: steps, hyperflow, vdn, sol_attn, sol_tau,\n"
      "         sage_attn, i8_gemm, motion_cache, video_shift, audio_shift,\n"
      "         shift -- true/false or a number; --turbo the catalog's\n"
      "         few-step LoRA off (none) or on, or a file in its place;\n"
      "         --lora adds one -- two run, the first on)\n"
      "  export <project.valtz> <asset-id|file> --format F -o <file>\n"
      "         [--quality 1..100]\n"
      "         [--bitrate 20M] [--max-bitrate 30M] [--video-quality 0..1]\n"
      "         [--keyframes <seconds>] [--b-frames on|off]\n"
      "         [--profile baseline|main|high|main10] [--level 4.1]\n"
      "         [--entropy cabac|cavlc] [--prores 4444xq|422|422lt|...]\n"
      "          (a file is imported first -- a video linked in place;\n"
      "          --quality is a JPEG's, 90 by default; the rest a movie's\n"
      "          encoding: H.264's / HEVC's rate, keyframes, B-frames,\n"
      "          profile, level, entropy; ProRes's flavour)\n"
      "  export --formats\n"
      "  enhance <prompt> [--video|--audio] [--model id] [--mode m]\n"
      "          [--size WxH] [--seconds s] [--frames n] [--partial]\n"
      "          [--no-mtp] [--assistant <id>] [--repeat <n>]\n"
      "          [--project <project.valtz> [--base <file|asset-id>]\n"
      "           [--ref <file|asset-id>]... [--first <file|asset-id>]\n"
      "           [--continue <clip>] [--song-sound]]\n"
      "          (--no-mtp: the assistant decodes without its MTP\n"
      "          head, to compare; the same reply, slower.\n"
      "          --assistant: that catalog assistant, not the one\n"
      "          chosen, for this run (valtzctl assistant to choose).\n"
      "          --model: the model the prompt is for -- one whose\n"
      "          makers publish a rewriter (Qwen-Image 2.1) gets theirs,\n"
      "          shown an edit's pictures; name them <image1>.. in the\n"
      "          prompt, the base first. --audio: a song, written by\n"
      "          YuE2's song writer (Auto's model) to --seconds.\n"
      "          --video: a clip written to MiniMax H3's own guide --\n"
      "          from words, opening on --first, or from references\n"
      "          (Auto's Ref2VA) -- --frames long)\n"
      "  upscale <project> <composition> [--layer L] [--model id]\n"
      "          [--seed n]   the layer's clip rendered at its scale\n"
      "          (FlashVSR), the layer then showing it at scale 1\n"
      "  intent <text> [--image name]...\n"
      "  fleet [status] [--json] | fleet browse [--seconds N]\n"
      "        | fleet create|join <name> [--discoverable]\n"
      "        | fleet set [name=N] [discoverable=on|off]\n"
      "            [accept=always|ask|never]\n"
      "            [schedule=mon-fri@09:00-18:00|off]\n"
      "        | fleet leave | fleet connect <host:port>\n"
      "        | fleet serve [--seconds N]\n"
      "         (the FLEET: Valtz on the Macs of one network as one --\n"
      "         a fleet's name and secret (asked for unechoed, or\n"
      "         $VALTZ_FLEET_SECRET), discoverable or not, taking jobs\n"
      "         or not; serve: a member at work until stopped. --fleet\n"
      "         on any command: a job this Mac cannot run, or not now,\n"
      "         goes to a member that can. $VALTZ_FLEET_CONFIG: another\n"
      "         member's configuration -- two on one Mac)\n"
      "  download <model-id> [--hf-token]\n"
      "          (--hf-token: asks for a Hugging Face access token, not\n"
      "          echoed, for a GATED model -- used for this download only,\n"
      "          kept nowhere; without it, $HF_TOKEN, or vpipe asks when\n"
      "          Hugging Face refuses)\n",
      stderr);
  return 2;
}

int
fail(const Error& e)
{
  std::fprintf(stderr, "valtzctl: %s: %s\n", to_str(e.code),
               e.message.c_str());
  return 1;
}

// --tune's "sol_attn=true,steps=6,sol_tau=1.2": Favor's Custom values.
Json
tune_spec(const std::string& spec)
{
  Json out = Json::object();
  std::size_t at = 0;
  while (at < spec.size()) {
    std::size_t end = spec.find(',', at);
    if (end == std::string::npos) {
      end = spec.size();
    }
    const std::string kv = spec.substr(at, end - at);
    at = end + 1;
    const auto eq = kv.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
    if (v == "true" || v == "on" || v == "yes") {
      out[k] = true;
    } else if (v == "false" || v == "off" || v == "no") {
      out[k] = false;
    } else if (v.find('.') != std::string::npos) {
      out[k] = std::atof(v.c_str());
    } else {
      out[k] = std::atoi(v.c_str());
    }
  }
  return out;
}

// "#rrggbb" or "#rrggbbaa" as [r, g, b, a], 0..1; null when it is not.
Json
rgba_arg(const std::string& v)
{
  if (v.size() != 7 && v.size() != 9) {
    return Json();
  }
  if (v[0] != '#') {
    return Json();
  }
  Json c = Json::array();
  for (std::size_t i = 1; i < v.size(); i += 2) {
    c.push_back(std::strtol(v.substr(i, 2).c_str(), nullptr, 16) / 255.0);
  }
  if (c.size() == 3) {
    c.push_back(1.0);
  }
  return c;
}

// "k=v,flag,..." as an object: flags true, numbers as numbers.
Json
options_arg(const std::string& v)
{
  Json out = Json::object();
  std::size_t start = 0;
  while (start <= v.size()) {
    const auto end = v.find(',', start);
    const std::string part = v.substr(start, end - start);
    if (const auto eq = part.find('='); eq != std::string::npos) {
      const std::string k = part.substr(0, eq), val = part.substr(eq + 1);
      char* rest = nullptr;
      const double d = std::strtod(val.c_str(), &rest);
      out[k] = rest && *rest == 0 && !val.empty() ? Json(d) : Json(val);
    } else if (!part.empty()) {
      out[part] = true;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return out;
}

// "a,b;c,d;..." as numbers.
std::vector<double>
numbers_arg(const std::string& v)
{
  std::vector<double> out;
  std::string n;
  for (char ch : v + ";") {
    if (ch == ',' || ch == ';') {
      if (!n.empty()) {
        out.push_back(std::atof(n.c_str()));
      }
      n.clear();
    } else {
      n += ch;
    }
  }
  return out;
}

// --turbo none|<catalog id>|<file> and --lora <file>[:scale]: the LoRA
// list, as Custom's panel sets it. --turbo turns the catalog's Turbo LoRA
// off (none) or on, or puts a file first, on; each --lora is added, on,
// after the Turbo LoRA -- two on at most, the first ones (vpipe's slots).
bool
lora_arg(Json& tuning, const std::string& flag, const std::string& v)
{
  if (!tuning.is_object()) {
    tuning = Json::object();
  }
  if (flag == "--turbo") {
    tuning["turbo"] = v == "none" ? Json(false) : Json(v);
    return true;
  }
  // --dit / --vae <file|folder>: a community checkpoint, on, in place of
  // the model's own part.
  if (flag == "--dit" || flag == "--vae") {
    const char* list = flag == "--dit" ? "dits" : "vaes";
    if (!tuning.contains(list)) {
      tuning[list] = Json::array();
    }
    tuning[list].push_back({{"path", v}, {"on", true}});
    return true;
  }
  if (flag != "--lora") {
    return false;
  }
  // path[:scale] -- a path may hold no ':' of its own here. (An older
  // path:kind:scale still reads: the last number is the strength.)
  std::string path = v;
  double scale = 1.0;
  if (const auto c = v.find(':'); c != std::string::npos) {
    path = v.substr(0, c);
    const auto last = v.rfind(':');
    scale = std::atof(v.substr(last + 1).c_str());
    if (scale <= 0) {
      scale = 1.0;
    }
  }
  // A list said replaces the preset's, so the Turbo LoRA comes back in by
  // "turbo" (unless --turbo says otherwise).
  if (!tuning.contains("loras")) {
    tuning["loras"] = Json::array();
  }
  if (!tuning.contains("turbo")) {
    tuning["turbo"] = true;
  }
  tuning["loras"].push_back({{"path", path}, {"scale", scale}, {"on", true}});
  return true;
}

// Drain events until `job` reaches a terminal event. Previews are
// summarized, streamed text is echoed, and progress -- ten counts a
// second -- is a line per phase and per tenth of it.
// A generation refused for memory, as a person reads it: what each
// budget needed and had, what vpipe asked for part by part and phase by
// phase, and what to change.
void
print_out_of_memory(const Json& d)
{
  auto gb = [](std::uint64_t b) {
    return b >= (1ull << 30)
               ? std::format("{:.1f} GB", static_cast<double>(b) / (1 << 30))
               : std::format("{} MB", b >> 20);
  };
  const Json m = jget(d, "memory", Json::object());
  const Json r = jget(m, "refusal", Json::object());
  std::fprintf(stderr, "%s\n",
               jget<std::string>(d, "message", "").c_str());
  std::fprintf(stderr, "  the %s needs ~%s:\n",
               jget<std::string>(r, "step", "generation").c_str(),
               gb(jget<std::uint64_t>(r, "need", 0)).c_str());
  for (const auto& p : jget(r, "parts", Json::array())) {
    std::fprintf(stderr, "    %-18s %9s\n",
                 jget<std::string>(p, "name", "").c_str(),
                 gb(jget<std::uint64_t>(p, "bytes", 0)).c_str());
  }
  for (const auto& g : jget(r, "gates", Json::array())) {
    std::fprintf(stderr, "  %s: ~%s wanted, ~%s available%s\n",
                 jget<std::string>(g, "name", "").c_str(),
                 gb(jget<std::uint64_t>(g, "need", 0)).c_str(),
                 gb(jget<std::uint64_t>(g, "have", 0)).c_str(),
                 jget(g, "ok", true) ? "" : "  <- short");
  }
  if (const Json plan = jget(m, "plan", Json()); plan.is_object()) {
    std::string phases;
    for (const auto& p : jget(plan, "phases", Json::array())) {
      phases += std::format("{}{} {}", phases.empty() ? "" : ", ",
                            jget<std::string>(p, "name", ""),
                            gb(jget<std::uint64_t>(p, "bytes", 0)));
    }
    std::fprintf(stderr, "  the plan: peak ~%s in '%s' (%s) of %s RAM\n",
                 gb(jget<std::uint64_t>(plan, "peak", 0)).c_str(),
                 jget<std::string>(plan, "phase", "").c_str(),
                 phases.c_str(),
                 gb(jget<std::uint64_t>(plan, "ram", 0)).c_str());
  }
  const Json s = jget(d, "suggest", Json::object());
  std::string tries;
  if (const Json l = jget(s, "length", Json()); l.is_object()) {
    tries += std::format("  a shorter clip: --frames under {} ({:.1f} s)\n",
                         jget(l, "frames", 0), jget(l, "seconds", 0.0));
  }
  if (const Json z = jget(s, "resolution", Json()); z.is_object()) {
    tries += std::format("  a smaller size: --size under {}x{}\n",
                         jget(z, "width", 0), jget(z, "height", 0));
  }
  if (const Json f = jget(s, "references", Json()); f.is_object()) {
    tries += std::format("  fewer references: {} now\n",
                         jget(f, "count", 0));
  }
  if (!tries.empty()) {
    std::fprintf(stderr, "try:\n%s", tries.c_str());
  }
}

int
follow(Controller& c, JobId job)
{
  int rc = 0;
  std::string phase;
  int tenth = -1;
  // An export's previews come twice a second: one line in five seconds.
  auto previewed = std::chrono::steady_clock::time_point{};
  for (;;) {
    Event ev;
    if (!c.events().wait(ev, 1000)) {
      continue;
    }
    if (ev.kind == "log") {
      continue;  // already printed by the log sink
    }
    if (ev.job != job) {
      continue;
    }
    if (ev.kind == "job.text") {
      std::cout << jget<std::string>(ev.data, "text", "") << std::flush;
      continue;
    }
    if (ev.kind == "job.progress" && ev.data.contains("phase")) {
      const auto now = jget<std::string>(ev.data, "phase", "");
      const auto done = jget<std::uint64_t>(ev.data, "done", 0);
      const auto total = jget<std::uint64_t>(ev.data, "total", 0);
      // A tenth of a count; an uncounted phase's own measure -- a song's
      // score in 200 tokens, the song in 15 s -- by its steps.
      const double made = jget(ev.data, "made", -1.0);
      const int t = total > 0 ? static_cast<int>(done * 10 / total)
                    : made >= 0
                        ? static_cast<int>(made / (now == "score" ? 200 : 15))
                        : -1;
      if (now == phase && t == tenth) {
        continue;
      }
      const auto label = jget<std::string>(ev.data, "label", "");
      std::string line = "  " + now + (label.empty() ? "" : " " + label);
      if (total > 0) {
        line += std::format(" {}% ({}/{})", done * 100 / total, done,
                            total);
      }
      const auto detail = jget<std::string>(ev.data, "detail", "");
      if (!detail.empty()) {
        line += " " + detail;
      }
      if (made >= 0) {
        line += std::format(" ({:.0f} {})", made,
                            now == "score" ? "tokens" : "s");
      }
      // The estimate, once there is one (controller/job-timing.h).
      if (const double left = jget(ev.data, "left", -1.0); left >= 0) {
        line += std::format(" ~{:.0f} s left", left);
      }
      std::fprintf(stderr, "%s\n", line.c_str());
      phase = now;
      tenth = t;
      continue;
    }
    if (ev.kind == "job.preview") {
      if (ev.data.contains("frame")) {
        const auto now = std::chrono::steady_clock::now();
        if (now - previewed < std::chrono::seconds(5)) {
          continue;
        }
        previewed = now;
      }
      std::string shape;
      if (ev.tensor) {
        for (auto d : ev.tensor->shape) {
          shape += (shape.empty() ? "" : "x") + std::to_string(d);
        }
      }
      if (ev.data.contains("frame")) {
        std::fprintf(stderr, "  preview frame %lld (%s)\n",
                     static_cast<long long>(jget<std::int64_t>(
                         ev.data, "frame", 0)),
                     shape.c_str());
        continue;
      }
      std::fprintf(stderr, "  preview step %d/%d (%s)\n",
                   jget(ev.data, "step", 0), jget(ev.data, "steps", 0),
                   shape.c_str());
      continue;
    }
    std::cout << ev.to_json() << "\n";
    if (ev.kind == "job.failed") {
      rc = 1;
      if (ev.data.contains("memory")) {
        print_out_of_memory(ev.data);
      }
    }
    if (ev.kind == "job.finished" || ev.kind == "job.failed" ||
        ev.kind == "job.cancelled") {
      // An intent job may be answered by the heuristic alone.
      return rc;
    }
  }
}

// The projects a run opened that were clean then: what it changes in
// them is saved as it ends. One left with unsaved changes (--unsaved
// before) is saved by `save` alone.
std::set<std::string> g_clean_at_open;

Result<ProjectId>
open_or_fail(Controller& c, const fs::path& p)
{
  auto id = c.open_project(p);
  if (id.ok() && !jget(c.project_state(*id), "dirty", true)) {
    g_clean_at_open.insert(id->str());
  }
  return id;
}

// A --ref: an asset id as is, or a file imported (copied) into the
// project first -- the asset that import creates.
Result<AssetId>
reference_asset(Controller& c, ProjectId pid, const std::string& ref,
                project::Placement placement = project::Placement::Copy)
{
  if (auto id = AssetId::parse(ref)) {
    return *id;
  }
  project::ImportOptions opts;
  opts.placement = placement;
  VALTZ_ASSIGN(JobId job, c.import_files(pid, {fs::path(ref)}, opts));
  AssetId asset;
  for (;;) {
    Event ev;
    if (!c.events().wait(ev, 1000) || ev.job != job) {
      continue;
    }
    if (ev.kind == "assets.changed") {
      if (auto id = AssetId::parse(jget<std::string>(ev.data, "asset",
                                                     ""))) {
        asset = *id;
      }
    } else if (ev.kind == "job.finished" || ev.kind == "job.failed") {
      break;
    }
  }
  if (asset.is_nil()) {
    return make_error(Code::Io, std::format("could not import {}", ref));
  }
  return asset;
}

}

namespace {

// "@<prompt-id>" in place of a prompt: that prompt's words, and the
// prompt reused (the result made from it). Anything else is the words.
Status
prompt_arg(Controller& c, ProjectId pid, const std::string& arg,
           std::string* text, std::optional<AssetId>* asset)
{
  *text = arg;
  if (!arg.starts_with("@")) {
    return ok_status();
  }
  auto id = AssetId::parse(arg.substr(1));
  project::Project* p = c.project(pid);
  if (!id || !p) {
    return make_error(Code::InvalidArgument, "not a prompt id: " + arg);
  }
  VALTZ_ASSIGN(*text, p->read_text(*id));
  *asset = *id;
  return ok_status();
}

// A layer as typed: "0" is layer 0, whose id is "" ("bg" too, as it was
// once called).
std::string
layer_id(const std::string& typed)
{
  return typed == "0" || typed == "bg" ? std::string() : typed;
}

}

// "mon-fri@09:00-18:00", "sat,sun@22:00-07:00", "every@...", "off": a
// fleet schedule (fleet::Schedule).
std::optional<Json>
parse_schedule(const std::string& spec)
{
  if (spec == "off" || spec == "none") {
    return Json{{"on", false}};
  }
  const auto at = spec.find('@');
  if (at == std::string::npos) {
    return std::nullopt;
  }
  static const std::array<std::string_view, 7> names = {
      "mon", "tue", "wed", "thu", "fri", "sat", "sun"};
  auto day = [&](std::string_view d) -> int {
    for (int i = 0; i < 7; ++i) {
      if (names[i] == d) {
        return i;
      }
    }
    return -1;
  };
  int days = 0;
  const std::string ds = spec.substr(0, at);
  if (ds == "every" || ds == "all") {
    days = 0x7f;
  } else {
    std::stringstream in(ds);
    std::string part;
    while (std::getline(in, part, ',')) {
      const auto dash = part.find('-');
      const int a = day(part.substr(0, dash));
      const int b = dash == std::string::npos ? a
                                              : day(part.substr(dash + 1));
      if (a < 0 || b < 0) {
        return std::nullopt;
      }
      for (int k = a;; k = (k + 1) % 7) {
        days |= 1 << k;
        if (k == b) {
          break;
        }
      }
    }
  }
  int h0 = 0, m0 = 0, h1 = 0, m1 = 0;
  if (std::sscanf(spec.c_str() + at + 1, "%d:%d-%d:%d", &h0, &m0, &h1,
                  &m1) != 4) {
    return std::nullopt;
  }
  return Json{{"on", true}, {"days", days}, {"from", h0 * 60 + m0},
              {"to", h1 * 60 + m1}};
}

// The fleet as a person reads it.
void
print_fleet(const Json& s)
{
  const Json c = jget(s, "config", Json::object());
  const Json sch = jget(c, "schedule", Json::object());
  std::printf("this Mac  %s  (%s)\n",
              jget<std::string>(c, "member_name", "").c_str(),
              jget<std::string>(c, "member_id", "").c_str());
  const auto fleet = jget<std::string>(c, "fleet", "");
  std::printf("fleet     %s%s\n", fleet.empty() ? "(none)" : fleet.c_str(),
              jget(c, "discoverable", false) ? "  discoverable" : "");
  std::string when;
  if (jget(sch, "on", false)) {
    const int from = jget(sch, "from", 0), to = jget(sch, "to", 0);
    when = std::format(", days {:#04x} {:02}:{:02}-{:02}:{:02}",
                       jget(sch, "days", 0), from / 60, from % 60, to / 60,
                       to % 60);
  }
  std::printf("jobs      %s%s%s\n",
              jget<std::string>(c, "accept", "").c_str(), when.c_str(),
              jget(c, "accepting", false) ? "  (taking jobs now)"
                                          : "  (not now)");
  if (const int port = jget(s, "port", 0); port > 0) {
    std::printf("listening on port %d\n", port);
  }
  for (const auto& m : jget(s, "members", Json::array())) {
    const Json self = jget(m, "self", Json::object());
    const Json mach = jget(self, "machine", Json::object());
    std::string what;
    for (const auto& f : jget(self, "features", Json::array())) {
      what += (what.empty() ? "" : ",") + f.get<std::string>();
    }
    const std::string state =
        jget(self, "serving", false) ? "serving"
        : jget(self, "busy", false)  ? "busy"
        : jget(self, "accepting", false) ? "idle"
                                         : "not-taking-jobs";
    std::printf("  member  %-24s %-10s %-15s %s %d GB  %zu models  %s\n",
                jget<std::string>(m, "name", "").c_str(),
                jget<std::string>(m, "state", "").c_str(),
                self.empty() ? "" : state.c_str(),
                jget<std::string>(mach, "chip", "").c_str(),
                jget(mach, "ram_gb", 0),
                jget(self, "installed", Json::array()).size(), what.c_str());
  }
  for (const auto& f : jget(s, "fleets", Json::array())) {
    std::printf("  on the network: fleet \"%s\", %d discoverable member(s)\n",
                jget<std::string>(f, "fleet", "").c_str(),
                jget(f, "members", 0));
  }
  if (const Json sv = jget(s, "serving", Json()); sv.is_object()) {
    std::printf("serving   %s for %s\n",
                jget<std::string>(sv, "title", "").c_str(),
                jget<std::string>(sv, "from", "").c_str());
  }
}

int
fleet_command(Controller& c, std::vector<std::string>& args)
{
  const std::string sub = args.empty() ? "status" : args[0];
  const bool as_json = std::erase(args, std::string("--json")) > 0;
  auto seconds = [&](int fallback) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
      if (args[i] == "--seconds") {
        return std::atoi(args[i + 1].c_str());
      }
    }
    return fallback;
  };
  auto show = [&] {
    const Json s = c.fleet_status();
    if (as_json) {
      std::cout << to_text(s, 2) << "\n";
    } else {
      print_fleet(s);
    }
  };
  auto configure = [&](const Json& j) -> int {
    if (auto st = c.fleet_configure(j); !st.ok()) {
      return fail(st.error());
    }
    // The network made again, members reached.
    std::this_thread::sleep_for(std::chrono::seconds(2));
    show();
    return 0;
  };
  if (sub == "status") {
    std::this_thread::sleep_for(std::chrono::seconds(seconds(3)));
    show();
    return 0;
  }
  if (sub == "browse") {
    c.fleet_browse(true);
    std::this_thread::sleep_for(std::chrono::seconds(seconds(3)));
    show();
    return 0;
  }
  if (sub == "set") {
    // name=, discoverable=on|off, accept=always|ask|never,
    // schedule=mon-fri@09:00-18:00|off
    Json j = Json::object();
    for (std::size_t i = 1; i < args.size(); ++i) {
      const auto eq = args[i].find('=');
      if (eq == std::string::npos) {
        return usage();
      }
      const std::string k = args[i].substr(0, eq);
      const std::string v = args[i].substr(eq + 1);
      if (k == "name") {
        j["member_name"] = v;
      } else if (k == "discoverable") {
        j["discoverable"] = v == "on" || v == "true" || v == "1";
      } else if (k == "accept") {
        j["accept"] = v;
      } else if (k == "schedule") {
        auto sch = parse_schedule(v);
        if (!sch) {
          return fail(make_error(Code::InvalidArgument,
                                 "schedule: mon-fri@09:00-18:00, or off"));
        }
        j["schedule"] = *sch;
      } else {
        return usage();
      }
    }
    return configure(j);
  }
  if (sub == "join" || sub == "create") {
    // The secret asked for at the terminal without echo, or
    // $VALTZ_FLEET_SECRET (scripts): never an argument, which the shell's
    // history would keep.
    if (args.size() < 2) {
      return usage();
    }
    std::string secret;
    if (const char* e = std::getenv("VALTZ_FLEET_SECRET"); e && *e) {
      secret = e;
    } else {
      char buf[256];
      if (!readpassphrase("Fleet secret: ", buf, sizeof buf,
                          RPP_REQUIRE_TTY)) {
        return fail(make_error(Code::InvalidArgument,
                               "no terminal to ask for the secret on"));
      }
      secret = buf;
      std::fill(std::begin(buf), std::end(buf), '\0');
    }
    Json j = {{"fleet", args[1]}, {"secret", secret}};
    std::fill(secret.begin(), secret.end(), '\0');
    if (std::ranges::find(args, "--discoverable") != args.end()) {
      j["discoverable"] = true;
    }
    return configure(j);
  }
  if (sub == "leave") {
    return configure({{"fleet", ""}});
  }
  if (sub == "connect") {
    // host:port -- a member reached without Bonjour.
    if (args.size() < 2 || args[1].rfind(':') == std::string::npos) {
      return usage();
    }
    const auto colon = args[1].rfind(':');
    c.fleet_connect(args[1].substr(0, colon),
                    std::atoi(args[1].c_str() + colon + 1));
    std::this_thread::sleep_for(std::chrono::seconds(seconds(3)));
    show();
    return 0;
  }
  if (sub == "serve") {
    // A member at work: its fleet's jobs taken and run, until stopped
    // (or --seconds N); what happens printed.
    const int limit = seconds(0);
    const auto end = std::chrono::steady_clock::now() +
                     std::chrono::seconds(limit > 0 ? limit : 1 << 30);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    show();
    std::string last;
    while (std::chrono::steady_clock::now() < end) {
      Event ev;
      if (!c.events().wait(ev, 500)) {
        continue;
      }
      if (ev.kind == "fleet.serving") {
        std::printf("serving: %s for %s -- %s%s\n",
                    jget<std::string>(ev.data, "title", "").c_str(),
                    jget<std::string>(ev.data, "from", "").c_str(),
                    jget<std::string>(ev.data, "state", "").c_str(),
                    jget<std::string>(ev.data, "message", "").empty()
                        ? ""
                        : (": " + jget<std::string>(ev.data, "message", ""))
                              .c_str());
      } else if (ev.kind == "fleet.progress") {
        const Json ph = jget(ev.data, "phase", Json::object());
        const auto line = std::format(
            "  {} {:.0f}%", jget<std::string>(ph, "phase", ""),
            100.0 * std::max(0.0f, jget(ev.data, "progress", 0.0f)));
        if (line != last) {
          std::puts(line.c_str());
          last = line;
        }
      } else if (ev.kind == "fleet.ask") {
        std::printf("asked: %s from %s (valtzctl answers yes)\n",
                    jget<std::string>(ev.data, "title", "").c_str(),
                    jget<std::string>(ev.data, "from", "").c_str());
        if (auto id = JobId::parse(jget<std::string>(ev.data, "job", ""));
            id && !jget(ev.data, "done", false)) {
          c.fleet_answer(*id, true);
        }
      } else if (ev.kind == "fleet.changed") {
        std::puts("fleet: members changed");
      }
      std::fflush(stdout);
    }
    return 0;
  }
  return usage();
}

int
main(int argc, char** argv)
{
  if (argc < 2) {
    return usage();
  }
  std::string cmd = argv[1];
  std::vector<std::string> args(argv + 2, argv + argc);

  if (cmd == "probe") {
    if (args.empty()) {
      return usage();
    }
    auto r = media::probe_file(args[0]);
    if (!r.ok()) {
      return fail(r.error());
    }
    std::cout << to_text(Json(*r), 2) << "\n";
    return 0;
  }

  // The prompt as a song's words: no controller needed.
  if (cmd == "song") {
    if (args.empty()) {
      return usage();
    }
    const auto s = assist::split_song(args[0]);
    std::cout << to_text(Json{{"style", s.style}, {"lyrics", s.lyrics},
                              {"sections", s.sections},
                              {"lines", s.lines}}, 2)
              << "\n";
    return 0;
  }

  // Extension packages as files: no controller needed.
  if (cmd == "ext" && !args.empty() &&
      (args[0] == "pack" || args[0] == "show" || args[0] == "check")) {
    if (args.size() < 2 || (args[0] == "pack" && args.size() < 3)) {
      return usage();
    }
    if (args[0] == "pack") {
      std::ifstream in(args[1]);
      const Json doc = Json::parse(in, nullptr, /*allow_exceptions=*/false);
      if (doc.is_discarded() || !doc.is_object()) {
        std::fprintf(stderr, "%s is not a JSON object\n", args[1].c_str());
        return 1;
      }
      if (const auto v = ext::admit(doc, ext::Interface::host()); !v.ok()) {
        std::fprintf(stderr, "warning: this Valtz would not take it: %s "
                     "%s\n", v.why.c_str(), to_text(v.args).c_str());
      }
      std::error_code ec;
      fs::create_directories(args[2], ec);
      const auto bytes = to_cbor(doc);
      const fs::path out = fs::path(args[2]) / ext::kManifestFile;
      std::ofstream o(out, std::ios::binary | std::ios::trunc);
      o.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
      if (!o) {
        std::fprintf(stderr, "cannot write %s\n", out.c_str());
        return 1;
      }
      std::printf("%s (%zu bytes)\n", out.c_str(), bytes.size());
      return 0;
    }
    if (args[0] == "show") {
      fs::path file = args[1];
      if (fs::is_directory(file)) {
        file /= ext::kManifestFile;
      }
      std::ifstream in(file, std::ios::binary);
      const std::vector<std::uint8_t> bytes(
          (std::istreambuf_iterator<char>(in)),
          std::istreambuf_iterator<char>());
      auto doc = ext::decode_manifest(bytes);
      if (!doc.ok()) {
        return fail(doc.error());
      }
      std::cout << to_text(*doc, 2) << "\n";
      return 0;
    }
    const auto e = ext::read_package(args[1]);
    Json j = ext::to_json(e, "en");
    j["manifest"] = e.doc;
    std::cout << to_text(j, 2) << "\n";
    return e.admitted() ? 0 : 1;
  }

  // "--unsaved": what the command changes stays in the working copy
  // (DESIGN §5b) instead of being saved to the package as it ends.
  const bool unsaved = std::erase(args, std::string("--unsaved")) > 0;
  ControllerConfig cfg;
  // Its working copies live on between runs: their history with them.
  cfg.keep_working_copies = true;
  cfg.with_engine = cmd == "generate" || cmd == "video" || cmd == "audio" ||
                    cmd == "upscale" || cmd == "quantize" ||
                    cmd == "transcribe" || cmd == "summarize" ||
                    (cmd == "ext" && (args.empty() || args[0] == "list")) ||
                    cmd == "status" || cmd == "families" ||
                    cmd == "enhance" ||
                    cmd == "intent" || cmd == "download" || cmd == "caps" ||
                    cmd == "auto" ||
                    (cmd == "export" &&
                     !(args.size() == 1 && args[0] == "--formats"));
  // "--fleet": a member of its fleet for this run (DESIGN §11) -- what
  // this Mac cannot run, or not now, goes to a member that can.
  const bool in_fleet = std::erase(args, std::string("--fleet")) > 0;
  cfg.fleet = in_fleet || cmd == "fleet";
  if (cmd == "fleet") {
    cfg.with_engine = args.empty() || args[0] == "serve" ||
                      args[0] == "status" || args[0] == "connect";
  }
  auto cr = Controller::create(cfg);
  if (!cr.ok()) {
    return fail(cr.error());
  }
  Controller& c = **cr;
  if (in_fleet) {
    // Its members reached first: a few seconds ($VALTZ_FLEET_WAIT).
    const char* w = std::getenv("VALTZ_FLEET_WAIT");
    const int wait = w ? std::atoi(w) : 5;
    for (int i = 0; i < wait * 10; ++i) {
      bool any = false;
      for (const auto& m : jget(c.fleet_status(), "members", Json())) {
        any = any || (jget<std::string>(m, "state", "") == "connected" &&
                      !jget(m, "self", Json::object()).empty());
      }
      if (any) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  // What the command changed, saved as it ends -- unless --unsaved, or
  // the project is untitled.
  struct SaveAtExit {
    Controller& c;
    bool        on;
    ~SaveAtExit()
    {
      if (!on) {
        return;
      }
      for (const auto pid : c.open_projects()) {
        const Json s = c.project_state(pid);
        if (jget(s, "dirty", false) && !jget(s, "untitled", false) &&
            g_clean_at_open.contains(pid.str())) {
          if (auto st = c.save_project(pid); !st.ok()) {
            std::fprintf(stderr, "valtzctl: could not save: %s\n",
                         st.error().message.c_str());
          }
        }
      }
    }
  } save_at_exit{c, !unsaved};

  if (cmd == "hw") {
    std::cout << to_text(Json(c.hardware()), 2) << "\n";
    const auto* a = c.assistant_model();
    std::cout << "assistant model for this tier: "
              << (a ? a->name : "none") << "\n";
    return 0;
  }
  if (cmd == "families") {
    const Json t = c.capability_tree();
    for (const auto& f : t["families"]) {
      std::string feats;
      for (const auto& ft : f["features"]) {
        feats += std::format(" [{}] {}", ft["available"].get<bool>()
                                             ? "x" : " ",
                             ft["feature"].get<std::string>());
      }
      std::printf("%s%s\n", f["name"].get<std::string>().c_str(),
                  feats.c_str());
      for (const auto& m : f["members"]) {
        const auto state = m["state"].get<std::string>();
        const auto src = m["source"].get<std::string>();
        std::printf("  %-9s %-6s %-40s %s\n", state.c_str(),
                    src.empty() ? "-" : src.c_str(),
                    m["label"].get<std::string>().c_str(),
                    m["model"].get<std::string>().c_str());
      }
    }
    return 0;
  }
  if (cmd == "storage") {
    Json r = c.storage_report();
    auto gb = [](const Json& b) {
      return std::format("{:8.2f} GB", b.get<double>() / 1e9);
    };
    // Each list in the order Settings > Storage offers: by size, by when
    // it was made, by name; descending by default.
    std::string by = "size";
    bool asc = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
      if (args[i] == "--sort" && i + 1 < args.size()) {
        by = args[++i];
        if (by != "size" && by != "created" && by != "name") {
          return usage();
        }
      } else if (args[i] == "--asc") {
        asc = true;
      } else if (args[i] == "--desc") {
        asc = false;
      }
    }
    auto order = [&](Json& items, const char* name_key) {
      std::vector<Json> v(items.begin(), items.end());
      std::ranges::stable_sort(v, [&](const Json& a, const Json& b) {
        int c = 0;
        if (by == "size") {
          c = jget<std::int64_t>(a, "bytes", 0) <
                      jget<std::int64_t>(b, "bytes", 0)
                  ? -1
              : jget<std::int64_t>(a, "bytes", 0) >
                      jget<std::int64_t>(b, "bytes", 0)
                  ? 1 : 0;
        } else if (by == "created") {
          c = jget<std::int64_t>(a, "created", 0) <
                      jget<std::int64_t>(b, "created", 0)
                  ? -1
              : jget<std::int64_t>(a, "created", 0) >
                      jget<std::int64_t>(b, "created", 0)
                  ? 1 : 0;
        } else {
          c = jget<std::string>(a, name_key, "").compare(
              jget<std::string>(b, name_key, ""));
          c = c < 0 ? -1 : c > 0 ? 1 : 0;
        }
        return asc ? c < 0 : c > 0;
      });
      items = Json(v);
    };
    order(r["models"]["items"], "repo");
    order(r["projects"]["items"], "name");
    auto made = [](const Json& j) {
      const std::time_t t = jget<std::int64_t>(j, "created", 0) / 1000;
      if (t <= 0) {
        return std::string("          ");
      }
      std::tm tm{};
      localtime_r(&t, &tm);
      char buf[16];
      std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
      return std::string(buf);
    };
    std::printf("volume   %s of %s free\n",
                gb(r["volume"]["free"]).c_str(),
                gb(r["volume"]["capacity"]).c_str());
    std::printf("models   %s  %s\n", gb(r["models"]["bytes"]).c_str(),
                r["models"]["root"].get<std::string>().c_str());
    for (const auto& m : r["models"]["items"]) {
      std::printf("  %s  %s  %s\n", gb(m["bytes"]).c_str(),
                  made(m).c_str(), m["repo"].get<std::string>().c_str());
    }
    std::printf("projects %s  %s\n", gb(r["projects"]["bytes"]).c_str(),
                r["projects"]["root"].get<std::string>().c_str());
    for (const auto& p : r["projects"]["items"]) {
      std::printf("  %s  %s  %s\n", gb(p["bytes"]).c_str(),
                  made(p).c_str(), p["name"].get<std::string>().c_str());
    }
    std::printf("cache    %s  (budget %s)\n",
                gb(r["cache"]["bytes"]).c_str(),
                gb(r["cache"]["budget"]).c_str());
    return 0;
  }
  if (cmd == "assistant") {
    // --drafter's bits, given anywhere on the line.
    int bits = c.assistants().value("drafter_bits", 8);
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
      if (args[i] == "--bits") {
        bits = std::atoi(args[i + 1].c_str());
      }
    }
    for (std::size_t i = 0; i < args.size(); ++i) {
      Status st = ok_status();
      if (args[i] == "--keep" && i + 1 < args.size()) {
        st = c.set_assistant_keep_loaded(std::atof(args[++i].c_str()));
      } else if (args[i] == "--video-every" && i + 1 < args.size()) {
        ++i;
        st = c.set_video_every(args[i] == "auto"
                                   ? 0.0
                                   : std::atof(args[i].c_str()));
      } else if (args[i] == "--bits" && i + 1 < args.size()) {
        ++i;
        st = c.set_assistant_drafter(
            c.assistants().value("drafter", std::string("mtp")), bits);
      } else if (args[i] == "--drafter" && i + 1 < args.size()) {
        st = c.set_assistant_drafter(args[++i], bits);
      } else {
        st = c.choose_assistant(args[i] == "auto" ? "" : args[i]);
      }
      if (!st.ok()) {
        return fail(st.error());
      }
    }
    const Json a = c.assistants();
    const auto choice = a["choice"].get<std::string>();
    std::printf("chosen   %s\nauto     %s\nusing    %s\nkept     %g s "
                "after a request\ndrafter  %s%s\nvideo    a frame every "
                "%g s%s (memory %g GB/s)\n",
                choice.empty() ? "auto" : choice.c_str(),
                a["auto"].get<std::string>().c_str(),
                a["using"].get<std::string>().c_str(),
                jget(a, "keep_loaded", 0.0),
                jget<std::string>(a, "drafter", "mtp").c_str(),
                jget<std::string>(a, "drafter", "") == "dflash"
                    ? std::format(" ({}-bit)", jget(a, "drafter_bits", 8))
                          .c_str()
                    : "",
                jget(a, "video_every_now", 1.0),
                jget(a, "video_every", 0.0) > 0 ? "" : " (auto)",
                jget(a, "memory_bandwidth_gbs", 0.0));
    for (const auto& m : a["models"]) {
      std::string drafter;
      if (m.contains("drafter")) {
        drafter = std::format("  drafter {} ({})",
                              m["drafter"]["id"].get<std::string>(),
                              m["drafter"]["state"].get<std::string>());
      }
      if (m.contains("dflash")) {
        drafter += std::format("  dflash {} ({})",
                               m["dflash"]["id"].get<std::string>(),
                               m["dflash"]["state"].get<std::string>());
      }
      const Json& sm = m["sampling"];
      const std::string sampler =
          sm.empty() ? std::string("greedy")
                     : std::format("t {} p {} k {} presence {}",
                                   jget(sm, "temperature", 1.0),
                                   jget(sm, "top_p", 1.0),
                                   jget(sm, "top_k", 0),
                                   jget(sm, "presence_penalty", 0.0));
      std::printf("  %-18s %-10s %-7s %5.1f GB  >=%2d GB  %s%s%s\n",
                  m["id"].get<std::string>().c_str(),
                  m["state"].get<std::string>().c_str(),
                  m["fits"].get<bool>() ? "fits" : "too big",
                  m["disk_gb"].get<double>(),
                  m["min_ram_gb"].get<int>(), sampler.c_str(),
                  m["mtp"].get<bool>() ? "  mtp" : "", drafter.c_str());
    }
    return 0;
  }
  if (cmd == "link" || cmd == "unlink") {
    if (args.empty() || (cmd == "link" && args.size() < 2)) {
      return usage();
    }
    auto st = cmd == "link" ? c.link_model(args[0], args[1])
                            : c.unlink_model(args[0]);
    return st.ok() ? 0 : fail(st.error());
  }
  if (cmd == "status") {
    // The ANE's reading is a delta: a first call starts it, a second a
    // second later reads it.
    c.machine_status();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    Json j = c.machine_status();
    if (!args.empty() && args[0] == "--thermal") {
      j["thermal"] = c.gpu_thermal(600);
    }
    std::cout << to_text(j, 2) << "\n";
    return 0;
  }
  if (cmd == "caps") {
    Json arr = Json::array();
    for (const auto& s : c.capabilities()) {
      arr.push_back(s);
    }
    std::cout << to_text(arr, 2) << "\n";
    return 0;
  }
  if (cmd == "paths") {
    const auto& p = c.paths();
    std::printf("projects  %s\nmodels    %s\ncache     %s (%llu of %llu MB, "
                "%s)\nengine    %s\n",
                p.projects.c_str(), p.models.c_str(), p.cache.c_str(),
                static_cast<unsigned long long>(c.cache().size_bytes() >> 20),
                static_cast<unsigned long long>(
                    c.cache().budget_bytes() >> 20),
                c.cache().on_internal_volume() ? "internal" : "EXTERNAL",
                p.engine.c_str());
    return 0;
  }
  if (cmd == "models") {
    models::ModelStore store(
        models::ModelStore::default_roots(c.paths().models));
    for (const auto& m : c.catalog().models()) {
      auto i = store.info(m);
      std::printf("%-16s %-10s %6.1f GB%s  >=%2u GB  %-28s %s\n",
                  m.id.c_str(), models::to_str(i.state), m.disk_gb,
                  m.disk_measured ? " " : "~", m.min_ram_gb,
                  m.hf_path.c_str(), i.dir.string().c_str());
    }
    return 0;
  }
  // A project as a document: save (or save as), revert, undo, redo, and
  // its history of changes.
  if (cmd == "save" || cmd == "revert" || cmd == "undo" || cmd == "redo" ||
      cmd == "changes") {
    if (args.empty()) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    if (!pid.ok()) {
      return fail(pid.error());
    }
    Status st = ok_status();
    if (cmd == "save") {
      const bool as = args.size() > 2 && args[1] == "--as";
      st = as ? c.save_project_as(*pid, args[2]) : c.save_project(*pid);
    } else if (cmd == "revert") {
      st = c.revert_project(*pid);
    } else if (cmd == "undo") {
      st = c.undo(*pid);
    } else if (cmd == "redo") {
      st = c.redo(*pid);
    }
    if (!st.ok()) {
      return fail(st.error());
    }
    Json out = c.project_state(*pid);
    if (cmd == "changes") {
      out["history"] = c.undo_history(*pid);
      out["opened"] = c.open_report(*pid);
    }
    std::cout << to_text(out, 2) << "\n";
    return 0;
  }
  if (cmd == "new") {
    if (args.empty()) {
      return usage();
    }
    std::string name = args.size() > 1 ? args[1]
                                       : fs::path(args[0]).stem().string();
    auto r = c.create_project(args[0], name);
    if (!r.ok()) {
      return fail(r.error());
    }
    std::cout << "created " << args[0] << " (" << r->str() << ")\n";
    return 0;
  }
  if (cmd == "info") {
    if (args.empty()) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    if (!pid.ok()) {
      return fail(pid.error());
    }
    auto* p = c.project(*pid);
    auto assets = p->assets();
    if (!assets.ok()) {
      return fail(assets.error());
    }
    std::printf("%s  (%zu assets)\n", p->name().c_str(), assets->size());
    for (const auto& a : *assets) {
      std::string stale;
      if (a.origin == project::Origin::Derived) {
        auto st = p->staleness(a.id);
        stale = st.ok() ? project::to_str(st->reason) : "?";
      }
      std::string detail;
      std::string exif;
      if (auto v = p->version(a.id); v.ok()) {
        const auto& f = v->info.frame;
        detail = std::format("{}x{} {} {}", f.width, f.height,
                             v->info.codec_name,
                             f.color.describe());
        // What the camera recorded, as the inspector reads it.
        if (!v->info.exif.empty()) {
          exif = to_text(v->info.exif);
        }
      }
      // The name column by display width -- printf pads bytes, which
      // splits CJK and pushes the columns out of line -- cut in the
      // middle so a " (2)" or an extension stays in view.
      std::printf("  %s  %-8s %-7s v%-3u %s %s %s\n",
                  a.id.str().c_str(), project::to_str(a.kind),
                  project::to_str(a.origin), a.head,
                  fit_columns(a.name, 32, /*middle=*/true).c_str(),
                  detail.c_str(), stale.c_str());
      for (const auto& m : a.modifiers) {
        std::printf("    modifier %s%s %s\n", m.kind.c_str(),
                    m.layer.empty() ? "" : (" @" + m.layer).c_str(),
                    to_text(m.params).c_str());
      }
      if (!exif.empty()) {
        std::printf("    exif %s\n", exif.c_str());
      }
    }
    return 0;
  }
  if (cmd == "import") {
    if (args.size() < 2) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    if (!pid.ok()) {
      return fail(pid.error());
    }
    project::ImportOptions opts;
    std::vector<fs::path> files;
    for (std::size_t i = 1; i < args.size(); ++i) {
      if (args[i] == "--copy") {
        opts.placement = project::Placement::Copy;
      } else if (args[i] == "--link") {
        opts.placement = project::Placement::Link;
      } else {
        files.emplace_back(args[i]);
      }
    }
    auto j = c.import_files(*pid, files, opts);
    if (!j.ok()) {
      return fail(j.error());
    }
    return follow(c, *j);
  }
  if (cmd == "links") {
    if (args.empty()) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);  // opening starts the check
    if (!pid.ok()) {
      return fail(pid.error());
    }
    auto* p = c.project(*pid);
    // Bind the Result: a range-for over `*p->assets()` would iterate a
    // destroyed temporary (C++20 does not extend its lifetime).
    auto assets = p->assets();
    if (!assets.ok()) {
      return fail(assets.error());
    }
    for (const auto& a : *assets) {
      if (!a.linked) {
        continue;
      }
      auto chk = p->check_link(a.id);
      std::printf("  %-10s %-32s %s\n",
                  chk.ok() ? project::to_str(chk->state) : "error",
                  a.name.c_str(),
                  chk.ok() ? chk->path.c_str() : chk.error().message.c_str());
    }
    return 0;
  }
  // "exposure=0.4,vibrance=0.3" as {"exposure": 0.4, ...}.
  auto key_values = [](const std::string& spec) {
    Json j = Json::object();
    std::size_t at = 0;
    while (at < spec.size()) {
      std::size_t end = spec.find(',', at);
      if (end == std::string::npos) { end = spec.size(); }
      const std::string kv = spec.substr(at, end - at);
      const std::size_t eq = kv.find('=');
      if (eq != std::string::npos) {
        j[kv.substr(0, eq)] = std::atof(kv.c_str() + eq + 1);
      }
      at = end + 1;
    }
    return j;
  };
  if (cmd == "keys") {
    static const std::set<std::string> kinds = {"adjust", "crop", "rotate",
                                                "speed", "audio"};
    if (args.size() < 3 || !kinds.contains(args[2])) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    auto aid = AssetId::parse(args[1]);
    if (!pid.ok() || !aid) {
      return usage();
    }
    auto a = c.project(*pid)->asset(*aid);
    if (!a.ok()) {
      return fail(a.error());
    }
    // A layer's track ("--layer 1"; layer 0 without); its content is what
    // the layer shows.
    std::string layer;
    for (std::size_t i = 3; i + 1 < args.size(); ++i) {
      if (args[i] == "--layer") {
        layer = layer_id(args[i + 1]);
        args.erase(args.begin() + static_cast<std::ptrdiff_t>(i),
                   args.begin() + static_cast<std::ptrdiff_t>(i) + 2);
        break;
      }
    }
    // A sound's pitch following its speed ("--follow-speed on|off").
    std::optional<bool> follow;
    for (std::size_t i = 3; i + 1 < args.size(); ++i) {
      if (args[i] == "--follow-speed") {
        if (args[2] != "audio" ||
            (args[i + 1] != "on" && args[i + 1] != "off")) {
          return usage();
        }
        follow = args[i + 1] == "on";
        args.erase(args.begin() + static_cast<std::ptrdiff_t>(i),
                   args.begin() + static_cast<std::ptrdiff_t>(i) + 2);
        break;
      }
    }
    // Keys count the composition's frames -- a still's pages, one a
    // "second".
    Rational rate{24, 1};
    if (a->cls == project::AssetClass::Still && a->pages > 1) {
      rate = {1, 1};
    } else if (a->cls == project::AssetClass::Composition) {
      if (auto m = c.movie_stack(*pid, *aid); m.ok() && m->rate.num > 0) {
        rate = m->rate;
      }
    }
    media::PixelSize content;
    const auto it = std::ranges::find(a->layers, layer, &project::Layer::id);
    if (it != a->layers.end() && it->source) {
      if (auto sz = c.canvas_size(*pid, *it->source); sz.ok()) {
        content = *sz;
      }
    }
    // No keys given: the track as it is, unchanged (clearing one takes
    // a key at its default).
    bool any_key = false;
    for (std::size_t i = 3; i < args.size(); ++i) {
      any_key = any_key || args[i].find(':') != std::string::npos;
    }
    // The pitch's following alone: the keys as they are.
    if (!any_key && follow) {
      const auto k = Controller::sound_keys_of(*a, layer);
      if (auto st = c.set_sound_keys(*pid, *aid, k, layer, follow);
          !st.ok()) {
        return fail(st.error());
      }
      if (auto now = c.project(*pid)->asset(*aid); now.ok()) {
        a = std::move(now);
      }
    }
    if (!any_key) {
      Json now;
      if (args[2] == "adjust") {
        now = media::to_json(Controller::adjustment_keys_of(*a, layer));
      } else if (args[2] == "speed") {
        now = media::to_json(Controller::speed_keys_of(*a, layer));
      } else if (args[2] == "audio") {
        now = media::to_json(Controller::sound_keys_of(*a, layer));
        now["follow_speed"] = Controller::pitch_follows_speed(*a, layer);
      } else {
        now = media::to_json(Controller::crop_keys_of(*a, layer));
      }
      std::printf("%s\n", to_text(now).c_str());
      return 0;
    }
    Json keys = Json::array();
    for (std::size_t i = 3; i < args.size(); ++i) {
      const std::size_t colon = args[i].find(':');
      if (colon == std::string::npos) {
        return usage();
      }
      Json k = key_values(args[i].substr(colon + 1));
      k["frame"] = std::atoll(args[i].substr(0, colon).c_str());
      if (args[2] == "crop" || args[2] == "rotate") {
        k["content_w"] = content.width;
        k["content_h"] = content.height;
      }
      keys.push_back(std::move(k));
    }
    const Json track = {{"keys", keys},
                        {"rate_num", rate.num},
                        {"rate_den", rate.den}};
    Status st = ok_status();
    Json out;
    if (args[2] == "adjust") {
      const auto k = media::keyed_adjustments_from_json(track);
      st = c.set_adjustment_keys(*pid, *aid, k, layer);
      out = media::to_json(k);
    } else if (args[2] == "speed") {
      const auto k = media::keyed_speed_from_json(track);
      st = c.set_speed_keys(*pid, *aid, k, layer);
      out = media::to_json(k);
    } else if (args[2] == "audio") {
      const auto k = media::keyed_sound_from_json(track);
      st = c.set_sound_keys(*pid, *aid, k, layer, follow);
      out = media::to_json(k);
      if (auto now = c.project(*pid)->asset(*aid); now.ok()) {
        out["follow_speed"] = Controller::pitch_follows_speed(*now, layer);
      }
    } else {
      // One track of the layer's crop; the other stays as it is.
      media::KeyedCrop k = Controller::crop_keys_of(*a, layer);
      if (args[2] == "crop") {
        k.place = media::keyed_crop_from_json(track).place;
        // The background is the layer's, whichever key names it.
        const char* pads[] = {"pad_r", "pad_g", "pad_b", "pad_a"};
        for (const auto& key : keys) {
          for (int i = 0; i < 4; ++i) {
            if (key.contains(pads[i])) {
              k.pad[i] = std::clamp(jget(key, pads[i], 0.0), 0.0, 1.0);
            }
          }
        }
      } else {
        Json t = track;
        t["rotate_keys"] = keys;
        t["keys"] = Json::array();
        k.turn = media::keyed_crop_from_json(t).turn;
      }
      k.rate = rate;
      k.place.rate = k.turn.rate = k.rate;
      // A track left empty starts at the first frame.
      if (k.place.empty()) {
        media::Crop c0;
        c0.content = content;
        k.place.keys.push_back({0, c0});
      }
      if (k.turn.empty()) {
        k.turn.keys.push_back({0, {}});
      }
      st = c.set_crop_keys(*pid, *aid, k, layer);
      out = media::to_json(k);
    }
    if (!st.ok()) {
      return fail(st.error());
    }
    std::cout << to_text(out) << "\n";
    return 0;
  }
  if (cmd == "canvas" || cmd == "timeline") {
    if (args.size() < 3) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    auto aid = AssetId::parse(args[1]);
    if (!pid.ok() || !aid) {
      return usage();
    }
    Status st = ok_status();
    if (cmd == "timeline") {
      st = c.set_timeline(*pid, *aid, std::atoll(args[2].c_str()));
    } else if (args[2] == "own") {
      st = c.reset_canvas(*pid, *aid);
    } else {
      int w = 0, h = 0;
      if (std::sscanf(args[2].c_str(), "%dx%d", &w, &h) != 2) {
        return usage();
      }
      std::string anchor = "c";
      for (std::size_t i = 3; i + 1 < args.size(); ++i) {
        if (args[i] == "--anchor") {
          anchor = args[i + 1];
        }
      }
      const double ax = anchor.find('l') != std::string::npos   ? 0
                        : anchor.find('r') != std::string::npos ? 1
                                                                : 0.5;
      const double ay = anchor.starts_with('t')   ? 0
                        : anchor.starts_with('b') ? 1
                                                  : 0.5;
      st = c.set_canvas(*pid, *aid, {w, h}, ax, ay);
    }
    if (!st.ok()) {
      return fail(st.error());
    }
    auto a = c.project(*pid)->asset(*aid);
    if (a.ok()) {
      std::cout << to_text({{"canvas", media::to_json(a->canvas)},
                            {"timeline", a->timeline_frames}})
                << "\n";
    }
    return 0;
  }
  if (cmd == "crop" || cmd == "trim") {
    if (args.size() < 2) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    auto aid = AssetId::parse(args[1]);
    if (!pid.ok() || !aid) {
      return usage();
    }
    if (cmd == "trim") {
      // trim <proj> <comp> [--layer L] [<in> <out>|none] [--offset F]
      //      [--duration F]
      auto a = c.project(*pid)->asset(*aid);
      if (!a.ok()) {
        return fail(a.error());
      }
      std::string layer;
      std::vector<std::string> marks;
      std::optional<std::int64_t> offset, duration;
      for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--layer" && i + 1 < args.size()) {
          layer = layer_id(args[++i]);
        } else if (args[i] == "--offset" && i + 1 < args.size()) {
          offset = std::atoll(args[++i].c_str());
        } else if (args[i] == "--duration" && i + 1 < args.size()) {
          duration = std::atoll(args[++i].c_str());
        } else {
          marks.push_back(args[i]);
        }
      }
      const auto it = std::ranges::find(a->layers, layer,
                                        &project::Layer::id);
      if (it == a->layers.end()) {
        return fail(make_error(Code::InvalidArgument, "no such layer"));
      }
      project::LayerTime t = it->time;
      if (marks.size() >= 2) {
        t.in = std::atoll(marks[0].c_str());
        t.out = std::atoll(marks[1].c_str());
        // Its source's own frames -- a sound's milliseconds.
        t.rate = {0, 1};
        if (it->source) {
          auto s = c.project(*pid)->asset(*it->source);
          if (s.ok() && s->kind == project::AssetKind::Audio) {
            t.rate = Rational(1000, 1);
          } else if (s.ok() && s->cls == project::AssetClass::Composition) {
            if (auto m = c.movie_stack(*pid, *it->source); m.ok()) {
              t.rate = m->rate;
            }
          } else if (auto v = c.project(*pid)->version(*it->source);
                     v.ok() && v->info.frame_rate.num > 0) {
            t.rate = v->info.frame_rate;
          }
        }
      } else if (marks.size() == 1 && marks[0] == "none") {
        t.in = t.out = -1;
        t.rate = {0, 1};
      }
      if (offset) {
        t.offset = *offset;
      }
      if (duration) {
        t.duration = *duration;
      }
      if (auto st = c.set_layer_time(*pid, *aid, layer, t); !st.ok()) {
        return fail(st.error());
      }
      std::cout << to_text({{"in", t.in}, {"out", t.out},
                            {"rate_num", t.rate.num},
                            {"rate_den", t.rate.den},
                            {"offset", t.offset},
                            {"duration", t.duration}})
                << "\n";
      return 0;
    }
    // "--layer L": that layer's crop -- its placement on the frame.
    std::string layer;
    std::string kv;
    for (std::size_t i = 2; i < args.size(); ++i) {
      if (args[i] == "--layer" && i + 1 < args.size()) {
        layer = layer_id(args[++i]);
      } else {
        kv = args[i];
      }
    }
    Json j = key_values(kv);
    // On the picture's own canvas, as it displays -- a layer's, on what
    // the layer shows.
    if (!j.empty()) {
      project::Project* p = c.project(*pid);
      AssetId shown = *aid;
      std::uint32_t version = 0;
      bool sized = true;
      if (auto a = p->asset(*aid); a.ok() && !a->layers.empty()) {
        const auto l = std::ranges::find(a->layers, layer,
                                         &project::Layer::id);
        if (l == a->layers.end()) {
          return fail(make_error(Code::InvalidArgument,
                                 "no such layer: " + layer));
        }
        if (l->source) {
          shown = *l->source;
          version = l->source_version;
        } else {
          sized = false;
        }
      }
      // A file as it displays; a composition or a markup at its size.
      Result<media::PixelSize> sz = c.canvas_size(*pid, shown);
      if (auto path = p->media_path(shown, version); path.ok()) {
        sz = media::oriented_size(*path);
      }
      if (!sz.ok()) {
        return fail(sz.error());
      }
      if (sized) {
        j["content_w"] = sz->width;
        j["content_h"] = sz->height;
      }
    }
    const auto crop = media::crop_from_json(j);
    if (auto st = c.set_crop(*pid, *aid, crop, layer); !st.ok()) {
      return fail(st.error());
    }
    std::cout << to_text(media::to_json(crop)) << "\n";
    return 0;
  }
  if (cmd == "adjust") {
    if (args.size() < 2) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    auto aid = AssetId::parse(args[1]);
    if (!pid.ok() || !aid) {
      return usage();
    }
    const auto adj = media::adjustments_from_json(
        key_values(args.size() > 2 ? args[2] : std::string()));
    if (auto st = c.set_adjustments(*pid, *aid, adj); !st.ok()) {
      return fail(st.error());
    }
    std::cout << to_text(media::to_json(adj)) << "\n";
    return 0;
  }
  if (cmd == "layers" || cmd == "flatten" || cmd == "pages") {
    if (args.size() < 2) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    if (!pid.ok()) {
      return fail(pid.error());
    }
    auto aid = AssetId::parse(args[1]);
    if (!aid) {
      return usage();
    }
    if (cmd == "flatten") {
      if (args.size() >= 3 && args[2] == "--asset") {
        auto made = c.flatten_asset(*pid, *aid,
                                    args.size() > 3 ? args[3] : "");
        if (!made.ok()) {
          return fail(made.error());
        }
        std::printf("%s\n", made->str().c_str());
        return 0;
      }
      if (args.size() < 4 || args[2] != "-o") {
        return usage();
      }
      // --page P: a still's page (from 1).
      std::int64_t page = 0;
      for (std::size_t i = 4; i + 1 < args.size(); ++i) {
        if (args[i] == "--page") {
          page = std::atoll(args[i + 1].c_str()) - 1;
        }
      }
      auto st = c.flatten(*pid, *aid, args[3], std::nullopt, std::nullopt,
                          {}, page);
      return st.ok() ? 0 : fail(st.error());
    }
    // A flag's value among the args (pages count from 1), and the args
    // without the flags.
    auto flag = [&](std::string_view name) -> std::optional<std::int64_t> {
      for (std::size_t i = 2; i + 1 < args.size(); ++i) {
        if (args[i] == name) {
          return std::atoll(args[i + 1].c_str());
        }
      }
      return std::nullopt;
    };
    auto text_flag = [&](std::string_view name)
        -> std::optional<std::string> {
      for (std::size_t i = 2; i + 1 < args.size(); ++i) {
        if (args[i] == name) {
          return args[i + 1];
        }
      }
      return std::nullopt;
    };
    std::vector<std::string> plain;
    for (std::size_t i = 0; i < args.size(); ++i) {
      if (args[i].starts_with("--")) {
        ++i;
        continue;
      }
      plain.push_back(args[i]);
    }
    const auto page_flag = flag("--page");
    const std::optional<std::int64_t> on_page =
        page_flag ? std::optional<std::int64_t>(*page_flag - 1)
                  : std::nullopt;
    if (cmd == "pages") {
      // pages <proj> <comp> [add [--after P] [--count N] | remove <P>]
      const std::string pop = plain.size() > 2 ? plain[2] : "";
      if (pop == "add") {
        const auto after = flag("--after");
        const auto count = flag("--count").value_or(1);
        std::optional<std::int64_t> at =
            after ? std::optional<std::int64_t>(*after - 1) : std::nullopt;
        for (std::int64_t i = 0; i < count; ++i) {
          auto r = c.add_page(*pid, *aid, at);
          if (!r.ok()) {
            return fail(r.error());
          }
          std::printf("added page %lld\n", static_cast<long long>(*r + 1));
          at = *r;
        }
      } else if (pop == "remove" && plain.size() > 3) {
        auto st = c.remove_page(*pid, *aid,
                                std::atoll(plain[3].c_str()) - 1);
        if (!st.ok()) {
          return fail(st.error());
        }
      } else if (!pop.empty()) {
        return usage();
      }
    }
    // layers <proj> <asset> [add [above] | move <layer> <by> | show|hide
    // <layer> | source <layer> <asset> | rename <layer> <name> |
    // remove <layer>]; with no op, the stack.
    Status st = ok_status();
    const std::string op =
        cmd == "pages" ? "" : plain.size() > 2 ? plain[2] : "";
    // "0" names layer 0, whose id is "" ("bg" too, as it once was).
    const std::string layer =
        plain.size() > 3 ? layer_id(plain[3]) : "";
    const std::string arg = plain.size() > 4 ? plain[4] : "";
    if (op == "add") {
      auto r = c.add_layer(*pid, *aid, layer, on_page);
      if (!r.ok()) {
        return fail(r.error());
      }
      std::printf("added layer %s\n", r->c_str());
    } else if (op == "move") {
      st = c.move_layer(*pid, *aid, layer, std::atoi(arg.c_str()));
    } else if (op == "show" || op == "hide") {
      st = c.set_layer_visible(*pid, *aid, layer, op == "show");
    } else if (op == "rename") {
      st = c.rename_layer(*pid, *aid, layer, arg);
    } else if (op == "source") {
      auto src = AssetId::parse(arg);
      if (!src) {
        return usage();
      }
      st = c.set_layer_source(*pid, *aid, layer, *src);
    } else if (op == "remove") {
      st = c.remove_layer(*pid, *aid, layer);
    } else if (op == "pages") {
      // The layer on pages <first> [count] (from 1; no count: to the last).
      st = c.set_layer_pages(*pid, *aid, layer,
                             std::atoll(arg.c_str()) - 1,
                             plain.size() > 5
                                 ? std::atoll(plain[5].c_str()) : 0);
    } else if (op == "markup") {
      // A still's page, or a timeline's frame (--frame F).
      const auto at_frame = flag("--frame");
      auto r = c.markup_layer(*pid, *aid, {plain.begin() + 3, plain.end()},
                              at_frame ? at_frame : on_page);
      if (!r.ok()) {
        return fail(r.error());
      }
      std::printf("markup layer %s\n", r->c_str());
    } else if (op == "duplicate") {
      auto r = c.duplicate_layer(*pid, *aid, layer, arg);
      if (!r.ok()) {
        return fail(r.error());
      }
      std::printf("duplicated as layer %s\n", r->c_str());
    } else if (op == "drawing") {
      auto r = c.markup_drawing(*pid, *aid, layer);
      if (!r.ok()) {
        return fail(r.error());
      }
      std::printf("%s\n", r->empty() ? "(nothing painted)"
                                      : r->string().c_str());
    } else if (op == "paste-drawing") {
      // <layer> <png> [dx dy]: a drawing laid over the markup's.
      const double dx = plain.size() > 5 ? std::atof(plain[5].c_str()) : 0;
      const double dy = plain.size() > 6 ? std::atof(plain[6].c_str()) : 0;
      st = c.paste_drawing(*pid, *aid, layer, arg, dx, dy);
    } else if (op == "clear-drawing") {
      st = c.clear_drawing(*pid, *aid, layer);
    } else if (op == "merge") {
      st = c.merge_layers(*pid, *aid, layer, arg);
    } else if (op == "mask") {
      st = c.set_layer_mask(*pid, *aid, layer, arg != "off");
    } else if (op == "decompose") {
      st = c.decompose(*pid, *aid, layer);
    } else if (op == "split") {
      // The timeline's scissors: <layer> <frame>, two parts.
      auto r = c.split_layer(*pid, *aid, layer, std::atoll(arg.c_str()));
      if (!r.ok()) {
        return fail(r.error());
      }
      std::printf("parts %s %s, layer %s\n", r->first.str().c_str(),
                  r->second.str().c_str(), r->layer.c_str());
    } else if (op == "slide") {
      // <layer> <offset>: where it starts on the timeline (a page).
      st = c.slide_layer(*pid, *aid, layer, std::atoll(arg.c_str()));
    } else if (op == "stretch") {
      // <layer> <frames>: a still's length on the timeline (its pages).
      st = c.stretch_layer(*pid, *aid, layer, std::atoll(arg.c_str()));
    } else if (op == "group") {
      // group <layer>... [--name N]: a folder of them.
      std::vector<std::string> ids;
      for (std::size_t i = 3; i < plain.size(); ++i) {
        ids.push_back(layer_id(plain[i]));
      }
      auto r = c.group_layers(*pid, *aid, ids,
                              text_flag("--name").value_or(""));
      if (!r.ok()) {
        return fail(r.error());
      }
      std::printf("folder %s\n", r->c_str());
    } else if (op == "ungroup") {
      st = c.ungroup_layers(*pid, *aid, plain.size() > 3 ? plain[3] : "");
    } else if (op == "place") {
      // place <layer> <above-layer|bottom> [folder]: in a folder, or out.
      std::optional<std::string> above;
      if (arg != "bottom") {
        above = layer_id(arg);
      }
      st = c.place_layers(*pid, *aid, {layer}, above,
                          plain.size() > 5 ? plain[5] : "");
    } else if (op == "flatten") {
      // The layer's composition flat, in its place: upscalable.
      auto made = c.flatten_layer(*pid, *aid, layer);
      if (!made.ok()) {
        return fail(made.error());
      }
      std::printf("flat %s\n", made->str().c_str());
    } else if (op == "stroke") {
      const auto xy = numbers_arg(arg);
      const Json o = options_arg(args.size() > 5 ? args[5] : "");
      Json s = {{"points", Json::array()},
                {"radius", jget(o, "r", 8.0)},
                {"softness", jget(o, "soft", 0.0)},
                {"erase", jget(o, "erase", false)}};
      for (std::size_t i = 0; i + 1 < xy.size(); i += 2) {
        s["points"].push_back({xy[i], xy[i + 1]});
      }
      if (Json col = rgba_arg(jget<std::string>(o, "color", ""));
          !col.is_null()) {
        s["color"] = col;
      }
      st = c.paint_stroke(*pid, *aid, layer, media::stroke_from_json(s));
    } else if (op == "shape" || op == "text") {
      auto a = c.project(*pid)->asset(*aid);
      if (!a.ok()) {
        return fail(a.error());
      }
      Json objects = Json::array();
      for (const auto& l : a->layers) {
        if (l.id == layer && l.source) {
          if (auto m = c.project(*pid)->asset(*l.source);
              m.ok() && m->markup) {
            objects = m->markup->objects;
          }
        }
      }
      Json obj;
      if (op == "shape") {
        const auto xy = numbers_arg(args.size() > 5 ? args[5] : "");
        const Json o = options_arg(args.size() > 6 ? args[6] : "");
        if (xy.size() != 4) {
          return usage();
        }
        obj = {{"kind", arg}, {"x0", xy[0]}, {"y0", xy[1]},
               {"x1", xy[2]}, {"y1", xy[3]},
               {"width", jget(o, "width", 4.0)}};
        if (Json s = rgba_arg(jget<std::string>(o, "stroke", ""));
            !s.is_null()) {
          obj["stroke"] = s;
        }
        if (Json f = rgba_arg(jget<std::string>(o, "fill", ""));
            !f.is_null()) {
          obj["fill"] = f;
        }
      } else {
        const auto xy = numbers_arg(arg);
        const Json o = options_arg(args.size() > 6 ? args[6] : "");
        if (xy.size() != 2 && xy.size() != 4) {
          return usage();
        }
        obj = {{"kind", "text"}, {"x0", xy[0]}, {"y0", xy[1]},
               {"text", args.size() > 5 ? args[5] : ""},
               {"font", {{"family", jget<std::string>(o, "font",
                                                      "Helvetica Neue")},
                         {"size", jget(o, "size", 48.0)},
                         {"bold", jget(o, "bold", false)},
                         {"italic", jget(o, "italic", false)},
                         {"underline", jget(o, "underline", false)}}}};
        if (xy.size() == 4) {
          // A text box (media/markup.h): wrapped, cut at a line.
          obj["x1"] = xy[2];
          obj["y1"] = xy[3];
          obj["box"] = true;
        }
        if (Json col = rgba_arg(jget<std::string>(o, "color", ""));
            !col.is_null()) {
          obj["stroke"] = col;
        }
      }
      objects.push_back(std::move(obj));
      st = c.set_markup_objects(*pid, *aid, layer, objects);
    } else if (op == "materialize") {
      std::vector<std::string> ids(args.begin() + std::min<std::size_t>(
                                                      4, args.size()),
                                   args.end());
      if (ids.empty()) {
        // All of them.
        auto a = c.project(*pid)->asset(*aid);
        for (const auto& l : a.ok() ? a->layers
                                    : std::vector<project::Layer>{}) {
          if (l.id != layer || !l.source) {
            continue;
          }
          auto m = c.project(*pid)->asset(*l.source);
          if (m.ok() && m->markup) {
            for (const auto& o : m->markup->objects) {
              ids.push_back(jget<std::string>(o, "id", ""));
            }
          }
        }
      }
      st = c.materialize_markup(*pid, *aid, layer, ids);
    } else if (!op.empty()) {
      return usage();
    }
    if (!st.ok()) {
      return fail(st.error());
    }
    auto a = c.project(*pid)->asset(*aid);
    if (!a.ok()) {
      return fail(a.error());
    }
    // The project's canvas: a frame of its own (DESIGN §10a), resized
    // as itself -- or, resized before that, a canvas round it.
    if (a->canvas.framed()) {
      const auto& k = a->canvas;
      const bool as_frame = k.width == k.frame_w &&
                            k.height == k.frame_h && k.x == 0 && k.y == 0;
      std::printf("  canvas %dx%d%s\n", k.frame_w, k.frame_h,
                  !k.set()   ? ""
                  : as_frame ? " (resized)"
                             : std::format(", on a {}x{} canvas (frame at "
                                           "{}, {})",
                                           k.width, k.height, k.x, k.y)
                                   .c_str());
    }
    if (a->timeline_frames > 0) {
      std::printf("  length %lld frames\n",
                  static_cast<long long>(a->timeline_frames));
    }
    const bool paged = a->cls == project::AssetClass::Still && a->pages > 1;
    if (paged) {
      std::printf("  %lld pages\n", static_cast<long long>(a->pages));
    }
    if (a->rate.num > 0) {
      std::printf("  %s at %lld/%lld fps\n", project::to_str(a->cls),
                  static_cast<long long>(a->rate.num),
                  static_cast<long long>(a->rate.den));
    }
    for (auto it = a->layers.rbegin(); it != a->layers.rend(); ++it) {
      std::string what = it->source ? it->source->str() : "(empty)";
      if (it->source) {
        if (auto s = c.project(*pid)->asset(*it->source); s.ok()) {
          what += std::format(" ({} {})", project::to_str(s->cls),
                              project::to_str(s->kind));
          if (s->markup) {
            what += std::format(" -- {}, {} object(s)",
                                s->markup->raster.empty()
                                    ? "nothing painted" : "painted",
                                s->markup->objects.size());
          }
        }
      }
      std::string when;
      if (paged) {
        // Its pages, from 1.
        const auto first = it->time.offset + 1;
        const auto last = it->time.duration > 0
            ? std::min(it->time.offset + it->time.duration, a->pages)
            : a->pages;
        when = first > a->pages ? std::string("  [on no page]")
               : first == last  ? std::format("  [page {}]", first)
                                : std::format("  [pages {}-{}]", first, last);
      } else if (!it->time.identity()) {
        when = std::format("  [in {} out {} from {} for {}]", it->time.in,
                           it->time.out, it->time.offset,
                           it->time.duration);
      }
      // In a folder: its id and name, set in.
      std::string in;
      for (const auto& f : a->layer_folders) {
        if (f.id == it->folder) {
          in = std::format("  {{{} {}}}", f.id, f.name);
        }
      }
      std::printf("  %-3s %s %-12s %s%s%s%s\n",
                  it->id.empty() ? "0" : it->id.c_str(),
                  it->visible ? "shown " : "hidden",
                  it->name.empty() ? "-" : it->name.c_str(),
                  what.c_str(), when.c_str(),
                  it->mask ? "  -- masks the layer below" : "", in.c_str());
    }
    for (const auto& t : a->transitions) {
      std::printf("  %s %s -> %s\n", t.kind.c_str(),
                  t.from.empty() ? "0" : t.from.c_str(),
                  t.to.empty() ? "0" : t.to.c_str());
    }
    return 0;
  }
  if (cmd == "classify") {
    // What weight files are, as Custom's panel files a drop: an
    // extension's rules first.
    for (const auto& a : args) {
      const Json k = c.classify_weights(a);
      const auto kind = jget<std::string>(k, "kind", "");
      const auto fam = jget<std::string>(k, "family", "");
      std::printf("%-5s %-12s %s\n", kind.empty() ? "?" : kind.c_str(),
                  fam.empty() ? "-" : fam.c_str(), a.c_str());
    }
    return 0;
  }
  if (cmd == "ext") {
    if (!args.empty() && (args[0] == "enable" || args[0] == "disable")) {
      if (args.size() < 2) {
        return usage();
      }
      if (auto st = c.set_extension_enabled(args[1], args[0] == "enable");
          !st.ok()) {
        return fail(st.error());
      }
      std::printf("%s %sd (taken at the next launch)\n", args[1].c_str(),
                  args[0].c_str());
      return 0;
    }
    if (!args.empty() && args[0] != "list") {
      return usage();
    }
    const Json r = c.extensions();
    const Json& host = r["interface"];
    std::printf("extension interface %d (reads %d-%d)\n",
                host["current"].get<int>(), host["oldest"].get<int>(),
                host["current"].get<int>());
    if (r["extensions"].empty()) {
      std::printf("no extension packages\n");
    }
    for (const auto& e : r["extensions"]) {
      std::printf("%-15s %s %s  (%s, interface %d)\n",
                  e["state"].get<std::string>().c_str(),
                  e["id"].get<std::string>().c_str(),
                  e["version"].get<std::string>().c_str(),
                  e["name"].get<std::string>().c_str(),
                  e["interface"].get<int>());
      std::printf("  %s\n", e["dir"].get<std::string>().c_str());
      if (!e["why"].get<std::string>().empty()) {
        std::printf("  why: %s %s\n", e["why"].get<std::string>().c_str(),
                    to_text(e["why_args"]).c_str());
      }
      for (const auto& b : e["backends"]) {
        std::printf("  backend %-10s %s %s\n",
                    b["state"].get<std::string>().c_str(),
                    b["file"].get<std::string>().c_str(),
                    b["why"].get<std::string>().c_str());
      }
      // What it declares is offered only when it was taken.
      const auto st = e["state"].get<std::string>();
      const bool taken = st == "ready" || st == "partial";
      for (const auto& m : e["models"]) {
        std::printf("  model %s%s\n", m.get<std::string>().c_str(),
                    taken ? "" : " (not offered)");
      }
      for (const auto& k : e["skills"]) {
        std::printf("  skill %s\n", k.get<std::string>().c_str());
      }
      if (e["recognize"].get<std::size_t>() > 0) {
        std::printf("  %zu weight rule(s)\n",
                    e["recognize"].get<std::size_t>());
      }
      for (const auto& w : e["withheld"]) {
        std::printf("  withheld %s: %s (%s)\n",
                    w["item"].get<std::string>().c_str(),
                    w["why"].get<std::string>().c_str(),
                    w["detail"].get<std::string>().c_str());
      }
    }
    return 0;
  }
  if (cmd == "tuning") {
    if (args.empty()) {
      return usage();
    }
    std::string pref = "balanced";
    bool edit = false;
    Json tune = Json::object();
    for (std::size_t i = 1; i < args.size(); ++i) {
      if (args[i] == "--speed") {
        pref = "speed";
      } else if (args[i] == "--quality") {
        pref = "quality";
      } else if (args[i] == "--edit") {
        edit = true;
      } else if (args[i] == "--tune" && i + 1 < args.size()) {
        tune.update(tune_spec(args[++i]));
      } else if ((args[i] == "--turbo" || args[i] == "--lora" ||
                  args[i] == "--dit" || args[i] == "--vae") &&
                 i + 1 < args.size()) {
        const std::string flag = args[i];
        lora_arg(tune, flag, args[++i]);
      } else {
        return usage();
      }
    }
    auto r = c.tuning(args[0], pref, edit, tune);
    if (!r.ok()) {
      return fail(r.error());
    }
    std::cout << to_text(*r, 2) << "\n";
    return 0;
  }
  if (cmd == "outline") {
    if (args.empty()) {
      return usage();
    }
    std::vector<assist::RefKind> row;
    std::string lang;
    for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
      if (args[i] == "--lang") {
        lang = args[i + 1];
      } else if (args[i] == "--row") {
        std::stringstream kinds(args[i + 1]);
        for (std::string k; std::getline(kinds, k, ',');) {
          row.push_back(k == "image"   ? assist::RefKind::Image
                        : k == "video" ? assist::RefKind::Video
                        : k == "audio" ? assist::RefKind::Audio
                                       : assist::RefKind::Other);
        }
      }
    }
    auto t = c.prompt_outline(args[0], row, lang);
    if (!t.ok()) {
      return fail(t.error());
    }
    if (t->empty()) {
      std::fprintf(stderr, "%s has no prompt template\n", args[0].c_str());
      return 1;
    }
    std::cout << *t << "\n";
    return 0;
  }
  if (cmd == "auto") {
    // What the model field's Auto picks, per modality and op.
    for (const auto& a : c.auto_models()) {
      std::printf("  %-6s %-9s -> %-16s (", a.modality.c_str(),
                  a.op.c_str(),
                  a.chosen.empty() ? "(none runs)" : a.chosen.c_str());
      for (std::size_t i = 0; i < a.order.size(); ++i) {
        const auto* m = c.catalog().find(a.order[i]);
        std::printf("%s%s%s", i ? ", " : "", a.order[i].c_str(),
                    m && c.runs(*m, a.modality, a.op) ? "" : "*");
      }
      std::printf(")\n");
    }
    std::printf("  (* cannot run here: not installed, too little RAM, or "
                "no engine graph yet)\n");
    return 0;
  }
  if (cmd == "compose" || cmd == "composition" || cmd == "transition") {
    if (args.empty()) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    if (!pid.ok()) {
      return fail(pid.error());
    }
    if (cmd == "composition") {
      if (args.size() > 1) {
        auto aid = AssetId::parse(args[1]);
        if (!aid) {
          return usage();
        }
        if (auto st = c.set_project_composition(*pid, *aid); !st.ok()) {
          return fail(st.error());
        }
      }
      auto now = c.project_composition(*pid);
      std::printf("%s\n", now ? now->str().c_str() : "(none)");
      return 0;
    }
    if (cmd == "transition") {
      if (args.size() < 5) {
        return usage();
      }
      auto aid = AssetId::parse(args[1]);
      if (!aid) {
        return usage();
      }
      const std::string kind = args.size() > 4 && args[4] != "none"
                                   ? args[4] : "";
      auto st = c.set_transition(*pid, *aid, layer_id(args[2]),
                                 layer_id(args[3]), kind);
      return st.ok() ? 0 : fail(st.error());
    }
    // compose <proj> still|timeline WxH [--rate N[/D]] [--project] [name]
    if (args.size() < 3 || (args[1] != "still" && args[1] != "timeline")) {
      return usage();
    }
    int w = 0, h = 0;
    if (std::sscanf(args[2].c_str(), "%dx%d", &w, &h) != 2) {
      return usage();
    }
    Rational rate{0, 1};
    bool as_project = false;
    std::string name;
    for (std::size_t i = 3; i < args.size(); ++i) {
      if (args[i] == "--rate" && i + 1 < args.size()) {
        long long n = 0, d = 1;
        if (std::sscanf(args[++i].c_str(), "%lld/%lld", &n, &d) < 1) {
          return usage();
        }
        rate = Rational(n, d > 0 ? d : 1);
      } else if (args[i] == "--project") {
        as_project = true;
      } else {
        name = args[i];
      }
    }
    auto made = c.create_composition(
        *pid,
        args[1] == "still" ? project::AssetClass::Still
                           : project::AssetClass::Composition,
        {w, h}, rate, name, as_project);
    if (!made.ok()) {
      return fail(made.error());
    }
    std::printf("%s\n", made->str().c_str());
    return 0;
  }
  if (cmd == "folders" || cmd == "folder" || cmd == "mv" || cmd == "rm" ||
      cmd == "rename" || cmd == "modify" || cmd == "capture" ||
      cmd == "instantiate" || cmd == "land" || cmd == "place" ||
      cmd == "guide" || cmd == "grab") {
    if (args.empty()) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    if (!pid.ok()) {
      return usage();
    }
    auto print_folders = [&] {
      auto fs = c.folders(*pid);
      if (fs.ok()) {
        for (const auto& f : *fs) {
          std::printf("%s  %s\n", jget<std::string>(f, "id", "").c_str(),
                      jget<std::string>(f, "name", "").c_str());
        }
      }
    };
    if (cmd == "folders") {
      print_folders();
      return 0;
    }
    if (cmd == "folder") {
      const std::string op = args.size() > 1 ? args[1] : "";
      Status st = ok_status();
      if (op == "add" && args.size() > 2) {
        auto f = c.create_folder(*pid, args[2]);
        if (!f.ok()) {
          return fail(f.error());
        }
      } else if (op == "rename" && args.size() > 3) {
        st = c.rename_folder(*pid, args[2], args[3]);
      } else if (op == "rm" && args.size() > 2) {
        st = c.delete_folder(*pid, args[2]);
      } else {
        return usage();
      }
      if (!st.ok()) {
        return fail(st.error());
      }
      print_folders();
      return 0;
    }
    auto aid = args.size() > 1 ? AssetId::parse(args[1]) : std::nullopt;
    if (!aid) {
      return usage();
    }
    if (cmd == "rename") {
      if (args.size() < 3) {
        return usage();
      }
      const Status st = c.rename_asset(*pid, *aid, args[2]);
      if (!st.ok()) {
        return fail(st.error());
      }
      return 0;
    }
    if (cmd == "mv" || cmd == "rm") {
      const Status st =
          cmd == "rm" ? c.remove_asset(*pid, *aid)
                      : c.move_asset(*pid, *aid,
                                     args.size() > 2 && args[2] != "top"
                                         ? args[2]
                                         : std::string());
      if (!st.ok()) {
        return fail(st.error());
      }
      return 0;
    }
    if (cmd == "place") {
      auto placed = c.place_in_project(
          *pid, *aid, args.size() > 2 ? layer_id(args[2]) : "");
      if (!placed.ok()) {
        return fail(placed.error());
      }
      std::printf("%s\n", *placed ? (*placed)->str().c_str()
                                   : "not placed: another kind than the "
                                     "project's");
      return 0;
    }
    if (cmd == "instantiate" || cmd == "land") {
      std::optional<AssetId> onto;
      std::optional<std::string> at;
      std::int64_t offset = 0;
      for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--at" && i + 1 < args.size()) {
          at = layer_id(args[++i]);
        } else if (args[i] == "--offset" && i + 1 < args.size()) {
          offset = std::atoll(args[++i].c_str());
        } else {
          onto = AssetId::parse(args[i]);
          if (!onto) {
            return usage();
          }
        }
      }
      auto put = cmd == "land"
                     ? c.place_result(*pid, *aid, onto, at, offset)
                     : c.instantiate(*pid, *aid, onto, at, offset);
      if (!put.ok()) {
        return fail(put.error());
      }
      std::printf("%s layer %s\n", put->first.str().c_str(),
                  put->second.empty() ? "0" : put->second.c_str());
      return 0;
    }
    if (cmd == "guide") {
      double seconds = 0;
      std::string model, name;
      for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--model" && i + 1 < args.size()) {
          model = args[++i];
        } else if (seconds == 0 && std::atof(args[i].c_str()) > 0) {
          seconds = std::atof(args[i].c_str());
        } else {
          name = args[i];
        }
      }
      auto g = c.continuation_guide(*pid, *aid, seconds, model, name);
      if (!g.ok()) {
        return fail(g.error());
      }
      std::printf("%s  %lld frames, %.3f s at %g fps\n",
                  jget<std::string>(*g, "asset", "").c_str(),
                  static_cast<long long>(jget<std::int64_t>(*g, "frames", 0)),
                  jget(*g, "seconds", 0.0), jget(*g, "fps", 0.0));
      return 0;
    }
    Result<AssetId> made = make_error(Code::InvalidArgument, "");
    if (cmd == "grab") {
      if (args.size() < 3) {
        return usage();
      }
      made = c.grab_frame(*pid, *aid, std::atoll(args[2].c_str()),
                          args.size() > 3 ? args[3] : "");
    } else if (cmd == "modify") {
      made = c.derive_modified(*pid, *aid, args.size() > 2 ? args[2] : "");
    } else if (cmd == "capture") {
      std::vector<std::string> layers;
      std::string name;
      for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--layers" && i + 1 < args.size()) {
          std::string list = args[++i];
          for (std::size_t at = 0; at <= list.size();) {
            const auto comma = list.find(',', at);
            const auto end = comma == std::string::npos ? list.size() : comma;
            const std::string id = list.substr(at, end - at);
            layers.push_back(layer_id(id));
            at = end + 1;
          }
        } else {
          name = args[i];
        }
      }
      made = c.capture(*pid, *aid, layers, name);
    }
    if (!made.ok()) {
      return fail(made.error());
    }
    std::printf("%s\n", made->str().c_str());
    return 0;
  }
  if (cmd == "upscale") {
    // A layer's clip rendered larger, at its layer's scale (DESIGN §4f):
    // the layer then shows the result at scale 1.
    if (args.size() < 2) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    auto aid = AssetId::parse(args[1]);
    if (!pid.ok() || !aid) {
      return usage();
    }
    UpscaleRequest req;
    req.project = *pid;
    req.composition = *aid;
    for (std::size_t i = 2; i + 1 < args.size(); ++i) {
      if (args[i] == "--layer") {
        req.layer = layer_id(args[++i]);
      } else if (args[i] == "--model") {
        req.model = args[++i];
      } else if (args[i] == "--seed") {
        req.seed = std::atoll(args[++i].c_str());
      }
    }
    auto j = c.upscale_layer(req);
    if (!j.ok()) {
      return fail(j.error());
    }
    return follow(c, *j);
  }
  if (cmd == "transcribe") {
    // A sound -- a clip's -- transcribed (DESIGN §4h): its speech and
    // the sound events heard, summarized in a text asset, printed.
    if (args.size() < 2) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    auto aid = AssetId::parse(args[1]);
    if (!pid.ok() || !aid) {
      return usage();
    }
    std::string model, language;
    for (std::size_t i = 2; i + 1 < args.size(); ++i) {
      if (args[i] == "--model") {
        model = args[++i];
      } else if (args[i] == "--language") {
        language = args[++i];
      }
    }
    auto made = c.transcribe(*pid, *aid, model, language);
    if (!made.ok()) {
      return fail(made.error());
    }
    if (const int rc = follow(c, made->second); rc != 0) {
      return rc;
    }
    auto text = c.project(*pid)->read_text(made->first);
    if (!text.ok()) {
      return fail(text.error());
    }
    std::printf("%s %s\n%s", made->first.str().c_str(), "transcript",
                text->c_str());
    return 0;
  }
  if (cmd == "summarize") {
    // A clip told scene by scene, then whole (DESIGN §4i): a text asset,
    // printed.
    if (args.size() < 2) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    auto aid = AssetId::parse(args[1]);
    if (!pid.ok() || !aid) {
      return usage();
    }
    std::string model;
    double every = 0;
    for (std::size_t i = 2; i + 1 < args.size(); ++i) {
      if (args[i] == "--model") {
        model = args[++i];
      } else if (args[i] == "--every") {
        every = std::atof(args[++i].c_str());
      }
    }
    auto made = c.summarize_video(*pid, *aid, model, every);
    if (!made.ok()) {
      return fail(made.error());
    }
    if (const int rc = follow(c, made->second); rc != 0) {
      return rc;
    }
    auto text = c.project(*pid)->read_text(made->first);
    if (!text.ok()) {
      return fail(text.error());
    }
    std::printf("%s %s\n%s", made->first.str().c_str(), "summary",
                text->c_str());
    return 0;
  }
  if (cmd == "history") {
    if (args.empty()) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    if (!pid.ok()) {
      return usage();
    }
    auto states = c.history(*pid);
    if (!states.ok()) {
      return fail(states.error());
    }
    for (const auto& s : *states) {
      const auto& e = s.entry;
      const std::time_t t = e.created_ms / 1000;
      char when[32] = "";
      std::strftime(when, sizeof when, "%F %T", std::localtime(&t));
      // A clip: "[clip 39 frames 1.6 s] ".
      const std::string clip =
          e.frames > 0 ? std::format("[clip {} frames {:.1f} s] ", e.frames,
                                     e.seconds)
                       : "";
      std::printf("%s  %-6s %5dx%-5d %s%s%s  %s\n    %s\n", when,
                  e.role.c_str(), e.width, e.height, clip.c_str(),
                  e.rendered ? "[as sent] " : "",
                  std::string(utf8_prefix(e.label, 48)).c_str(),
                  e.generation.str().c_str(), s.file.c_str());
    }
    if (states->empty()) {
      std::printf("(no history yet)\n");
    }
    return 0;
  }
  // Prompts as assets (DESIGN §10c): listed, captured, changed in place
  // while nothing has been made from them.
  if (cmd == "prompts") {
    if (args.empty()) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    if (!pid.ok()) {
      return fail(pid.error());
    }
    project::Project* p = c.project(*pid);
    auto all = p->assets();
    if (!all.ok()) {
      return fail(all.error());
    }
    for (const auto& a : *all) {
      if (!Controller::is_prompt(a)) {
        continue;
      }
      auto deps = p->dependents(a.id, false);
      auto t = p->read_text(a.id);
      std::printf("%s  v%u  %zu made  %s\n", a.id.str().c_str(), a.head,
                  deps.ok() ? deps->size() : 0,
                  t.ok() ? one_line(*t).c_str() : "?");
    }
    return 0;
  }
  if (cmd == "prompt") {
    if (args.size() < 3) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    if (!pid.ok()) {
      return fail(pid.error());
    }
    if (args[1] == "add") {
      std::string name;
      if (args.size() > 4 && args[3] == "--name") {
        name = args[4];
      }
      auto a = c.capture_prompt(*pid, args[2], {}, {}, std::nullopt, name);
      if (!a.ok()) {
        return fail(a.error());
      }
      std::cout << a->str() << "\n";
      return 0;
    }
    auto id = AssetId::parse(args[2]);
    if (!id) {
      return usage();
    }
    if (args[1] == "show") {
      auto t = c.project(*pid)->read_text(*id);
      if (!t.ok()) {
        return fail(t.error());
      }
      std::cout << *t << "\n";
      return 0;
    }
    if (args[1] == "set" && args.size() > 3) {
      if (auto st = c.set_prompt_text(*pid, *id, args[3]); !st.ok()) {
        return fail(st.error());
      }
      return 0;
    }
    return usage();
  }
  if (cmd == "recipe") {
    if (args.size() < 2) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    auto aid = AssetId::parse(args[1]);
    if (!pid.ok() || !aid) {
      return usage();
    }
    project::Project* p = c.project(*pid);
    auto a = p ? p->asset(*aid) : Result<project::Asset>(make_error(
                                      Code::NotFound, "project not open"));
    if (!a.ok()) {
      return fail(a.error());
    }
    auto r = p->recipe(a->recipe);
    if (!r.ok()) {
      return fail(r.error());
    }
    Json j = *r;
    // How long its current version took to make, phase by phase.
    if (auto v = p->version(*aid); v.ok()) {
      if (!v->timing.empty()) {
        j["timing"] = v->timing;
      }
      // What it made beside its file: a song's score.
      if (!v->outputs.empty()) {
        j["outputs"] = v->outputs;
      }
    }
    std::cout << to_text(j, 2) << "\n";
    return 0;
  }
  if (cmd == "thumb") {
    if (args.size() < 2) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    auto aid = AssetId::parse(args[1]);
    if (!pid.ok() || !aid) {
      return usage();
    }
    int px = args.size() > 2 ? std::atoi(args[2].c_str()) : 256;
    auto t = c.thumbnail(*pid, *aid, px);
    if (!t.ok()) {
      return fail(t.error());
    }
    std::cout << t->string() << "\n";
    return 0;
  }
  if (cmd == "generate") {
    if (args.size() < 2) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    if (!pid.ok()) {
      return fail(pid.error());
    }
    GenerateImageRequest req;
    req.project = *pid;
    if (auto st = prompt_arg(c, *pid, args[1], &req.prompt,
                             &req.prompt_asset);
        !st.ok()) {
      return fail(st.error());
    }
    for (std::size_t i = 2; i < args.size(); ++i) {
      const std::string& a = args[i];
      auto next = [&]() -> std::string {
        return i + 1 < args.size() ? args[++i] : std::string();
      };
      if (a == "--model") {
        req.model = next();
      } else if (a == "--steps") {
        req.steps = std::atoi(next().c_str());
      } else if (a == "--seed") {
        req.seed = std::atoll(next().c_str());
      } else if (a == "--prompt-name") {
        req.prompt_name = next();
      } else if (a == "--size") {
        std::string s = next();
        std::sscanf(s.c_str(), "%dx%d", &req.width, &req.height);
      } else if (a == "--no-preview") {
        req.preview = false;
      } else if (a == "--speed") {
        req.preference = "speed";
      } else if (a == "--quality") {
        req.preference = "quality";
      } else if (a == "--tune") {
        req.tuning.update(tune_spec(next()));
      } else if (a == "--turbo" || a == "--lora" || a == "--dit" ||
                 a == "--vae") {
        lora_arg(req.tuning, a, next());
      } else if (a == "--mode") {
        req.mode = next();
      } else if (a == "--adjust") {
        // "exposure=0.4,vibrance=0.3"
        Json j = Json::object();
        std::string spec = next();
        std::size_t at = 0;
        while (at < spec.size()) {
          std::size_t end = spec.find(',', at);
          if (end == std::string::npos) { end = spec.size(); }
          const std::string kv = spec.substr(at, end - at);
          const std::size_t eq = kv.find('=');
          if (eq != std::string::npos) {
            j[kv.substr(0, eq)] = std::atof(kv.c_str() + eq + 1);
          }
          at = end + 1;
        }
        req.base_adjust = media::adjustments_from_json(j);
      } else if (a == "--ref" || a == "--base") {
        auto r = reference_asset(c, *pid, next());
        if (!r.ok()) {
          return fail(r.error());
        }
        if (a == "--base") {
          req.base = *r;
        } else {
          req.references.push_back(*r);
        }
      }
    }
    auto j = c.generate_image(req);
    if (!j.ok()) {
      return fail(j.error());
    }
    return follow(c, *j);
  }
  if (cmd == "video") {
    if (args.size() < 2) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    if (!pid.ok()) {
      return fail(pid.error());
    }
    GenerateVideoRequest req;
    req.project = *pid;
    if (auto st = prompt_arg(c, *pid, args[1], &req.prompt,
                             &req.prompt_asset);
        !st.ok()) {
      return fail(st.error());
    }
    double seconds = 0;
    for (std::size_t i = 2; i < args.size(); ++i) {
      const std::string& a = args[i];
      auto next = [&]() -> std::string {
        return i + 1 < args.size() ? args[++i] : std::string();
      };
      if (a == "--model") {
        req.model = next();
      } else if (a == "--size") {
        std::string sz = next();
        std::sscanf(sz.c_str(), "%dx%d", &req.width, &req.height);
      } else if (a == "--frames") {
        req.frames = std::atoi(next().c_str());
      } else if (a == "--seconds") {
        seconds = std::atof(next().c_str());
      } else if (a == "--fps") {
        req.fps = std::atof(next().c_str());
      } else if (a == "--steps") {
        req.steps = std::atoi(next().c_str());
      } else if (a == "--speed") {
        req.preference = "speed";
      } else if (a == "--quality") {
        req.preference = "quality";
      } else if (a == "--seed") {
        req.seed = std::atoll(next().c_str());
      } else if (a == "--prompt-name") {
        req.prompt_name = next();
      } else if (a == "--no-turbo") {
        req.turbo = false;
      } else if (a == "--tune") {
        req.tuning.update(tune_spec(next()));
      } else if (a == "--turbo" || a == "--lora" || a == "--dit" ||
                 a == "--vae") {
        lora_arg(req.tuning, a, next());
      } else if (a == "--first" || a == "--ref" || a == "--continue") {
        auto r = reference_asset(c, *pid, next());
        if (!r.ok()) {
          return fail(r.error());
        }
        if (a == "--first") {
          req.first = *r;
        } else if (a == "--ref") {
          req.references.push_back(*r);
        } else {
          req.continue_from = *r;
        }
      } else if (a == "--tail-seconds") {
        req.tail_seconds = std::atof(next().c_str());
      } else if (a == "--song-sound") {
        req.reference_sound = true;
      } else {
        return usage();
      }
    }
    if (seconds > 0 && req.frames <= 0) {
      req.frames = static_cast<int>(
          seconds * (req.fps > 0 ? req.fps : 24.0) + 0.5);
    }
    auto j = c.generate_video(req);
    if (!j.ok()) {
      return fail(j.error());
    }
    return follow(c, *j);
  }
  if (cmd == "audio") {
    if (args.size() < 2) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    if (!pid.ok()) {
      return fail(pid.error());
    }
    GenerateAudioRequest req;
    req.project = *pid;
    if (auto st = prompt_arg(c, *pid, args[1], &req.prompt,
                             &req.prompt_asset);
        !st.ok()) {
      return fail(st.error());
    }
    auto read_text = [](const std::string& path) -> Result<std::string> {
      std::ifstream in(path, std::ios::binary);
      if (!in) {
        return make_error(Code::Io, std::format("cannot read {}", path));
      }
      std::ostringstream ss;
      ss << in.rdbuf();
      return ss.str();
    };
    for (std::size_t i = 2; i < args.size(); ++i) {
      const std::string& a = args[i];
      auto next = [&]() -> std::string {
        return i + 1 < args.size() ? args[++i] : std::string();
      };
      if (a == "--model") {
        req.model = next();
      } else if (a == "--lyrics" || a == "--score") {
        auto t = read_text(next());
        if (!t.ok()) {
          return fail(t.error());
        }
        (a == "--lyrics" ? req.lyrics : req.score) = *t;
      } else if (a == "--plan") {
        req.plan = next();
      } else if (a == "--max-seconds") {
        req.max_seconds = std::atof(next().c_str());
      } else if (a == "--steps") {
        req.steps = std::atoi(next().c_str());
      } else if (a == "--speed") {
        req.preference = "speed";
      } else if (a == "--quality") {
        req.preference = "quality";
      } else if (a == "--seed") {
        req.seed = std::atoll(next().c_str());
      } else if (a == "--prompt-name") {
        req.prompt_name = next();
      } else if (a == "--tune") {
        req.tuning.update(tune_spec(next()));
      } else if (a == "--vae") {
        lora_arg(req.tuning, a, next());
      } else if (a == "--voice" || a == "--ref") {
        // A voice to speak in (MOSS-TTS clones it): a sound, or a clip.
        auto r = reference_asset(c, *pid, next(), project::Placement::Auto);
        if (!r.ok()) {
          return fail(r.error());
        }
        req.row.push_back(*r);
        if (a == "--voice") {
          req.voice = *r;
        }
      } else if (a == "--seconds") {
        req.seconds = std::atof(next().c_str());
      } else {
        return usage();
      }
    }
    auto j = c.generate_audio(req);
    if (!j.ok()) {
      return fail(j.error());
    }
    return follow(c, *j);
  }
  if (cmd == "export") {
    if (!args.empty() && args[0] == "--formats") {
      for (const auto& f : engine::export_formats()) {
        std::printf("  %-12s .%-4s %-7s %s\n", std::string(f.name).c_str(),
                    std::string(f.extension).c_str(),
                    f.sound ? "sound" : f.video ? "video" : "picture",
                    std::string(f.description).c_str());
      }
      return 0;
    }
    if (args.size() < 2) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    if (!pid.ok()) {
      return fail(pid.error());
    }
    ExportRequest req;
    req.project = *pid;
    for (std::size_t i = 2; i < args.size(); ++i) {
      const std::string& a = args[i];
      auto next = [&]() -> std::string {
        return i + 1 < args.size() ? args[++i] : std::string();
      };
      if (a == "--format") {
        req.format = next();
      } else if (a == "-o" || a == "--output") {
        req.destination = fs::absolute(next());
      } else if (a == "--quality") {
        req.quality = std::atoi(next().c_str());
      } else if (a == "--bitrate" || a == "--max-bitrate") {
        // Bits per second: "20M", "800k", "2500000".
        const std::string v = next();
        double n = std::atof(v.c_str());
        if (!v.empty() && (v.back() == 'M' || v.back() == 'm')) n *= 1e6;
        if (!v.empty() && (v.back() == 'k' || v.back() == 'K')) n *= 1e3;
        (a == "--bitrate" ? req.video.bitrate : req.video.max_bitrate) =
            static_cast<std::int64_t>(std::llround(n));
      } else if (a == "--video-quality") {
        req.video.quality = std::atof(next().c_str());
      } else if (a == "--keyframes") {
        req.video.keyframe_seconds = std::atof(next().c_str());
      } else if (a == "--b-frames") {
        req.video.b_frames = next() == "on" ? 1 : 0;
      } else if (a == "--profile") {
        req.video.profile = next();
      } else if (a == "--level") {
        req.video.level = next();
      } else if (a == "--entropy") {
        req.video.entropy = next();
      } else if (a == "--prores") {
        req.video.prores = next();
      }
    }
    if (req.format.empty() || req.destination.empty()) {
      return usage();
    }
    auto asset = reference_asset(c, *pid, args[1], project::Placement::Auto);
    if (!asset.ok()) {
      return fail(asset.error());
    }
    req.asset = *asset;
    auto j = c.export_asset(req);
    if (!j.ok()) {
      return fail(j.error());
    }
    return follow(c, *j);
  }
  if (cmd == "enhance") {
    if (args.empty()) {
      return usage();
    }
    EnhancePromptRequest req;
    req.prompt = args[0];
    // The suggestion each time it grows (assist.partial), as the app
    // shows it while the assistant writes.
    bool partial = false;
    int repeat = 1;
    // Pictures need the project they are imported into: find it first.
    for (std::size_t i = 1; i + 1 < args.size(); ++i) {
      if (args[i] == "--project") {
        auto pid = open_or_fail(c, args[i + 1]);
        if (!pid.ok()) {
          return fail(pid.error());
        }
        req.project = *pid;
      }
    }
    for (std::size_t i = 1; i < args.size(); ++i) {
      const std::string& a = args[i];
      auto next = [&]() -> std::string {
        return i + 1 < args.size() ? args[++i] : std::string();
      };
      if (a == "--video") {
        req.target = "video";
      } else if (a == "--audio") {
        req.target = "audio";
      } else if (a == "--model") {
        req.model = next();
      } else if (a == "--mode") {
        req.mode = next();
      } else if (a == "--size") {
        req.size = next();
      } else if (a == "--seconds") {
        req.seconds = std::atof(next().c_str());
      } else if (a == "--frames") {
        req.frames = std::atoi(next().c_str());
      } else if (a == "--song-sound") {
        req.reference_sound = true;
      } else if (a == "--partial") {
        partial = true;
      } else if (a == "--no-mtp") {
        // The plain decode, to compare with the MTP head's.
        c.set_assistant_mtp(false);
      } else if (a == "--assistant") {
        // Another catalog assistant than the tier's pick.
        c.set_assistant(next());
      } else if (a == "--repeat") {
        // Asked again in this process: the second time is the helper
        // kept loaded (its keep_loaded), as the app asks it.
        repeat = std::max(1, std::atoi(next().c_str()));
      } else if (a == "--project") {
        next();
      } else if (a == "--ref" || a == "--base" || a == "--first" ||
                 a == "--continue") {
        if (!req.project) {
          return fail(make_error(Code::InvalidArgument,
                                 a + " needs --project <project.valtz>"));
        }
        auto r = reference_asset(c, *req.project, next());
        if (!r.ok()) {
          return fail(r.error());
        }
        if (a == "--base" || a == "--first") {
          req.base = *r;
        } else if (a == "--continue") {
          req.continue_from = *r;
          req.references.push_back(*r);
        } else {
          req.references.push_back(*r);
        }
      }
    }
    // A song or a clip is written for the model that makes it: Auto's,
    // unless named -- a clip from references, Auto's Ref2VA.
    if (req.target != "image" && req.model.empty()) {
      req.model = "auto";
    }
    int rc = 0;
    for (int run = 1; run <= repeat; ++run) {
      const auto t0 = std::chrono::steady_clock::now();
      double first = -1;
      const auto since = [&] {
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - t0)
            .count();
      };
      auto j = c.enhance_prompt(req);
      if (!j.ok()) {
        return fail(j.error());
      }
      for (bool done = false; !done;) {
        Event ev;
        if (!c.events().wait(ev, 1000) || ev.job != *j) {
          continue;
        }
        if (ev.kind == "job.text") {
          if (first < 0) {
            first = since();
          }
          std::cerr << jget<std::string>(ev.data, "text", "");
          continue;
        }
        if (ev.kind == "assist.partial" && !partial) {
          continue;
        }
        std::cout << "\n" << ev.to_json() << "\n";
        if (ev.kind == "job.failed") {
          rc = 1;
        }
        done = ev.kind == "job.finished" || ev.kind == "job.failed" ||
               ev.kind == "job.cancelled";
      }
      if (repeat > 1) {
        std::fprintf(stderr, "\nrun %d: first text after %.1f s, done "
                             "after %.1f s\n", run, first, since());
      }
    }
    return rc;
  }
  if (cmd == "intent") {
    if (args.empty()) {
      return usage();
    }
    std::vector<assist::Attachment> atts;
    for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
      if (args[i] == "--image") {
        atts.push_back({"image", args[i + 1], ""});
      }
    }
    auto j = c.detect_intent(args[0], atts);
    if (!j.ok()) {
      return fail(j.error());
    }
    // The heuristic answer is posted synchronously.
    Event ev;
    while (c.events().wait(ev, 100)) {
      if (ev.job == *j) {
        std::cout << ev.to_json() << "\n";
        break;
      }
    }
    const auto* a = c.assistant_model();
    if (!a) {
      return 0;
    }
    return follow(c, *j);
  }
  if (cmd == "output") {
    // output <proj> [color=rec2020] [fps=30|30000/1001] [channels=1|2]
    // [rate=44100]: the project's output (DESIGN §6b), shown -- or set.
    if (args.empty()) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    if (!pid.ok()) {
      return usage();
    }
    auto o = c.project_output(*pid);
    if (!o.ok()) {
      return fail(o.error());
    }
    project::OutputSettings want = *o;
    for (std::size_t i = 1; i < args.size(); ++i) {
      const auto eq = args[i].find('=');
      if (eq == std::string::npos) {
        return usage();
      }
      const std::string k = args[i].substr(0, eq);
      const std::string v = args[i].substr(eq + 1);
      if (k == "color") {
        want.color = v;
      } else if (k == "fps") {
        const auto slash = v.find('/');
        want.fps = slash == std::string::npos
                       ? Rational(std::atoll(v.c_str()), 1)
                       : Rational(std::atoll(v.substr(0, slash).c_str()),
                                  std::atoll(v.substr(slash + 1).c_str()));
      } else if (k == "channels") {
        want.channels = std::atoi(v.c_str());
      } else if (k == "rate") {
        want.sample_rate = std::atoi(v.c_str());
      } else {
        return usage();
      }
    }
    if (!(want == *o)) {
      if (auto st = c.set_project_output(*pid, want); !st.ok()) {
        return fail(st.error());
      }
    }
    std::printf("%s\n", to_text(Json(want)).c_str());
    return 0;
  }
  if (cmd == "camera") {
    // camera --sources | camera <proj> [--still | --seconds s]
    // [--camera id]: a still (the default) or a clip taken with a camera,
    // a flat asset of the project (DESIGN §7b).
    const Json src = c.camera_sources();
    if (!args.empty() && args[0] == "--sources") {
      for (const auto& s : jget(src, "cameras", Json::array())) {
        std::printf("  %s%s  %s\n", jget<std::string>(s, "name", "").c_str(),
                    jget(s, "preferred", false) ? " (default)" : "",
                    jget<std::string>(s, "id", "").c_str());
      }
      std::printf("  camera permission: %s\n",
                  jget<std::string>(src, "camera", "").c_str());
      return 0;
    }
    if (args.empty()) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    if (!pid.ok()) {
      return usage();
    }
    std::string camera;
    double seconds = 0;
    for (std::size_t i = 1; i < args.size(); ++i) {
      if (args[i] == "--camera" && i + 1 < args.size()) {
        camera = args[++i];
      } else if (args[i] == "--seconds" && i + 1 < args.size()) {
        seconds = std::atof(args[++i].c_str());
      }
    }
    if (jget<std::string>(src, "camera", "") == "undetermined") {
      media::request_camera_access();
    }
    if (auto st = c.start_camera(*pid, camera, seconds > 0); !st.ok()) {
      return fail(st.error());
    }
    // Its first frames, and a moment for its exposure to settle.
    for (int i = 0; i < 100 && jget(c.camera_state(), "frames", 0) < 15;
         ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    Result<AssetId> made = make_error(Code::InvalidArgument, "");
    if (seconds > 0) {
      if (auto st = c.camera_record(); !st.ok()) {
        return fail(st.error());
      }
      for (double t = 0; t < seconds; t += 0.25) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        std::fprintf(stderr, "\r  recording %.1f s",
                     jget(c.camera_state(), "seconds", 0.0));
      }
      std::fprintf(stderr, "\n");
      made = c.camera_stop_recording();
    } else {
      made = c.camera_snap();
    }
    (void)c.stop_camera();
    if (!made.ok()) {
      return fail(made.error());
    }
    std::printf("%s %s\n", seconds > 0 ? "recorded" : "took",
                made->str().c_str());
    return 0;
  }
  if (cmd == "record") {
    // record --sources | record <proj> [<sound-comp>] [--source id]
    // [--seconds s]: a microphone or the system's audio, into the
    // composition -- or, with none, the asset list (DESIGN §7b).
    const Json src = c.capture_sources();
    if (!args.empty() && args[0] == "--sources") {
      for (const auto& s : jget(src, "sources", Json::array())) {
        std::printf("  %-9s %s%s  %s\n",
                    jget<std::string>(s, "kind", "").c_str(),
                    jget<std::string>(s, "name", "").c_str(),
                    jget(s, "preferred", false) ? " (default)" : "",
                    jget<std::string>(s, "id", "").c_str());
      }
      std::printf("  microphone permission: %s\n",
                  jget<std::string>(src, "microphone", "").c_str());
      return 0;
    }
    if (args.empty()) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    if (!pid.ok()) {
      return usage();
    }
    std::optional<AssetId> aid;
    std::size_t first = 1;
    if (args.size() > 1 && args[1].rfind("--", 0) != 0) {
      aid = AssetId::parse(args[1]);
      if (!aid) {
        return usage();
      }
      first = 2;
    }
    std::string source;
    double seconds = 5;
    for (std::size_t i = first; i + 1 < args.size(); ++i) {
      if (args[i] == "--source") {
        source = args[++i];
      } else if (args[i] == "--seconds") {
        seconds = std::atof(args[++i].c_str());
      }
    }
    if (source.empty()) {
      // The default microphone.
      for (const auto& s : jget(src, "sources", Json::array())) {
        if (source.empty() && jget<std::string>(s, "kind", "") ==
                                  "microphone") {
          source = jget<std::string>(s, "id", "");
        }
      }
    }
    if (auto st = c.start_capture(*pid, aid, source); !st.ok()) {
      return fail(st.error());
    }
    for (double t = 0; t < seconds; t += 0.25) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      const Json s = c.capture_state();
      std::fprintf(stderr, "\r  recording %.1f s  level %-20s",
                   jget(s, "seconds", 0.0),
                   std::string(static_cast<std::size_t>(
                                   20 * jget(s, "level", 0.0)), '#')
                       .c_str());
    }
    std::fprintf(stderr, "\n");
    auto made = c.stop_capture();
    if (!made.ok()) {
      return fail(made.error());
    }
    std::printf("recorded %s\n", made->str().c_str());
    return 0;
  }
  if (cmd == "quantize") {
    if (args.empty()) {
      return usage();
    }
    auto j = c.quantize_model(args[0]);
    if (!j.ok()) {
      return fail(j.error());
    }
    return follow(c, *j);
  }
  if (cmd == "fleet") {
    return fleet_command(c, args);
  }
  if (cmd == "download") {
    // A gated model's token, asked for at the terminal without echo --
    // never an argument, which the shell's history would keep.
    std::string token;
    if (std::erase(args, std::string("--hf-token")) > 0) {
      char buf[512];
      if (!readpassphrase("Hugging Face access token: ", buf, sizeof buf,
                          RPP_REQUIRE_TTY)) {
        return fail(make_error(Code::InvalidArgument,
                               "no terminal to ask for the token on"));
      }
      token = buf;
      std::fill(std::begin(buf), std::end(buf), '\0');
    }
    if (args.empty()) {
      return usage();
    }
    auto j = c.download_model(args[0], token);
    std::fill(token.begin(), token.end(), '\0');
    if (!j.ok()) {
      return fail(j.error());
    }
    return follow(c, *j);
  }
  return usage();
}
