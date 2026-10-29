#include "valtz/assist/assistant.h"
#include "valtz/assist/drama3.h"

#include <string>

// Compiled in (core/CMakeLists.txt, valtz_embed): MiniMax H3's
// prompt-writing skill from 3rd-party/minimax-h3, unmodified, under the
// MiniMax H3 Community License (its LICENSE and NOTICE), and Valtz's own
// skills, from core/resources/skills -- Qwen-Image's among them, which a
// download of QwenLM's own rewriters replaces (Controller::skill_text),
// and FLUX.3 Image Klein's, in Valtz's words after BFL's prompting guide
// and the system prompts of its prompt upsampler (Apache-2.0), which
// wrote the captions the model was trained on -- and Fish Audio's guide
// to directing Drama 3 (3rd-party/fish-audio-drama3, CC BY 4.0).
namespace valtz::resources {
extern const char kQwenImageT2iSkill[];
extern const char kQwenImageEditSkill[];
extern const char kFlux3KleinT2iSkill[];
extern const char kFlux3KleinEditSkill[];
extern const char kYue2SongSkill[];
extern const char kMinimaxMusic3Skill[];
extern const char kMossTtsSkill[];
extern const char kDrama3Skill[];
extern const char kDrama3Guide[];
extern const char kValtzAgentSkill[];
extern const char kMinimaxH3Skill[];
extern const char kMinimaxH3BaseGuide[];
extern const char kMinimaxH3RefGuide[];
extern const char kMossImageSkill[];
extern const char kMossImageLayoutGuide[];
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
  if (name == "flux3-klein-t2i") {
    return resources::kFlux3KleinT2iSkill;
  }
  if (name == "flux3-klein-edit") {
    return resources::kFlux3KleinEditSkill;
  }
  if (name == "yue2-song") {
    return resources::kYue2SongSkill;
  }
  if (name == "minimax-music3") {
    return resources::kMinimaxMusic3Skill;
  }
  if (name == "moss-tts") {
    return resources::kMossTtsSkill;
  }
  if (name == "drama3") {
    // Valtz's instructions, then the guide they point to.
    static const std::string s =
        std::string(resources::kDrama3Skill) +
        "\n\n# Fish Audio's guide: Drama 3\n\n" +
        drama3_guide(resources::kDrama3Guide);
    return s;
  }
  if (name == "valtz-agent") {
    return resources::kValtzAgentSkill;
  }
  if (name == "minimax-h3-base") {
    static const std::string s = h3_skill(false);
    return s;
  }
  if (name == "minimax-h3-ref") {
    static const std::string s = h3_skill(true);
    return s;
  }
  if (name == "moss-image") {
    // SKILL.md, then the layout reference it says to read first.
    static const std::string s =
        std::string(resources::kMossImageSkill) +
        "\n\n# File: references/layout.md\n\n" +
        resources::kMossImageLayoutGuide;
    return s;
  }
  return {};
}

}
