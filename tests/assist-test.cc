#include "testing.h"

#include <cmath>

#include "valtz/assist/assistant.h"
#include "valtz/assist/drama3.h"
#include "valtz/assist/moss-image.h"
#include "valtz/assist/segments.h"
#include "valtz/assist/transcript.h"
#include "valtz/assist/video-summary.h"
#include "valtz/base/text.h"
#include "valtz/models/catalog.h"

using namespace valtz;
using namespace valtz::assist;

TEST(assist, extracts_json_from_chatty_replies)
{
  auto r = extract_json_object(
      "Sure! Here you go:\n```json\n{\"prompt\": \"a {braced} fox\", "
      "\"negative\": \"blur\"}\n```\nEnjoy.");
  REQUIRE_OK(r);
  CHECK(jget<std::string>(*r, "prompt", "") == "a {braced} fox");
  CHECK(!extract_json_object("no json here").ok());
  CHECK(!extract_json_object("{broken").ok());
}

TEST(assist, parses_enhance_reply)
{
  auto r = parse_enhance_reply(
      "{\"prompt\": \"A red fox in fresh snow at dawn, low sun\", "
      "\"negative\": \"blurry\", \"title\": \"Fox at dawn\"}");
  REQUIRE_OK(r);
  CHECK(r->title == "Fox at dawn");
  CHECK(!parse_enhance_reply("{\"prompt\": \"\"}").ok());
}

TEST(assist, parses_intent_reply)
{
  auto r = parse_intent_reply(
      "{\"intent\": \"animate-image\", \"confidence\": 1.7, "
      "\"params\": {\"aspect\": \"16:9\"}}");
  REQUIRE_OK(r);
  CHECK(r->intent == Intent::AnimateImage);
  CHECK(r->confidence <= 1.0f);
  CHECK(!parse_intent_reply("{\"intent\": \"fly\"}").ok());
}

TEST(assist, heuristic_intents)
{
  std::vector<Attachment> img = {{"image", "cat.png", ""}};
  CHECK(heuristic_intent("a castle at night", {}).intent ==
        Intent::GenerateImage);
  CHECK(heuristic_intent("make the sky purple", img).intent ==
        Intent::EditImage);
  CHECK(heuristic_intent("animate this, slow pan", img).intent ==
        Intent::AnimateImage);
  CHECK(heuristic_intent("a 10 second clip of waves", {}).intent ==
        Intent::GenerateVideo);
  CHECK(heuristic_intent("upscale to 4k", img).intent == Intent::Upscale);
  CHECK(heuristic_intent("", img).intent == Intent::Describe);
  // Word boundaries: "addition" is not "add", "cliparts" not "clip".
  CHECK(heuristic_intent("cliparts of an addition sign", img).intent !=
        Intent::GenerateVideo);
  CHECK(jget<std::string>(heuristic_intent("cinematic wide shot", {}).params,
                          "aspect", "") == "16:9");
}

// An inline picture becomes what the model calls it; the base at the
// very start needs no name, and one the model is not given is dropped
// with its space.
// A song's words: the lines before the first section header are its
// style, the rest its lyrics; a [Style] section is the style wherever it
// is; none, and the description alone makes the music.
TEST(assist, a_song_s_style_and_lyrics)
{
  SongText t = split_song("Indie pop, warm female vocal,\n"
                          "acoustic guitar\n\n[Intro]\n\n[Verse]\n"
                          "Streetlights hum a quiet tune\r\n"
                          "Shadows dancing with the moon\n\n"
                          "[Chorus 2]\nStay awake with me tonight\n\n");
  CHECK(t.style == "Indie pop, warm female vocal, acoustic guitar");
  CHECK(t.lyrics == "[Intro]\n\n[Verse]\nStreetlights hum a quiet tune\n"
                    "Shadows dancing with the moon\n\n[Chorus 2]\n"
                    "Stay awake with me tonight");
  CHECK(t.sections == 3 && t.lines == 3);
  SongText d = split_song("  Lo-fi hip hop, rainy night  ");
  CHECK(d.style == "Lo-fi hip hop, rainy night");
  CHECK(d.lyrics.empty() && d.sections == 0);
  // Lyrics alone; a style section after them.
  SongText l = split_song("[Verse]\nla la\n[Style]\njazz\nswing\n");
  CHECK(l.style == "jazz, swing");
  CHECK(l.lyrics == "[Verse]\nla la");
  // Brackets inside a line are words, not a header.
  SongText w = split_song("[Verse]\nsing [softly] now\n[not a] header]");
  CHECK(w.sections == 1 && w.lines == 2);
  CHECK(split_song("").style.empty());
  // A CAPTION model's style (MiniMax-Music3's): its lines kept, one under
  // another, commas and all; the lyrics as ever.
  const std::string cap =
      "Global Metadata\nBasic Attributes: bpm is 96. Acoustic Pop,\n\n"
      "Vocal Details\nVocal Gender & Timbre: Singer A (Female).\n\n"
      "[Verse]\nMorning light\n\n[Instrumental]\n";
  SongText m = split_song(cap, true);
  CHECK(m.style == "Global Metadata\nBasic Attributes: bpm is 96. "
                   "Acoustic Pop,\nVocal Details\nVocal Gender & Timbre: "
                   "Singer A (Female).");
  CHECK(m.lyrics == "[Verse]\nMorning light\n\n[Instrumental]");
  CHECK(m.sections == 2 && m.lines == 1);
  CHECK(split_song(cap).style.find('\n') == std::string::npos);
}

// A clip's references, named as its model names them, where they are
// mentioned; one it does not read is dropped with its space.
TEST(assist, inline_media_become_their_tags)
{
  const std::string P(kInlinePicture);
  CHECK(tag_inline(P + " sings " + P + " on " + P,
                   {"<Picture 1>", "<Audio 1>", ""}) ==
        "<Picture 1> sings <Audio 1> on");
  CHECK(tag_inline("as " + P + "  moves", {}) == "as moves");
}

// A prompt's references (DESIGN §10c): a mention becomes the tag of
// where its medium sits in the row, counted within its kind; a tag names
// whatever is at its place -- or nothing yet; back in the mention form,
// each names its medium's place in the row, for the model's own tagging.
TEST(assist, positional_reference_tags)
{
  const std::string P(kInlinePicture);
  using K = RefKind;
  const std::vector<K> row = {K::Image, K::Video, K::Image, K::Audio};
  CHECK(ref_tag(K::Image, 0) == "<valtz_ref_img_0>");
  // The row's 3rd medium is its 2nd picture; the 2nd its 1st clip.
  CHECK(positional_prompt("put " + P + " in " + P, {2, 1}, row) ==
        "put <valtz_ref_img_1> in <valtz_ref_vid_0>");
  // A mention of nothing in the row is dropped with its space.
  CHECK(positional_prompt("a " + P + " b", {-1}, row) == "a b");
  const std::string text = "<valtz_ref_img_1> sings <valtz_ref_aud_0> "
                           "over <valtz_ref_aud_1> <valtz_ref_x_0>";
  const auto tags = find_ref_tags(text);
  REQUIRE(tags.size() == 3u);
  CHECK(tags[0].kind == K::Image && tags[0].index == 1);
  CHECK(tags[2].kind == K::Audio && tags[2].index == 1);
  CHECK((unbound_ref_tags(text, row) ==
         std::vector<std::string>{"<valtz_ref_aud_1>"}));
  // Mentions and tags in one text: each a U+FFFC with its row index.
  std::vector<int> at;
  const std::string m = mention_form(P + " then <valtz_ref_img_0>", row,
                                     {3}, &at);
  CHECK(m == P + " then " + P);
  CHECK((at == std::vector<int>{3, 0}));
  // The words alone.
  CHECK(prompt_words("<valtz_ref_img_0> in the " + P + " rain") ==
        "in the rain");
  CHECK(prompt_words("the cat from <valtz_ref_img_0>, sitting") ==
        "the cat from, sitting");
}

TEST(assist, inline_pictures_become_tags)
{
  const std::string P(kInlinePicture);
  const std::string fmt = "<image{n}>";
  // [base] put the fox [ref] in the boat
  CHECK(tag_pictures(P + " put the fox " + P + " in the boat",
                     {{1, true}, {2, false}}, fmt) ==
        "put the fox <image2> in the boat");
  // The base named in the middle is tagged too.
  CHECK(tag_pictures("make " + P + " look like " + P,
                     {{1, true}, {2, false}}, fmt) ==
        "make <image1> look like <image2>");
  // Only space before it still counts as the start.
  CHECK(tag_pictures("  " + P + "\tsnow", {{1, true}}, fmt) == "snow");
  // A video, or a picture past what the model takes: dropped.
  CHECK(tag_pictures("a " + P + " b " + P + " c",
                     {{0, false}, {3, false}}, fmt) == "a b <image3> c");
  // No tags for the model (or not an edit): the words alone.
  CHECK(tag_pictures(P + " fox " + P + " boat", {{1, true}, {2, false}},
                     "") == "fox boat");
  // More markers than assets: the extra ones are dropped.
  CHECK(tag_pictures("x " + P, {}, fmt) == "x");
}

