// The on-device assistant: prompt enhancement and intent detection.
//
// Everything here is PROMPT CONSTRUCTION and REPLY PARSING -- pure
// functions, testable without a model. The controller runs the prompt
// through the engine's `chat` op on the tier's assistant model (Qwen3.5
// 9B on 16 GB, a 27B from 24 GB; see models/capabilities.h).
//
// vpipe has no constrained (grammar/JSON-schema) decoding yet, so replies
// are asked for as a single JSON object, extracted leniently (code
// fences and chatter around it are tolerated) and validated here. A
// reply that does not validate is an error the caller can retry.
//
// Intent detection has a heuristic path that needs no model: it answers
// instantly while the LLM is loading or absent, and the LLM refines it.

#ifndef VALTZ_ASSIST_ASSISTANT_H
#define VALTZ_ASSIST_ASSISTANT_H

#include "valtz/base/json.h"
#include "valtz/base/result.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace valtz::assist {

enum class Intent : std::uint8_t {
  GenerateImage,
  EditImage,       // change an attached picture
  GenerateVideo,
  AnimateImage,    // attached picture -> video
  Upscale,
  Describe,        // caption / describe the attachments
  Unknown,
};

const char* to_str(Intent);
Intent intent_from_str(std::string_view);

struct Attachment {
  std::string kind;     // "image", "video", "audio", "text"
  std::string name;
  std::string caption;  // when already known
};

struct IntentResult {
  Intent      intent = Intent::Unknown;
  float       confidence = 0.0f;
  std::string subject;     // what the request is about, if named
  Json        params = Json::object();  // aspect, duration, style, ...
  bool        from_model = false;
};

struct EnhanceRequest {
  std::string             prompt;
  // "image" | "video" | "audio" (a song: style tags, then lyrics under
  // section headers -- what split_song reads)
  std::string             target = "image";
  std::string             style;             // optional user style hint
  std::vector<Attachment> attachments;
  // The instructions written for the target model (Controller::
  // skill_text): its makers' own rewriter, a skill of Valtz's or an
  // extension's; empty = Valtz's generic ones. Qwen-Image's ask for
  // {"rewritten_prompt" or "prompt", "wh_ratio", "ratio_follow"}.
  std::string             system;
  // Pictures the assistant is shown ahead of the text (the engine puts
  // them there), numbered <image1>.. in that order -- the base first.
  int                     pictures = 0;
  // The output size the user chose ("1600x1216"): part of the request,
  // so a rewriter that picks a shape picks that one. "" = its choice.
  std::string             size;
  // The model reads English only (catalog `prompting.language`: Krea 2,
  // FLUX.2): the prompt is written in English, a request in another
  // language translated. Otherwise it keeps the request's language --
  // Qwen-Image, MiniMax H3 and YuE2 read Chinese as they read English.
  bool                    english = false;
  // A song's length as the user set it (the Length row), in seconds:
  // the lyrics are written to fit it. 0 = the model decides. A clip's
  // length, for a clip written to its makers' guide (below).
  double                  seconds = 0;
  // SPEECH (an audio target the model speaks: MOSS-TTS's skill): its
  // length is the words', and `voice` says a reference voice is given --
  // the instruction then describes the delivery, not the voice.
  bool                    speech = false;
  bool                    voice = false;
  // A clip written to the prompt-writing guide its makers publish
  // (`system`: MiniMax H3's skill; set, the reply comes a section per
  // key -- clip_sections). The guide names the modes; Valtz says which
  // this is -- "T2VA" (words alone), "I2VA"
  // (opening on <Picture 1>, its first frame), "Ref2VA" (full
  // reference) -- and lists the references one line each, named as the
  // model names them ("<Picture 1>: a picture, \"cat.jpg\" -- image 1
  // shown above"). A small assistant told only the guide guesses the
  // mode, the length and the labels.
  std::string              video_mode;
  std::vector<std::string> references;
  // Ref2VA: the summary's task types as the references are used
  // ("reference generation + audio reuse").
  std::string              video_tasks;
};

struct EnhanceResult {
  // The suggestion. A song's reply in parts -- {"style", "lyrics"} -- is
  // put together as the prompt holds a song: the style line, a blank
  // line, the lyrics (split_song reads it back).
  std::string prompt;
  std::string negative;
  std::string title;       // a short name for the result asset
  // From a model's own rewriter: the aspect ratio the prompt was
  // written for ("3:2"), or the picture whose shape the result follows
  // ("<image1>"). Empty when it said nothing.
  std::string aspect;
  std::string follow;
};

// ---- pictures in a prompt ---------------------------------------------

// Where a picture sits inline in a prompt: U+FFFC, the object
// replacement character the app's text view puts for an attachment.
inline constexpr std::string_view kInlinePicture = "\xEF\xBF\xBC";

// What the model calls an inline picture: its number in the pictures
// the model is given (1-based, the base first; 0 = one it is not
// given), and whether it is the base.
struct InlinePicture {
  int  number = 0;
  bool base = false;
};

