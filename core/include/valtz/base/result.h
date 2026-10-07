// valtz::Result -- the error channel of the controller layer.
//
// C++20 has no std::expected, and exceptions are the wrong channel here
// for a structural reason: the SwiftUI front end calls this layer through
// Swift/C++ interop, and a C++ exception that unwinds into a Swift frame
// terminates the process. So every call that can fail on user data
// (a bad path, a full disk, a missing model) returns a Result, and
// exceptions are reserved for programming errors inside the layer.
//
// Error carries a stable numeric Code (the UI switches on it) and a
// human-readable message (the UI shows it). It is not a stack of
// causes; context is added by rewriting the message at the boundary
// that knows it.

#ifndef VALTZ_BASE_RESULT_H
#define VALTZ_BASE_RESULT_H

#include "valtz/base/message.h"

#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace valtz {

enum class Code : std::uint16_t {
  Ok = 0,
  InvalidArgument,  // the caller's input is malformed
  NotFound,         // a named object (asset, model, file) does not exist
  AlreadyExists,
  Io,               // filesystem / database I/O failed
  Corrupt,          // stored data failed to decode or verify
  Unsupported,      // valid request this build / machine cannot serve
  Busy,             // the resource is held; retry later
  Cancelled,
  OutOfMemory,      // the plan does not fit the machine
  Engine,           // the execution engine (vpipe) reported a failure
  Network,
  Internal,         // a bug in this layer
};

const char* to_str(Code);

struct Error {
  Code        code = Code::Internal;
  std::string message;  // English, for logs, valtzctl, and as the fallback
  // Set when the text is for a person: a Message key (message.h) and its
  // arguments, which the app localizes.
  std::string key;
  MessageArgs args;
};

inline Error
make_error(Code code, std::string message)
{
  return Error{code, std::move(message), {}, {}};
}

// A user-facing error: `message` is the English rendering of `m`.
inline Error
make_error(Code code, const Message& m, MessageArgs args = {})
{
  std::string text = render_message(m.english, args);
  return Error{code, std::move(text), std::string(m.key), std::move(args)};
}

template <class T>
class [[nodiscard]] Result {
public:
  Result(T value) : _v(std::in_place_index<0>, std::move(value)) {}
  Result(Error err) : _v(std::in_place_index<1>, std::move(err)) {}

  bool ok() const noexcept { return _v.index() == 0; }
  explicit operator bool() const noexcept { return ok(); }

  T&       value() &       { return std::get<0>(_v); }
  const T& value() const & { return std::get<0>(_v); }
  T&&      value() &&      { return std::get<0>(std::move(_v)); }

  T&       operator*() &       { return value(); }
  const T& operator*() const & { return value(); }
  T*       operator->()        { return &value(); }
  const T* operator->() const  { return &value(); }

  const Error& error() const { return std::get<1>(_v); }
  Code code() const { return ok() ? Code::Ok : error().code; }

private:
  std::variant<T, Error> _v;
};

template <>
class [[nodiscard]] Result<void> {
public:
  Result() = default;
  Result(Error err) : _err(std::move(err)), _ok(false) {}

  bool ok() const noexcept { return _ok; }
  explicit operator bool() const noexcept { return ok(); }

  const Error& error() const { return _err; }
  Code code() const { return _ok ? Code::Ok : _err.code; }

private:
  Error _err;
  bool  _ok = true;
};

using Status = Result<void>;

inline Status ok_status() { return Status{}; }

}

// Early-return helpers. VALTZ_TRY(expr) propagates a failed Status;
// VALTZ_ASSIGN(decl, expr) binds a successful Result's value.
#define VALTZ_TRY(expr)                                   \
  do {                                                    \
    auto _valtz_st = (expr);                              \
    if (!_valtz_st.ok()) {                                \
      return _valtz_st.error();                           \
    }                                                     \
  } while (0)

#define VALTZ_CONCAT_(a, b) a##b
#define VALTZ_CONCAT(a, b) VALTZ_CONCAT_(a, b)
#define VALTZ_ASSIGN_IMPL_(tmp, decl, expr)               \
  auto tmp = (expr);                                      \
  if (!tmp.ok()) {                                        \
    return tmp.error();                                   \
  }                                                       \
  decl = std::move(tmp).value()
#define VALTZ_ASSIGN(decl, expr)                          \
  VALTZ_ASSIGN_IMPL_(VALTZ_CONCAT(_valtz_r, __LINE__), decl, expr)

#endif