// A model maker's rewriter: its instructions verbatim, the pictures named
// as the model names them, the request after; and its reply shape.
// The helper's language: English for a model that reads only English
// (Krea 2, FLUX.2), the request translated; otherwise the request's own
// -- Qwen-Image, MiniMax H3 and YuE2 read Chinese as they read English.
TEST(assist, the_helper_writes_in_the_model_s_language)
{
  EnhanceRequest r;
  r.prompt = "雪地里的狐狸";
  r.english = true;
  CHECK(build_enhance_prompt(r).find(
            "Write the prompt in English, translating the request") !=
        std::string::npos);
  r.english = false;
  CHECK(build_enhance_prompt(r).find(
            "Write the prompt in Chinese, the language of the request") !=
        std::string::npos);
  r.target = "video";
  CHECK(build_enhance_prompt(r).find("video generation model") !=
        std::string::npos);
  CHECK(build_enhance_prompt(r).find("in Chinese") != std::string::npos);
  r.target = "audio";
  CHECK(build_enhance_prompt(r).find(
            "write the rest in Chinese, the language of the request") !=
        std::string::npos);
  r.target = "image";
  r.prompt = "a fox in snow";
  r.english = true;
  CHECK(build_enhance_prompt(r).find("Write the prompt in English, 60") !=
        std::string::npos);
  // A maker's rewriter keeps its own rules, told the language -- and,
  // for an English-only model, to write in English.
  EnhanceRequest m;
  m.system = "Rewrite the prompt.";
  m.prompt = "雪地里的狐狸";
  m.english = true;
  CHECK(build_enhance_prompt(m).starts_with(
      "The user's request below is written in Chinese.\n"
      "Write the rewritten prompt in English.\n"));
  m.english = false;
  CHECK(build_enhance_prompt(m).find("Write the rewritten prompt") ==
        std::string::npos);
}

// FLUX.3 Image Klein's prompt writer (core/resources/skills/flux3-*),
// written as BFL's own upsampler writes for it: a full scene caption to
// generate, a real photograph's convention opening it unless a style is
// asked for; an edit an expanded instruction -- its voice by the task's
// shape, the end state and its effects, a closing clause of what stays
// -- the pictures called by order (the model is told no tags). The
// catalog names both; FLUX.3 is prompted in English.
TEST(assist, flux3_klein_is_written_a_caption)
{
  const std::string_view t2i = rewrite_prompt("flux3-klein-t2i");
  const std::string_view edit = rewrite_prompt("flux3-klein-edit");
  CHECK(t2i.find("detailed, self-contained caption") !=
        std::string_view::npos);
  CHECK(t2i.find("\"Full-color outdoor photograph, ...\"") !=
        std::string_view::npos);
  CHECK(t2i.find("TEXT IN THE PICTURE") != std::string_view::npos);
  CHECK(edit.find("CHOOSE THE VOICE BY WHAT IS ASKED") !=
        std::string_view::npos);
  CHECK(edit.find("\"the first image\", \"the second image\"") !=
        std::string_view::npos);
  CHECK(edit.find("never <image1>, \"image 1\" or any tag") !=
        std::string_view::npos);
  auto cat = models::Catalog::builtin();
  REQUIRE_OK(cat);
  const auto* m = cat->find("flux3-klein");
  REQUIRE(m);
  const Json en = jget(m->prompting, "enhance", Json::object());
  CHECK(!rewrite_prompt(jget<std::string>(en, "generate", "")).empty());
  CHECK(!rewrite_prompt(jget<std::string>(en, "edit", "")).empty());
  CHECK(jget<std::string>(m->prompting, "language", "") == "english");
  auto r = parse_enhance_reply(
      "{\"prompt\": \"Change the daylight street in the first image to "
      "night under a full moon. Keep the buildings unchanged.\", "
      "\"title\": \"Night\"}");
  REQUIRE_OK(r);
  CHECK(r->title == "Night");
}

// MOSS-IMAGE's prompt (assist/moss-image.h): a draft as its makers'
// skill has a model write it, compiled as their scripts/compile_prompt.py
// compiles it -- these expectations are that script's own tests, and its
// output for the same draft, byte for byte.
TEST(assist, a_moss_image_draft_compiles_as_its_script)
{
  const Json draft = Json::parse(R"({
    "width": 1536, "height": 1024,
    "bbox_coord_space": "abs", "bbox_format": "xyxy",
    "summary": {"meta": ["poster"], "canvas": "single",
                "caption": "A ceramic campaign."},
    "items": [
      {"label": "text", "caption": "Dark-green lettering.",
       "text": "把日常，过成诗。\n\"MOSS\" 🪴", "bbox": [928, 272, 1408, 448]},
      {"label": "object", "caption": "An ivory ceramic cup.",
       "bbox": [176, 224, 800, 864], "tags": ["ceramic"]}]})");
  auto p = moss_image_prompt(draft, 1536, 1024);
  REQUIRE_OK(p);
  CHECK(*p ==
        R"({"summary":{"meta":["poster"],"canvas":"single","caption":)"
        R"("A ceramic campaign."},"items":[{"label":"text","caption":)"
        R"("Dark-green lettering.","text":"把日常，过成诗。\n\"MOSS\" 🪴",)"
        R"("bbox":[-240,160,-64,640]},{"label":"object","caption":)"
        R"("An ivory ceramic cup.","bbox":[-288,-592,352,32],"tags":)"
        R"(["ceramic"]}],"bbox_coord_space":"abs_centralized",)"
        R"("bbox_format":"yxyx"})");
  CHECK(p->find('\n') == std::string::npos);
  CHECK(moss_layout_caption(*p) == "A ceramic campaign.");
  CHECK(moss_layout_caption("a red fox").empty());
  // Fractional edges enclosed, left / top floored, right / bottom ceiled;
  // a hair past a whole pixel is that pixel (round to 9 places first).
  auto box = [](Json b, int w, int h) {
    Json d = {{"summary", {{"caption", "x"}}},
              {"items", Json::array({{{"label", "object"},
                                      {"caption", "y"},
                                      {"bbox", b}}})}};
    auto r = moss_image_prompt(d, w, h);
    return r.ok() ? Json::parse(*r)["items"][0]["bbox"] : Json();
  };
  CHECK(box({0, 0, 768, 1344}, 768, 1344) ==
        Json::array({-672, -384, 672, 384}));
  CHECK(box({10.9, 20.1, 100.01, 200.99}, 768, 1344) ==
        Json::array({-652, -374, -471, -283}));
  CHECK(box({1.00000000001, 2, 100, 200}, 768, 1344) ==
        Json::array({-670, -383, -472, -284}));
  // A caption is the prompt, its wording exact.
  const std::string caption =
      "An ivory cup marked \"茶\".\nPreserve this second line.";
  auto c = moss_image_prompt({{"caption", caption}}, 1024, 1024);
  REQUIRE_OK(c);
  CHECK(*c == caption);
  CHECK(!moss_image_prompt({{"caption", ""}}, 1024, 1024).ok());
  CHECK(!moss_image_prompt({{"caption", caption}, {"items", Json::array()}},
                           1024, 1024)
             .ok());
  // The canvas is the picture's: a multiple of 16 a side.
  CHECK(!moss_image_prompt(draft, 1530, 1024).ok());
  // The enhancer's reply read: a caption draft is a prompt.
  auto r = parse_enhance_reply("{\"caption\": \"A fox at dawn.\"}");
  REQUIRE_OK(r);
  CHECK(r->prompt == "A fox at dawn.");
  CHECK(partial_enhance_reply("{\"caption\": \"A fox at d") == "A fox at d");
}