// `prompt` with the k-th inline picture replaced by its tag --
// `tag_format` with "{n}" its number ("<image{n}>" gives "<image2>") --
// or dropped: one the model is not given, any when `tag_format` is
// empty, and the base when nothing but space comes before it (the
// picture being edited needs no name at the start). Runs of spaces left
// behind are collapsed and the ends trimmed.
std::string tag_pictures(std::string_view prompt,
                         const std::vector<InlinePicture>& at,
                         std::string_view tag_format);

// ---- references in a prompt -----------------------------------------
//
// A prompt REFERS to the media in its row (the prompt's reference row)
// in two ways (DESIGN §10c):
//   * a MENTION (U+FFFC, kInlinePicture, with the medium it stands for)
//     is bound to the medium: it follows it wherever the row puts it;
//   * a TAG -- "<valtz_ref_img_0>" -- is bound to a POSITION: the row's
//     first picture ("img"), clip ("vid") or sound ("aud"), counted from
//     0 within its kind, in the row's order. Whatever is there now names
//     it; nothing there yet leaves it UNBOUND.
// A captured prompt (a prompt asset) holds tags only: its mentions are
// made the tags of where their media sat. A model is given neither: each
// is replaced by what that model calls the medium (<image2>, <Picture
// 1>) -- through the mention form, which every model's tagging reads.

enum class RefKind : std::uint8_t { Image, Video, Audio, Other };

// "<valtz_ref_img_0>", "<valtz_ref_vid_1>", "<valtz_ref_aud_2>".
std::string ref_tag(RefKind, int index);

struct RefTag {
  RefKind     kind = RefKind::Other;
  int         index = 0;
  std::size_t pos = 0;   // in the text, bytes
  std::size_t len = 0;
};
// The tags in `text`, in order.
std::vector<RefTag> find_ref_tags(std::string_view text);

// `prompt` with its k-th mention made the tag of its medium's place:
// `mentions[k]` is the medium's index in `row` (the kinds of the row's
// media, in its order), or -1 -- not in the row: dropped with its space.
std::string positional_prompt(std::string_view prompt,
                              const std::vector<int>& mentions,
                              const std::vector<RefKind>& row);

// The tags of `text` that name nothing in `row`, as written.
std::vector<std::string> unbound_ref_tags(std::string_view text,
                                          const std::vector<RefKind>& row);

// Back to the mention form: each tag made U+FFFC, and the index in
// `row` of the medium it names (-1 unbound), in order -- with the text's
// own mentions passed through, `mentions` theirs. What the models'
// tagging (tag_pictures, tag_inline) reads.
std::string mention_form(std::string_view text,
                         const std::vector<RefKind>& row,
                         const std::vector<int>& mentions,
                         std::vector<int>* out);

// A model's prompt OUTLINE (catalog `prompting.outline`): the form its
// prompts were trained in -- MiniMax H3 Ref2VA's six sections, FL2VA's
// three, a song's style and sections -- each with a short hint, for an
// empty prompt box to start from (the assistant's button, with nothing
// to enhance). It is written for what the prompt's ROW holds, so its
// references are already named where they belong:
//   * a line with {image}, {video} or {audio} is written once for each
//     medium of that kind in `row`, in order, the marker made its tag
//     (<valtz_ref_img_0>, ...); with none of that kind it is left out.
//     One opening with {each image} (video, audio) is too, the marker
//     dropped: a line about each that need not name it;
//   * {subject} is the next subject number, counted from 1 again in
//     each section (the lines between blank lines): two sections that
//     list the same media number them alike;
//   * a line opening with {if image}, {if video} or {if audio} is kept,
//     the marker dropped, only when the row holds that kind; {if none}
//     only when it holds nothing.
// The outline is a string, its lines as an array, or either by language
// ({"en": ..., "zh-Hans": ...}, picked as text_in picks for `language`).
// "" for a model without one.
std::string prompt_outline(const Json& outline, std::string_view language,
                           const std::vector<RefKind>& row);

// The words alone: mentions and tags dropped, spaces as tag_pictures
// leaves them -- what names a result.
std::string prompt_words(std::string_view prompt);

// Words without their Markdown (the app's prompt editor writes it, the
// model is sent it as written): `**`, `*`, `__`, `_` around a word,
// `~~`, backticks, `<u>` / `</u>`, a heading's `#`s. For a NAME made from
// a prompt.
std::string without_markdown(std::string_view words);

// ---- a song's words --------------------------------------------------

// A SONG's words, as YuE2 reads them: its STYLE -- genre, mood,
// instruments, voice: the [Tags] -- and its LYRICS, sectioned by
// headers on lines of their own ([Intro], [Verse], [Chorus], [Bridge],
// [Outro]; an empty section is an instrumental one). One prompt holds
// both: the lines before the first header are the style, the rest are
// the lyrics -- none, and the description alone makes the music. A
// "[Style]" or "[Tags]" section is the style wherever it is. The
// style's lines are joined as one list of tags (", "); the lyrics are
// kept as written, blank lines between sections and all.
struct SongText {
  std::string style;
  std::string lyrics;
  int         sections = 0;  // the lyrics' headers
  int         lines = 0;     // their sung lines: not headers, not blank
};
SongText split_song(std::string_view prompt);

