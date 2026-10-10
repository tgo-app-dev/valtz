#include "testing.h"

#include "valtz/assist/assistant.h"
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
                              {RefKind::Image, RefKind::Audio})
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
  CHECK(prompt_outline(jget(fl2va->prompting, "outline", Json()), "en", {})
            .starts_with("integrated_multimodal_description: [Shot 1] ("));
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