// Where the script refuses, a helper's slip is mended -- it has no
// second try: a draft laid out on another canvas, or in fractions of it,
// is scaled to the picture's; a box past the edge clipped; unknown fields
// left out; a text item without its words an object.
TEST(assist, a_moss_image_draft_is_mended_where_it_can_be)
{
  auto items = [](const std::string& prompt) {
    return Json::parse(prompt)["items"];
  };
  // Half the size: the boxes doubled -- the cup lands where it would.
  auto half = moss_image_prompt(Json::parse(R"({
    "width": 768, "height": 512, "summary": {"caption": "c"},
    "items": [{"label": "object", "caption": "cup",
               "bbox": [88, 112, 400, 432]}]})"), 1536, 1024);
  REQUIRE_OK(half);
  CHECK(items(*half)[0]["bbox"] == Json::array({-288, -592, 352, 32}));
  // Fractions of the canvas.
  auto frac = moss_image_prompt(Json::parse(R"({
    "summary": {"caption": "c"},
    "items": [{"label": "object", "caption": "o",
               "bbox": [0.25, 0.25, 0.75, 0.75]}]})"), 1024, 1024);
  REQUIRE_OK(frac);
  CHECK(items(*frac)[0]["bbox"] == Json::array({-256, -256, 256, 256}));
  // Past the edge: clipped. Unknown fields: left out. A text item with
  // no words: an object. A box with nothing left: dropped.
  auto mended = moss_image_prompt(Json::parse(R"({
    "width": 1024, "height": 1024, "summary": {"caption": "c",
                                               "font": "x"},
    "items": [
      {"label": "text", "caption": "a sign", "bbox": [-10, 0, 100, 1100],
       "font_size": 32, "children": []},
      {"label": "banner", "caption": "a flag", "bbox": [0, 0, 10, 10]},
      {"label": "object", "caption": "gone", "bbox": [1100, 0, 1200, 10]},
      {"label": "object", "bbox": [0, 0, 10, 10]}]})"), 1024, 1024);
  REQUIRE_OK(mended);
  const Json m = items(*mended);
  REQUIRE(m.size() == 2);
  CHECK(m[0]["label"] == "object");
  CHECK(m[0]["bbox"] == Json::array({-512, -512, 512, -412}));
  CHECK(!m[0].contains("font_size"));
  CHECK(m[1]["label"] == "object");
  CHECK(mended->find("\"font\"") == std::string::npos);
  // Nothing to compile: refused.
  CHECK(!moss_image_prompt(Json::parse(R"({"summary": {"caption": ""},
    "items": [{"label": "object", "caption": "o",
               "bbox": [0, 0, 10, 10]}]})"), 1024, 1024).ok());
  CHECK(!moss_image_prompt(Json::parse(R"({"summary": {"caption": "c"},
    "items": []})"), 1024, 1024).ok());
}

// MOSS-Image's own prompt-enhancer skill, as its makers publish it
// (3rd-party/moss-image), composed as an agent reads it: SKILL.md, then
// the layout reference it points to.
TEST(assist, moss_image_is_enhanced_by_its_makers_skill)
{
  const std::string_view s = rewrite_prompt("moss-image");
  CHECK(s.find("# MOSS-Image Prompt Enhancer") != std::string_view::npos);
  CHECK(s.find("# File: references/layout.md") != std::string_view::npos);
  CHECK(s.find("# Pixel Layout draft") != std::string_view::npos);
  auto cat = models::Catalog::builtin();
  REQUIRE_OK(cat);
  for (const char* id : {"moss-image-sft", "moss-image-base"}) {
    const auto* m = cat->find(id);
    REQUIRE(m);
    CHECK(jget<std::string>(jget(m->prompting, "enhance", Json::object()),
                            "generate", "") == "moss-image");
  }
  EnhanceRequest er;
  er.prompt = "a poster for a tea shop";
  er.system = std::string(s);
  er.skill_note = "Reply with the draft itself.";
  er.size = "1024x768";
  const std::string text = build_enhance_prompt(er);
  CHECK(text.find("Reply with the draft itself.\n\nThe user's request "
                  "is:\na poster for a tea shop (Output size: 1024x768.)") !=
        std::string::npos);
}

// Qwen-Image's prompt writer, generate and edit: Valtz's own guides
// (core/resources/skills/qwen-image-*.md) unless QwenLM's rewriters are
// downloaded (Controller::skill_text). Either way the request is framed
// alike -- its language, the pictures by the model's names -- and either
// reply is read: Valtz's {"prompt", ...} or QwenLM's {"rewritten_prompt",
// ...}, each with the shape it picked.
TEST(assist, qwen_images_writer_frames_the_request)
{
  const std::string_view edit = rewrite_prompt("qwen-image-2.1-edit");
  const std::string_view t2i = rewrite_prompt("qwen-image-2.1-t2i");
  CHECK(edit.find("<image1> is the picture being edited") !=
        std::string_view::npos);
  CHECK(edit.find("Edit text in the picture") != std::string_view::npos);
  CHECK(t2i.find("TEXT IN THE PICTURE") != std::string_view::npos);
  CHECK(rewrite_prompt("nope").empty());

  EnhanceRequest r;
  r.system = std::string(edit);
  r.prompt = "put the fox <image2> in the boat";
  r.pictures = 2;
  r.size = "1024x768";
  const std::string s = build_enhance_prompt(r);
  CHECK(s.starts_with("The user's request below is written in English.\n"
                      "The input images above are, in order: <image1>, "
                      "<image2>. <image1> is the image being edited."));
  r.prompt = "把<image2>放到船上";
  CHECK(build_enhance_prompt(r).starts_with(
      "The user's request below is written in Chinese."));
  CHECK(s.find("THE KINDS OF EDIT") != std::string::npos);
  CHECK(s.ends_with("\n\nThe user's request is:\nput the fox <image2> in "
                    "the boat (Output size: 1024x768.)"));

  EnhanceRequest t;
  t.system = std::string(t2i);
  t.prompt = "a fox in snow";
  CHECK(build_enhance_prompt(t).ends_with(
      "\n\nThe user's request is:\na fox in snow"));

  auto ours = parse_enhance_reply(
      "{\"prompt\": \"Put the red fox from <image2> in the boat.\", "
      "\"wh_ratio\": \"\", \"ratio_follow\": \"<image1>\", "
      "\"title\": \"Fox in the boat\"}");
  REQUIRE_OK(ours);
  CHECK(ours->prompt == "Put the red fox from <image2> in the boat.");
  CHECK(ours->follow == "<image1>" && ours->aspect.empty());
  CHECK(ours->title == "Fox in the boat");
  auto theirs = parse_enhance_reply(
      "{\"rewritten_prompt\": \"The image is a wide photograph of a fox.\", "
      "\"wh_ratio\": \"3:2\", \"ratio_follow\": \"\"}");
  REQUIRE_OK(theirs);
  CHECK(theirs->prompt == "The image is a wide photograph of a fox.");
  CHECK(theirs->aspect == "3:2");
  CHECK(theirs->follow.empty());
}

// YuE2's song writer (core/resources/skills/yue2-song.md, compiled in):
// named by YuE2's catalog entry, told the song's length, and replied in
// parts -- the style line, then the lyrics -- which make the prompt a
// song is held in. Lyrics the person wrote under headers are put back as
// written, whatever the reply did with them.
TEST(assist, yue2_s_song_writer)
{
  const std::string_view skill = rewrite_prompt("yue2-song");
  REQUIRE(!skill.empty());
  CHECK(skill.find("[Pre-Chorus]") != std::string_view::npos);
  CHECK(skill.find("\"lyrics\"") != std::string_view::npos);
  auto cat = models::Catalog::builtin();
  REQUIRE_OK(cat);
  const auto* yue = cat->find("yue2-3b");
  REQUIRE(yue);
  CHECK(jget<std::string>(jget(yue->prompting, "enhance", Json::object()),
                          "generate", "") == "yue2-song");

  EnhanceRequest r;
  r.target = "audio";
  r.system = std::string(skill);
  r.prompt = "一首关于夏天海边的摇滚歌曲";
  r.seconds = 90;
  std::string s = build_enhance_prompt(r);
  CHECK(s.starts_with("The user's request below is written in Chinese.\n"));
  CHECK(s.find("The user's request is:\n一首关于夏天海边的摇滚歌曲\n\n"
               "Song length: about 1:30.") != std::string::npos);
  r.seconds = 0;
  CHECK(build_enhance_prompt(r).ends_with(
      "Song length: not given (about 2 to 3 minutes)."));

  // The reply in parts; a small model's lists; music without words.
  auto e = parse_enhance_reply(
      R"({"style": "English, warm female vocal, city pop, 96 BPM",)"
      R"( "lyrics": "[Verse]\nla la la\n", "title": "City Lights"})");
  REQUIRE_OK(e);
  CHECK(e->prompt ==
        "English, warm female vocal, city pop, 96 BPM\n\n[Verse]\nla la la");
  CHECK(e->title == "City Lights");
  const SongText song = split_song(e->prompt);
  CHECK(song.style == "English, warm female vocal, city pop, 96 BPM");
  CHECK(song.sections == 1 && song.lines == 1);
  e = parse_enhance_reply(
      R"({"style": ["Chinese", "rock", "128 BPM"],)"
      R"( "lyrics": ["[Verse]", "海风卷起狂沙 烈日灼烧脸颊"]})");
  REQUIRE_OK(e);
  CHECK(e->prompt == "Chinese, rock, 128 BPM\n\n[Verse]\n"
                     "海风卷起狂沙 烈日灼烧脸颊");
  e = parse_enhance_reply(
      R"({"style": "instrumental, jazz piano trio, 120 BPM",)"
      R"( "lyrics": "", "title": "Smoky Club"})");
  REQUIRE_OK(e);
  CHECK(e->prompt == "instrumental, jazz piano trio, 120 BPM");

  // The person's sectioned lyrics, kept as written.
  const std::string asked =
      "dreamy synthwave\n\n[Verse]\nneon on the water\n\n[Chorus]\n"
      "stay, stay with me";
  const std::string polished =
      "English, warm female vocal, synthwave\n\n[Verse]\nNeon lights "
      "reflect on water\n\n[Chorus]\nStay with me tonight";
  CHECK(keep_song_lyrics(polished, asked) ==
        "English, warm female vocal, synthwave\n\n[Verse]\nneon on the "
        "water\n\n[Chorus]\nstay, stay with me");
  // No lyrics of its own (or only empty headers): the suggestion's.
  CHECK(keep_song_lyrics(polished, "a synthwave song") == polished);
  CHECK(keep_song_lyrics(polished, "synthwave\n\n[Intro]\n[Verse]") ==
        polished);
  // A caption's lines stay lines as the lyrics are put back.
  CHECK(keep_song_lyrics("Global Metadata\nBasic Attributes: bpm is 90.\n\n"
                         "[Verse]\npolished",
                         asked, true) ==
        "Global Metadata\nBasic Attributes: bpm is 90.\n\n[Verse]\nneon "
        "on the water\n\n[Chorus]\nstay, stay with me");
}

// A name made from a prompt written in Markdown (the Prompt Editor's):
// its words without the markers -- an underscore inside a word kept.
// SPEECH (MOSS-TTS): the prompt's fields at its top, then the words --
// hints left as they were and "None" unset; the language guessed by the
// words' script; the voice director's skill and its reply, whole or as
// it comes, put back in that form.
TEST(assist, speech_s_fields_and_words)
{
  SpeechText s = split_speech(
      "Instruction: A warm, unhurried narrator.\n"
      "- quality: Studio recording\n"
      "Sound event: (e.g. Laughter)\n"
      "AMBIENT_SOUND: None\n"
      "Language\xef\xbc\x9a English\n"
      "\n"
      "Once upon a time, [pause 0.8s] there was a fox.\n"
      "It lived by the sea.\n"
      "(The words to speak.)");
  CHECK(s.instruction == "A warm, unhurried narrator.");
  CHECK(s.quality == "Studio recording");
  CHECK(s.sound_event.empty() && s.ambient_sound.empty());
  CHECK(s.language == "English");
  CHECK(s.text ==
        "Once upon a time, [pause 0.8s] there was a fox.\n"
        "It lived by the sea.");
  // Words alone; a colon in them is not a field.
  CHECK(split_speech("Note: this is spoken.").text ==
        "Note: this is spoken.");
  CHECK(split_speech("Note: this is spoken.").instruction.empty());
  // Put back together, the set fields only.
  CHECK(speech_prompt(s) ==
        "Instruction: A warm, unhurried narrator.\n"
        "Quality: Studio recording\nLanguage: English\n\n"
        "Once upon a time, [pause 0.8s] there was a fox.\n"
        "It lived by the sea.");
  CHECK(split_speech(speech_prompt(s)).text == s.text);

  CHECK(speech_language("Hello there.") == "English");
  CHECK(speech_language("今天天气很好。") == "Chinese");
  CHECK(speech_language("今日はいい天気ですね。") == "Japanese");
  CHECK(speech_language("안녕하세요") == "Korean");
  CHECK(speech_language("Привет, мир") == "Russian");
  CHECK(speech_language("Ça va très bien.").empty());

  const std::string_view skill = rewrite_prompt("moss-tts");
  REQUIRE(!skill.empty());
  CHECK(skill.find("sound_event") != std::string_view::npos);
  CHECK(skill.find("[pause") != std::string_view::npos);
  auto cat = models::Catalog::builtin();
  REQUIRE_OK(cat);
  for (const char* id : {"moss-tts-v1.5", "moss-tts-v1.5-w8g64"}) {
    const auto* m = cat->find(id);
    REQUIRE(m);
    CHECK(jget<std::string>(jget(m->prompting, "enhance", Json::object()),
                            "generate", "") == "moss-tts");
  }
  EnhanceRequest r;
  r.target = "audio";
  r.system = std::string(skill);
  r.speech = true;
  r.voice = true;
  r.prompt = "a bedtime story about a fox";
  r.seconds = 20;
  const std::string q = build_enhance_prompt(r);
  CHECK(q.find("Length: about 20 seconds of speech -- about 50 words") !=
        std::string::npos);
  CHECK(q.find("A reference voice is given") != std::string::npos);
  CHECK(q.find("Song length") == std::string::npos);

  auto e = parse_enhance_reply(
      R"({"instruction": "Soft and slow.", "quality": "",)"
      R"( "sound_event": "", "ambient_sound": "", "language": "English",)"
      R"( "text": "Good night, little fox.", "title": "Good Night"})");
  REQUIRE_OK(e);
  CHECK(e->prompt == "Instruction: Soft and slow.\nLanguage: English\n\n"
                     "Good night, little fox.");
  CHECK(e->title == "Good Night");
  CHECK(partial_enhance_reply(
            R"({"instruction": "Soft and slow.", "language": "English",)"
            R"( "text": "Good ni)") ==
        "Instruction: Soft and slow.\nLanguage: English\n\nGood ni");
}

// DRAMA 3: a scene written as Fish Audio's guide writes it -- "Name:
// line" -- made speaker tags in the order the names first speak, a line
// without a name going on with the one before; taken for a scene only
// when it surely is one; a tagged text or any other as written.
TEST(assist, drama3_s_names_are_its_speakers)
{
  const Drama3Script s = drama3_script(
      "Narrator: [calm, measured storyteller voice] The ship had been "
      "silent for three days.\n"
      "Captain: [low, gravelly] Status report.\n"
      "Engineer: [fast, nervous] Uh, sir, the reactor is not great.\n"
      "It is going to <emphasis>blow</emphasis>.\n"
      "Captain\xef\xbc\x9a [slow, dangerously quiet] Define \"not great.\"");
  REQUIRE(s.names.size() == 3);
  CHECK(s.names[0] == "Narrator" && s.names[2] == "Engineer");
  CHECK(s.text ==
        "<|speaker:0|>[calm, measured storyteller voice] The ship had been "
        "silent for three days.\n"
        "<|speaker:1|>[low, gravelly] Status report.\n"
        "<|speaker:2|>[fast, nervous] Uh, sir, the reactor is not great.\n"
        "It is going to <emphasis>blow</emphasis>.\n"
        "<|speaker:1|>[slow, dangerously quiet] Define \"not great.\"");
  // Two names each once: a scene only with a voice for each.
  const std::string two = "Anna: Hi.\nBen: Hello there.";
  CHECK(drama3_script(two).names.empty());
  CHECK(drama3_script(two).text == two);
  CHECK(drama3_script(two, 2).text ==
        "<|speaker:0|>Hi.\n<|speaker:1|>Hello there.");
  // Steps, a heading, prose, a tagged text: spoken as written.
  const std::string steps = "Step one: mix the flour.\nStep two: add water.";
  CHECK(drama3_script(steps).text == steps);
  CHECK(drama3_script("Note: this is spoken.").names.empty());
  CHECK(drama3_script("Chapter 1: The Storm\nIt was a dark night.")
            .names.empty());
  CHECK(drama3_script("[sighs] Fine. If we miss it, it's on you.").text ==
        "[sighs] Fine. If we miss it, it's on you.");
  CHECK(drama3_script("Well, he said: go home.\nAnd so: we went.")
            .names.empty());
  const std::string tagged =
      "<|speaker:0|> The door opened. <|speaker:1|> Is anyone there?";
  CHECK(drama3_script(tagged, 2).text == tagged);
}

// A voice's WORDS for Drama 3: its transcript's lines from its start,
// whole, as long as they end within the seconds kept; the clip cut a
// little past the last, before the next; a first line longer than that
// alone.
TEST(assist, drama3_s_voice_is_cut_to_its_words)
{
  Transcript t;
  t.seconds = 30;
  t.lines = {{6.1, 11.0, "And then the tide came in.", "English"},
             {0.4, 5.6, "We asked over twenty different people.", "English"},
             {11.1, 14.0, "Nobody saw it.", "English"}};
  const Drama3Voice v = drama3_voice(t);
  CHECK(v.text ==
        "We asked over twenty different people. And then the tide came "
        "in.");
  CHECK(std::abs(v.seconds - 11.08) < 1e-6);   // up to the next line
  const Drama3Voice all = drama3_voice(t, 20);
  CHECK(all.text.ends_with("Nobody saw it."));
  CHECK(std::abs(all.seconds - 14.3) < 1e-6);
  t.lines = {{0.2, 15.5, "A very long first sentence.", "English"}};
  CHECK(drama3_voice(t).text == "A very long first sentence.");
  CHECK(std::abs(drama3_voice(t).seconds - 15.8) < 1e-6);
  // Nothing said.
  t.lines.clear();
  CHECK(drama3_voice(t).text.empty());
  // From a transcript asset's data.
  t.lines = {{0.0, 3.9, "Hello.", "English"}};
  t.seconds = 4.0;
  CHECK(drama3_voice(to_json(t)).text == "Hello.");
  CHECK(std::abs(drama3_voice(to_json(t)).seconds - 4.0) < 1e-6);
}

// Drama 3's script writer: Valtz's instructions, then Fish Audio's guide
// as plain text -- its directing sections, examples as lines, no web
// service (no API, voice IDs, curl, components) -- and the enhancer
// told the voices; its reply the script.
TEST(assist, drama3_is_written_a_script_by_its_makers_guide)
{
  const std::string_view skill = rewrite_prompt("drama3");
  REQUIRE(!skill.empty());
  CHECK(skill.find("Fish Audio's guide: Drama 3") != std::string_view::npos);
  CHECK(skill.find("## Direct the performance") != std::string_view::npos);
  CHECK(skill.find("Example (Sheila): [barely holding it together, forcing "
                   "a smile] I'm fine.") != std::string_view::npos);
  CHECK(skill.find("Captain: [low, gravelly, tired but in control] Status "
                   "report.") != std::string_view::npos);
  CHECK(skill.find("Note: Language tags are experimental.") !=
        std::string_view::npos);
  CHECK(skill.find("<|phoneme_start|>") != std::string_view::npos);
  for (const char* gone : {"curl", "import ", "<AudioClip", "Voice cast",
                           "bf322df2096a46f18c579d0baa36f41d", "---\ntitle",
                           "<Support", "/developer-guide/"}) {
    CHECK(skill.find(gone) == std::string_view::npos);
  }
  auto cat = models::Catalog::builtin();
  REQUIRE_OK(cat);
  for (const char* id : {"drama-3", "drama-3-int4"}) {
    const auto* m = cat->find(id);
    REQUIRE(m);
    CHECK(m->has(models::Capability::TextToSpeech));
    CHECK(jget<std::string>(jget(m->prompting, "enhance", Json::object()),
                            "generate", "") == "drama3");
  }
  EnhanceRequest r;
  r.target = "audio";
  r.system = std::string(skill);
  r.speech = true;
  r.speakers = true;
  r.voice = true;
  r.voices = 2;
  r.prompt = "a captain and an engineer argue as the reactor fails";
  std::string q = build_enhance_prompt(r);
  CHECK(q.find("2 reference voices are given, in the order the script's "
               "speakers first speak") != std::string::npos);
  CHECK(q.find("instruction describes") == std::string::npos);
  r.voices = 0;
  r.voice = false;
  q = build_enhance_prompt(r);
  CHECK(q.find("the model chooses each voice") != std::string::npos);

  auto e = parse_enhance_reply(
      R"({"prompt": "Captain: [tired] Status report.\nEngineer: [nervous])"
      R"( Not great.", "title": "Reactor"})");
  REQUIRE_OK(e);
  CHECK(e->prompt ==
        "Captain: [tired] Status report.\nEngineer: [nervous] Not great.");
  CHECK(partial_enhance_reply(R"({"prompt": "Captain: [tired] Sta)") ==
        "Captain: [tired] Sta");
}

