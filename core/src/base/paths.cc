#include "valtz/base/paths.h"

#include <cstdlib>

namespace valtz {

AppPaths
AppPaths::standard(const std::filesystem::path& support_override)
{
  const char* h = std::getenv("HOME");
  std::filesystem::path home = h ? h : "/tmp";
  AppPaths p;
  p.support = support_override.empty()
                  ? home / "Library/Application Support" / kBundleId
                  : support_override;
  p.models = p.support / "models";
  p.cache = p.support / "cache";
  p.engine = p.support / "engine";
  p.projects = home / "Movies/Valtz";
  return p;
}

}
