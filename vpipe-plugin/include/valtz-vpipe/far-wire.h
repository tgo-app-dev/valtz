#ifndef VALTZ_VPIPE_FAR_WIRE_H
#define VALTZ_VPIPE_FAR_WIRE_H

// FAR MEMORY'S WIRE (DESIGN §11e): how a Mac lending its memory -- the
// core's FarLender, fleet/far-lend.h -- and the Mac using it -- the
// plugin's valtz-far transport, behind vpipe's generative-models/
// far-memory.h -- talk. Both compile this; neither includes vpipe here.
//
// A CONNECTION is TCP, one of several on each Thunderbolt cable (a
// request waits a round trip; several connections a cable keep it
// busy). It opens with a HELLO each way -- the magic, the version, a
// nonce, and from the lender the bytes it lends -- then, where the lender
// has a key, each side PROVES it holds it (HMAC-SHA256 over both nonces,
// as valtz-tp's links do); the bytes then cross in the clear, on a cable
// between two Macs.
//
// A lender lends to ONE USER at a time: its first connection, when none
// is open, starts a SESSION -- the memory sized then, as the lender's Mac
// can spare it, and taken -- and the session ends, the memory given back,
// when its last connection closes.
//
// After the proof the lender says what it lends this user: the session's
// bytes, or 0 -- lent to another, or nothing to spare -- and closes.
//
// Then REQUESTS, one at a time on a connection: a header -- the op, the
// far offset, the length -- a write's bytes after it; the lender answers
// with a status, a read's bytes after it. The lender keeps nothing but the
// bytes: what is where is the user's to know, and a new user (a model
// loaded again) starts from nothing.

#include <CommonCrypto/CommonHMAC.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace valtz::far_wire {

inline constexpr char kMagic[4] = {'V', 'F', 'A', 'R'};
// 2: RDMA calls; 3: a call answers each expert's row (far-compute/2), the
// GGUF's tables behind their head
inline constexpr std::uint32_t kVersion = 3;
inline constexpr std::size_t kNonce = 16;

// kRdma: the rest over RDMA (far-rdma.h) -- `offset` the links' count,
// their setup following; the connection then carries nothing more but
// kCompute.
//
// kCompute (PROTOTYPE, DESIGN §11e "Run there"): the lender RUNS experts
// it holds on its GPU and answers each one's output row -- `offset` the
// calls, `length` the bytes after the header: a ComputeForm, the calls,
// then the row's activation (`hidden` elements of the form's type); the
// answer a status, then the calls' rows (`hidden` of the same type each,
// in call order: vpipe's far-compute/2).
//
// kOpenFile (PROTOTYPE, DESIGN §11e "Its disk too"): a FILE of the
// lender's, opened -- one of the model's, a copy kept there under the root
// it was started with (`far-lend --files`) -- `length` the bytes after
// the header: its size (8 bytes), then its name under that root; the
// answer a status, then its id (8 bytes). Its bytes are then READ as the
// lent memory's are, at file_offset(id, pos): the lender reads them off
// its drive.
enum : std::uint32_t { kRead = 1, kWrite = 2, kRdma = 3, kCompute = 4,
                       kOpenFile = 5 };
enum : std::uint64_t { kOk = 0, kOutOfRange = 1, kBadOp = 2,
                       kNoCompute = 3, kNoFile = 4 };

// A file's bytes as a read names them: the top bit set, the file's id
// in the next 19, its position in the low 44 (16 TB).
inline constexpr std::uint64_t kFileBit = std::uint64_t(1) << 63;
inline constexpr int kFilePosBits = 44;
inline constexpr std::uint64_t
file_offset(std::uint64_t id, std::uint64_t pos)
{
  return kFileBit | (id << kFilePosBits) | pos;
}
inline constexpr bool
is_file(std::uint64_t off)
{
  return (off & kFileBit) != 0;
}
inline constexpr std::uint64_t
file_id(std::uint64_t off)
{
  return (off & ~kFileBit) >> kFilePosBits;
}
inline constexpr std::uint64_t
file_pos(std::uint64_t off)
{
  return off & ((std::uint64_t(1) << kFilePosBits) - 1);
}

