// The fleet's configuration, framing and offers (fleet.h): plain data,
// apart from the network (fleet.mm).

#include "valtz/fleet/fleet.h"

#include "valtz/base/hash.h"
#include "valtz/project/records.h"

#include <CommonCrypto/CommonHMAC.h>

#include <format>
#include <fstream>
#include <sys/stat.h>

namespace fs = std::filesystem;

namespace valtz::fleet {

namespace {

bool
day_on(int days, int wday)
{
  // tm_wday counts from Sunday; the schedule from Monday.
  return (days >> ((wday + 6) % 7)) & 1;
}

}  // namespace

bool
Schedule::open_at(std::time_t t) const
{
  if (!on) {
    return true;
  }
  std::tm tm{};
  localtime_r(&t, &tm);
  const int minute = tm.tm_hour * 60 + tm.tm_min;
  const int yesterday = (tm.tm_wday + 6) % 7;
  if (from == to) {
    return day_on(days, tm.tm_wday);
  }
  if (from < to) {
    return day_on(days, tm.tm_wday) && minute >= from && minute < to;
  }
  // Past midnight: the evening is today's, the small hours yesterday's.
  return (minute >= from && day_on(days, tm.tm_wday)) ||
         (minute < to && day_on(days, yesterday));
}

bool
Config::accepting_at(std::time_t t) const
{
  return accept != "never" && schedule.open_at(t);
}

std::string
derive_key(std::string_view fleet, std::string_view secret)
{
  const std::string msg = std::format("valtz-fleet/{}:{}", kProtocol, fleet);
  std::uint8_t mac[CC_SHA256_DIGEST_LENGTH];
  CCHmac(kCCHmacAlgSHA256, secret.data(), secret.size(), msg.data(),
         msg.size(), mac);
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  for (std::uint8_t b : mac) {
    out += hex[b >> 4];
    out += hex[b & 15];
  }
  return out;
}

Json
to_json(const Schedule& s)
{
  return {{"on", s.on}, {"days", s.days}, {"from", s.from}, {"to", s.to}};
}

Schedule
schedule_from_json(const Json& j)
{
  Schedule s;
  s.on = jget(j, "on", false);
  s.days = jget(j, "days", 0x7f) & 0x7f;
  s.from = std::clamp(jget(j, "from", s.from), 0, 24 * 60);
  s.to = std::clamp(jget(j, "to", s.to), 0, 24 * 60);
  return s;
}

Json
to_json(const Config& c)
{
  return {{"member_id", c.member_id},
          {"member_name", c.member_name},
          {"fleet", c.fleet},
          {"secret_set", !c.key.empty()},
          {"discoverable", c.discoverable},
          {"accept", c.accept},
          {"schedule", to_json(c.schedule)},
          {"joined", c.joined()},
          {"accepting", c.accepting_at(std::time(nullptr))}};
}

Result<Config>
load_config(const fs::path& path)
{
  Config c;
  std::error_code ec;
  if (fs::exists(path, ec)) {
    std::ifstream in(path);
    const Json j = Json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
      return make_error(Code::Corrupt,
                        std::format("{} is not JSON", path.string()));
    }
    c.member_id = jget<std::string>(j, "member_id", "");
    c.member_name = jget<std::string>(j, "member_name", "");
    c.fleet = jget<std::string>(j, "fleet", "");
    c.key = jget<std::string>(j, "key", "");
    c.discoverable = jget(j, "discoverable", false);
    c.accept = jget<std::string>(j, "accept", "always");
    if (c.accept != "always" && c.accept != "ask" && c.accept != "never") {
      c.accept = "always";
    }
    c.schedule = schedule_from_json(jget(j, "schedule", Json::object()));
  }
  if (c.member_id.empty()) {
    c.member_id = Uuid::v7().str();
  }
  if (c.member_name.empty()) {
    c.member_name = default_member_name();
  }
  return c;
}