TEST(assist, names_leave_markdown_out)
{
  CHECK(without_markdown("# Harbour at dawn\n\nA **red** *boat*, "
                         "<u>snake_case</u> ~~old~~ `seed` _soft_ "
                         "***both***") ==
        "Harbour at dawn\n\nA red boat, snake_case old seed soft both");
  CHECK(without_markdown("## Shot 2: open sea") == "Shot 2: open sea");
  CHECK(without_markdown("镜头推进，海面泛着**金色**的光") ==
        "镜头推进，海面泛着金色的光");
  CHECK(without_markdown("plain words") == "plain words");
}

// The suggestion as it is written: every cut of a reply reads as the
// prompt so far, and the whole reply as parse_enhance_reply reads it.
TEST(assist, a_reply_is_read_as_it_comes)
{
  const std::string reply =
      "```json\n{\"title\": \"Harbour\", \"prompt\": \"A **red** boat,\\n"
      "\\\"calm\\\" \\u00e9t\\u00e9 \\ud83d\\udea2 港口\", "
      "\"negative\": \"blur\"}\n```";
  auto whole = parse_enhance_reply(reply);
  CHECK(whole.ok());
  std::string last;
  for (std::size_t n = 0; n <= reply.size(); ++n) {
    const std::string now = partial_enhance_reply(reply.substr(0, n));
    // Valid UTF-8 at every cut, and it only ever grows.
    std::size_t i = 0;
    bool valid = true;
    while (i < now.size()) {
      if (utf8_next(now, i) == U'�') {
        valid = false;
      }
    }
    CHECK(valid);
    CHECK(now.starts_with(last));
    last = now;
  }
  CHECK(last == whole->prompt);
  CHECK(last == "A **red** boat,\n\"calm\" été \U0001F6A2 港口");
  // Nothing of the prompt yet: empty.
  CHECK(partial_enhance_reply("{\"title\": \"Harb").empty());
  CHECK(partial_enhance_reply("Sure! Here").empty());
  // An escape cut short is left out until it is whole.
  CHECK(partial_enhance_reply("{\"prompt\": \"a\\") == "a");
  CHECK(partial_enhance_reply("{\"prompt\": \"a\\u00") == "a");
  CHECK(partial_enhance_reply("{\"prompt\": \"a\\ud83d\\ude") == "a");
  // A rewriter's key; a song's parts, lists included.
  CHECK(partial_enhance_reply("{\"rewritten_prompt\": \"a fox") == "a fox");
  CHECK(partial_enhance_reply("{\"style\": \"Indie pop, warm\", "
                              "\"lyrics\": \"[Verse]\\nRain on") ==
        "Indie pop, warm\n\n[Verse]\nRain on");
  CHECK(partial_enhance_reply("{\"style\": [\"jazz\", \"trio\"], "
                              "\"lyrics\": [\"[Verse]\", \"la") ==
        "jazz, trio\n\n[Verse]\nla");
  // A value of no interest, nested, is passed over.
  CHECK(partial_enhance_reply("{\"meta\": {\"a\": [1, \"}\"]}, "
                              "\"prompt\": \"ok") == "ok");
}

