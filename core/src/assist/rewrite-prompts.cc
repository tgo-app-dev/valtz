#include "valtz/assist/assistant.h"

#include <string>

// Compiled in (core/CMakeLists.txt, valtz_embed): MiniMax H3's
// prompt-writing skill from 3rd-party/minimax-h3, unmodified, under the
// MiniMax H3 Community License (its LICENSE and NOTICE), and Valtz's own
// skills, from core/resources/skills -- Qwen-Image's among them, which a
// download of QwenLM's own rewriters replaces (Controller::skill_text).
namespace valtz::resources {
extern const char kQwenImageT2iSkill[];
extern const char kQwenImageEditSkill[];
extern const char kYue2SongSkill[];
extern const char kMossTtsSkill[];
extern const char kMinimaxH3Skill[];
extern const char kMinimaxH3BaseGuide[];
extern const char kMinimaxH3RefGuide[];
}

namespace valtz::assist {

namespace {

// The skill as an agent reads it: SKILL.md, then each reference file it
// points to under the path it names them by.
std::string
h3_skill(bool full_reference)
{
  std::string s = resources::kMinimaxH3Skill;
  s += "\n\n# File: references/base-en.txt\n\n";
  s += resources::kMinimaxH3BaseGuide;
  if (full_reference) {
    s += "\n\n# File: references/ref-en.txt\n\n";
    s += resources::kMinimaxH3RefGuide;
  }
  return s;
}

}

std::string_view
rewrite_prompt(std::string_view name)
{
  if (name == "qwen-image-2.1-t2i") {
    return resources::kQwenImageT2iSkill;
  }
  if (name == "qwen-image-2.1-edit") {
    return resources::kQwenImageEditSkill;
  }
  if (name == "yue2-song") {
    return resources::kYue2SongSkill;
  }
  if (name == "moss-tts") {
    return resources::kMossTtsSkill;
  }
  if (name == "minimax-h3-base") {
    static const std::string s = h3_skill(false);
    return s;
  }
  if (name == "minimax-h3-ref") {
    static const std::string s = h3_skill(true);
    return s;
  }
  return {};
}

}