// SPEECH's words, as MOSS-TTS reads them (DESIGN §4g): the prompt's
// FIELDS -- lines "Instruction: ...", "Quality: ...", "Sound event:
// ...", "Ambient sound: ...", "Language: ..." at its top (any case; "-"
// before, "_" for the space, as MOSS's own prompt has them) -- then the
// TEXT, the words spoken: everything after. A field left as its hint (a
// value all in parentheses) or "None" is unset, and a line of the text
// all in parentheses (the outline's hint) is not spoken. Each field
// conditions the whole utterance; "[pause 1.5s]" in the text is a pause.
struct SpeechText {
  std::string instruction;
  std::string quality;
  std::string sound_event;
  std::string ambient_sound;
  std::string language;
  std::string text;
};
SpeechText split_speech(std::string_view prompt);
// The prompt of `s`: its set fields, a blank line, its text.
std::string speech_prompt(const SpeechText& s);
// The language of `text` by its script, by its English name as MOSS-TTS
// takes it ("Chinese", "Japanese", "Korean", "Russian", ...; "English"
// for plain ASCII letters); "" when it cannot tell (Latin with accents:
// French, German, ... -- the model decides).
std::string speech_language(std::string_view text);

// A suggested song with the request's own lyrics put back. Lyrics under
// section headers in the request are the person's words, and a small
// assistant told to keep them word for word does not always: the
// suggestion's style, then the request's lyrics as written. A request
// without sectioned lyrics leaves the suggestion as it is.
std::string keep_song_lyrics(std::string_view suggestion,
                             std::string_view request);

// `prompt` with the k-th inline item (U+FFFC) replaced by `tags[k]`, or
// dropped where that is empty or missing -- a clip's references named
// as its model names them ("<Picture 1>", "<Video 1>", "<Audio 1>").
// Spaces as tag_pictures leaves them.
std::string tag_inline(std::string_view prompt,
                       const std::vector<std::string>& tags);

// Enhancement instructions compiled in, by name: a model maker's skills
// from 3rd-party/ (with their licenses) -- "minimax-h3-base" (MiniMax
// H3's prompt-writing skill with its T2VA / I2VA / FL2VA / L2VA guide)
// and "minimax-h3-ref" (with the full-reference guide too) -- and
// Valtz's own skills from core/resources/skills -- "yue2-song" (a style
// line and sectioned lyrics, replied as {"style", "lyrics", "title"}),
// "moss-tts", and "qwen-image-2.1-t2i" / "qwen-image-2.1-edit"
// (Qwen-Image's, which QwenLM's own rewriters replace once downloaded:
// Controller::skill_text). Empty for a name it does not know.
std::string_view rewrite_prompt(std::string_view name);

// `text` with each name a model writes (first: "<Picture 1>") made what
// the prompt box binds (second: "<valtz_ref_img_0>", the row's tag), in
// one pass -- a suggestion written in the model's names, put back in the
// row's. Names not listed stay as written ("<Subject 1>").
std::string retag(std::string_view text,
                  const std::vector<std::pair<std::string, std::string>>&
                      names);

std::string build_enhance_prompt(const EnhanceRequest&);
// The reply's "prompt" -- or a rewriter's "rewritten_prompt", a song's
// "style" and "lyrics", a clip's sections (clip_sections).
Result<EnhanceResult> parse_enhance_reply(std::string_view reply);

// A clip's prompt from the sections a reply gives one by one (MiniMax
// H3's skill: a small assistant asked for the whole text leaves one out,
// repeats one, reorders them): put together in its guide's order and
// form -- the base modes' "integrated_multimodal_description: [Shot
// 1] ...", "overall_soundscape: ...", "non_diegetic_music: ..." a blank
// line apart; the full-reference mode's six, each name on a line of its
// own and its text after it. A section's name repeated at its head is
// dropped; missing ones are left out (a reply still being written).
std::string clip_sections(const std::map<std::string, std::string>&);

// The suggestion so far, from a reply still being written (the chat's
// streamed text): what parse_enhance_reply will make the prompt -- the
// "prompt" (a rewriter's "rewritten_prompt") as far as it has come, or a
// song's style and lyrics -- its escapes decoded, an escape or a
// character cut short left out. Empty until its words begin. It is what
// the person watches being written; the finished reply is still parsed
// whole (keep_song_lyrics may yet change a song's lyrics).
std::string partial_enhance_reply(std::string_view reply);

std::string build_intent_prompt(std::string_view text,
                                const std::vector<Attachment>&);
Result<IntentResult> parse_intent_reply(std::string_view reply);

IntentResult heuristic_intent(std::string_view text,
                              const std::vector<Attachment>&);

// The first balanced {...} object in `s`, parsed; tolerates fences and
// surrounding prose.
Result<Json> extract_json_object(std::string_view s);

void to_json(Json&, const IntentResult&);
void to_json(Json&, const EnhanceResult&);

}

#endif