// A model's prompt outline, written for what the prompt's row holds: a
// line per medium where it names one, its subjects numbered alike in
// each section, conditional lines kept or dropped -- in the UI's
// language, English otherwise.
TEST(assist, a_prompt_outline_follows_the_row)
{
  const Json outline = Json::array({
      "subjects:",
      "<Subject {subject}> is in {image}.",
      "<Subject {subject}> is in {video}.",
      "{audio} is a sound.",
      "{if none}<Subject 1> is someone.",
      "",
      "kept:",
      "{each image}<Subject {subject}>: fully_preserved",
      "{each video}<Subject {subject}>: weak_reference",
      "{if audio}The sound follows.",
      "",
      "{if video}only with a clip",
  });
  using K = RefKind;
  CHECK(prompt_outline(outline, "en", {K::Image, K::Audio, K::Image,
                                       K::Video}) ==
        "subjects:\n"
        "<Subject 1> is in <valtz_ref_img_0>.\n"
        "<Subject 2> is in <valtz_ref_img_1>.\n"
        "<Subject 3> is in <valtz_ref_vid_0>.\n"
        "<valtz_ref_aud_0> is a sound.\n"
        "\n"
        "kept:\n"
        "<Subject 1>: fully_preserved\n"
        "<Subject 2>: fully_preserved\n"
        "<Subject 3>: weak_reference\n"
        "The sound follows.\n"
        "\n"
        "only with a clip");
  // Nothing in the row: the general lines; a section left empty closes
  // up, with no blank line at the end.
  CHECK(prompt_outline(outline, "en", {}) ==
        "subjects:\n"
        "<Subject 1> is someone.\n"
        "\n"
        "kept:");
  // By language: the UI's, its language alone, then English.
  const Json by_lang = {{"en", "Style: ..."}, {"zh-Hans", Json::array({
                            "风格：", "", "[Verse]"})}};
  CHECK(prompt_outline(by_lang, "zh-Hans", {}) == "风格：\n\n[Verse]");
  CHECK(prompt_outline({{"en", "E"}, {"zh", "Z"}}, "zh-Hans", {}) == "Z");
  CHECK(prompt_outline(by_lang, "fr", {}) == "Style: ...");
  CHECK(prompt_outline(Json(), "en", {}).empty());
}