// vpipe's FarExpertForm and FarExpertCall (generative-models/
// far-memory.h), as the wire carries them.
struct ComputeForm {
  std::uint32_t format;          // 1: DeepSeek V4.1's FP4 experts
  std::uint32_t dtype;           // 0 f16, 1 bf16
  std::uint32_t hidden, inter;
  float swiglu_limit;
  std::uint32_t arg[3];          // the format's own
};
struct ComputeCall {
  std::uint64_t part[6];         // w1, its scales, w3, .., w2, ..
  float weight;
  std::uint32_t pad;
};
static_assert(sizeof(ComputeForm) == 32 && sizeof(ComputeCall) == 56);
inline constexpr std::uint32_t kComputeFp4 = 1;
// A GGUF's own blocks: parts 0, 2, 4 w1's, w3's and w2's slabs; arg[0] |
// arg[1] << 32 the far offset of the tables' head (vpipe's FarGgufTables:
// magic "VGIT", version 1, then its 5266 u64 words), arg[2] the three
// ggml types.
inline constexpr std::uint32_t kComputeGguf = 2;
inline constexpr std::uint32_t kGgufTablesMagic = 0x54494756u;
inline constexpr std::uint64_t kGgufTablesHead = 16;
inline constexpr std::uint64_t kComputeGgufTableBytes = 5266 * 8;

// A ggml block's bytes (256 values) for the types kComputeGguf takes; 0
// for another.
inline std::uint64_t
gguf_block_bytes(std::uint32_t type)
{
  return type == 16 ? 66 : type == 17 ? 74 : type == 22 ? 82
         : type == 18 ? 98 : type == 29 ? 56 : 0;
}
inline constexpr std::size_t kComputeMaxCalls = 64;

// An FP4 expert's part k in bytes: codes [rows, cols / 2], scales [rows,
// cols / 32] -- w1, w3 [inter, hidden], w2 [hidden, inter].
inline std::uint64_t
compute_part_bytes(const ComputeForm& f, int k)
{
  const std::uint64_t h = f.hidden, i = f.inter;
  const std::uint64_t cells = h * i;
  if (f.format == kComputeGguf) {
    if (k % 2 != 0) { return 0; }
    const std::uint32_t t = (f.arg[2] >> (8 * (k / 2))) & 0xff;
    // w1, w3 [inter, hidden], w2 [hidden, inter]: rows of 256-value blocks
    return cells / 256 * gguf_block_bytes(t);
  }
  return k % 2 == 0 ? cells / 2 : cells / 32;
}

struct Hello {
  char magic[4];
  std::uint32_t version;
  std::uint8_t nonce[kNonce];
  std::uint64_t bytes;     // unused (0): the lent bytes follow the proof
  std::uint32_t keyed;     // the lender's: whether a proof is asked
  std::uint32_t pad;
  // The user's: one number for all its connections. A lender serves one
  // user at a time -- its memory is that user's until the last of its
  // connections closes -- and answers another with no bytes.
  std::uint64_t user;
};

struct Request {
  std::uint32_t op;
  std::uint32_t pad;
  std::uint64_t offset;
  std::uint64_t length;
};

using Proof = std::array<std::uint8_t, CC_SHA256_DIGEST_LENGTH>;

// HMAC-SHA256 of `who` ('l' the lender, 'u' the user) and both nonces,
// the prover's own first.
inline Proof
proof(const std::vector<std::uint8_t>& key, char who, const std::uint8_t* a,
      const std::uint8_t* b)
{
  std::vector<std::uint8_t> msg = {'v', 'a', 'l', 't', 'z', '-', 'f', 'a',
                                   'r', '/', '1', (std::uint8_t)who};
  msg.insert(msg.end(), a, a + kNonce);
  msg.insert(msg.end(), b, b + kNonce);
  Proof out{};
  CCHmac(kCCHmacAlgSHA256, key.data(), key.size(), msg.data(), msg.size(),
         out.data());
  return out;
}

inline std::vector<std::uint8_t>
from_hex(const char* h, std::size_t n)
{
  std::vector<std::uint8_t> out;
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
  };
  if (n % 2 != 0) { return {}; }
  for (std::size_t i = 0; i + 1 < n; i += 2) {
    const int a = nib(h[i]), b = nib(h[i + 1]);
    if (a < 0 || b < 0) { return {}; }
    out.push_back((std::uint8_t)(a * 16 + b));
  }
  return out;
}

}

#endif
