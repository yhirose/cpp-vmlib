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
#include <set>
#include <stdexcept>
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

// What every intrinsic's facts (name, arity, destination, opcode) have to
// agree on once lowered. Each id is compiled in value position with literal
// arguments, and the Insn compile_intrinsic emitted is read back: the
// destination (when there is one) in a, the arguments after it, the unused
// operand fields zero. That layout is what the bytecode dump reads back with
// arity + has_dst operands, so the two are pinned to each other here.
void check_intrinsic_layout(IntrinsicId id) {
  const char* name = name_of(id);
  Module m;
  m.capture_maps.push_back({});
  m.funcs.resize(1);
  FuncWriter w(m);
  const auto [t] = w.locals("t");
  std::vector<NodeId> args;
  for (uint32_t i = 0; i < intrinsic_arity(id); ++i) {
    args.push_back(w.I(static_cast<int64_t>(i) + 1));
  }
  w.add(w.set(t, w.in(id, args)));
  w.write(m.funcs[0], "main");
  if (auto err = verify(m)) {
    std::fprintf(stderr, "FAIL: %s: malformed IR: %s\n", name, err->c_str());
    ++g_failures;
    return;
  }
  const vm::Program p = vm::compile(m);
  const vm::Insn* found = nullptr;
  for (const vm::Insn& in : p.chunks[0].code) {
    if (in.op == vm::op_of(id)) found = &in;
  }
  if (found == nullptr) {
    std::fprintf(stderr, "FAIL: %s: its opcode was not emitted\n", name);
    ++g_failures;
    return;
  }
  const bool dst = vm::intrinsic_has_dst(id);
  const int32_t base = found->a;
  const int32_t arity = static_cast<int32_t>(intrinsic_arity(id));
  const int32_t got[4] = {found->a, found->b, found->c, found->d};
  for (int32_t f = 0; f < 4; ++f) {
    // With a destination: a = dst (the first argument's register, released
    // before it), then one field per argument. Without: the arguments from
    // a, two at most.
    int32_t arg = -1;
    if (dst) {
      arg = f - 1;
    } else if (f < 2) {
      arg = f;
    }
    int32_t want = (arg >= 0 && arg < arity) ? base + arg : 0;
    if (dst && f == 0) want = base;
    if (got[f] != want) {
      std::fprintf(stderr, "FAIL: %s: operand field %d is r%d, want r%d\n",
                   name, f, got[f], want);
      ++g_failures;
    }
  }
  const std::string text = vm::to_string(p);
  // The dump prints arity + has_dst operands for it: count the registers on
  // the instruction's line.
  const std::string head = std::string("\t") + vm::detail::name_of(found->op) +
                           " r";
  const size_t at = text.find(head);
  if (arity + (dst ? 1 : 0) > 0) {
    if (at == std::string::npos) {
      std::fprintf(stderr, "FAIL: %s: the dump lost its operands\n", name);
      ++g_failures;
    } else {
      const size_t end = text.find('\t', at + 1);
      const std::string line = text.substr(at, end - at);
      int regs = 0;
      for (size_t k = 0; k + 1 < line.size(); ++k) {
        if ((line[k] == 'r' && (line[k + 1] >= '0' && line[k + 1] <= '9')) &&
            (k == 0 || line[k - 1] == ' ')) {
          ++regs;
        }
      }
      if (regs != arity + (dst ? 1 : 0)) {
        std::fprintf(stderr, "FAIL: %s: the dump prints %d operand(s), want %d\n",
                     name, regs, arity + (dst ? 1 : 0));
        ++g_failures;
      }
    }
  }
}

// One Insn per opcode through the bytecode dump: every opcode has a name of
// its own and a format that reads nothing outside the program it was given.
void check_every_opcode_dumps() {
  std::set<std::string> seen;
  vm::Program p;
  p.consts.push_back(coreir::Const{});
  p.natives.push_back("native");
  p.positions.push_back(SrcPos{1, 1});
  vm::Chunk ch;
  ch.name = "all";
  ch.switch_tables.emplace_back();
  for (uint8_t i = 0; i <= static_cast<uint8_t>(vm::Op::DropSet); ++i) {
    const auto op = static_cast<vm::Op>(i);
    const std::string name = vm::detail::name_of(op);
    if (name == "?") {
      std::fprintf(stderr, "FAIL: opcode %d has no name\n", i);
      ++g_failures;
    } else if (!seen.insert(name).second) {
      std::fprintf(stderr, "FAIL: opcode name \"%s\" is shared\n",
                   name.c_str());
      ++g_failures;
    }
    vm::Insn in;
    in.op = op;
    ch.code.push_back(in);
    ch.code_pos.push_back(0);
  }
  p.chunks.push_back(std::move(ch));
  const std::string text = vm::to_string(p);
  for (const std::string& name : seen) {
    if (text.find("\t" + name) == std::string::npos) {
      std::fprintf(stderr, "FAIL: the dump never printed \"%s\"\n",
                   name.c_str());
      ++g_failures;
    }
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
  for (uint8_t i = 0; i <= static_cast<uint8_t>(kLastTag); ++i) {
    check_round_trip(static_cast<Tag>(i), "Tag");
  }
  for (uint8_t i = 0; i <= static_cast<uint8_t>(kLastUnOp); ++i) {
    check_round_trip(static_cast<UnOp>(i), "UnOp");
  }
  for (uint8_t i = 0; i <= static_cast<uint8_t>(kLastBinOp); ++i) {
    check_round_trip(static_cast<BinOp>(i), "BinOp");
  }
  for (uint8_t i = 0; i <= static_cast<uint8_t>(VarKind::Cell); ++i) {
    check_round_trip(static_cast<VarKind>(i), "VarKind");
  }
  for (uint8_t i = 0; i <= static_cast<uint8_t>(kLastIntrinsic);
       ++i) {
    check_round_trip(static_cast<IntrinsicId>(i), "IntrinsicId");
  }
  for (uint8_t i = 0; i <= static_cast<uint8_t>(ConstKind::Str); ++i) {
    check_round_trip(static_cast<ConstKind>(i), "ConstKind");
  }

  // Each intrinsic's opcode is its own, and the opcode names it back.
  for (uint8_t i = 0; i <= static_cast<uint8_t>(kLastIntrinsic); ++i) {
    const auto id = static_cast<IntrinsicId>(i);
    const std::optional<IntrinsicId> back = vm::intrinsic_of(vm::op_of(id));
    if (!back.has_value() || *back != id) {
      std::fprintf(stderr, "FAIL: %s does not round-trip through op_of\n",
                   name_of(id));
      ++g_failures;
    }
    check_intrinsic_layout(id);
  }
  check_every_opcode_dumps();

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