// The built-in outlines are MiniMax's own prompt forms: Ref2VA's six
// sections, in order (VIDEO_PROMPT_WRITING_GUIDE_ref), FL2VA's three --
// and a song's style before its sections.
TEST(assist, built_in_outlines_follow_their_guides)
{
  auto cat = models::Catalog::builtin();
  REQUIRE_OK(cat);
  const auto outline = [&](const char* id, const char* lang) {
    const auto* m = cat->find(id);
    return m ? prompt_outline(jget(m->prompting, "outline", Json()), lang,
                              {RefKind::Image, RefKind::Audio}, {0})
             : std::string();
  };
  for (const char* lang : {"en", "zh-Hans"}) {
    const std::string ref = outline("minimax-h3-ref2va", lang);
    std::size_t at = 0;
    for (const char* s : {"subject_definitions:\n", "summary:\n",
                          "retention_analysis:\n", "detailed_description:\n",
                          "overall_soundscape:\n", "non_diegetic_music:\n"}) {
      const auto next = ref.find(s, at);
      CHECK(next != std::string::npos);
      at = next == std::string::npos ? at : next;
    }
    CHECK(ref.find("<valtz_ref_img_0>") != std::string::npos);
    CHECK(ref.find("<valtz_ref_aud_0>") != std::string::npos);
    CHECK(ref.find("{") == std::string::npos);  // every marker spent
    // FL2VA opening on the row's picture: the base guide's I2VA line,
    // word for word; the fields with their text on their lines.
    const std::string fl = outline("minimax-h3-fl2va", lang);
    CHECK(fl.starts_with("For the target video, at 0.00 seconds into the "
                         "target video, <valtz_ref_img_0> (from [Shot 1]) "
                         "is fully referenced.\n\n"
                         "integrated_multimodal_description: [Shot 1]"));
    CHECK(fl.find("\n\noverall_soundscape:") != std::string::npos);
    CHECK(fl.find("\n\nnon_diegetic_music:") != std::string::npos);
    CHECK(fl.find("{") == std::string::npos);
    const std::string song = outline("yue2-3b", lang);
    CHECK(split_song(song).sections == 4);
  }
  CHECK(outline("krea2-turbo", "en").empty());
  // From words alone: no instruction line.
  const auto* fl2va = cat->find("minimax-h3-fl2va");
  REQUIRE(fl2va != nullptr);
  const Json fl_outline = jget(fl2va->prompting, "outline", Json());
  CHECK(prompt_outline(fl_outline, "en", {})
            .starts_with("integrated_multimodal_description: [Shot 1] ("));
  // Opening on one picture and ending on another (FL2VA): the base
  // guide's FL2VA line, its pictures bare as that case names them, at
  // the clip's length; one shot, reaching the last frame.
  for (const char* lang : {"en", "zh-Hans"}) {
    const std::string fl =
        prompt_outline(fl_outline, lang,
                       {RefKind::Image, RefKind::Audio, RefKind::Image},
                       {2, 0, 124.0 / 24.0});
    CHECK(fl.starts_with(
        "How the reference pictures align with the target video — "
        "<valtz_ref_img_1> (from Shot 1) aligns with the 0.00-second mark "
        "of the target video; <valtz_ref_img_0> (from Shot 1) aligns with "
        "the 5.17-second mark of the target video.\n\n"
        "integrated_multimodal_description: [Shot 1]"));
    CHECK(fl.find("fully referenced") == std::string::npos);
    CHECK(fl.find("[Shot 2]") == std::string::npos);
    CHECK(fl.find("{") == std::string::npos);
    const auto desc = fl.find("integrated_multimodal_description");
    CHECK(fl.find("<valtz_ref_img_1>", desc) < fl.find("<valtz_ref_img_0>",
                                                       desc));
  }
  // A last frame needs a first; a keyframe that is no picture is none.
  CHECK(prompt_outline(fl_outline, "en", {RefKind::Image}, {-1, 0})
            .starts_with("integrated_multimodal_description: [Shot 1] ("));
  CHECK(prompt_outline(fl_outline, "en", {RefKind::Audio, RefKind::Image},
                       {0, 1})
            .starts_with("integrated_multimodal_description: [Shot 1] ("));
}

// Stacked markers at a line's head must all hold; {first} and {last}
// name the keyframes wherever they are in the row.
TEST(assist, an_outline_s_keyframe_markers)
{
  const Json outline = Json::array({
      "{if first}{unless last}opens on {first}",
      "{if first}{if last}{first} to {last} in {seconds} s",
      "{unless first}from words",
  });
  using K = RefKind;
  CHECK(prompt_outline(outline, "en", {K::Video, K::Image}, {1}) ==
        "opens on <valtz_ref_img_0>");
  CHECK(prompt_outline(outline, "en", {K::Image, K::Video, K::Image},
                       {0, 2, 2.5}) ==
        "<valtz_ref_img_0> to <valtz_ref_img_1> in 2.50 s");
  CHECK(prompt_outline(outline, "en", {K::Image}) == "from words");
}

// MiniMax H3's own prompt-writing skill is built in, as an agent reads
// it -- SKILL.md, then the guide files it names -- and the catalog gives
// it to both H3 models: FL2VA the base guide, Ref2VA the full-reference
// one too.
TEST(assist, h3_s_own_skill_is_built_in)
{
  const std::string_view base = rewrite_prompt("minimax-h3-base");
  const std::string_view ref = rewrite_prompt("minimax-h3-ref");
  CHECK(base.starts_with("---\nname: h3-prompt-writing"));
  CHECK(base.find("# File: references/base-en.txt") != std::string::npos);
  CHECK(base.find("For the target video, at 0.00 seconds into the target "
                  "video, <Picture 1> (from [Shot 1]) is fully "
                  "referenced.") != std::string::npos);
  CHECK(base.find("# File: references/ref-en.txt") == std::string::npos);
  CHECK(ref.find("# File: references/base-en.txt") != std::string::npos);
  CHECK(ref.find("# File: references/ref-en.txt") != std::string::npos);
  CHECK(ref.find("| `retention_analysis` |") != std::string::npos);
  auto cat = models::Catalog::builtin();
  REQUIRE_OK(cat);
  const auto skill = [&](const char* id) {
    const auto* m = cat->find(id);
    return m ? jget<std::string>(jget(m->prompting, "enhance", Json()),
                                 "generate", "")
             : std::string();
  };
  CHECK(skill("minimax-h3-fl2va") == "minimax-h3-base");
  CHECK(skill("minimax-h3-ref2va") == "minimax-h3-ref");
}

// A clip asked of the assistant through its makers' guide: the guide
// whole, then what Valtz knows that the guide leaves to the writer --
// the mode, the length, the frame, the references by the model's names,
// the kinds there are none of -- and a reply section by section.
TEST(assist, a_clip_is_asked_for_through_its_guide)
{
  EnhanceRequest r;
  r.target = "video";
  r.system = std::string(rewrite_prompt("minimax-h3-base"));
  r.prompt = "she waves";
  r.seconds = 124.0 / 24.0;
  r.size = "832x480";
  r.video_mode = "I2VA";
  r.references = {"<Picture 1>: the picture the video opens on, "
                  "\"cat.jpg\" -- image 1 shown above"};
  const std::string i2va = build_enhance_prompt(r);
  CHECK(i2va.find("=== The skill ===") != std::string::npos);
  CHECK(i2va.find("# File: references/base-en.txt") != std::string::npos);
  CHECK(i2va.find("Mode: I2VA") != std::string::npos);
  CHECK(i2va.find("Valtz writes the I2VA instruction line itself") !=
        std::string::npos);
  CHECK(i2va.find("Length: 5.17 seconds. The video ends at 00:05.167") !=
        std::string::npos);
  CHECK(i2va.find("Frame: 832x480 (landscape).") != std::string::npos);
  CHECK(i2va.find("- <Picture 1>: the picture the video opens on") !=
        std::string::npos);
  CHECK(i2va.find("There is no <Video N> here.") != std::string::npos);
  CHECK(i2va.find("There is no <Picture N> here.") == std::string::npos);
  CHECK(i2va.find("{\"integrated_multimodal_description\": ") !=
        std::string::npos);
  CHECK(i2va.find("write every section in English") != std::string::npos);
  CHECK(i2va.ends_with("The user's request:\nshe waves"));

  r.system = std::string(rewrite_prompt("minimax-h3-ref"));
  r.video_mode = "Ref2VA";
  r.video_tasks = "video continuation + audio reference";
  r.references = {"<Video 1>: a clip, \"fox.mp4\" -- the clip the target "
                  "video continues from; its sound is <Audio 1>"};
  const std::string ref = build_enhance_prompt(r);
  CHECK(ref.find("Mode: full-reference (Ref2VA)") != std::string::npos);
  CHECK(ref.find("[video continuation + audio reference]") !=
        std::string::npos);
  CHECK(ref.find("There is no <Picture N> here.") != std::string::npos);
  CHECK(ref.find("There is no <Audio N> here.") == std::string::npos);
  CHECK(ref.find("{\"subject_definitions\": ") != std::string::npos);
  CHECK(ref.find("\"detailed_description\": ") != std::string::npos);

  // Opening on one picture and ending on another: the FL2VA case, its
  // pictures bare as the guide names them there -- none missing.
  r.system = std::string(rewrite_prompt("minimax-h3-base"));
  r.video_mode = "FL2VA";
  r.video_tasks.clear();
  r.references = {"Picture 1: the picture the video opens on (its first "
                  "frame), \"a.png\" -- image 1 shown above",
                  "Picture 2: the picture the video ends on (its last "
                  "frame), \"b.png\" -- image 2 shown above"};
  const std::string fl2va = build_enhance_prompt(r);
  CHECK(fl2va.find("Mode: FL2VA -- the video opens on Picture 1") !=
        std::string::npos);
  CHECK(fl2va.find("ends on Picture 2, its last frame at 5.17 seconds") !=
        std::string::npos);
  CHECK(fl2va.find("Valtz writes the FL2VA instruction line itself") !=
        std::string::npos);
  CHECK(fl2va.find("- Picture 2: the picture the video ends on") !=
        std::string::npos);
  CHECK(fl2va.find("There is no <Picture N> here.") == std::string::npos);
  CHECK(fl2va.find("There is no <Video N> here.") != std::string::npos);
  CHECK(fl2va.find("{\"integrated_multimodal_description\": ") !=
        std::string::npos);

  r.video_mode = "T2VA";
  r.references.clear();
  r.seconds = 0;
  const std::string t2va = build_enhance_prompt(r);
  CHECK(t2va.find("Mode: T2VA") != std::string::npos);
  CHECK(t2va.find("Length:") == std::string::npos);

  // Another video skill (an extension's) keeps the rewriter's framing
  // and reply (docs/EXTENSIONS.md).
  EnhanceRequest other;
  other.target = "video";
  other.system = "Rewrite the request. Reply {\"rewritten_prompt\": ...}";
  other.prompt = "a kite";
  const std::string own = build_enhance_prompt(other);
  CHECK(own.find("=== The skill ===") == std::string::npos);
  CHECK(own.find("Rewrite the request.") != std::string::npos);
}

