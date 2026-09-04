// name_of / from_name -- the IR's vocabulary, both ways -- plus the Literal
// accessors that round out Module::int_const to the other four kinds.
//
// Each enum's names live in exactly one place (the name_of switch); from_name
// is nothing but a scan over name_of. This test pins the property a second
// definition could silently break: every enumerator survives a name_of ->
// from_name round trip, and an unrecognized string never resolves to one.

#include <cstdio>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "vmlib.h"

using namespace coreir;

namespace {

int g_failures = 0;

template <class E>
void check_round_trip(E v, const char* enum_name) {
  const char* name = name_of(v);
  const std::optional<E> back = from_name<E>(name);
  if (!back.has_value() || *back != v) {
    std::fprintf(stderr, "FAIL: %s round trip broke on \"%s\"\n", enum_name,
                 name);
    ++g_failures;
  }
}

template <class E>
void check_rejects_bogus(const char* enum_name) {
  if (from_name<E>("not-a-real-name").has_value()) {
    std::fprintf(stderr, "FAIL: %s accepted a bogus name\n", enum_name);
    ++g_failures;
  }
  if (from_name<E>("").has_value()) {
    std::fprintf(stderr, "FAIL: %s accepted an empty name\n", enum_name);
    ++g_failures;
  }
}

}  // namespace

extern "C" {
void coreir_rt_out(int64_t) {}
void coreir_rt_out_str(const char*, int64_t) {}
void coreir_rt_out_raw(const char*, int64_t) {}
int64_t coreir_rt_in(int64_t, int64_t) { return 0; }
[[noreturn]] void coreir_rt_fail(const char* msg, int64_t, int64_t) {
  throw std::runtime_error(msg);
}
void coreir_rt_poll(void) {}
}

int main() {
  for (uint8_t i = 0; i <= static_cast<uint8_t>(Tag::Yield); ++i) {
    check_round_trip(static_cast<Tag>(i), "Tag");
  }
  for (uint8_t i = 0; i <= static_cast<uint8_t>(UnOp::WrapU32); ++i) {
    check_round_trip(static_cast<UnOp>(i), "UnOp");
  }
  for (uint8_t i = 0; i <= static_cast<uint8_t>(BinOp::UGe); ++i) {
    check_round_trip(static_cast<BinOp>(i), "BinOp");
  }
  for (uint8_t i = 0; i <= static_cast<uint8_t>(VarKind::Cell); ++i) {
    check_round_trip(static_cast<VarKind>(i), "VarKind");
  }
  for (uint8_t i = 0; i <= static_cast<uint8_t>(IntrinsicId::ToFloat32);
       ++i) {
    check_round_trip(static_cast<IntrinsicId>(i), "IntrinsicId");
  }
  for (uint8_t i = 0; i <= static_cast<uint8_t>(ConstKind::Str); ++i) {
    check_round_trip(static_cast<ConstKind>(i), "ConstKind");
  }

  check_rejects_bogus<Tag>("Tag");
  check_rejects_bogus<UnOp>("UnOp");
  check_rejects_bogus<BinOp>("BinOp");
  check_rejects_bogus<VarKind>("VarKind");
  check_rejects_bogus<IntrinsicId>("IntrinsicId");
  check_rejects_bogus<ConstKind>("ConstKind");

  // name_of(Tag) pins two boundary cases the design leans on: TryCatch's
  // bare-tag name matches what the Dumper already prints for it ("try"), and
  // Unary is a genuinely new word (the Dumper prints the operator instead).
  if (std::string_view(name_of(Tag::TryCatch)) != "try") {
    std::fprintf(stderr, "FAIL: name_of(Tag::TryCatch) drifted from \"try\"\n");
    ++g_failures;
  }
  if (std::string_view(name_of(Tag::Unary)) != "unary") {
    std::fprintf(stderr, "FAIL: name_of(Tag::Unary) is not \"unary\"\n");
    ++g_failures;
  }

  // Module's Literal accessors: const_kind says which of the four payloads a
  // Literal actually holds, and int_const's three new siblings decode it.
  // Nothing in this library calls them yet (int_const is the only kind pl0
  // ever needs), so this is their one exercise.
  {
    Module m;
    Builder b(m);
    const SrcPos p{1, 1};
    const NodeId i = b.literal(42, p);
    const NodeId bo = b.bool_literal(true, p);
    const NodeId d = b.double_literal(3.5, p);
    const NodeId n = b.nil_literal(p);
    const NodeId s = b.str_literal("hi", p);

    if (m.const_kind(i) != ConstKind::Int || m.int_const(i) != 42) {
      std::fprintf(stderr, "FAIL: int literal accessors\n");
      ++g_failures;
    }
    if (m.const_kind(bo) != ConstKind::Bool || !m.bool_const(bo)) {
      std::fprintf(stderr, "FAIL: bool literal accessors\n");
      ++g_failures;
    }
    if (m.const_kind(d) != ConstKind::Double || m.double_const(d) != 3.5) {
      std::fprintf(stderr, "FAIL: double literal accessors\n");
      ++g_failures;
    }
    if (m.const_kind(n) != ConstKind::Nil) {
      std::fprintf(stderr, "FAIL: nil literal accessor\n");
      ++g_failures;
    }
    if (m.const_kind(s) != ConstKind::Str || m.str_const(s) != "hi") {
      std::fprintf(stderr, "FAIL: str literal accessors\n");
      ++g_failures;
    }
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "names: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("names OK\n");
  return 0;
}
