#include "valtz/base/version.h"

#include <format>

namespace valtz {

std::string_view
valtz_version()
{
  return VALTZ_VERSION;
}

std::string_view
valtz_build()
{
  return VALTZ_BUILD;
}

std::string
valtz_software()
{
  return std::format("Valtz {} build {}", valtz_version(), valtz_build());
}

std::string
software_stamp(std::string_view engine)
{
  return engine.empty() ? valtz_software()
                        : std::format("{} ({})", valtz_software(), engine);
}

}