// A clip's reply comes section by section and is put together in its
// guide's order and form, whatever order it came in -- a section a
// small model wrote inside another taken out, a timestamp on the first
// shot dropped -- also while it is still being written.
TEST(assist, a_clip_s_sections_are_put_together)
{
  auto base = parse_enhance_reply(
      "{\"non_diegetic_music\": \"N/A\", \"overall_soundscape\": "
      "\"Rain.\", \"integrated_multimodal_description\": "
      "\"[Shot 1] At 00:00.000, a fox runs. [Shot 2] At 00:03.000, it "
      "stops.\", \"title\": \"Fox\"}");
  REQUIRE_OK(base);
  CHECK(base->prompt ==
        "integrated_multimodal_description: [Shot 1] A fox runs. [Shot 2] "
        "At 00:03.000, it stops.\n\n"
        "overall_soundscape: Rain.\n\n"
        "non_diegetic_music: N/A");
  CHECK(base->title == "Fox");

  auto ref = parse_enhance_reply(
      "{\"subject_definitions\": [\"<Subject 1> is the fox in <Picture "
      "1>.\"], \"summary\": \"[reference generation] It runs.\", "
      "\"retention_analysis\": \"retention_analysis: <Subject 1> "
      "(appears in [Shot 1]): fully_preserved - its fur.\", "
      "\"detailed_description\": \"Live-action.\\n[Shot 1] The fox "
      "runs.\\n\\noverall_soundscape: Wind.\\n\\nnon_diegetic_music:"
      " N/A\"}");
  REQUIRE_OK(ref);
  CHECK(ref->prompt ==
        "subject_definitions:\n<Subject 1> is the fox in <Picture 1>.\n\n"
        "summary:\n[reference generation] It runs.\n\n"
        "retention_analysis:\n<Subject 1> (appears in [Shot 1]): "
        "fully_preserved - its fur.\n\n"
        "detailed_description:\nLive-action.\n[Shot 1] The fox runs.\n\n"
        "overall_soundscape:\nWind.\n\n"
        "non_diegetic_music:\nN/A");

  // As it comes: the sections so far, the last one cut where it is.
  CHECK(partial_enhance_reply(
            "{\"integrated_multimodal_description\": \"[Shot 1] A fox\", "
            "\"overall_soundscape\": \"Ra") ==
        "integrated_multimodal_description: [Shot 1] A fox\n\n"
        "overall_soundscape: Ra");
  CHECK(partial_enhance_reply("{\"subject_definitions\": \"<Sub") ==
        "subject_definitions:\n<Sub");
}

// Valtz's own SEGMENTS of a prompt (DESIGN §4q): "regenerate_prompt:" at
// a line's start, any case, to the end -- the model reads what is before.
TEST(assist, a_prompt_s_segments_are_valtz_s)
{
  const auto none = prompt_segments("a fox in snow\n");
  CHECK(none.body == "a fox in snow\n");
  CHECK(none.segments.empty());
  // A model's own section is no segment of Valtz's.
  CHECK(prompt_segments("overall_soundscape: wind").segments.empty());
  const auto one = prompt_segments(
      "<Picture 1> walks on.\n\n  Regenerate_Prompt : a woman in red\n"
      "walks along the shore.\n");
  CHECK(one.body == "<Picture 1> walks on.");
  REQUIRE(one.segments.contains("regenerate_prompt"));
  CHECK(one.segments.at("regenerate_prompt") ==
        "a woman in red\nwalks along the shore.");
  // An empty one is none; the words are written back as read.
  CHECK(prompt_segments("a fox\nregenerate_prompt:  \n").segments.empty());
  CHECK(prompt_segments("a fox\nregenerate_prompt:").body == "a fox");
  CHECK(with_segments("a fox ", one.segments) ==
        "a fox\n\nregenerate_prompt: a woman in red\nwalks along the "
        "shore.");
  CHECK(with_segments("a fox", {}) == "a fox");
  const auto back = prompt_segments(with_segments("a fox", one.segments));
  CHECK(back.body == "a fox");
  CHECK(back.segments == one.segments);
}

// What a REFINE on FL2VA reads where none is given, made from a Ref2VA
// prompt: its six sections as the base guide's three, its labels words.
TEST(assist, a_refine_s_words_are_made_from_the_prompt)
{
  CHECK(refine_prompt(
            "subject_definitions:\n<Subject 1> is the woman in <Picture "
            "1>.\n\nsummary:\n[reference generation] She walks.\n\n"
            "retention_analysis:\n<Subject 1>: fully_preserved.\n\n"
            "detailed_description:\nLive-action.\n[Shot 1] <Subject 1> "
            "walks along the shore.\n\noverall_soundscape:\nWaves.\n\n"
            "non_diegetic_music:\nN/A") ==
        "integrated_multimodal_description: subject 1 is the woman in "
        "picture 1.\nLive-action.\n[Shot 1] subject 1 walks along the "
        "shore.\n\noverall_soundscape: Waves.\n\n"
        "non_diegetic_music: N/A");
  CHECK(refine_prompt("<Picture 1> sings <Audio 1> into a microphone ") ==
        "picture 1 sings audio 1 into a microphone");
  // Not a label: as it is.
  CHECK(refine_prompt("a <b>bold</b> <Picture x>") ==
        "a <b>bold</b> <Picture x>");
  CHECK(refine_prompt("  ").empty());
}

// A Ref2VA clip refined on another model: the enhancer is asked for its
// words too, and they land after the clip's sections as Valtz's segment
// -- the reply whole, and as it comes.
TEST(assist, a_regenerated_clip_s_words_are_written_by_the_enhancer)
{
  EnhanceRequest r;
  r.target = "video";
  r.system = "The skill.";
  r.video_mode = "Ref2VA";
  r.prompt = "she walks along the shore";
  CHECK(build_enhance_prompt(r).find("regenerate_prompt") ==
        std::string::npos);
  r.regenerate = true;
  CHECK(build_enhance_prompt(r).find("\"regenerate_prompt\"") !=
        std::string::npos);
  auto reply = parse_enhance_reply(
      "{\"subject_definitions\": \"<Subject 1> is the woman in <Picture "
      "1>.\", \"detailed_description\": \"[Shot 1] She walks.\", "
      "\"overall_soundscape\": \"Waves.\", \"non_diegetic_music\": "
      "\"N/A\", \"regenerate_prompt\": \"A woman in a red coat walks "
      "along a grey shore at dusk.\", \"title\": \"Shore\"}");
  REQUIRE_OK(reply);
  const auto segs = prompt_segments(reply->prompt);
  CHECK(segs.body.starts_with("subject_definitions:\n<Subject 1>"));
  CHECK(segs.body.ends_with("non_diegetic_music:\nN/A"));
  CHECK(segs.segments.at("regenerate_prompt") ==
        "A woman in a red coat walks along a grey shore at dusk.");
  CHECK(partial_enhance_reply(
            "{\"detailed_description\": \"[Shot 1] She walks.\", "
            "\"regenerate_prompt\": \"A woman in a red") ==
        "detailed_description:\n[Shot 1] She walks.\n\n"
        "regenerate_prompt: A woman in a red");
}