Status
save_config(const fs::path& path, const Config& c)
{
  Json j = {{"member_id", c.member_id},
            {"member_name", c.member_name},
            {"fleet", c.fleet},
            {"key", c.key},
            {"discoverable", c.discoverable},
            {"accept", c.accept},
            {"schedule", to_json(c.schedule)}};
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  const fs::path tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) {
      return make_error(Code::Io,
                        std::format("cannot write {}", tmp.string()));
    }
    out << to_text(j, 2) << "\n";
  }
  // Its key opens the fleet: its owner's alone.
  ::chmod(tmp.c_str(), 0600);
  fs::rename(tmp, path, ec);
  if (ec) {
    return make_error(Code::Io, std::format("cannot write {}: {}",
                                                 path.string(), ec.message()));
  }
  return ok_status();
}

std::vector<std::uint8_t>
frame(const Json& message)
{
  const std::vector<std::uint8_t> body = to_cbor(message);
  const auto n = static_cast<std::uint32_t>(body.size());
  std::vector<std::uint8_t> out;
  out.reserve(body.size() + 4);
  out.push_back(static_cast<std::uint8_t>(n >> 24));
  out.push_back(static_cast<std::uint8_t>(n >> 16));
  out.push_back(static_cast<std::uint8_t>(n >> 8));
  out.push_back(static_cast<std::uint8_t>(n));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

void
Deframer::feed(std::span<const std::uint8_t> bytes)
{
  // What was read is dropped once it is more than what is left.
  if (_at > 0 && _at >= _buf.size() - _at) {
    _buf.erase(_buf.begin(), _buf.begin() + static_cast<long>(_at));
    _at = 0;
  }
  _buf.insert(_buf.end(), bytes.begin(), bytes.end());
}

std::optional<Result<Json>>
Deframer::next()
{
  if (_buf.size() - _at < 4) {
    return std::nullopt;
  }
  const std::uint8_t* p = _buf.data() + _at;
  const std::size_t n = (std::size_t(p[0]) << 24) | (std::size_t(p[1]) << 16) |
                        (std::size_t(p[2]) << 8) | std::size_t(p[3]);
  if (n > kMaxFrame) {
    return Result<Json>(make_error(Code::Corrupt, "fleet frame too large"));
  }
  if (_buf.size() - _at < 4 + n) {
    return std::nullopt;
  }
  auto j = from_cbor(std::span<const std::uint8_t>(p + 4, n));
  _at += 4 + n;
  return j;
}

Json
to_json(const Offer& o)
{
  Json inputs = Json::array();
  for (const auto& in : o.inputs) {
    std::error_code ec;
    const auto size = fs::file_size(in.path, ec);
    inputs.push_back({{"role", in.role},
                      {"hash", in.hash.hex()},
                      {"size", ec ? 0 : static_cast<std::int64_t>(size)},
                      {"ext", in.path.extension().string()},
                      {"info", in.info}});
  }
  return {{"job", o.id.str()},     {"op", o.op},
          {"model", o.model},      {"title", o.title},
          {"params", o.params},    {"inputs", inputs}};
}

Result<Offer>
offer_from_json(const Json& j)
{
  Offer o;
  auto id = JobId::parse(jget<std::string>(j, "job", ""));
  if (!id) {
    return make_error(Code::InvalidArgument, "fleet offer without a job");
  }
  o.id = *id;
  o.op = jget<std::string>(j, "op", "");
  o.model = jget<std::string>(j, "model", "");
  o.title = jget<std::string>(j, "title", "");
  o.params = jget(j, "params", Json::object());
  for (const auto& in : jget(j, "inputs", Json::array())) {
    engine::JobInput ji;
    ji.role = jget<std::string>(in, "role", "");
    auto h = ContentHash::parse(jget<std::string>(in, "hash", ""));
    if (!h) {
      return make_error(Code::InvalidArgument, "fleet input without a hash");
    }
    ji.hash = *h;
    // Where it will be: named by its hash, its extension kept (a reader
    // goes by it).
    std::string ext = jget<std::string>(in, "ext", "");
    if (ext.size() > 12 || ext.find('/') != std::string::npos) {
      ext.clear();
    }
    ji.path = h->hex() + ext;
    ji.info = jget(in, "info", media::MediaInfo{});
    o.inputs.push_back(std::move(ji));
  }
  return o;
}

}
