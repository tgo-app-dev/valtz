// A model's files FETCHED FROM A URL of its makers' own (DESIGN §4u) --
// not a hub vpipe knows, whose downloads vpipe's model-fetch makes:
// Practical-RIFE's zips live on Google Drive. NSURLSession, written to
// a file as it comes, counted, stopped when asked; Drive's "can't scan
// this file for viruses" page followed through its own form; a zip
// opened by the system's ditto, the files asked for taken out of it.

#ifndef VALTZ_BASE_URL_FETCH_H
#define VALTZ_BASE_URL_FETCH_H

#include "valtz/base/result.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace valtz {

// How far it is: bytes so far and the whole (0: not said). False stops
// it (Code::Cancelled).
using FetchProgress = std::function<bool(std::uint64_t, std::uint64_t)>;

struct Fetched {
  std::uint64_t bytes = 0;
  std::string   content_type;  // the server's, lower case, no parameters
};

// `url` into `out` (written whole or removed), following redirects.
Result<Fetched> fetch_url(const std::string& url,
                          const std::filesystem::path& out,
                          const FetchProgress& progress = {});

// Google Drive's page in a file's place -- "Google Drive can't scan this
// file for viruses ... Would you still like to download this file?" --
// as the URL its Download button sends: its form's action and its hidden
// fields (id, export, confirm, uuid). Empty when `html` is no such page.
std::string drive_confirm_url(std::string_view html);

// The files named `take` (by name, wherever they lie in the archive --
// macOS's __MACOSX shadows left out) copied out of zip `zip` into `dir`.
// Every one must be there. Unpacked by /usr/bin/ditto into a folder of
// its own beside `dir`, gone after.
Status take_from_zip(const std::filesystem::path& zip,
                     const std::vector<std::string>& take,
                     const std::filesystem::path& dir);

}  // namespace valtz

#endif
