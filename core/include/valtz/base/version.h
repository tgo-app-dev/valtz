// Which Valtz this is, as a file it hands out says it (DESIGN §7 Export).
//
// The version is the app's CFBundleShortVersionString, the BUILD its
// CFBundleVersion -- the source tree's commit count, what a release is
// numbered by ("Valtz-141"): the version alone has said 0.1.0 for every
// release, so it identifies none. Every file exported, a picture, a movie,
// a sound or a PDF, names the two and the engine's own line in brackets
// -- "Valtz 0.1.0 build 141 (Vpipe 0.1 29ef2378*0)" -- as vpipe's
// save-image has always named a generated picture's maker, its model after
// a "with".

#ifndef VALTZ_BASE_VERSION_H
#define VALTZ_BASE_VERSION_H

#include <string>
#include <string_view>

namespace valtz {

std::string_view valtz_version();
std::string_view valtz_build();

// "Valtz 0.1.0 build 141".
std::string valtz_software();

// valtz_software() with `engine`'s line in brackets ("Vpipe 0.1 29ef2378*0":
// Engine::software()); alone when there is none.
std::string software_stamp(std::string_view engine);

}

#endif