// The model's names in a suggestion made the row's tags, in one pass:
// what the prompt box binds; names not listed stay.
TEST(assist, a_suggestion_s_names_become_the_row_s_tags)
{
  const std::vector<std::pair<std::string, std::string>> names = {
    {"<Picture 1>", "<valtz_ref_img_0>"},
    {"<Picture 10>", "<valtz_ref_img_9>"},
    {"<Audio 1>", "<valtz_ref_aud_0>"},
  };
  CHECK(retag("<Subject 1> is in <Picture 1>; <Picture 10> and <Audio 1>, "
              "<Audio 2>.", names) ==
        "<Subject 1> is in <valtz_ref_img_0>; <valtz_ref_img_9> and "
        "<valtz_ref_aud_0>, <Audio 2>.");
  CHECK(retag("<Picture 1>", {}) == "<Picture 1>");
  // FL2VA's bare names: whole words only.
  const std::vector<std::pair<std::string, std::string>> bare = {
    {"Picture 1", "<valtz_ref_img_1>"},
    {"Picture 2", "<valtz_ref_img_0>"},
  };
  CHECK(retag("Picture 1 (from Shot 1); Picture 2. Picture 10, "
              "APicture 1, Picture 1x", bare) ==
        "<valtz_ref_img_1> (from Shot 1); <valtz_ref_img_0>. Picture 10, "
        "APicture 1, Picture 1x");
}

// A sound TRANSCRIBED (DESIGN §4h): Qwen3-ASR's lines, their language
// taken off their words; BEATs' windows joined into events, each label's
// touching windows one span at its best score; the summary of both.
TEST(assist, a_transcript_is_summarized)
{
  using namespace valtz::assist;
  auto l = transcript_line(
      {{"text", "language English<asr_text> Hello there. "},
       {"start_us", 400000}, {"end_us", 3200000}});
  REQUIRE(l.has_value());
  CHECK(l->text == "Hello there.");
  CHECK(l->language == "English");
  CHECK(std::abs(l->start - 0.4) < 1e-9 && std::abs(l->end - 3.2) < 1e-9);
  CHECK(!transcript_line({{"text", "language None<asr_text>  "}}));
  const std::vector<Json> windows = {
    {{"timestamp_us", 0}, {"duration_us", 10000000},
     {"tags", {{{"label", "Speech"}, {"score", 0.9}},
               {{"label", "Music"}, {"score", 0.2}}}}},
    {{"timestamp_us", 8000000}, {"duration_us", 10000000},
     {"tags", {{{"label", "Speech"}, {"score", 0.8}},
               {{"label", "Music"}, {"score", 0.5}}}}},
    {{"timestamp_us", 30000000}, {"duration_us", 10000000},
     {"tags", {{{"label", "Speech"}, {"score", 0.7}}}}},
  };
  auto ev = sound_events(windows);
  REQUIRE(ev.size() == 3);
  CHECK(ev[0].label == "Speech" && ev[0].start == 0 && ev[0].end == 18 &&
        std::abs(ev[0].score - 0.9) < 1e-9);
  CHECK(ev[1].label == "Music" && ev[1].start == 8 && ev[1].end == 18);
  CHECK(ev[2].label == "Speech" && ev[2].start == 30);
  Transcript t;
  t.seconds = 40;
  t.tagged = true;
  t.lines = {*l};
  t.events = ev;
  const auto s = transcript_summary("talk.wav", t);
  CHECK(s.starts_with("# Transcript of talk.wav (00:40) · English\n"));
  CHECK(s.find("[00:00.4 - 00:03.2] Hello there.\n") != std::string::npos);
  CHECK(s.find("## Sound events\n[00:00 - 00:18] Speech (90%)") !=
        std::string::npos);
  t.lines.clear();
  t.tagged = false;
  const auto quiet = transcript_summary("talk.wav", t);
  CHECK(quiet.find("(no speech)") != std::string::npos);
  CHECK(quiet.find("Sound events") == std::string::npos);
}


// A clip summarized (DESIGN §4i): the stage's beats read as scenes and
// the whole, written as Markdown -- the whole first, then each scene with
// its span, the last kept within the clip -- and the language the model
// writes in named from the interface's.
TEST(assist, a_video_summary_is_written)
{
  const auto sc = assist::summary_scene(
      {{"kind", "scene"}, {"index", 0}, {"start", 0.0}, {"end", 11.0},
       {"frames", 11}, {"at_cut", true}, {"text", "A woman holds a bird."}});
  REQUIRE(sc);
  CHECK(sc->end == 11.0);
  CHECK(sc->frames == 11);
  CHECK(sc->at_cut);
  CHECK(!assist::summary_scene({{"kind", "scene"}, {"text", ""}}));
  CHECK(!assist::summary_scene({{"kind", "overall"}, {"text", "x"}}));
  CHECK(assist::summary_overall({{"kind", "overall"}, {"text", "All."}}) ==
        "All.");
  CHECK(assist::summary_overall({{"kind", "scene"}, {"text", "x"}}).empty());

  assist::VideoSummary v;
  v.seconds = 37.8;
  v.every = 1;
  v.overall = "A clockmaker and her bird.";
  v.scenes.push_back(*sc);
  v.scenes.push_back({11.0, 39.0, 28, false, "A kitchen counter."});
  const std::string t = assist::summary_text("montage.mp4", v);
  CHECK(t.starts_with("# Summary of montage.mp4 (0:37)\n\n"
                      "A clockmaker and her bird.\n"));
  CHECK(t.find("**0:00 - 0:11** A woman holds a bird.") !=
        std::string::npos);
  // The last scene's end, at the clip's.
  CHECK(t.find("**0:11 - 0:37** A kitchen counter.") != std::string::npos);
  CHECK(assist::to_json(v)["scenes"][1]["end"].get<double>() == 37.8);
  v.scenes.clear();
  CHECK(assist::summary_text("x", v).find("(nothing seen)") !=
        std::string::npos);

  CHECK(assist::language_name("zh-Hans") == "Simplified Chinese");
  CHECK(assist::language_name("zh-Hant-TW") == "Traditional Chinese");
  CHECK(assist::language_name("fr-CA") == "French");
  CHECK(assist::language_name("en") == "English");
  CHECK(assist::language_name("") == "English");
  CHECK(assist::language_name("tlh") == "English");
}

// A SHOT's prompt (the agent's generate_video): the description's shots
// are its own clip's -- a model writing a scene numbers them through it --
// and a continuation given no subjects names what its clip shows.
TEST(assist, a_shot_counts_its_own_shots)
{
  ShotText t;
  t.description = "[Shot 2] At 00:04.000, he looks up. [Shot 3] At "
                  "00:06.500, the beam sweeps over him.";
  t.continued = "<Video 1>";
  t.continued_sound = "<Audio 1>";
  const std::string p = shot_prompt(t);
  CHECK(p.find("[Shot 1] The shot continues seamlessly from the final "
               "moment of <Video 1>. he looks up. [Shot 2] At 00:02.500, "
               "the beam sweeps over him.") != std::string::npos);
  CHECK(p.find("[Shot 3]") == std::string::npos);
  CHECK(p.find("<Subject 1> is everyone and everything seen in <Video 1>") !=
        std::string::npos);
  CHECK(p.find("<Subject 1> (appears in [Shot 1]): fully_preserved") !=
        std::string::npos);
  // No summary given: what the description says happens, its markers
  // and times left out.
  CHECK(p.find("[video continuation + audio reference] The target video "
               "continues directly from the end of <Video 1>. he looks up. "
               "the beam sweeps over him.\n") != std::string::npos);
  // Already its own: as written.
  ShotText w;
  w.description = "[Shot 1] A pier at dusk. [Shot 2] At 00:02.000, closer.";
  const std::string q = shot_prompt(w);
  CHECK(q.find("integrated_multimodal_description: [Shot 1] A pier at "
               "dusk. [Shot 2] At 00:02.000, closer.") != std::string::npos);
  // A picture referred to, no subjects: who or what it shows.
  ShotText r;
  r.description = "[Shot 1] He waves.";
  r.pictures = {"<Picture 1>"};
  CHECK(shot_prompt(r).find("<Subject 1> is who or what <Picture 1> "
                            "shows.") != std::string::npos);
}
