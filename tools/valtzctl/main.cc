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
//                  [--last <picture>] [--tune k=v,...]
//   valtzctl tuning <model-id> [--speed|--quality] [--edit]
//                   [--tune k=v,...]          Favor's options
//   valtzctl enhance <prompt> [--video [--frames n] | --audio
//                    [--seconds s]] [--partial]   the suggestion as it
//                    is written
//   valtzctl intent <text> [--image name]...
//   valtzctl download <model-id> [--from huggingface|modelscope]
//   valtzctl sources [<name>]       where downloads come from
//   valtzctl wired [auto|<GB>]      the wired memory limit
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
#include <time.h>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
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
      "  link <model-id> <file|folder> | unlink <model-id> [--drawthings]\n"
      "         (a model kept somewhere else -- ComfyUI's, Draw Things',\n"
      "         anywhere -- used from there; its files checked first: the\n"
      "         model found in or around the folder linked, a wrong or\n"
      "         broken one refused. Draw Things .ckpts of its DiT, text\n"
      "         encoder, VAE -- Qwen-Image 2.1, MiniMax H3; a folder of\n"
      "         them -- link those parts alone, the configs and tokenizer\n"
      "         where the model is; --drawthings undoes that)\n"
      "  check <model-id> [file|folder]  (its files checked as a link's\n"
      "         are -- that path's, else where it is installed)\n"
      "  compress <model-id> --bits 8|4 [--keep-original]\n"
      "         (Settings > Capabilities' compress button: the download\n"
      "         replaced by a copy quantized to 8 or 4 bits, its DiT then\n"
      "         its text encoder; download it again for the original.\n"
      "         --keep-original: the copy in use BESIDE the download, to\n"
      "         try first)\n"
      "  compress <model-id> --use compressed|original\n"
      "  compress <model-id> --keep compressed|original\n"
      "         (the two kept: which one runs; one kept, the other\n"
      "         deleted)\n"
      "  assistant [<model-id>|auto] [--keep <seconds>]\n"
      "            [--drafter mtp|dflash] [--bits 8|4]\n"
      "            [--video-every <seconds>|<n>/s|auto]\n"
      "            [--video-size standard|high|highest]\n"
      "            [--context <tokens>|auto]\n"
      "         (Settings > Agentic Helper: the helpers, which is chosen\n"
      "         and which runs, each one's sampler; with an id, that one\n"
      "         chosen -- kept for the app and every later run; --keep:\n"
      "         how long it stays loaded after a request, 0 not at all;\n"
      "         --drafter: what it drafts with -- its MTP head, or a\n"
      "         DFlash 2 drafter held at --bits; --video-every: a video\n"
      "         summary's seconds a frame (0.125-60, or 8/s, 4/s, ...),\n"
      "         auto: by the clip's length, about 80 frames of a short\n"
      "         one -- 40 where memory moves under 200 GB/s -- one a\n"
      "         second (every 2) of a long one; --video-size: its\n"
      "         frames' size, standard 576x320, high 864x480, highest\n"
      "         1152x640 (portrait: turned); --context: its context at\n"
      "         most -- auto, its own as this Mac's memory holds it)\n"
      "  status [--thermal]   (the machine's load, as the app's status bar\n"
      "         shows it: ANE, GPU, memory; --thermal adds whether the GPU\n"
      "         is held back, from a 600 ms sample)\n"
      "  <command> ... --memory   (as it ends: what the engine holds in\n"
      "         memory -- the models kept warm, checkpoints, K/V -- as the\n"
      "         status bar's RAM popover shows it)\n"
      "  probe <file>\n"
      "  new <project.valtz> [name]\n"
      "  info <project.valtz>\n"
      "  paths\n"
      "  locations [projects|models|cache <dir>|--default [--move|--keep|\n"
      "         --drop]]   (Settings > General > Storage: where the projects,\n"
      "         models and cache are kept -- the models moved there, or kept\n"
      "         where they are and still used; the cache moved, kept or\n"
      "         dropped. A move across drives is copied, counted, then the\n"
      "         old files deleted)\n"
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
      "         cut|dissolve|dip-black|dip-white|none  (where two layers\n"
      "         overlap in time; from ends first)\n"
      "  flatten <project.valtz> <asset-id> --asset [name]  (a flat asset\n"
      "         of it, as drawn: a picture, a movie, a sound)\n"
      "  history <project.valtz>  (the generation history: each edit's\n"
      "          base as the model got it, each result, oldest first)\n"
      "  atlas <project.valtz> [--json]  (the project as a graph, left to\n"
      "         right: each asset, the op that made it from its inputs,\n"
      "         a composition's layers into it; column by column, leaves\n"
      "         -- removable -- marked)\n"
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
      "         between -- or a key's interp=hold|spline on to the\n"
      "         next, adjust, crop and rotate; adjust keys as for\n"
      "         adjust; crop keys offset_x,\n"
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
      "           [--speed|--quality] [--tune k=v,...] [--repeat n]\n"
      "           [--base <file|asset-id>] [--ref <file|asset-id>]...\n"
      "           [--mode auto|edit|generate] [--adjust k=v,...]\n"
      "           (files are imported first; --base is the picture to\n"
      "           edit, resampled to the output; --ref pictures are\n"
      "           drawn from as they are -- with only refs, the model\n"
      "           composes a new picture from them; --adjust lays the\n"
      "           Adjust panel on the base first: exposure (stops),\n"
      "           contrast, highlights, shadows, vibrance, saturation,\n"
      "           temperature, tint (-1..1), on a RAW's development;\n"
      "           --repeat makes it n times in this process, the seed\n"
      "           one up each time, each one's phases timed -- the\n"
      "           later ones as the app makes them, kernels tuned)\n"
      "  video <project.valtz> <prompt> [--model id] [--size WxH]\n"
      "        [--frames n | --seconds s] [--fps n] [--steps n]\n"
      "        [--speed|--quality] [--seed n] [--no-turbo]\n"
      "        [--first <file|asset-id> [--last <file|asset-id>]]\n"
      "        [--tune k=v,...]\n"
      "        [--ref <file|asset-id> [--whole]]... [--continue <clip>]\n"
      "        [--tail-seconds s] [--song-sound]\n"
      "        (a clip with its soundtrack, MiniMax H3; the length goes\n"
      "        up to one the model makes -- 17n+5 frames -- and the Turbo\n"
      "        LoRA, when installed, runs 4/6/8 steps; --first is a picture\n"
      "        the clip opens on, cropped to fill the frame, --last one it\n"
      "        ends on (FL2VA: Picture 1 and Picture 2). --ref: a\n"
      "        picture, clip or sound to draw on (Ref2VA: a subject, a\n"
      "        motion, a voice, a song; name them <Picture 1>, <Video 1>,\n"
      "        <Audio 1> in the prompt, numbered as the job lists them --\n"
      "        a clip or a sound cut to the clip's length, or --whole);\n"
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
      "  audio <project.valtz> <prompt> --model minimax-music3\n"
      "        [--max-seconds s] [--steps n] [--speed|--quality] [--seed n]\n"
      "        (a song, MiniMax-Music3: the prompt is its description --\n"
      "        a line, or its three sections: Global Metadata, Vocal\n"
      "        Details, Arrangement, kept line by line -- then lyrics\n"
      "        under [Verse] / [Chorus] headers, always: music without\n"
      "        words is headers alone, [Instrumental]. No score: no\n"
      "        --plan, no --score. 44.1 kHz, up to 6 min; Fast and Med\n"
      "        hold its flow matching in int8 on an M5. A WAV)\n"
      "  audio <project.valtz> <prompt> --model moss-tts-v1.5[-w8g64]\n"
      "        [--voice <file|asset>] [--seconds s] [--seed n]\n"
      "        (SPEECH, MOSS-TTS: the prompt is its fields -- lines\n"
      "        \"Instruction: ...\", \"Quality: ...\", \"Sound event: ...\",\n"
      "        \"Ambient sound: ...\", \"Language: ...\" -- then a blank\n"
      "        line and the words, \"[pause 1.5s]\" a pause. --voice: a\n"
      "        sound (or a clip) whose voice it speaks in, cloned from up\n"
      "        to 12 s of it -- with one, Auto picks a speech model.\n"
      "        --seconds: about how long. A 24 kHz mono WAV)\n"
      "  audio <project.valtz> <script> --model drama-3[-int4]\n"
      "        [--voice <file|asset> [--voice-text <words>]]... [--seed n]\n"
      "        (a SCRIPT performed, Fish Audio Drama 3: directions in\n"
      "        brackets where they act -- \"[sighs]\", \"<whisper>\" round\n"
      "        words; a scene a line a turn, \"Name: [direction] line\",\n"
      "        the names its speakers in the order they first speak. Each\n"
      "        --voice is the next speaker's, cloned from its sound and the\n"
      "        words said in it: --voice-text, else its transcript -- Valtz\n"
      "        transcribes it first when it has none. A 44.1 kHz mono WAV)\n"
      "  record --sources | record <project.valtz> [<sound-comp-id>]\n"
      "        [--source id] [--seconds s]  (a microphone -- the default\n"
      "        one without --source -- or the system's audio, \"system\",\n"
      "        recorded into a composition of sound alone: a flat sound,\n"
      "        on its blank layer or a new one; with none, a flat sound\n"
      "        alone; --voice-over <frame> onto any timeline as a new\n"
      "        layer from that frame, --cue-after <s> the picture as if\n"
      "        started s seconds in: the head before it trimmed)\n"
      "  output <project.valtz> [color=rec709|srgb|display-p3|rec2020|\n"
      "        rec2100-pq|rec2100-hlg] [fps=24|30000/1001] [channels=1|2]\n"
      "        [rate=48000]\n"
      "        [intermediate=prores4444xq|prores4444|prores422hq|prores422|\n"
      "        hevc]  (the project's output: what exporting it writes; fps\n"
      "        is its timeline's, set while that is empty; intermediate how\n"
      "        the clips made in it are kept -- ProRes 422 HQ unless set)\n"
      "  camera --sources | camera <project.valtz> [--seconds s]\n"
      "        [--camera id]  (a still -- or, with --seconds, a clip with\n"
      "        the microphone's sound -- taken with a camera: a flat asset)\n"
      "  quantize <model-id>  (a quantized variant made from its source,\n"
      "        as Settings > Capabilities' Quantize: moss-tts-v1.5-w8g64\n"
      "        from moss-tts-v1.5, w8 group-64, into the models folder)\n"
      "  song <project.valtz> <text>  (the prompt as generate_audio reads\n"
      "        it: its style and lyrics)\n"
      "  outline <model> [--row image,video,audio] [--lang zh-Hans]\n"
      "        [--first n|none] [--last n|none] [--seconds s]\n"
      "        (the model's prompt template -- what an empty prompt box\n"
      "        starts from -- written for a reference row holding those\n"
      "        kinds, in order: each named by its tag where it belongs;\n"
      "        a clip's opening and closing pictures by their places in\n"
      "        the row, as the model takes them unless given)\n"
      "  prompts <project.valtz>  (the prompts: each made from, how much)\n"
      "  prompt <project.valtz> add <text> [--name N] | show <id>\n"
      "         | set <id> <text>\n"
      "         | apply <before> <enhanced> [--from <id>] [--for MODEL]\n"
      "           [--name N]  (a suggestion applied, as the app's Apply:\n"
      "           the words before it kept -- the box's prompt --from --\n"
      "           and the suggestion a prompt made from them, its\n"
      "           upstream; prints both)\n"
      "         (a prompt as an asset: its media named by POSITION in\n"
      "         the reference row -- <valtz_ref_img_0> the first picture,\n"
      "         _vid_ a clip, _aud_ a sound -- and each model told its\n"
      "         own names for them; set is refused once something was\n"
      "         made from it. @<id> in place of any prompt uses it;\n"
      "         --prompt-name N on generate, video and audio names the\n"
      "         prompt captured, as --name here: the person's name, kept\n"
      "         as its words change)\n"
      "  classify <file|folder>...  (lora, dit or vae: how a drop is filed)\n"
      "  ASSET OPERATIONS (one protocol: what is done to an asset with\n"
      "  no prompt -- a task, new assets made from it):\n"
      "  ops   (each, what it takes, whether it runs here and on what)\n"
      "  transcribe <project.valtz> <sound|clip-id> [--model ID]\n"
      "         [--language L]  (its speech line by line, the sound\n"
      "         events heard: a text asset made from it, printed)\n"
      "  summarize <project.valtz> <clip-id> [--model ID]\n"
      "         [--every S|<n>/s] [--size standard|high|highest]\n"
      "         (watched by the helper -- a frame every S seconds, at\n"
      "         that size, the settings' by default -- scene by scene,\n"
      "         then whole: a text asset made from it, printed)\n"
      "  separate <project.valtz> <sound-id> [--model ID]  (a song split\n"
      "         into its vocals and its accompaniment: two sounds made\n"
      "         from it, their files printed)\n"
      "  layers <project.valtz> <picture-id> [add [above] | move <layer> <by>\n"
      "         | show|hide <layer> | lock|unlock <layer>\n"
      "         | separator <layer> on|off  (a track of its own in the\n"
      "         timeline's track view)\n"
      "         | source <layer> <picture-id>\n"
      "         | rename <layer> <name> | remove <layer>\n"
      "         | markup [selected...] | merge <layer> <layer>\n"
      "         | mask <layer> on|off\n"
      "         | stroke <layer> <x,y;x,y;...>\n"
      "                 [r=8,soft=0.5,color=#rrggbbaa,erase]\n"
      "         | shape <layer> line|rect|ellipse <x0,y0,x1,y1>\n"
      "                 [stroke=#..,fill=#..,width=4]\n"
      "         | text <layer> <x,y|x0,y0,x1,y1> <text> [font=Name,size=48,\n"
      "                 bold,italic,underline,color=#..]\n"
      "                 (a box: wrapped in it, the lines that fit whole;\n"
      "                 align=left|center|right|justified, spacing=1.4)\n"
      "         | style <layer> <object-id> <options>  (a text's or\n"
      "                 shape's: color, fill, width, font, size, bold,\n"
      "                 italic, align, spacing, and effects -- shadow[=#..\n"
      "                 |none],shadow_blur=,shadow_x=,shadow_y=, glow[=#..\n"
      "                 |none],glow_radius=; a gradient -- gradient=linear|\n"
      "                 radial|none,stops=#..@0/#..@1,from=u:v,to=u:v;\n"
      "                 a line's or outline's dash=dashed|dotted|solid, a\n"
      "                 line's arrows=start|end|both|none; any one's\n"
      "                 rotate=<degrees> and flip=h|v|hv|none, about its\n"
      "                 middle; text and shape take them too; a shape's\n"
      "                 WORDS text=<words>,text_color=#..,size=,font=,\n"
      "                 align= (centred in it); a line's or path's point\n"
      "                 ANCHORED anchor<point>=<object-id>:<connector-id>\n"
      "                 -- anchor0= a line's start, anchor1= its end --\n"
      "                 or =none: free)\n"
      "         | shape <layer> polygon <x,y;x,y;x,y...> [options]\n"
      "                 (a polygon through its corners; a PATH -- the\n"
      "                 curvature pen's: smooth=all, or smooth=1+3 the\n"
      "                 points from 1 the curve passes through without a\n"
      "                 turn; open, a curve not closed: two points or more)\n"
      "         | arrange <layer> <how> <object-id...>  (the order:\n"
      "                 to-top|to-bottom|up|down; lined up: left|center|\n"
      "                 right|top|middle|bottom -- one object on the\n"
      "                 canvas; spaced: distribute-h|distribute-v)\n"
      "         | connectors <layer> add|remove <object-id...>  (a\n"
      "                 line's, shape's or path's CONNECTORS: the middle\n"
      "                 of each segment, four round an ellipse -- or none:\n"
      "                 what was anchored to them free where it is)\n"
      "         | materialize <layer> [object-id...]\n"
      "         | duplicate <layer> [name]  (a copy right above it: a\n"
      "                 markup's drawing and objects its own)\n"
      "         | drawing <layer>  (the file its painted pixels are in)\n"
      "         | paste-drawing <layer> <png> [dx dy]  (floating, lifted)\n"
      "         | clear-drawing <layer> [x,y] | drawing-at <layer> <x,y>\n"
      "         | lift-drawing <layer> <x,y>  (an object, moved as one)\n"
      "         | place-drawings <layer>  (the lifted ones laid back: one\n"
      "                 touching another, the two are one drawing)\n"
      "         | split <layer> <frame> | slide <layer> <frame>\n"
      "         | stretch <layer> <frames> | move-key <layer> <from> <to>\n"
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
      "         stretch: a still's -- picture, markup -- length, frames;\n"
      "         move-key: the layer's key at <from> -- frames from its\n"
      "         start, every track keyed there -- moved to <to>, as the\n"
      "         timeline's key is dragged)\n"
      "  pages <project.valtz> <still-id> [add [--after P] [--count N]\n"
      "         | remove <P>]  (a still's pages, from 1: drawn a page at a\n"
      "         time; a new page continues what runs across it, its keys\n"
      "         -- keys <still> crop 0:... count a layer's pages from its\n"
      "         first, as frames -- where their pages went; export writes\n"
      "         one file a page, name-1.png, name-2.png ...)\n"
      "  flatten <project.valtz> <picture-id> -o <file.png> [--page P]\n"
      "  tuning <model-id> [--speed|--quality] [--edit] [--continues]\n"
      "         [--tune k=v,...]\n"
      "         [--turbo none|<id>|<file>] [--lora <file>[:scale]]...\n"
      "         [--dit <file|folder>] [--vae <file|folder>]\n"
      "         (Favor's options for the model -- their values at the\n"
      "         preference, with --tune's laid on as Custom does, their\n"
      "         ranges and what this Mac can have; --continues: as for a\n"
      "         clip continued, its continuation presets. --tune on\n"
      "         generate and video sets them: steps, hyperflow, vdn,\n"
      "         sol_attn, sol_tau, sage_attn, sage_pv8,\n"
      "         sage_pv8_all_rows, i8_gemm, i8_gemm_tier,\n"
      "         motion_cache, video_shift, audio_shift, shift --\n"
      "         true/false, a number or a word (i8_gemm_tier=accurate|\n"
      "         fast|token|token_plain); --turbo the catalog's\n"
      "         few-step LoRA off (none) or on, or a file in its place;\n"
      "         --lora adds one -- eight run, the first on)\n"
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
      "          [--no-mtp] [--assistant <id>] [--repeat <n>] [--speed]\n"
      "          [--project <project.valtz> [--base <file|asset-id>]\n"
      "           [--ref <file|asset-id>]... [--first <file|asset-id>]\n"
      "           [--last <file|asset-id>] [--continue <clip>]\n"
      "           [--song-sound]]\n"
      "          (--no-mtp: the assistant decodes without its MTP\n"
      "          head, to compare; the same reply, slower. --speed:\n"
      "          what it read and wrote, how fast, its drafter's share.\n"
      "          --assistant: that catalog assistant, not the one\n"
      "          chosen, for this run (valtzctl assistant to choose).\n"
      "          --model: the model the prompt is for -- one whose\n"
      "          makers publish a rewriter (Qwen-Image 2.1) gets theirs,\n"
      "          shown an edit's pictures; name them <image1>.. in the\n"
      "          prompt, the base first. --audio: a song, written by\n"
      "          the song writer of Auto's model (YuE2's; with --model\n"
      "          minimax-music3, its sectioned description) to --seconds.\n"
      "          --video: a clip written to MiniMax H3's own guide --\n"
      "          from words, opening on --first (ending on --last:\n"
      "          FL2VA), or from references (Auto's Ref2VA) -- --frames\n"
      "          long)\n"
      "  upscale <project> <composition> [--layer L] [--model id]\n"
      "          [--seed n]   the layer's clip rendered at its scale\n"
      "          (FlashVSR), the layer then showing it at scale 1\n"
      "  upscaler [<model-id> [k=v,...] [--reset]]   Settings >\n"
      "          Upscaling: each upscaler's int8 GEMM, Sage, Sol (on|off),\n"
      "          kept for every later upscale\n"
      "  upscaler use <model-id> | auto [video|image]   the upscaler\n"
      "          Render Upscaled runs for its kind (auto: the best\n"
      "          installed -- FlashVSR+ Turbo, then Base, then 1.1)\n"
      "  intent <text> [--image name]...\n"
      "  fleet [status] [--json] | fleet browse [--seconds N]\n"
      "        | fleet create|join <name> [--discoverable]\n"
      "        | fleet set [name=N] [discoverable=on|off]\n"
      "            [accept=always|ask|never]\n"
      "            [schedule=mon-fri@09:00-18:00|off]\n"
      "        | fleet offload [<task|model> never|local-first|\n"
      "            fleet-first|always|default]\n"
      "        | fleet share [<task|model> on|off]\n"
      "        | fleet leave | fleet connect <host:port>\n"
      "        | fleet serve [--seconds N]\n"
      "         (the FLEET: Valtz on the Macs of one network as one --\n"
      "         a fleet's name and secret (asked for unechoed, or\n"
      "         $VALTZ_FLEET_SECRET), discoverable or not, taking jobs\n"
      "         or not; serve: a member at work until stopped. --fleet\n"
      "         on any command: a job this Mac cannot run, or not now,\n"
      "         goes to a member that can -- or as `fleet offload` says,\n"
      "         per task (generate-image, ..., chat) and per model;\n"
      "         `fleet share`: what this Mac offers the others.\n"
      "         $VALTZ_FLEET_CONFIG: another member's configuration --\n"
      "         two on one Mac)\n"
      "  agent <project> <request> [--auto] [--backend helper|cloud]\n"
      "        [--composition <id>] [--ref <file|asset-id>]... [--speed]\n"
      "        [--assistant <id>]\n"
      "        | agent tools | agent settings [backend=helper|cloud]\n"
      "            [helper=<id>|same] [memory=64|128|192|<GB>|none]\n"
      "            [expert_cache=<GB>|auto]\n"
      "            [review=on|off] [lease=<s>] [provider=anthropic|openai|\n"
      "            openai-compatible] [model=<id>] [base_url=<url>]\n"
      "            [max_tokens=<n>] [context=<tokens>] [compact=on|off]\n"
      "        | agent key <provider> [--remove]\n"
      "         (the AGENT: the request worked on in the project by the\n"
      "         helper -- or a fleet member's, or a cloud model -- calling\n"
      "         the editor's tools; each step asked for at the terminal\n"
      "         unless --auto. settings helper: the model it thinks with\n"
      "         here -- DeepSeek V4.1 Flash, a Qwen -- same: the Agentic\n"
      "         Helper's; memory: the most that model may hold, its expert\n"
      "         cache sized within it. key: a cloud provider's API key,\n"
      "         asked for unechoed, kept in the Keychain)\n"
      "  download <model-id> [--hf-token] [--from <source>]\n"
      "          [--pause-after <s> | --cancel-after <s>]\n"
      "        | download <model-id> --cancel\n"
      "          (--hf-token: asks for a Hugging Face access token, not\n"
      "          echoed, for a GATED model -- used for this download only,\n"
      "          kept nowhere; without it, $HF_TOKEN, or vpipe asks when\n"
      "          Hugging Face refuses. --pause-after: stopped s in, what\n"
      "          came down kept -- `download` again goes on from there;\n"
      "          --cancel-after: stopped and deleted; --cancel alone: what\n"
      "          a paused download left, deleted. --from: where from,\n"
      "          one of `sources` -- the one chosen without it)\n"
      "  sources [<name>]   where downloads come from -- vpipe's sources,\n"
      "          the one chosen (Settings > Capabilities' Download From);\n"
      "          a name chooses it for later downloads, the app's too\n"
      "  wired [auto|<GB>]  the WIRED memory local models may hold -- the\n"
      "          helper, the agent, picture, clip and sound models\n"
      "          (Settings > General > Memory): Automatic, the limit in\n"
      "          force, what is wired now; GB sets one (8, 12.5), auto\n"
      "          gives it back to vpipe, the app's too\n",
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
    // A number as one; any other word as it is (i8_gemm_tier=token).
    char* rest = nullptr;
    const double d = std::strtod(v.c_str(), &rest);
    const bool number = !v.empty() && rest && *rest == '\0';
    if (v == "true" || v == "on" || v == "yes") {
      out[k] = true;
    } else if (v == "false" || v == "off" || v == "no") {
      out[k] = false;
    } else if (!number) {
      out[k] = v;
    } else if (v.find('.') != std::string::npos) {
      out[k] = d;
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

// A markup object's STYLE from its options (media/markup.h): a text's
// align= and spacing=, color= (a text's, a line's, an outline's), fill=,
// width=, size=, font=, bold, italic; and its EFFECTS -- shadow (or
// shadow=#rrggbbaa; none takes it off) with shadow_blur=, shadow_x=,
// shadow_y= (pixels, y down), and glow (=#rrggbbaa | none) with
// glow_radius= -- the glow kept inside the shadow, as the app keeps them.
void
style_opts(Json& obj, const Json& o)
{
  const auto kind = jget<std::string>(obj, "kind", "");
  const bool text = kind == "text";
  // A shape's WORDS (text=, empty: none) and their colour, set in it.
  const bool shape = kind == "rect" || kind == "ellipse" ||
                     kind == "polygon";
  if (shape && o.contains("text")) {
    const auto words = o["text"].is_string() ? o["text"].get<std::string>()
                                             : std::string();
    if (words.empty()) {
      obj.erase("text");
    } else {
      obj["text"] = words;
    }
  }
  if (Json tc = rgba_arg(jget<std::string>(o, "text_color", ""));
      shape && !tc.is_null()) {
    obj["text_color"] = tc;
  }
  const bool words = text || (shape && obj.contains("text"));
  if (words && o.contains("align")) {
    obj["align"] = jget<std::string>(o, "align", "left");
  }
  if (words && o.contains("spacing")) {
    obj["line_spacing"] = jget(o, "spacing", 1.0);
  }
  // Its points ANCHORED: anchor<point>=<object>:<connector> (none: free).
  if (kind == "line" || kind == "polygon") {
    std::map<int, Json> by_point;
    for (const auto& a : jget(obj, "anchors", Json::array())) {
      by_point[jget(a, "point", 0)] = a;
    }
    for (const auto& [k, v] : o.items()) {
      if (!k.starts_with("anchor") || k.size() == 6) {
        continue;
      }
      const int point = std::atoi(k.c_str() + 6);
      const auto spec = v.is_string() ? v.get<std::string>() : "";
      const auto colon = spec.find(':');
      if (colon == std::string::npos) {
        by_point.erase(point);
      } else {
        by_point[point] = {{"point", point},
                           {"to", spec.substr(0, colon)},
                           {"connector", spec.substr(colon + 1)}};
      }
    }
    Json kept = Json::array();
    for (auto& [k, a] : by_point) {
      kept.push_back(std::move(a));
    }
    if (kept.empty()) {
      obj.erase("anchors");
    } else {
      obj["anchors"] = std::move(kept);
    }
  }
  if (Json col = rgba_arg(jget<std::string>(o, "color", ""));
      !col.is_null()) {
    obj["stroke"] = col;
  }
  if (Json f = rgba_arg(jget<std::string>(o, "fill", "")); !f.is_null()) {
    obj["fill"] = f;
  }
  if (o.contains("width")) {
    obj["width"] = jget(o, "width", 4.0);
  }
  // A line's or outline's dashes (dash=dashed|dotted|solid), a line's
  // arrowheads (arrows=start|end|both|none).
  if (!text && o.contains("dash")) {
    const auto d = jget<std::string>(o, "dash", "solid");
    if (d == "dashed" || d == "dotted") {
      obj["dash"] = d;
    } else {
      obj.erase("dash");
    }
  }
  if (jget<std::string>(obj, "kind", "") == "line" && o.contains("arrows")) {
    const auto a = jget<std::string>(o, "arrows", "none");
    if (a == "start" || a == "end" || a == "both") {
      obj["arrows"] = a;
    } else {
      obj.erase("arrows");
    }
  }
  // Its turn (rotate=<degrees>, + clockwise) and flips (flip=h|v|hv|
  // none), about its middle.
  if (o.contains("rotate")) {
    const double r = jget(o, "rotate", 0.0);
    if (r != 0) {
      obj["rotate"] = r;
    } else {
      obj.erase("rotate");
    }
  }
  if (o.contains("flip")) {
    const auto f = jget<std::string>(o, "flip", "none");
    obj.erase("flip_x");
    obj.erase("flip_y");
    if (f.find('h') != std::string::npos) {
      obj["flip_x"] = true;
    }
    if (f.find('v') != std::string::npos) {
      obj["flip_y"] = true;
    }
  }
  if (words) {
    Json font = jget(obj, "font", Json::object());
    if (o.contains("font")) {
      font["family"] = jget<std::string>(o, "font", "Helvetica Neue");
    }
    if (o.contains("size")) {
      font["size"] = jget(o, "size", 48.0);
    }
    for (const char* k : {"bold", "italic", "underline"}) {
      if (o.contains(k)) {
        font[k] = jget(o, k, false);
      }
    }
    obj["font"] = font;
  }
  // A GRADIENT: gradient=linear|radial|none, its colours
  // stops=#rrggbbaa@0/#rrggbbaa@1 (two at least; none given: the
  // object's colour to white), its ends from=u:v, to=u:v in its box's
  // units.
  if (o.contains("gradient") || o.contains("stops") ||
      o.contains("from") || o.contains("to")) {
    const auto kind = jget<std::string>(
        o, "gradient", jget<std::string>(jget(obj, "gradient", Json()),
                                         "kind", "linear"));
    if (kind == "none") {
      obj.erase("gradient");
    } else if (jget<std::string>(obj, "kind", "") != "line") {
      Json g = jget(obj, "gradient", Json::object());
      g["kind"] = kind == "radial" ? "radial" : "linear";
      if (o.contains("stops")) {
        Json stops = Json::array();
        const auto spec = jget<std::string>(o, "stops", "");
        std::size_t at = 0;
        while (at <= spec.size()) {
          const auto end = spec.find('/', at);
          const auto one = spec.substr(at, end - at);
          if (const auto a = one.find('@'); a != std::string::npos) {
            if (Json c = rgba_arg(one.substr(0, a)); !c.is_null()) {
              stops.push_back({{"at", std::atof(one.c_str() + a + 1)},
                               {"color", c}});
            }
          }
          if (end == std::string::npos) {
            break;
          }
          at = end + 1;
        }
        g["stops"] = std::move(stops);
      }
      if (jget(g, "stops", Json::array()).size() < 2) {
        const Json own = jget<std::string>(obj, "kind", "") == "text"
                             ? jget(obj, "stroke", Json::array({0, 0, 0, 1}))
                             : jget(obj, "fill", Json::array({0, 0, 0, 1}));
        g["stops"] = Json::array({{{"at", 0}, {"color", own}},
                                  {{"at", 1},
                                   {"color", Json::array({1, 1, 1, 1})}}});
      }
      for (const char* k : {"from", "to"}) {
        if (o.contains(k)) {
          const auto v = jget<std::string>(o, k, "");
          const auto c = v.find(':');
          if (c != std::string::npos) {
            g[k] = {std::atof(v.c_str()), std::atof(v.c_str() + c + 1)};
          }
        }
      }
      obj["gradient"] = std::move(g);
    }
  }
  Json fx = jget(obj, "effects", Json::array());
  auto replace = [&](const std::string& kind, Json made) {
    Json out = Json::array();
    for (const auto& e : fx) {
      if (jget<std::string>(e, "kind", "") != kind) {
        out.push_back(e);
      }
    }
    if (!made.is_null()) {
      out.push_back(std::move(made));
    }
    fx = std::move(out);
  };
  for (const std::string kind : {"shadow", "glow"}) {
    if (!o.contains(kind)) {
      continue;
    }
    const Json v = o[kind];
    if (v.is_string() && v.get<std::string>() == "none") {
      replace(kind, Json());
      continue;
    }
    Json col = v.is_string() ? rgba_arg(v.get<std::string>()) : Json();
    if (col.is_null()) {
      col = kind == "glow" ? jget(obj, "stroke", Json::array({1, 1, 1, 1}))
                           : Json::array({0, 0, 0, 0.5});
    }
    replace(kind, kind == "glow"
                      ? Json{{"kind", "glow"}, {"color", col},
                             {"radius", jget(o, "glow_radius", 12.0)}}
                      : Json{{"kind", "shadow"}, {"color", col},
                             {"blur", jget(o, "shadow_blur", 8.0)},
                             {"dx", jget(o, "shadow_x", 4.0)},
                             {"dy", jget(o, "shadow_y", 4.0)}});
  }
  Json ordered = Json::array();
  for (const auto& e : fx) {
    if (jget<std::string>(e, "kind", "") == "glow") {
      ordered.push_back(e);
    }
  }
  for (const auto& e : fx) {
    if (jget<std::string>(e, "kind", "") != "glow") {
      ordered.push_back(e);
    }
  }
  if (ordered.empty()) {
    obj.erase("effects");
  } else {
    obj["effects"] = std::move(ordered);
  }
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

// A model's files checked (models/model-check.h): what was found, then
// each note -- errors first.
void
print_check(const Json& r)
{
  const auto moved = jget<std::string>(r, "moved", "");
  std::printf("%s: %s%s\n",
              !jget(r, "usable", false)  ? "refused"
              : jget(r, "confirmed", false) ? "confirmed"
                                            : "unconfirmed",
              jget<std::string>(r, "path", "").c_str(),
              moved.empty() ? ""
              : moved == "up" ? " (found around the path given)"
                              : " (found inside the path given)");
  for (const auto& n : jget(r, "notes", Json::array())) {
    std::printf("  [%s] %s\n", jget<std::string>(n, "level", "").c_str(),
                jget<std::string>(n, "message", "").c_str());
  }
}

// `made`, if given: the asset the job finished with.
int
follow(Controller& c, JobId job, std::string* made = nullptr)
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
      // A compressed copy's passes (a pipeline's parts).
      if (const int passes = jget(ev.data, "passes", 0); passes > 1) {
        line += std::format(" pass {}/{} ({:.0f}% in all)",
                            jget(ev.data, "pass", 0), passes,
                            jget(ev.data, "estimate", 0.0) * 100);
      }
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
    if (ev.kind == "job.finished" && made != nullptr) {
      *made = jget<std::string>(ev.data, "asset", "");
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

// A video summary's frame interval as typed: seconds a frame ("2",
// "0.25"), or frames a second ("8/s"); "auto" 0.
double
every_arg(const std::string& arg)
{
  if (arg == "auto") {
    return 0;
  }
  if (arg.ends_with("/s")) {
    const double rate = std::atof(arg.c_str());
    return rate > 0 ? 1.0 / rate : 0.0;
  }
  return std::atof(arg.c_str());
}

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

// The offload policies: each task's, then each model set apart.
void
print_offload(const Json& s)
{
  const Json c = jget(s, "config", Json::object());
  const Json off = jget(c, "offload", Json::object());
  const Json tasks = jget(off, "tasks", Json::object());
  std::printf("offload policies (while in a fleet%s)\n",
              jget(c, "joined", false) ? "" : " -- this Mac is in none");
  for (const auto& t : jget(s, "tasks", Json::array())) {
    const auto op = jget<std::string>(t, "op", "");
    std::printf("  %-18s %s\n", op.c_str(),
                jget<std::string>(tasks, op.c_str(), "local-first").c_str());
  }
  const Json models = jget(off, "models", Json::object());
  for (const auto& [id, p] : models.items()) {
    std::printf("  model %-24s %s\n", id.c_str(),
                p.get<std::string>().c_str());
  }
}

// What this Mac offers the fleet: each task, the models withheld.
void
print_share(const Json& s)
{
  const Json held = jget(jget(s, "config", Json::object()), "withheld",
                         Json::object());
  const Json tasks = jget(held, "tasks", Json::array());
  std::printf("shared with the fleet\n");
  for (const auto& t : jget(s, "tasks", Json::array())) {
    const auto op = jget<std::string>(t, "op", "");
    const bool off = std::ranges::find(tasks, Json(op)) != tasks.end();
    std::printf("  %-18s %s%s\n", op.c_str(), off ? "off" : "on",
                jget(t, "runs", false) ? "" : "  (not run here)");
  }
  for (const auto& id : jget(held, "models", Json::array())) {
    std::printf("  model %-24s off\n", id.get<std::string>().c_str());
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
  if (sub == "offload" || sub == "share") {
    // <task|model> <value>: a task is a fleet task's op, else a model.
    if (args.size() == 3) {
      const Json st = c.fleet_status();
      bool task = false;
      for (const auto& t : jget(st, "tasks", Json::array())) {
        task = task || jget<std::string>(t, "op", "") == args[1];
      }
      Json v;
      if (sub == "offload") {
        v = args[2] == "default" ? std::string() : args[2];
      } else if (args[2] == "on" || args[2] == "off") {
        v = args[2] == "on";
      } else {
        return usage();
      }
      const Json j = {{sub, {{task ? "tasks" : "models", {{args[1], v}}}}}};
      if (auto r = c.fleet_configure(j); !r.ok()) {
        return fail(r.error());
      }
    } else if (args.size() != 1) {
      return usage();
    }
    const Json st = c.fleet_status();
    if (as_json) {
      std::cout << to_text(st, 2) << "\n";
    } else if (sub == "offload") {
      print_offload(st);
    } else {
      print_share(st);
    }
    return 0;
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

// A turn's speed as `agent --speed` (and `enhance --speed`) prints it
// (agent::turn_stats): "prefill 680 tok in 0.21 s = 3238.1 tok/s ·
// decode 71 tok in 1.53 s = 46.5 tok/s · mtp 2.37 a round, 69% kept ·
// 7700 reused (memory) · ctx 8501 · turn 2.10 s".
std::string
turn_speed_line(const Json& s)
{
  std::string out;
  auto add = [&](const std::string& part) {
    out += (out.empty() ? "" : " · ") + part;
  };
  for (const char* side : {"prefill", "decode"}) {
    const std::string tokens = std::format("{}_tokens", side);
    const std::string ms = std::format("{}_ms", side);
    const std::string tps = std::format("{}_tps", side);
    if (s.contains(tokens)) {
      std::string p = std::format(
          "{} {} tok", side, jget<std::int64_t>(s, tokens.c_str(), 0));
      if (s.contains(ms)) {
        p += std::format(" in {:.2f} s", jget(s, ms.c_str(), 0.0) / 1000.0);
      }
      if (s.contains(tps)) {
        p += std::format(" = {:.1f} tok/s", jget(s, tps.c_str(), 0.0));
      }
      add(p);
    } else if (s.contains(ms)) {
      add(std::format("{} {:.2f} s", side,
                      jget(s, ms.c_str(), 0.0) / 1000.0));
    }
  }
  if (s.contains("per_round")) {
    std::string d = std::format("{} {:.2f} a round",
                                jget<std::string>(s, "drafter", "draft"),
                                jget(s, "per_round", 0.0));
    if (s.contains("accepted_pct")) {
      d += std::format(", {}% kept", jget<std::int64_t>(s, "accepted_pct",
                                                         0));
    }
    add(d);
  }
  if (s.contains("cached_tokens")) {
    add(std::format("{} cached", jget<std::int64_t>(s, "cached_tokens", 0)));
  }
  if (s.contains("reused")) {
    add(std::format("{} reused ({})", jget<std::int64_t>(s, "reused", 0),
                    jget<std::string>(s, "restored", "")));
  }
  if (s.contains("ctx_pos")) {
    add(std::format("ctx {}", jget<std::int64_t>(s, "ctx_pos", 0)));
  }
  if (s.contains("first_ms") && !s.contains("prefill_tps")) {
    add(std::format("first words {:.2f} s",
                    jget(s, "first_ms", 0.0) / 1000.0));
  }
  add(std::format("turn {:.2f} s", jget(s, "turn_ms", 0.0) / 1000.0));
  return out;
}

// THE AGENT at the terminal (DESIGN §9b): the request worked on in the
// project, its words printed as they are written, each step that changes
// the document asked for (Y / n / a / e / q) unless --auto; done, more
// can be said at the prompt. --speed: each turn's prefill and decode.
int
agent_command(Controller& c, std::vector<std::string> args)
{
  if (args.empty()) {
    return usage();
  }
  if (args[0] == "tools") {
    std::cout << to_text(c.agent_tools(), 2) << "\n";
    return 0;
  }
  if (args[0] == "settings") {
    Json set = Json::object();
    Json cloud = Json::object();
    for (std::size_t i = 1; i < args.size(); ++i) {
      const auto eq = args[i].find('=');
      if (eq == std::string::npos) {
        return usage();
      }
      const std::string k = args[i].substr(0, eq);
      const std::string v = args[i].substr(eq + 1);
      if (k == "backend") {
        set["backend"] = v;
      } else if (k == "helper") {
        set["model"] = v == "same" ? std::string() : v;
      } else if (k == "memory") {
        if (v != "none" && std::atoi(v.c_str()) <= 0) {
          return usage();
        }
        set["memory_gb"] = v == "none" ? 0 : std::atoi(v.c_str());
      } else if (k == "expert_cache") {
        if (v != "auto" && std::atoi(v.c_str()) <= 0) {
          return usage();
        }
        set["expert_cache_gb"] = v == "auto" ? 0 : std::atoi(v.c_str());
      } else if (k == "review") {
        set["review"] = v == "on" || v == "true";
      } else if (k == "compact") {
        set["compact"] = v == "on" || v == "true";
      } else if (k == "lease") {
        set["lease"] = std::atof(v.c_str());
      } else if (k == "provider" || k == "model" || k == "base_url") {
        cloud[k] = v;
      } else if (k == "max_tokens") {
        cloud[k] = std::atoi(v.c_str());
      } else if (k == "context") {
        cloud["context_tokens"] = std::atoll(v.c_str());
      } else {
        return usage();
      }
    }
    if (!cloud.empty()) {
      set["cloud"] = cloud;
    }
    if (!set.empty()) {
      if (auto st = c.set_agent_settings(set); !st.ok()) {
        return fail(st.error());
      }
    }
    std::cout << to_text(c.agent_settings(), 2) << "\n";
    return 0;
  }
  if (args[0] == "key") {
    if (args.size() < 2) {
      return usage();
    }
    if (std::erase(args, std::string("--remove")) > 0) {
      auto st = c.set_agent_key(args[1], "");
      return st.ok() ? 0 : fail(st.error());
    }
    // Asked for unechoed -- never an argument, which the shell's
    // history would keep -- and handed to the Keychain alone.
    char buf[512];
    const std::string q = args[1] + " API key: ";
    if (!readpassphrase(q.c_str(), buf, sizeof buf, RPP_REQUIRE_TTY)) {
      return fail(make_error(Code::InvalidArgument,
                             "no terminal to ask for the key on"));
    }
    std::string key = buf;
    std::fill(std::begin(buf), std::end(buf), '\0');
    auto st = c.set_agent_key(args[1], key);
    std::fill(key.begin(), key.end(), '\0');
    return st.ok() ? 0 : fail(st.error());
  }
  if (args.size() < 2) {
    return usage();
  }
  auto pid = open_or_fail(c, args[0]);
  if (!pid.ok()) {
    return fail(pid.error());
  }
  AgentRequest req;
  req.project = *pid;
  bool autonomous = false;
  bool speed = false;
  std::string prompt;
  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto next = [&]() -> std::string {
      return i + 1 < args.size() ? args[++i] : std::string();
    };
    if (a == "--auto") {
      autonomous = true;
    } else if (a == "--speed") {
      speed = true;
    } else if (a == "--backend") {
      req.backend = next();
    } else if (a == "--assistant") {
      // Another catalog helper for this run only (its turns and its
      // enhancer): a smaller one beside a picture model on a small Mac.
      c.set_assistant(next());
    } else if (a == "--composition") {
      auto id = AssetId::parse(next());
      if (!id) {
        return fail(make_error(Code::InvalidArgument, "not an asset id"));
      }
      req.composition = *id;
    } else if (a == "--page" || a == "--frame" || a == "--layer") {
      // Where the person is, as the app's stage tells it: "this page".
      const std::string v = next();
      if (a == "--layer") {
        req.stage["layer"] = v == "0" ? std::string() : v;
      } else {
        req.stage[a.substr(2)] = std::atoll(v.c_str());
      }
    } else if (a == "--ref") {
      auto r = reference_asset(c, *pid, next());
      if (!r.ok()) {
        return fail(r.error());
      }
      req.row.push_back(*r);
    } else {
      prompt += (prompt.empty() ? "" : " ") + a;
    }
  }
  if (!req.stage.empty()) {
    const auto on = req.composition ? req.composition
                                    : c.project_composition(*pid);
    if (on) {
      req.stage["composition"] = on->str();
    }
  }
  const bool tty = ::isatty(STDIN_FILENO) != 0;
  if (!autonomous && !tty) {
    return fail(make_error(Code::InvalidArgument,
                           "no terminal to review the steps on: --auto"));
  }
  req.prompt = prompt;
  req.review = !autonomous;
  auto id = c.agent_start(req);
  if (!id.ok()) {
    return fail(id.error());
  }
  std::fprintf(stderr, "agent: %s\n", id->c_str());
  auto line = [](const char* q) {
    std::cout << q << std::flush;
    std::string s;
    if (!std::getline(std::cin, s)) {
      return std::string("\x04");
    }
    return s;
  };
  std::map<int, std::string> printed;   // each step's state, as shown
  int speed_shown = 0;                  // --speed: the turns printed
  int compacted = 0;                    // the compactions said
  bool writing = false;
  const auto t0 = std::chrono::steady_clock::now();
  for (;;) {
    Event ev;
    if (!c.events().wait(ev, 500)) {
      continue;
    }
    if (ev.kind == "agent.text" &&
        jget<std::string>(ev.data, "agent", "") == *id) {
      // This Mac's helper: vpipe's own stream prints it already.
      if (jget<std::string>(jget(c.agent_state(*id), "backend",
                                 Json::object()),
                            "kind", "") == "local") {
        continue;
      }
      if (!writing) {
        std::cout << "\n";
        writing = true;
      }
      std::cout << jget<std::string>(ev.data, "text", "") << std::flush;
      continue;
    }
    if (ev.kind != "agent.changed" ||
        jget<std::string>(ev.data, "agent", "") != *id) {
      continue;
    }
    const Json st = c.agent_state(*id);
    const std::string state = jget<std::string>(st, "state", "");
    if (writing && state != "thinking") {
      std::cout << "\n";
      writing = false;
    }
    if (const int n = jget(st, "compactions", 0); n > compacted) {
      compacted = n;
      std::fprintf(stderr,
                   "  · compacted (%d): the conversation kept within its "
                   "model's %lld tokens\n",
                   n,
                   static_cast<long long>(jget<std::int64_t>(
                       jget(st, "context", Json::object()), "limit", 0)));
    }
    if (speed && jget(st, "turns", 0) > speed_shown) {
      speed_shown = jget(st, "turns", 0);
      std::fprintf(stderr, "  · turn %d: %s\n", speed_shown,
                   turn_speed_line(jget(st, "stats", Json::object()))
                       .c_str());
    }
    for (const auto& step : jget(st, "steps", Json::array())) {
      const int n = jget(step, "n", 0);
      const std::string s = jget<std::string>(step, "state", "");
      if (printed[n] == s || s == "proposed") {
        continue;
      }
      printed[n] = s;
      const std::string title = jget<std::string>(step, "title", "");
      if (s == "running") {
        std::fprintf(stderr, "  [%d] %s %s\n", n, title.c_str(),
                     utf8_truncate(to_text(jget(step, "args", Json())),
                                   200).c_str());
      } else if (s == "done" || s == "waiting") {
        std::fprintf(stderr, "      %s %s\n", s == "done" ? "ok" : "...",
                     utf8_truncate(to_text(jget(step, "result", Json())),
                                   240).c_str());
      } else if (s == "failed") {
        std::fprintf(stderr, "      failed: %s\n",
                     jget<std::string>(step, "error", "").c_str());
      } else if (s == "skipped") {
        std::fprintf(stderr, "  [%d] %s skipped\n", n, title.c_str());
      }
    }
    if (state == "review") {
      for (const auto& step : jget(st, "steps", Json::array())) {
        if (jget<std::string>(step, "state", "") != "proposed") {
          continue;
        }
        const int n = jget(step, "n", 0);
        std::cout << std::format("\n  [{}] {} {}\n", n,
                                 jget<std::string>(step, "title", ""),
                                 to_text(jget(step, "args", Json()), 2));
        const std::string a =
            line("  Run it? [Y]es / [n]o / [a]ll from here / [e]dit / "
                 "[q]uit: ");
        Status r = ok_status();
        if (a == "\x04" || a == "q") {
          r = c.agent_stop(*id);
        } else if (a == "n") {
          r = c.agent_review(*id, n, "skip", Json(),
                             line("  Why (told to it; Enter: none)? "));
        } else if (a == "a") {
          r = c.agent_review(*id, n, "approve_all");
        } else if (a == "e") {
          const Json edited = Json::parse(line("  Its arguments (JSON): "),
                                          nullptr, false);
          r = edited.is_object() ? c.agent_review(*id, n, "approve", edited)
                                 : c.agent_review(*id, n, "approve");
        } else {
          r = c.agent_review(*id, n, "approve");
        }
        if (!r.ok()) {
          std::fprintf(stderr, "valtzctl: %s\n", r.error().message.c_str());
        }
      }
    } else if (state == "asking") {
      const Json steps = jget(st, "steps", Json::array());
      const std::string q = steps.empty()
          ? std::string()
          : jget<std::string>(jget(steps.back(), "args", Json()),
                              "question", "");
      std::cout << "\n  It asks: " << q << "\n";
      const std::string a = tty ? line("  > ") : std::string("\x04");
      (void)(a == "\x04" ? c.agent_stop(*id) : c.agent_say(*id, a));
    } else if (state == "done" || state == "failed") {
      if (state == "failed") {
        std::fprintf(stderr, "agent: %s\n",
                     jget<std::string>(st, "error", "").c_str());
      }
      const double secs = std::chrono::duration<double>(
          std::chrono::steady_clock::now() - t0).count();
      std::fprintf(stderr, "agent: %s after %.0f s, %d turns, %zu steps; "
                           "usage %s\n",
                   state.c_str(), secs, jget(st, "turns", 0),
                   jget(st, "steps", Json::array()).size(),
                   to_text(jget(st, "usage", Json())).c_str());
      const std::string more = tty ? line("\n> ") : std::string();
      if (more.empty() || more == "\x04" || more == "q") {
        (void)c.agent_stop(*id);
      } else {
        (void)c.agent_say(*id, more);
      }
    } else if (state == "stopped") {
      return jget<std::string>(st, "error", "").empty() ? 0 : 1;
    }
  }
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
                    cmd == "quantize" || cmd == "compress" || cmd == "ops" ||
                    find_asset_operation(cmd) != nullptr ||
                    (cmd == "ext" && (args.empty() || args[0] == "list")) ||
                    cmd == "status" || cmd == "families" ||
                    cmd == "enhance" ||
                    cmd == "intent" || cmd == "download" || cmd == "caps" ||
                    cmd == "sources" || cmd == "wired" ||
                    cmd == "auto" || cmd == "agent" ||
                    (cmd == "export" &&
                     !(args.size() == 1 && args[0] == "--formats"));
  // "--memory": what the engine holds in memory as the command ends --
  // the models kept warm, the checkpoints resident (the status bar's RAM
  // popover, Controller::memory_held).
  const bool show_memory = std::erase(args, std::string("--memory")) > 0;
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
  // Printed as main returns, whichever command ran -- before the
  // controller goes, with what it holds.
  struct MemoryAtEnd {
    Controller& c;
    bool        on;
    ~MemoryAtEnd()
    {
      if (on) {
        std::cout << to_text(Json{{"memory", c.memory_held()}}, 2) << "\n";
      }
    }
  } memory_at_end{c, show_memory};
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
        // Compressed in its download's place, or what compressing would
        // take (DESIGN §8b).
        std::string small;
        if (const Json cm = jget(m, "compressed", Json()); cm.is_object()) {
          small = !jget(cm, "original_kept", false)
              ? std::format("  {}-bit copy (was {:.1f} GB)",
                            jget(cm, "bits", 0),
                            jget(cm, "original_bytes", 0.0) / 1e9)
              : std::format("  {}-bit copy ({:.1f} GB) beside the "
                            "original, {} in use", jget(cm, "bits", 0),
                            jget(cm, "bytes", 0.0) / 1e9,
                            jget(cm, "in_use", true) ? "the copy"
                                                     : "the original");
        } else if (const Json cp = jget(m, "compress", Json());
                   cp.is_object()) {
          for (const auto& w : jget(cp, "widths", Json::array())) {
            small += std::format("  {}-bit ~{:.1f} GB", jget(w, "bits", 0),
                                 jget(w, "bytes", 0.0) / 1e9);
          }
          if (small.empty()) {
            small = "  (compress: " + jget<std::string>(cp, "why", "") +
                    ")";
          }
        }
        // Where it downloads from, where that differs: a source that
        // does not carry it, a mirror's own name for it.
        if (state != "installed" && m.contains("sources")) {
          const auto hf = jget<std::string>(m, "hf_path", "");
          for (const auto& s : jget(t, "download_sources", Json::array())) {
            const auto name = jget<std::string>(s, "name", "");
            std::string there;
            for (const auto& ms : m["sources"]) {
              if (jget<std::string>(ms, "name", "") == name) {
                there = jget<std::string>(ms, "path", "");
              }
            }
            const auto label = jget<std::string>(s, "label", name);
            if (there.empty()) {
              small += "  (not on " + label + ")";
            } else if (there != hf) {
              small += "  (" + label + ": " + there + ")";
            }
          }
        }
        std::printf("  %-9s %-6s %-40s %s%s\n", state.c_str(),
                    src.empty() ? "-" : src.c_str(),
                    m["label"].get<std::string>().c_str(),
                    m["model"].get<std::string>().c_str(), small.c_str());
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
        st = c.set_video_every(every_arg(args[++i]));
      } else if (args[i] == "--video-size" && i + 1 < args.size()) {
        st = c.set_video_size(args[++i]);
      } else if (args[i] == "--bits" && i + 1 < args.size()) {
        ++i;
        st = c.set_assistant_drafter(
            c.assistants().value("drafter", std::string("mtp")), bits);
      } else if (args[i] == "--drafter" && i + 1 < args.size()) {
        st = c.set_assistant_drafter(args[++i], bits);
      } else if (args[i] == "--context" && i + 1 < args.size()) {
        ++i;
        st = c.set_assistant_context(
            args[i] == "auto" ? 0 : std::atoll(args[i].c_str()));
      } else {
        st = c.choose_assistant(args[i] == "auto" ? "" : args[i]);
      }
      if (!st.ok()) {
        return fail(st.error());
      }
    }
    const Json a = c.assistants();
    const auto choice = a["choice"].get<std::string>();
    if (const auto ctx = jget<std::int64_t>(a, "context", 0); ctx > 0) {
      std::printf("context  at most %lld tokens\n",
                  static_cast<long long>(ctx));
    } else {
      std::printf("context  auto: each helper's own, as memory holds it\n");
    }
    std::printf("chosen   %s\nauto     %s\nusing    %s\nkept     %g s "
                "after a request\ndrafter  %s%s\n",
                choice.empty() ? "auto" : choice.c_str(),
                a["auto"].get<std::string>().c_str(),
                a["using"].get<std::string>().c_str(),
                jget(a, "keep_loaded", 0.0),
                jget<std::string>(a, "drafter", "mtp").c_str(),
                jget<std::string>(a, "drafter", "") == "dflash"
                    ? std::format(" ({}-bit)", jget(a, "drafter_bits", 8))
                          .c_str()
                    : "");
    const double every = jget(a, "video_every", 0.0);
    const std::string video_every =
        every > 0 ? std::format("a frame every {:g} s", every)
        : c.video_every(10) < 0.2
            ? std::string("auto: 8 a second up to 10 s, 4 up to 20, 2 up "
                          "to 40, then one a second")
            : std::string("auto: 8 a second up to 5 s, 4 up to 10, 2 up "
                          "to 20, 1 up to 40, then one every 2 s");
    std::printf("video    %s; frames %s (memory %g GB/s)\n",
                video_every.c_str(),
                jget<std::string>(a, "video_size", "standard").c_str(),
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
      if (m.contains("context")) {
        // Given, of the model's own (Auto's; what this Mac's memory
        // holds, the most Settings' Context offers).
        drafter += std::format("  context {} of {} (auto {}, fits {})",
                               jget(m["context"], "here", 0),
                               jget(m["context"], "model", 0),
                               jget(m["context"], "auto", 0),
                               jget(m["context"], "fits", 0));
      }
      // Never Auto's: chosen by name alone (an abliterated helper).
      if (jget(m, "by_name", false)) {
        drafter += "  by name only";
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
  if (cmd == "unlink") {
    if (args.empty()) {
      return usage();
    }
    const bool parts = args.size() > 1 && args[1] == "--drawthings";
    auto st = c.unlink_model(args[0], parts);
    return st.ok() ? 0 : fail(st.error());
  }
  if (cmd == "link" || cmd == "check") {
    if (args.empty() || (cmd == "link" && args.size() < 2)) {
      return usage();
    }
    auto r = cmd == "link"
        ? c.link_model(args[0], args[1])
        : c.check_model(args[0], args.size() > 1 ? args[1] : "");
    if (!r.ok()) {
      return fail(r.error());
    }
    print_check(*r);
    if (cmd == "link") {
      const bool linked = jget(*r, "linked", false);
      std::printf("%s %s\n", linked ? "linked" : "not linked:",
                  jget<std::string>(*r, "path", "").c_str());
      const Json parts = jget(*r, "parts", Json::object());
      for (auto it = parts.begin(); linked && it != parts.end(); ++it) {
        std::printf("  its %s: %s\n", it.key().c_str(),
                    it->get<std::string>().c_str());
      }
      return linked ? 0 : 1;
    }
    return jget(*r, "usable", false) ? 0 : 1;
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
    const AppPaths p = c.paths();
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
  if (cmd == "locations") {
    const auto print = [&c] {
      const Json l = c.locations();
      const auto line = [](const char* what, const Json& j) {
        const bool there = jget(j, "there", false);
        std::printf("%-9s %s  (%s%s)\n", what,
                    jget<std::string>(j, "path", "").c_str(),
                    !there ? "NOT THERE"
                    : jget(j, "internal", true) ? "internal"
                                                : "EXTERNAL",
                    jget(j, "is_default", false) ? ", default" : "");
      };
      line("projects", jget(l, "projects", Json::object()));
      line("models", jget(l, "models", Json::object()));
      for (const auto& k : jget(jget(l, "models", Json::object()), "kept",
                                Json::array())) {
        line("  kept", k);
      }
      const Json cache = jget(l, "cache", Json::object());
      line("cache", cache);
      std::printf("          %llu MB in use at %s\n",
                  static_cast<unsigned long long>(
                      jget<std::uint64_t>(cache, "bytes", 0) >> 20),
                  jget<std::string>(cache, "in_use", "").c_str());
      if (const Json m = jget(l, "moving", Json()); m.is_object()) {
        std::printf("moving    %s to %s\n",
                    jget<std::string>(m, "what", "").c_str(),
                    jget<std::string>(m, "to", "").c_str());
      }
    };
    if (args.empty()) {
      print();
      return 0;
    }
    const std::string what = args[0];
    std::string how = "move";
    fs::path dir;
    bool to_default = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
      if (args[i] == "--move" || args[i] == "--keep" ||
          args[i] == "--drop") {
        how = args[i].substr(2);
      } else if (args[i] == "--default") {
        to_default = true;
      } else {
        dir = args[i];
      }
    }
    if (to_default) {
      dir = jget<std::string>(
          jget(c.locations(), what.c_str(), Json::object()), "default", "");
    }
    if (dir.empty()) {
      std::fprintf(stderr, "usage: valtzctl locations projects|models|cache "
                           "<dir>|--default [--move|--keep|--drop]\n");
      return 2;
    }
    auto r = c.set_location(what, dir, how);
    if (!r.ok()) {
      std::fprintf(stderr, "%s\n", r.error().message.c_str());
      return 1;
    }
    int rc = 0;
    if (!r->is_nil()) {
      rc = follow(c, *r);
    }
    print();
    return rc;
  }
  if (cmd == "models") {
    const auto& store = c.model_store();
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
      const std::string spec = args[i].substr(colon + 1);
      Json k = key_values(spec);
      k["frame"] = std::atoll(args[i].substr(0, colon).c_str());
      // "interp=hold|linear|spline": how it goes on to the next key.
      if (const auto at = spec.find("interp="); at != std::string::npos) {
        const auto end = spec.find(',', at);
        k["interp"] = spec.substr(at + 7, end == std::string::npos
                                              ? std::string::npos
                                              : end - at - 7);
      }
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
    } else if (op == "lock" || op == "unlock") {
      st = c.set_layer_locked(*pid, *aid, layer, op == "lock");
    } else if (op == "separator") {
      // `separator <L> on|off`: a track of its own in the track view.
      st = c.set_layer_separator(*pid, *aid, layer, arg != "off");
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
      // <layer> <png> [dx dy]: a drawing floating on the markup, lifted.
      const double dx = plain.size() > 5 ? std::atof(plain[5].c_str()) : 0;
      const double dy = plain.size() > 6 ? std::atof(plain[6].c_str()) : 0;
      auto r = c.paste_drawing(*pid, *aid, layer, arg, dx, dy);
      if (!r.ok()) {
        return fail(r.error());
      }
      std::printf("floating as object %s\n", r->c_str());
    } else if (op == "clear-drawing" || op == "drawing-at" ||
               op == "lift-drawing") {
      // <layer> [x,y]: the drawing painted there (4 pixels' reach).
      std::optional<std::array<double, 2>> at;
      double x = 0, y = 0;
      if (std::sscanf(arg.c_str(), "%lf,%lf", &x, &y) == 2) {
        at = std::array<double, 2>{x, y};
      }
      if (op == "clear-drawing") {
        st = c.clear_drawing(*pid, *aid, layer, at, 4);
      } else if (!at) {
        return usage();
      } else if (op == "drawing-at") {
        auto r = c.drawing_at(*pid, *aid, layer, x, y, 4);
        if (!r.ok()) {
          return fail(r.error());
        }
        if (r->empty()) {
          std::printf("(no drawing there)\n");
        } else {
          std::printf("drawing at %d,%d %dx%d\n", r->x, r->y, r->w, r->h);
        }
      } else {
        auto r = c.lift_drawing(*pid, *aid, layer, x, y, 4);
        if (!r.ok()) {
          return fail(r.error());
        }
        std::printf("lifted as object %s\n", r->c_str());
      }
    } else if (op == "place-drawings") {
      st = c.place_drawings(*pid, *aid, layer);
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
    } else if (op == "move-key") {
      // <layer> <from> <to>: its key, every track's, at another frame.
      if (plain.size() < 6) {
        return usage();
      }
      st = c.move_layer_key(*pid, *aid, layer, std::atoll(arg.c_str()),
                            std::atoll(plain[5].c_str()));
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
      if (op == "shape" && arg == "polygon") {
        // A POLYGON through its corners "x,y;x,y;...": its box their
        // bounds, its points in the box's units (media/markup.h).
        const auto xy = numbers_arg(args.size() > 5 ? args[5] : "");
        const Json o = options_arg(args.size() > 6 ? args[6] : "");
        const bool open = jget(o, "open", false);
        if (xy.size() < (open ? 4u : 6u) || xy.size() % 2 != 0) {
          return usage();
        }
        double x0 = xy[0], y0 = xy[1], x1 = xy[0], y1 = xy[1];
        for (std::size_t i = 0; i < xy.size(); i += 2) {
          x0 = std::min(x0, xy[i]);
          x1 = std::max(x1, xy[i]);
          y0 = std::min(y0, xy[i + 1]);
          y1 = std::max(y1, xy[i + 1]);
        }
        Json pts = Json::array();
        for (std::size_t i = 0; i < xy.size(); i += 2) {
          pts.push_back({x1 > x0 ? (xy[i] - x0) / (x1 - x0) : 0.5,
                         y1 > y0 ? (xy[i + 1] - y0) / (y1 - y0) : 0.5});
        }
        obj = {{"kind", "polygon"}, {"x0", x0}, {"y0", y0},
               {"x1", x1}, {"y1", y1}, {"points", std::move(pts)},
               {"width", jget(o, "width", 4.0)}};
        // A PATH: its smooth points -- all, or by their place from 1
        // ("1+3") -- and an open one.
        if (o.contains("smooth")) {
          const auto which = o["smooth"].is_number()
                                 ? std::to_string(jget<int>(o, "smooth", 0))
                                 : jget<std::string>(o, "smooth", "");
          Json flags = Json::array();
          for (std::size_t i = 0; i < xy.size() / 2; ++i) {
            flags.push_back(which == "all");
          }
          for (std::size_t at = 0; which != "all" && at < which.size();) {
            const auto end = which.find('+', at);
            const int n = std::atoi(which.substr(at, end - at).c_str());
            if (n >= 1 && static_cast<std::size_t>(n) <= flags.size()) {
              flags[n - 1] = true;
            }
            if (end == std::string::npos) {
              break;
            }
            at = end + 1;
          }
          obj["smooth"] = std::move(flags);
        }
        if (open) {
          obj["open"] = true;
        }
        if (Json st = rgba_arg(jget<std::string>(o, "stroke", ""));
            !st.is_null()) {
          obj["stroke"] = st;
        }
        style_opts(obj, o);
      } else if (op == "shape") {
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
        style_opts(obj, o);  // its effects
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
        // Its colour, alignment, spacing, effects.
        style_opts(obj, o);
      }
      objects.push_back(std::move(obj));
      st = c.set_markup_objects(*pid, *aid, layer, objects);
    } else if (op == "style") {
      // An object's style changed: style <layer> <object-id> <options>.
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
      bool found = false;
      for (auto& obj : objects) {
        if (jget<std::string>(obj, "id", "") == arg) {
          style_opts(obj, options_arg(args.size() > 5 ? args[5] : ""));
          found = true;
        }
      }
      if (!found) {
        return fail(make_error(Code::NotFound,
                               std::format("layer {} has no object '{}'",
                                           layer.empty() ? "0" : layer,
                                           arg)));
      }
      st = c.set_markup_objects(*pid, *aid, layer, objects);
    } else if (op == "arrange") {
      // arrange <layer> <how> <object-id>...: up or down the order, lined
      // up, spaced evenly (media::arrange_objects).
      if (plain.size() < 6) {
        return usage();
      }
      st = c.arrange_markup(*pid, *aid, layer,
                            {plain.begin() + 5, plain.end()}, arg);
    } else if (op == "connectors") {
      // connectors <layer> add|remove <object-id>...: their default
      // connectors, or none (media::with_default_connectors).
      if (plain.size() < 6 || (arg != "add" && arg != "remove")) {
        return usage();
      }
      st = c.markup_connectors(*pid, *aid, layer,
                               {plain.begin() + 5, plain.end()},
                               arg == "add");
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
      // A track separator: its track of its own in the track view.
      std::printf("  %-3s %s%s %-12s %s%s%s%s%s\n",
                  it->id.empty() ? "0" : it->id.c_str(),
                  it->visible ? "shown " : "hidden",
                  it->locked ? " locked" : "",
                  it->name.empty() ? "-" : it->name.c_str(),
                  what.c_str(), when.c_str(),
                  it->mask ? "  -- masks the layer below" : "", in.c_str(),
                  it->track_separator ? "  -- a track separator" : "");
      // A markup's objects, by id (what `style` and `arrange` name),
      // bottom first: kind, a text's words and alignment, effects, where
      // it lies (its bounds, as the stage outlines it).
      auto src = it->source ? c.project(*pid)->asset(*it->source)
                            : Result<project::Asset>(make_error(
                                  Code::NotFound, "none"));
      if (src.ok() && src->markup) {
        for (const auto& o : src->markup->objects) {
          std::string more;
          if (auto t = jget<std::string>(o, "text", ""); !t.empty()) {
            more += std::format(" \"{}\"", one_line(utf8_truncate(t, 32)));
          }
          if (auto al = jget<std::string>(o, "align", ""); !al.empty()) {
            more += " " + al;
          }
          for (const auto& e : jget(o, "effects", Json::array())) {
            more += " +" + jget<std::string>(e, "kind", "?");
          }
          if (const Json g = jget(o, "gradient", Json()); g.is_object()) {
            more += std::format(" {} gradient ({} stops)",
                                jget<std::string>(g, "kind", "linear"),
                                jget(g, "stops", Json::array()).size());
          }
          if (const Json pts = jget(o, "points", Json()); pts.is_array()) {
            // A path's smooth points by their place from 1 ("~2~4"),
            // and whether it is open.
            std::string smooth;
            const Json f = jget(o, "smooth", Json::array());
            for (std::size_t i = 0; f.is_array() && i < f.size(); ++i) {
              if (f[i].is_boolean() && f[i].get<bool>()) {
                smooth += std::format("~{}", i + 1);
              }
            }
            more += std::format(" {} {}{}{}", pts.size(),
                                smooth.empty() ? "corners" : "points",
                                smooth.empty() ? "" : " smooth" + smooth,
                                jget(o, "open", false) ? " open" : "");
          }
          if (const double t = jget(o, "rotate", 0.0); t != 0) {
            more += std::format(" turned {:.4g}", t);
          }
          if (jget(o, "flip_x", false) || jget(o, "flip_y", false)) {
            more += std::format(" flipped {}{}",
                                jget(o, "flip_x", false) ? "h" : "",
                                jget(o, "flip_y", false) ? "v" : "");
          }
          // Its connectors where they are ("c1@200,100"), its points
          // anchored ("0>r:c2": its point 0 on r's connector c2).
          if (const auto cs = media::connector_points(o); !cs.empty()) {
            more += " connectors";
            for (const auto& cp : cs) {
              more += std::format(" {}@{:.4g},{:.4g}", cp.id, cp.x, cp.y);
            }
          }
          for (const auto& an : jget(o, "anchors", Json::array())) {
            more += std::format(" {}>{}:{}", jget(an, "point", 0),
                                jget<std::string>(an, "to", ""),
                                jget<std::string>(an, "connector", ""));
          }
          const auto r = media::object_bounds(o);
          more += std::format(" at {:.4g},{:.4g} {:.4g}x{:.4g}", r[0], r[1],
                              r[2], r[3]);
          std::printf("        %s %s%s\n",
                      jget<std::string>(o, "id", "").c_str(),
                      jget<std::string>(o, "kind", "").c_str(),
                      more.c_str());
        }
      }
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
  if (cmd == "upscaler") {
    // Settings > Upscaling: an upscaler's values laid on the kept ones
    // ("k=v,..."), or its defaults again (--reset); or the one a kind
    // runs ("use <id>", "use auto [video|image]"); then every one's.
    if (!args.empty() && args[0] == "use") {
      if (args.size() < 2) {
        return usage();
      }
      std::string kind = args.size() > 2 ? args[2] : "";
      if (kind.empty()) {
        const auto* m = c.catalog().find(args[1]);
        kind = m && m->has(models::Capability::UpscaleImage) ? "image"
                                                             : "video";
      }
      if (auto st = c.use_upscaler(kind, args[1]); !st.ok()) {
        return fail(st.error());
      }
    } else if (!args.empty()) {
      bool reset = false;
      Json values = Json::object();
      for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--reset") {
          reset = true;
        } else {
          values.update(tune_spec(args[i]));
        }
      }
      if (auto st = c.set_upscale_tuning(args[0], values, reset);
          !st.ok()) {
        return fail(st.error());
      }
    }
    const Json all = c.upscale_settings();
    for (const auto& k : all["kinds"]) {
      const auto chosen = jget<std::string>(k, "chosen", "");
      const auto runs = jget<std::string>(k, "runs", "");
      std::printf("%s: %s, runs %s\n",
                  k["kind"].get<std::string>().c_str(),
                  chosen.empty() ? "auto" : chosen.c_str(),
                  runs.empty() ? "none (none installed)" : runs.c_str());
    }
    for (const auto& u : all["upscalers"]) {
      std::printf("%-20s %-16s %-6s %s%s%s\n",
                  u["id"].get<std::string>().c_str(),
                  u["name"].get<std::string>().c_str(),
                  u["kind"].get<std::string>().c_str(),
                  u["state"].get<std::string>().c_str(),
                  jget(u, "runs", false) ? "  (in use)" : "",
                  jget(u, "custom", false) ? "  (changed)" : "");
      const Json& t = u["tuning"];
      for (const auto& o : t["options"]) {
        const auto key = o["key"].get<std::string>();
        const auto why = jget<std::string>(o, "why", "");
        if (why == "not-for-model") {
          continue;
        }
        const Json v = jget(t["values"], key.c_str(), Json());
        std::string shown = v.is_boolean() ? (v.get<bool>() ? "on" : "off")
                                           : to_text(v);
        std::printf("  %-12s %s%s\n", key.c_str(), shown.c_str(),
                    why.empty() ? "" : std::format("  ({})", why).c_str());
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
    bool continues = false;
    Json tune = Json::object();
    for (std::size_t i = 1; i < args.size(); ++i) {
      if (args[i] == "--continues") {
        continues = true;
      } else if (args[i] == "--speed") {
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
    auto r = c.tuning(args[0], pref, edit, tune, continues);
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
    assist::OutlineFrames frames;
    // A keyframe's place in the row; "none": the clip has none.
    const auto place = [](const std::string& v) {
      return v == "none" ? -2 : std::atoi(v.c_str());
    };
    for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
      if (args[i] == "--lang") {
        lang = args[i + 1];
      } else if (args[i] == "--first") {
        frames.first = place(args[i + 1]);
      } else if (args[i] == "--last") {
        frames.last = place(args[i + 1]);
      } else if (args[i] == "--seconds") {
        frames.seconds = std::atof(args[i + 1].c_str());
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
    auto t = c.prompt_outline(args[0], row, lang, frames);
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
      std::printf("%s  %lld frames, %.3f s at %g fps, showing %s\n",
                  jget<std::string>(*g, "asset", "").c_str(),
                  static_cast<long long>(jget<std::int64_t>(*g, "frames", 0)),
                  jget(*g, "seconds", 0.0), jget(*g, "fps", 0.0),
                  jget<std::string>(*g, "shows", "").c_str());
      // A timeline's: where its continuation goes on.
      if (const auto tl = jget<std::string>(*g, "composition", "");
          !tl.empty()) {
        std::printf("continues %s from frame %lld\n", tl.c_str(),
                    static_cast<long long>(jget<std::int64_t>(*g, "end", 0)));
      }
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
  if (cmd == "ops") {
    // The ASSET OPERATIONS (DESIGN §4j) as this Mac runs them: each, what
    // it takes, and on what model.
    for (const auto& op : c.asset_operations_json()) {
      std::printf("%-11s %s%s\n", jget<std::string>(op, "name", "").c_str(),
                  jget<std::string>(op, "label", "").c_str(),
                  jget(op, "on_layer", false) ? " (a layer's)" : "");
      for (const auto& t : op["takes"]) {
        const auto model = jget<std::string>(t, "model_name", "");
        std::printf("  %-6s %-14s %s%s\n",
                    jget<std::string>(t, "kind", "").c_str(),
                    jget<std::string>(t, "availability", "").c_str(),
                    model.empty() ? "-" : model.c_str(),
                    jget(t, "ready", false) ? "" : " (not here)");
      }
    }
    return 0;
  }
  if (const AssetOperation* op = find_asset_operation(cmd)) {
    // An ASSET OPERATION (DESIGN §4j): `transcribe`, `summarize`,
    // `separate`, `upscale` (on a composition's --layer) -- one request
    // whatever it is, its task followed, what it made printed: a text's
    // words, a picture's, a clip's or a sound's file.
    if (args.size() < 2) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    auto aid = AssetId::parse(args[1]);
    if (!pid.ok() || !aid) {
      return usage();
    }
    Controller::AssetOpRequest req;
    req.project = *pid;
    req.asset = *aid;
    req.operation = std::string(op->name);
    for (std::size_t i = 2; i + 1 < args.size(); ++i) {
      if (args[i] == "--model") {
        req.model = args[++i];
      } else if (args[i] == "--layer") {
        req.layer = layer_id(args[++i]);
      } else if (args[i] == "--language") {
        req.options["language"] = args[++i];
      } else if (args[i] == "--every") {
        req.options["every"] = every_arg(args[++i]);
      } else if (args[i] == "--size") {
        req.options["size"] = args[++i];
      } else if (args[i] == "--seed") {
        req.options["seed"] = std::atoll(args[++i].c_str());
      } else if (args[i] == "--split") {
        req.options["split"] = args[++i];
      }
    }
    auto made = c.run_asset_operation(std::move(req));
    if (!made.ok()) {
      return fail(made.error());
    }
    if (const int rc = follow(c, made->job); rc != 0) {
      return rc;
    }
    project::Project* p = c.project(*pid);
    for (std::size_t i = 0; i < made->made.size(); ++i) {
      const AssetId id = made->made[i];
      const std::string what = i < op->makes.size()
                                   ? std::string(op->makes[i])
                                   : std::string("made");
      auto a = p->asset(id);
      if (a.ok() && a->kind == project::AssetKind::Text) {
        auto text = p->read_text(id);
        if (!text.ok()) {
          return fail(text.error());
        }
        std::printf("%s %s\n%s", id.str().c_str(), what.c_str(),
                    text->c_str());
        continue;
      }
      auto file = p->media_path(id);
      std::printf("%s %s %s\n", id.str().c_str(), what.c_str(),
                  file.ok() ? file->c_str() : "(not made)");
    }
    return 0;
  }
  // The ATLAS (DESIGN §10a Atlas): the graph the app draws, by column.
  if (cmd == "atlas") {
    const bool as_json = std::erase(args, std::string("--json")) > 0;
    if (args.empty()) {
      return usage();
    }
    auto pid = open_or_fail(c, args[0]);
    if (!pid.ok()) {
      return fail(pid.error());
    }
    auto graph = c.atlas(*pid);
    if (!graph.ok()) {
      return fail(graph.error());
    }
    if (as_json) {
      std::printf("%s\n", to_text(*graph, 2).c_str());
      return 0;
    }
    project::Project* p = c.project(*pid);
    std::map<std::string, std::string> names;
    if (auto all = p->assets(); all.ok()) {
      for (const auto& a : *all) {
        names[a.id.str()] = a.name;
      }
    }
    auto name = [&](const std::string& id) {
      const auto f = names.find(id);
      return f == names.end() ? id : f->second;
    };
    auto nodes = (*graph)["nodes"];
    std::stable_sort(nodes.begin(), nodes.end(),
                     [](const Json& a, const Json& b) {
                       const int ca = a["column"].get<int>();
                       const int cb = b["column"].get<int>();
                       if (ca != cb) {
                         return ca < cb;
                       }
                       return a["row"].get<double>() < b["row"].get<double>();
                     });
    std::printf("%zu nodes, %zu edges, %d columns, %.2f slots high\n",
                nodes.size(), (*graph)["edges"].size(),
                (*graph)["columns"].get<int>(),
                (*graph)["height"].get<double>());
    for (const auto& n : nodes) {
      const auto type = jget<std::string>(n, "type", "");
      std::string what;
      if (type == "asset") {
        what = std::format("{}", name(jget<std::string>(n, "asset", "")));
        if (const auto d = jget<std::string>(n, "destination", "");
            !d.empty()) {
          what += std::format("  [{}]", d);
        }
        if (jget(n, "project", false)) {
          what += "  [project]";
        }
        if (jget(n, "leaf", false)) {
          what += "  leaf";
        }
        if (jget(n, "loose", false)) {
          what += "  loose";
        }
      } else if (type == "layer") {
        std::string look;
        for (const auto& l : n.value("look", Json::array())) {
          look += (look.empty() ? "" : ",") + l.get<std::string>();
        }
        const auto layer = jget<std::string>(n, "layer", "");
        what = std::format(
            "layer {} -> {}  {}", layer.empty() ? "0" : layer,
            name(jget<std::string>(n, "into", "")),
            look.empty() ? "as it is" : look);
      } else {
        const auto model = jget<std::string>(n, "model", "");
        what = std::format("{}{} -> {}", jget<std::string>(n, "op", ""),
                           model.empty() ? "" : " · " + model,
                           name(jget<std::string>(n, "into", "")));
      }
      std::printf("  c%-2d %6.2f  %-5s %s\n", n["column"].get<int>(),
                  n["row"].get<double>(),
                  type == "asset" ? "asset" : "op", what.c_str());
    }
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
      // An enhanced one: the prompt it was made from (its upstream).
      std::string from;
      if (auto r = p->recipe(a.recipe); r.ok() && !r->inputs.empty()) {
        from = "  <- " + r->inputs.front().asset.str();
      }
      std::printf("%s  v%u  %zu made%s  %s\n", a.id.str().c_str(), a.head,
                  deps.ok() ? deps->size() : 0, from.c_str(),
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
    if (args[1] == "apply") {
      if (args.size() < 4) {
        return usage();
      }
      Controller::SuggestionApplied req;
      req.project = *pid;
      req.before = args[2];
      req.enhanced = args[3];
      for (std::size_t i = 4; i + 1 < args.size(); i += 2) {
        if (args[i] == "--from") {
          req.from = AssetId::parse(args[i + 1]);
        } else if (args[i] == "--for") {
          req.written_for = args[i + 1];
        } else if (args[i] == "--name") {
          req.name = args[i + 1];
        } else {
          return usage();
        }
      }
      auto r = c.apply_suggestion(req);
      if (!r.ok()) {
        return fail(r.error());
      }
      std::cout << "before " << r->before.str() << "\n"
                << "enhanced " << r->enhanced.str() << "\n";
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
    int repeat = 1;
    for (std::size_t i = 2; i < args.size(); ++i) {
      const std::string& a = args[i];
      auto next = [&]() -> std::string {
        return i + 1 < args.size() ? args[++i] : std::string();
      };
      if (a == "--model") {
        req.model = next();
      } else if (a == "--repeat") {
        repeat = std::max(1, std::atoi(next().c_str()));
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
    if (repeat == 1) {
      auto j = c.generate_image(req);
      if (!j.ok()) {
        return fail(j.error());
      }
      return follow(c, *j);
    }
    // Made again in this process, as the app makes a picture after the
    // first: what the session keeps (tuned kernels, models kept, the
    // conditioning) found. Each run's phases from its version's timing.
    for (int run = 1; run <= repeat; ++run) {
      auto j = c.generate_image(req);
      if (!j.ok()) {
        return fail(j.error());
      }
      std::string made;
      if (const int rc = follow(c, *j, &made); rc != 0) {
        return rc;
      }
      std::string line;
      if (const auto aid = AssetId::parse(made)) {
        if (auto v = c.project(*pid)->version(*aid); v.ok()) {
          line = std::format("{:.2f} s", jget(v->timing, "seconds", 0.0));
          for (const auto& ph : jget(v->timing, "sequence",
                                     Json::array())) {
            if (ph.is_array() && ph.size() == 2) {
              line += std::format(" · {} {:.2f}", ph[0].get<std::string>(),
                                  ph[1].get<double>());
            }
          }
        }
      }
      std::fprintf(stderr, "  run %d of %d: %s\n", run, repeat,
                   line.c_str());
      if (req.seed >= 0) {
        ++req.seed;
      }
    }
    return 0;
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
      } else if (a == "--first" || a == "--last" || a == "--ref" ||
                 a == "--continue") {
        auto r = reference_asset(c, *pid, next());
        if (!r.ok()) {
          return fail(r.error());
        }
        if (a == "--first") {
          req.first = *r;
        } else if (a == "--last") {
          req.last = *r;
        } else if (a == "--ref") {
          req.references.push_back(*r);
        } else {
          req.continue_from = *r;
        }
      } else if (a == "--whole") {
        // The --ref before it kept whole: not cut to the clip's length.
        if (req.references.empty()) {
          return usage();
        }
        req.whole.push_back(req.references.back());
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
        // Drama 3's speakers, each --voice the next.
        auto r = reference_asset(c, *pid, next(), project::Placement::Auto);
        if (!r.ok()) {
          return fail(r.error());
        }
        req.row.push_back(*r);
        if (a == "--voice") {
          if (!req.voice) {
            req.voice = *r;
          }
          req.voices.push_back(*r);
        }
      } else if (a == "--voice-text") {
        // The words the latest --voice says (Drama 3): none, heard.
        if (req.voices.empty()) {
          return usage();
        }
        req.voice_texts.resize(req.voices.size());
        req.voice_texts.back() = next();
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
    bool speed = false;   // --speed: its prefill and decode
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
      } else if (a == "--speed") {
        speed = true;
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
                 a == "--last" || a == "--continue") {
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
        } else if (a == "--last") {
          req.last = *r;
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
        if (speed && ev.kind == "assist.enhanced") {
          std::fprintf(stderr, "  · speed: %s\n",
                       turn_speed_line(jget(ev.data, "stats",
                                            Json::object())).c_str());
        }
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
    // [rate=44100] [intermediate=prores4444]: the project's output
    // (DESIGN §6b), shown -- or set.
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
      } else if (k == "intermediate") {
        want.intermediate = v;
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
    // A VOICE-OVER onto the composition, from its frame; the picture as
    // if started `cue` seconds in (its head trimmed by as much).
    std::optional<std::int64_t> voice_at;
    std::optional<double> cue_after;
    for (std::size_t i = first; i + 1 < args.size(); ++i) {
      if (args[i] == "--source") {
        source = args[++i];
      } else if (args[i] == "--seconds") {
        seconds = std::atof(args[++i].c_str());
      } else if (args[i] == "--voice-over") {
        voice_at = std::atoll(args[++i].c_str());
      } else if (args[i] == "--cue-after") {
        cue_after = std::atof(args[++i].c_str());
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
    if (auto st = c.start_capture(*pid, aid, source, voice_at); !st.ok()) {
      return fail(st.error());
    }
    // The host clock's seconds (mach_absolute_time's, as
    // CACurrentMediaTime counts them).
    const auto host_now = [] {
      return static_cast<double>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW))
             / 1e9;
    };
    bool cued = false;
    for (double t = 0; t < seconds; t += 0.25) {
      if (voice_at && cue_after && !cued && t >= *cue_after) {
        cued = c.cue_capture(host_now()).ok();
      }
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
  if (cmd == "compress") {
    int bits = 0;
    bool keep_original = false;
    std::string use;
    std::string keep;
    for (std::size_t i = 0; i < args.size();) {
      const bool value = i + 1 < args.size();
      if (args[i] == "--keep-original") {
        keep_original = true;
      } else if (value && (args[i] == "--bits" || args[i] == "--use" ||
                           args[i] == "--keep")) {
        if (args[i] == "--bits") {
          bits = std::atoi(args[i + 1].c_str());
        } else {
          (args[i] == "--use" ? use : keep) = args[i + 1];
        }
        args.erase(args.begin() + static_cast<std::ptrdiff_t>(i));
      } else {
        ++i;
        continue;
      }
      args.erase(args.begin() + static_cast<std::ptrdiff_t>(i));
    }
    const auto which = [](const std::string& w) {
      return w == "compressed" ? 1 : w == "original" ? 0 : -1;
    };
    if (!args.empty() && (!use.empty() || !keep.empty())) {
      const int w = which(use.empty() ? keep : use);
      if (w < 0) {
        return usage();
      }
      if (!use.empty()) {
        if (auto st = c.use_compressed(args[0], w == 1); !st.ok()) {
          return fail(st.error());
        }
        std::printf("%s: its %s in use\n", args[0].c_str(),
                    w == 1 ? "compressed copy" : "original");
        return 0;
      }
      auto freed = c.keep_compressed(args[0], w == 1);
      if (!freed.ok()) {
        return fail(freed.error());
      }
      std::printf("%s: its %s kept, %.2f GB freed\n", args[0].c_str(),
                  w == 1 ? "compressed copy" : "original",
                  static_cast<double>(*freed) / 1e9);
      return 0;
    }
    if (args.empty() || bits == 0) {
      return usage();
    }
    auto j = c.compress_model(args[0], bits, keep_original);
    if (!j.ok()) {
      return fail(j.error());
    }
    return follow(c, *j);
  }
  if (cmd == "fleet") {
    return fleet_command(c, args);
  }
  if (cmd == "agent") {
    return agent_command(c, args);
  }
  if (cmd == "sources") {
    // Settings › Capabilities' Download From: vpipe's sources, the one
    // chosen -- and a name, chosen for every download after.
    if (!args.empty()) {
      if (Status st = c.set_download_source(args[0]); !st.ok()) {
        return fail(st.error());
      }
    }
    const Json d = c.download_sources();
    const auto chosen = jget<std::string>(d, "chosen", "");
    for (const auto& s : jget(d, "sources", Json::array())) {
      const auto name = jget<std::string>(s, "name", "");
      std::printf("%s %-12s %s%s\n", name == chosen ? "*" : " ",
                  name.c_str(), jget<std::string>(s, "label", "").c_str(),
                  jget(s, "default", false) ? "  (vpipe's default)" : "");
    }
    return 0;
  }
  if (cmd == "wired") {
    // Settings › General › Memory: the wired limit -- set, or Automatic.
    if (!args.empty()) {
      std::uint64_t bytes = 0;
      if (args[0] != "auto") {
        char* end = nullptr;
        const double gb = std::strtod(args[0].c_str(), &end);
        if (end == args[0].c_str() || *end != '\0' || gb <= 0) {
          return usage();
        }
        bytes = static_cast<std::uint64_t>(gb * (1ull << 30));
      }
      if (Status st = c.set_wired_limit(bytes); !st.ok()) {
        return fail(st.error());
      }
    }
    const Json w = c.wired_limit();
    auto gb = [](const Json& j, const char* k) {
      return static_cast<double>(jget<std::uint64_t>(j, k, 0)) /
             static_cast<double>(1ull << 30);
    };
    if (jget<std::uint64_t>(w, "chosen", 0) == 0) {
      std::printf("limit     Automatic (%.1f GB here)\n",
                  gb(w, "automatic"));
    } else {
      std::printf("limit     %.1f GB (Automatic: %.1f GB)\n",
                  gb(w, "chosen"), gb(w, "automatic"));
    }
    std::printf("in force  %.1f GB%s\n", gb(w, "in_force"),
                jget(w, "pending", false)
                    ? "  (the lower one waits for the work under way)"
                    : "");
    std::printf("wired     %.1f GB now\n", gb(w, "used"));
    std::printf("range     %.1f-%.1f GB (the most the GPU keeps "
                "resident)\n", gb(w, "min"), gb(w, "max"));
    return 0;
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
    // Settings › Capabilities' Pause and Cancel (stop_download): s into
    // the download -- paused, what came down kept for the next
    // `download` to go on from; cancelled, deleted -- or, --cancel
    // alone, what a paused one left deleted.
    const bool discard = std::erase(args, std::string("--cancel")) > 0;
    double pause_after = -1;
    double cancel_after = -1;
    std::string from;  // a download source, "" the one chosen
    for (std::size_t i = 0; i + 1 < args.size();) {
      if (args[i] == "--from") {
        from = args[i + 1];
        args.erase(args.begin() + static_cast<std::ptrdiff_t>(i),
                   args.begin() + static_cast<std::ptrdiff_t>(i) + 2);
      } else if (args[i] == "--pause-after" || args[i] == "--cancel-after") {
        (args[i] == "--pause-after" ? pause_after : cancel_after) =
            std::atof(args[i + 1].c_str());
        args.erase(args.begin() + static_cast<std::ptrdiff_t>(i),
                   args.begin() + static_cast<std::ptrdiff_t>(i) + 2);
      } else {
        ++i;
      }
    }
    if (args.empty()) {
      return usage();
    }
    const std::string id = args[0];
    // What is on disk of it now.
    const auto left = [&c, &id] {
      const auto* e = c.catalog().find(id);
      if (!e) {
        return;
      }
      const auto i = c.model_store().info(*e);
      std::printf("%s: %s%s\n", id.c_str(), models::to_str(i.state),
                  i.state == models::InstallState::Missing
                      ? ""
                      : std::format(", {} at {}", models::size_text(i.bytes),
                                    i.dir.string()).c_str());
    };
    if (discard && pause_after < 0 && cancel_after < 0) {
      Status st = c.stop_download(id, true);
      if (!st.ok()) {
        return fail(st.error());
      }
      left();
      return 0;
    }
    auto j = c.download_model(id, token, from);
    std::fill(token.begin(), token.end(), '\0');
    if (!j.ok()) {
      return fail(j.error());
    }
    std::jthread stopper;
    if (pause_after >= 0 || cancel_after >= 0) {
      const bool drop = cancel_after >= 0;
      const auto wait = std::chrono::duration<double>(
          drop ? cancel_after : pause_after);
      stopper = std::jthread([&c, id, drop, wait](std::stop_token stop) {
        const auto until = std::chrono::steady_clock::now() + wait;
        while (!stop.stop_requested() &&
               std::chrono::steady_clock::now() < until) {
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (stop.stop_requested()) {
          return;
        }
        std::fprintf(stderr, "  %s\n", drop ? "cancelling" : "pausing");
        if (Status st = c.stop_download(id, drop); !st.ok()) {
          std::fprintf(stderr, "  %s\n", st.error().message.c_str());
        }
      });
    }
    const int rc = follow(c, *j);
    stopper.request_stop();
    left();
    return rc;
  }
  return usage();
}
