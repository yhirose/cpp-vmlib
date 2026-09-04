// This binder's whole reason to exist is proving five of vmlib.h's own
// recipes work end to end, against a real oracle (`go run`), not just
// against this repository's own tests -- see the top-level README's
// "Fixed-width integers", "`float`", "Static calls", "Struct fields" and
// "Switch" sections:
//
//   * every int32/uint32 var stays normalized (README's convention) and
//     gets WrapI32/WrapU32 after Add/Sub/Mul, WrapI32 after Div (the
//     INT32_MIN / -1 case), and no wrap at all for Div/comparisons on a
//     normalized operand;
//   * every float32 var is a double re-rounded through ToFloat32 after
//     every arithmetic op, never after a comparison;
//   * a call to another top-level func builds that func's closure once
//     per activation of the calling function -- into a Cell reserved in
//     the caller's own frame -- and every call site in that body reads it
//     back with VarRef(Cell) + CallValue instead of paying a fresh
//     MakeClosure per call. Every current sample only has `main` (which
//     runs exactly once) doing the calling, so this already is the
//     README's "once, at module initialization" in practice; a func other
//     than `main` invoked more than once, or one that recurses through
//     `switch`, would pay the preamble again on each activation, which the
//     stronger form (hoisted to a module-wide cell, reached through a
//     capture) avoids at the cost of a reference cycle for any recursive
//     call graph -- not implemented here, since no sample needs it yet;
//   * a `type ... struct` declaration assigns its fields slots in
//     declaration order, and every field read/write goes through
//     FieldGet/FieldSet at that slot rather than ObjectLit's key
//     comparison -- see copy_struct for the one recipe wrinkle this front
//     end needed that the README's own section does not spell out in
//     code: Go structs are values, so a struct crossing into new storage
//     (a var, a plain or field assign, a struct literal's own field) is a
//     fresh ObjectLit, not the source's ObjectObj shared by reference;
//   * a `switch` lowers straight to coreir::Switch with no extra work,
//     because Go's own case semantics (no fallthrough) already are
//     Switch's; `case a, b:` shares one compiled body NodeId between both
//     keys rather than compiling it twice.
//
//   * goroutines are coroutines on vmlib's scheduler (`go f(args)` is
//     Enqueue(CoroCreate(wrapper)), main itself runs as the first one --
//     see emit_bootstrap), and an unbuffered channel is an object whose
//     two operations are funcs this binder writes in IR once per module
//     (emit_channel_runtime) over CoroCurrent / CoroYield / Enqueue.
//
// What is deliberately absent, because it is not what this front end is
// for: methods, multiple return values, `select`, buffered channels, and
// a struct as a func parameter or return type (structs here are
// local-only). A source file here is real Go, though a narrow slice of
// it -- every sample also runs unmodified under `go run`.

#include "binder.h"

#include <cctype>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <peglib.h>

#include "grammar.h"
#include "vmlib.h"

using namespace peg;
using namespace peg::udl;
using namespace coreir;

namespace go_mini {
namespace {

// Struct and Chan are deliberately last and outside type_from_name's
// vocabulary: a struct's identity is its name in the struct table
// (Binder::structs), a channel's is its element type, and neither is a
// fixed word this front end knows ahead of time, so resolving either needs
// more than this free function (Binder::resolve_type). Both carry their
// second half in the `struct_id` field beside them: the structs index for
// a Struct, the element Type (one of the six scalars) for a Chan.
enum class Type { I32, I64, U32, F32, F64, Bool, Struct, Chan };

const char* type_name(Type t) {
  switch (t) {
    case Type::I32: return "int32";
    case Type::I64: return "int64";
    case Type::U32: return "uint32";
    case Type::F32: return "float32";
    case Type::F64: return "float64";
    case Type::Bool: return "bool";
    case Type::Struct: return "struct";
    case Type::Chan: return "chan";
  }
  return "?";
}

std::optional<Type> type_from_name(const std::string& s) {
  if (s == "int32") return Type::I32;
  if (s == "int64") return Type::I64;
  if (s == "uint32") return Type::U32;
  if (s == "float32") return Type::F32;
  if (s == "float64") return Type::F64;
  if (s == "bool") return Type::Bool;
  return std::nullopt;
}

SrcPos pos_of(const Ast& a) {
  return {static_cast<uint32_t>(a.line), static_cast<uint32_t>(a.column)};
}

[[noreturn]] void fail(const Ast& a, const std::string& msg) {
  coreir_rt::fail(msg, static_cast<uint32_t>(a.line),
              static_cast<uint32_t>(a.column));
}

int64_t parse_int(const std::string& s, const Ast& at) {
  errno = 0;
  char* end = nullptr;
  const long long v = std::strtoll(s.c_str(), &end, 10);
  if (errno != 0 || end != s.c_str() + s.size()) {
    fail(at, "integer literal out of range");
  }
  return static_cast<int64_t>(v);
}

const Ast* find_child(const Ast& a, std::string_view name) {
  for (const auto& n : a.nodes) {
    if (n->name == name) return n.get();
  }
  return nullptr;
}

BinOp binop_of(std::string_view t, const Ast& at) {
  if (t == "+") return BinOp::Add;
  if (t == "-") return BinOp::Sub;
  if (t == "*") return BinOp::Mul;
  if (t == "/") return BinOp::Div;
  if (t == "==") return BinOp::Eq;
  if (t == "!=") return BinOp::Ne;
  if (t == "<") return BinOp::Lt;
  if (t == "<=") return BinOp::Le;
  if (t == ">") return BinOp::Gt;
  if (t == ">=") return BinOp::Ge;
  fail(at, "unknown operator '" + std::string(t) + "'");
}

// Whether a Type needs its struct_id to be fully described: a Struct (which
// struct) and a Chan (which element type). Every other Type is
// self-describing.
bool has_second_half(Type t) { return t == Type::Struct || t == Type::Chan; }

// Two Struct-typed values (or hints) are the same type only if they name
// the same struct, two Chan-typed ones only if they carry the same element
// -- the one comparison Type alone, a plain enum, cannot make.
bool same_type(Type t1, int32_t struct_id1, Type t2, int32_t struct_id2) {
  return t1 == t2 && (!has_second_half(t1) || struct_id1 == struct_id2);
}

// A struct- or chan-typed target gives a literal nothing to take on. Hints
// exist for Go's untyped constants -- `var x int32 = 1` is what narrows the
// 1 -- and no literal syntax here can ever become a struct or a channel, so
// such a target hands down nullopt and lets the assignment's own same_type
// check produce the message rather than literal_as's cruder "cannot use an
// integer literal as a struct". Four positions want exactly this (vardecl,
// a plain assign, a field assign and a structlit's fieldinit), which is why
// it is a function rather than the same ternary spelled out four times.
std::optional<Type> hint_for(Type t) {
  if (has_second_half(t)) return std::nullopt;
  return t;
}

// A resolved `type` token: a fixed scalar, or Type::Struct plus which entry
// of Binder::structs it names, or Type::Chan plus its element Type (as an
// int, in the same field).
struct TypeRef { Type type; int32_t struct_id = -1; };

// A variable's slot and static type; a function's own call-target cells
// (README's Static calls recipe) and, while its body is being compiled,
// how many locals and cells it has claimed so far. struct_id is only
// meaningful when type is a Struct or a Chan (TypeRef's rule).
struct LocalInfo { int32_t slot; Type type; int32_t struct_id = -1; };

struct FuncCtx {
  std::map<std::string, LocalInfo> locals;
  std::vector<std::string> local_names;  // parallel to Func::num_locals
  int32_t next_local = 0;
  // The Static calls recipe's cells: one per distinct callee this body
  // reaches, claimed the first time a call to it is emitted and filled by
  // the preamble build() puts ahead of the body. A `go` statement claims
  // further cells of its own for the arguments it evaluates.
  std::map<std::string, int32_t> call_cells;  // callee name -> cell index
  int32_t next_cell = 0;
  std::optional<TypeRef> ret_type;

  int32_t cell_for(const std::string& callee) {
    auto it = call_cells.find(callee);
    if (it != call_cells.end()) return it->second;
    const int32_t c = next_cell++;
    call_cells[callee] = c;
    return c;
  }
};

struct FuncInfo {
  int32_t index = 0;
  bool has_ret = false;
  TypeRef ret{Type::I64, -1};
  std::vector<TypeRef> param_types;
};

struct TypedExpr { NodeId node; Type type; int32_t struct_id = -1; };

// One struct field: its static type (README's Struct fields section) and
// the props slot ObjectLit/FieldGet/FieldSet agree on -- declaration order,
// the same contract a local's slot index already is.
struct FieldDef { std::string name; Type type; int32_t struct_id; int32_t slot; };
struct StructDef { std::string name; std::vector<FieldDef> fields; };

struct Binder {
  Module m;
  std::map<std::string, FuncInfo> funcs;
  std::map<std::string, int32_t> struct_ids;  // struct name -> structs index
  std::vector<StructDef> structs;

  // Struct(struct_id).name when t == Type::Struct, "chan <elem>" for a
  // Chan, else type_name(t) -- the one place a diagnostic needs a type's
  // own spelling rather than the bare word "struct" or "chan".
  std::string describe_type(Type t, int32_t struct_id) const {
    if (t == Type::Struct) return structs[static_cast<size_t>(struct_id)].name;
    if (t == Type::Chan) {
      return std::string("chan ") + type_name(static_cast<Type>(struct_id));
    }
    return type_name(t);
  }

  // A `type` token as any of the three things it can name: one of
  // type_from_name's six fixed words; `chan T` for one of those six (the
  // grammar captures the two words as one token, whitespace and all); or a
  // struct declared by an earlier (in source order -- this front end
  // resolves field and var types in one linear pass, so a struct's own
  // fields may only name structs declared above it) structdecl.
  TypeRef resolve_type(const std::string& s, const Ast& at) {
    if (auto t = type_from_name(s)) return {*t, -1};
    if (s.size() > 4 && s.compare(0, 4, "chan") == 0 &&
        std::isspace(static_cast<unsigned char>(s[4]))) {
      std::string elem = s.substr(4);
      const size_t b = elem.find_first_not_of(" \t\r\n");
      const size_t e = elem.find_last_not_of(" \t\r\n");
      elem = b == std::string::npos ? "" : elem.substr(b, e - b + 1);
      auto t = type_from_name(elem);
      if (!t) fail(at, "unsupported channel element type '" + elem + "'");
      return {Type::Chan, static_cast<int32_t>(*t)};
    }
    auto it = struct_ids.find(s);
    if (it == struct_ids.end()) fail(at, "unknown type '" + s + "'");
    return {Type::Struct, it->second};
  }

  // A type in a func signature: resolve_type, minus structs -- a struct as
  // a param or return raises value-semantics questions (does the callee's
  // copy alias the caller's?) this front end does not take on. A channel
  // is a reference in Go itself, so it passes through unchanged.
  TypeRef resolve_sig_type(const std::string& s, const Ast& at) {
    const TypeRef t = resolve_type(s, at);
    if (t.type == Type::Struct) {
      fail(at, "a struct cannot be a parameter or return type here");
    }
    return t;
  }

  // A field's index within its struct (== its ObjectLit/FieldGet/FieldSet
  // slot), or -1 if there is no field by that name -- the one lookup
  // fieldaccess, chained assignment and structlit all need, over the same
  // small vector a real struct's field count keeps linear fine for.
  static int32_t find_field_index(const StructDef& sd, const std::string& name) {
    for (size_t i = 0; i < sd.fields.size(); ++i) {
      if (sd.fields[i].name == name) return static_cast<int32_t>(i);
    }
    return -1;
  }

  // A local by name, or the one diagnostic there is for not finding it.
  // Hands back the LocalInfo rather than a built VarRef because the
  // assignment forms want the slot, not a value node.
  static const LocalInfo& local_of(const Ast& at, FuncCtx& ctx,
                                   const std::string& name) {
    auto it = ctx.locals.find(name);
    if (it == ctx.locals.end()) fail(at, "undeclared: " + name);
    return it->second;
  }

  // Every struct value this front end builds is the same ObjectLit: one key
  // per field, pushed in StructDef's own declaration order, because that
  // order *is* the slot contract FieldGet/FieldSet index by (the top-level
  // README's Struct fields section). The two places that build one -- a
  // structlit, and copy_struct's fresh copy of an existing value -- both
  // arrive with their values already in that order, so this is the single
  // place the order turns into props, and so the single place that promise
  // can be broken.
  TypedExpr make_struct(int32_t sid, const std::vector<NodeId>& values,
                        SrcPos p) {
    Builder b(m);
    const StructDef& sd = structs[static_cast<size_t>(sid)];
    std::vector<std::pair<NodeId, NodeId>> kvs;
    kvs.reserve(sd.fields.size());
    for (size_t i = 0; i < sd.fields.size(); ++i) {
      kvs.emplace_back(b.str_literal(sd.fields[i].name, p), values[i]);
    }
    return {b.object_lit(kvs, p), Type::Struct, sid};
  }

  // Go structs are values: `l.To = p` (or `Line{To: p}`, or `var q = p`)
  // must not leave l.To and p pointing at the same ObjectObj, the way a
  // plain FieldSet/Assign of the read-out NodeId would -- vmlib's own
  // Scope names this exact gap ("structs a front end wants value semantics
  // for emits explicit copies for"; FieldGet/FieldSet themselves stay
  // reference-shaped, the same ObjectObj Index/SetIndex already are). This
  // is that copy: a fresh ObjectLit, one FieldGet per field, recursing so a
  // struct nested inside a struct is copied too rather than the inner
  // ObjectObj carried over by reference. A value already built by a
  // structlit right here (Tag::ObjectLit) is already fresh -- nothing has
  // had the chance to alias it yet -- so copying it again would only cost
  // an allocation, not fix a bug.
  TypedExpr copy_struct(TypedExpr v, SrcPos p) {
    if (v.type != Type::Struct) return v;
    if (m.at(v.node).tag == Tag::ObjectLit) return v;
    Builder b(m);
    const StructDef& sd = structs[static_cast<size_t>(v.struct_id)];
    std::vector<NodeId> values;
    values.reserve(sd.fields.size());
    for (const auto& fd : sd.fields) {
      const NodeId raw = b.field_get(v.node, fd.slot, fd.name, p);
      values.push_back(copy_struct({raw, fd.type, fd.struct_id}, p).node);
    }
    return make_struct(v.struct_id, values, p);
  }

  // One step of a field chain: the field `a.nodes[i]` names on the value
  // `recv` already stands for, or the diagnostic for why there is none.
  // A read (`p.X`) and a chained write (`p.From.X = v`) resolve every step
  // of their chain in exactly this way, and both owe the same two
  // complaints in the same order -- "that ident is not a struct at all"
  // before "that struct has no field by this name" -- so the step is
  // written here once rather than once per direction, and a third time for
  // the write's own last field.
  const FieldDef& field_of(const Ast& a, const TypedExpr& recv, size_t i) {
    if (recv.type != Type::Struct) {
      fail(a, "'" + std::string(a.nodes[i - 1]->token) + "' (" +
                  describe_type(recv.type, recv.struct_id) + ") has no fields");
    }
    const StructDef& sd = structs[static_cast<size_t>(recv.struct_id)];
    const std::string fname(a.nodes[i]->token);
    const int32_t idx = find_field_index(sd, fname);
    if (idx < 0) fail(a, sd.name + " has no field '" + fname + "'");
    return sd.fields[static_cast<size_t>(idx)];
  }

  // The value a field chain lands on: the local `a.nodes[0]` names, then
  // one FieldGet per ident in a.nodes[1 .. last). Both node shapes that
  // carry a chain -- "fieldaccess" and an "assign" with fields -- have the
  // receiver first and the idents after it, so both walk with this; they
  // differ only in where they stop. fieldaccess walks every ident and keeps
  // the value it arrives at; a chained assign stops one ident short,
  // because that last one is the field it is about to FieldSet rather than
  // read. Walking the intermediate fields with FieldGet is what makes
  // `p.From.X = v` write through `p`: FieldGet hands back the very
  // ObjectObj the parent holds, so the FieldSet at the end lands on the
  // struct actually stored there rather than on a copy.
  TypedExpr walk_fields(const Ast& a, FuncCtx& ctx, size_t last, SrcPos p) {
    const LocalInfo& li = local_of(a, ctx, std::string(a.nodes[0]->token));
    Builder b(m);
    TypedExpr cur{b.varref(VarKind::Local, li.slot, p), li.type,
                 li.struct_id};
    for (size_t i = 1; i < last; ++i) {
      const FieldDef& fd = field_of(a, cur, i);
      cur = {b.field_get(cur.node, fd.slot, fd.name, p), fd.type,
             fd.struct_id};
    }
    return cur;
  }

  // A value on its way into a field, in the three steps it always owes:
  // emitted with the field's own type as its hint, checked against that
  // type through same_type (so two different structs are not
  // interchangeable merely because both are Type::Struct), and
  // copy_struct'd, because a struct crossing into a field is new storage.
  // structlit and a chained assign are the only two ways a value gets into
  // a field, and both owe all three in this order -- a check that ran
  // against the copy, or a copy that never ran, is exactly the value-
  // semantics bug copy_struct exists to prevent, so the order is fixed
  // here once instead of restated at each site. `at` and `p` stay separate:
  // structlit reports a mismatch at the fieldinit but positions its IR at
  // the literal, and collapsing them would move source positions in the
  // emitted nodes.
  NodeId emit_field_value(const Ast& expr, FuncCtx& ctx, const FieldDef& fd,
                          const Ast& at, SrcPos p) {
    TypedExpr v = emit_expr(expr, ctx, hint_for(fd.type));
    if (!same_type(v.type, v.struct_id, fd.type, fd.struct_id)) {
      fail(at, "cannot assign " + describe_type(v.type, v.struct_id) +
                   " to field '" + fd.name + "' (" +
                   describe_type(fd.type, fd.struct_id) + ")");
    }
    return copy_struct(v, p).node;
  }

  // A value on its way into a local, in the same three steps
  // emit_field_value owes for a field: emitted with the destination's own
  // type as its hint, checked through same_type (two different structs are
  // not interchangeable merely because both are Type::Struct), and
  // copy_struct'd, because a local is new storage. `var x T = v` and
  // `x = v` differ only in whether the slot is being created or looked up;
  // everything after that is written once here, so a check that ran
  // against the copy -- or a copy that never ran -- cannot appear in one of
  // them and not the other.
  NodeId emit_local_value(const Ast& expr, FuncCtx& ctx, Type ty,
                          int32_t struct_id, const Ast& at, SrcPos p) {
    TypedExpr val = emit_expr(expr, ctx, hint_for(ty));
    if (!same_type(val.type, val.struct_id, ty, struct_id)) {
      fail(at, "cannot assign " + describe_type(val.type, val.struct_id) +
                   " to " + describe_type(ty, struct_id));
    }
    return copy_struct(val, p).node;
  }

  // -- Pass 0: register every struct's fields, in source order ------------
  //
  // Struct fields (like func params/returns) are always the six scalar
  // types or an already-declared struct -- never resolved through this
  // pass's own not-yet-complete table, so a struct can only nest one
  // declared above it, not later (it is not there yet) or itself (rejected
  // below rather than left to recurse forever the first time anything
  // tries to construct or copy_struct one).
  void register_structs(const Ast& program) {
    for (const auto& declPtr : program.nodes) {
      const Ast& decl = *declPtr;
      if (decl.tag != "structdecl"_) continue;
      const std::string name(find_child(decl, "ident")->token);
      if (struct_ids.count(name)) fail(decl, "'" + name + "' is already defined");
      if (type_from_name(name)) {
        fail(decl, "'" + name + "' shadows a built-in type");
      }
      const int32_t sid = static_cast<int32_t>(structs.size());
      struct_ids[name] = sid;
      structs.push_back({name, {}});

      const Ast* fields = find_child(decl, "fields");
      StructDef& sd = structs[static_cast<size_t>(sid)];
      for (const auto& fPtr : fields->nodes) {
        const Ast& f = *fPtr;
        const std::string fname(find_child(f, "ident")->token);
        if (find_field_index(sd, fname) >= 0) {
          fail(f, "duplicate field '" + fname + "'");
        }
        const TypeRef fty =
            resolve_type(std::string(find_child(f, "type")->token), f);
        // The name went into struct_ids above, before its own fields are
        // resolved (they have to be, to end up in structs[sid]) -- which
        // would otherwise let a struct name itself: a type nothing could
        // ever construct a value of, and one copy_struct would recurse on
        // forever if anything ever did.
        if (fty.struct_id == sid) {
          fail(f, "invalid recursive type '" + name + "'");
        }
        const int32_t slot = static_cast<int32_t>(sd.fields.size());
        sd.fields.push_back({fname, fty.type, fty.struct_id, slot});
      }
    }
  }

  // -- Pass 1: register every func's signature, main first --------------
  //
  // main first because vm::run's entry point is always funcs[0], and this
  // front end (like real Go) does not require main to be declared first in
  // the source.
  void register_funcs(const Ast& program) {
    const Ast* main_fn = nullptr;
    std::vector<const Ast*> others;
    for (const auto& fn : program.nodes) {
      if (fn->tag != "func"_) continue;  // a structdecl -- register_structs's
      const std::string name(find_child(*fn, "ident")->token);
      if (name == "main") {
        if (main_fn != nullptr) fail(*fn, "'main' is already defined");
        main_fn = fn.get();
      } else {
        others.push_back(fn.get());
      }
    }
    if (main_fn == nullptr) fail(program, "no 'main' function");

    std::vector<const Ast*> ordered{main_fn};
    ordered.insert(ordered.end(), others.begin(), others.end());

    for (const Ast* fn : ordered) {
      const std::string name(find_child(*fn, "ident")->token);
      if (funcs.count(name)) fail(*fn, "'" + name + "' is already defined");

      FuncInfo info;
      info.index = static_cast<int32_t>(m.funcs.size());
      if (const Ast* rt = find_child(*fn, "type")) {
        info.has_ret = true;
        info.ret = resolve_sig_type(std::string(rt->token), *rt);
      }
      const Ast* params = find_child(*fn, "params");
      for (const auto& p : params->nodes) {
        info.param_types.push_back(
            resolve_sig_type(std::string(find_child(*p, "type")->token), *p));
      }
      funcs[name] = info;

      Func f;
      f.name = name;
      f.num_params = static_cast<int32_t>(info.param_types.size());
      m.funcs.push_back(f);
    }
  }

  // -- The runtime this front end writes in its own IR --------------------
  //
  // An unbuffered channel is an object {recvq: [...], sendq: [...]} whose
  // queues hold waiters, each a {co, value} object: the coroutine parked
  // on the channel and the value it is sending or (once woken) received.
  // $chan_send and $chan_recv are the two operations, as ordinary funcs
  // built here once and called through the same Static calls cells any
  // user func is; the `$` keeps them out of the source language's reach.
  // The library supplies only the primitives -- CoroCurrent, CoroYield,
  // Enqueue -- and Go's own rendezvous rule (a sender with a receiver
  // waiting hands off and goes on; one without parks until a receiver
  // takes the value and wakes it; symmetrically for a receiver) is
  // written here, not in vmlib.h, because it is Go's rule and not every
  // language's.
  void emit_channel_runtime() {
    Builder b(m);
    const SrcPos p{0, 0};
    auto L = [&](int32_t i) { return b.varref(VarKind::Local, i, p); };
    auto S = [&](const char* s) { return b.str_literal(s, p); };
    auto len = [&](NodeId v) { return b.intrinsic(IntrinsicId::Len, {v}, p); };
    // queue[0], and queue = queue[1:] -- the take-the-first step both
    // directions share; `w` is the local the waiter lands in.
    auto take_first = [&](NodeId ch, const char* q, int32_t w) {
      const NodeId queue = b.index(ch, S(q), p);
      return std::vector<NodeId>{
          b.assign(VarKind::Local, w, b.index(queue, b.literal(0, p), p), p),
          b.set_index(ch, S(q),
                      b.intrinsic(IntrinsicId::ArraySlice,
                                  {queue, b.literal(1, p), len(queue)}, p),
                      p)};
    };
    auto waiting = [&](NodeId ch, const char* q) {
      return b.binary(BinOp::Gt, len(b.index(ch, S(q), p)), b.literal(0, p),
                      p);
    };
    auto park = [&](NodeId ch, const char* q, int32_t w, NodeId value) {
      return std::vector<NodeId>{
          b.assign(VarKind::Local, w,
                   b.object_lit({{S("co"),
                                  b.intrinsic(IntrinsicId::CoroCurrent, {}, p)},
                                 {S("value"), value}},
                                p),
                   p),
          b.intrinsic(IntrinsicId::ArrayPush, {b.index(ch, S(q), p), L(w)}, p),
          b.intrinsic(IntrinsicId::CoroYield, {b.nil_literal(p)}, p)};
    };
    auto wake = [&](int32_t w) {
      return b.intrinsic(IntrinsicId::Enqueue,
                         {b.index(L(w), S("co"), p)}, p);
    };

    // $chan_send(ch, v): locals ch, v, w.
    {
      std::vector<NodeId> handoff = take_first(L(0), "recvq", 2);
      handoff.push_back(b.set_index(L(2), S("value"), L(1), p));
      handoff.push_back(wake(2));
      Func f;
      f.name = "$chan_send";
      f.num_params = 2;
      f.num_locals = 3;
      f.local_names = {"ch", "v", "w"};
      f.body = b.scope(0, 3,
                       b.make_if(waiting(L(0), "recvq"), b.block(handoff, p),
                                 b.block(park(L(0), "sendq", 2, L(1)), p), p),
                       p);
      funcs["$chan_send"].index = static_cast<int32_t>(m.funcs.size());
      m.funcs.push_back(f);
    }
    // $chan_recv(ch) -> value: locals ch, w.
    {
      std::vector<NodeId> take = take_first(L(0), "sendq", 1);
      take.push_back(wake(1));
      take.push_back(b.make_return(b.index(L(1), S("value"), p), p));
      std::vector<NodeId> wait = park(L(0), "recvq", 1, b.nil_literal(p));
      wait.push_back(b.make_return(b.index(L(1), S("value"), p), p));
      Func f;
      f.name = "$chan_recv";
      f.num_params = 1;
      f.num_locals = 2;
      f.local_names = {"ch", "w"};
      f.body = b.scope(0, 2,
                       b.make_if(waiting(L(0), "sendq"), b.block(take, p),
                                 b.block(wait, p), p),
                       p);
      funcs["$chan_recv"].index = static_cast<int32_t>(m.funcs.size());
      m.funcs.push_back(f);
    }
  }

  // Go's main is a goroutine: it can block on a channel, and a program
  // whose every goroutine is blocked is a deadlock rather than a hang.
  // vmlib's entry frame is neither (a CoroYield there has no coroutine to
  // suspend), so funcs[0] is a bootstrap that spawns main as the first
  // goroutine -- Enqueue(CoroCreate(wrapper)) -- and returns; the
  // scheduler drains the queue from there, and its end-of-run deadlock
  // check is Go's "all goroutines are asleep". The wrapper takes the one
  // argument the scheduler passes (nil) leniently and calls main with
  // none. One consequence to know: where Go ends the program when main
  // returns, this scheduler runs the goroutines still runnable to their
  // own ends, so a sample that wants Go's output must not leave any with
  // output pending -- the samples synchronize through channels instead.
  void emit_bootstrap() {
    Builder b(m);
    const SrcPos p{0, 0};
    const int32_t empty_cmap = static_cast<int32_t>(m.capture_maps.size());
    m.capture_maps.push_back({});
    Func w;
    w.name = "$main";
    w.lenient_arity = true;
    w.body = b.call_value(b.make_closure(funcs.at("main").index, empty_cmap, p),
                          {}, p);
    const int32_t widx = static_cast<int32_t>(m.funcs.size());
    m.funcs.push_back(w);
    Func& entry = m.funcs[0];
    entry.name = "$entry";
    entry.body = b.intrinsic(
        IntrinsicId::Enqueue,
        {b.intrinsic(IntrinsicId::CoroCreate,
                     {b.make_closure(widx, empty_cmap, p)}, p)},
        p);
  }

  // -- Literals -----------------------------------------------------------
  //
  // An int literal starts out as I64 and is only reinterpreted as `ty`
  // here -- narrowing one into i32/u32/f32 is emit_convert's I64 row
  // (README's own point: a literal goes through the same Wrap a variable
  // holding that value would, so `square(50000)` sees exactly what `x * x`
  // would compute from a local holding 50000), so this delegates to it
  // rather than restating it. Only the two refusals stay local, since a
  // literal can say why better than a conversion can ("cannot use an
  // integer literal as bool" rather than "cannot convert int64 to bool").
  TypedExpr literal_as(int64_t v, Type ty, const Ast& at, SrcPos p) {
    Builder b(m);
    if (ty == Type::Bool) fail_at(p, "cannot use an integer literal as bool");
    if (ty == Type::Struct) {
      fail_at(p, "cannot use an integer literal as a struct");
    }
    return emit_convert(ty, {b.literal(v, p), Type::I64}, at);
  }

  [[noreturn]] void fail_at(SrcPos p, const std::string& msg) {
    coreir_rt::fail(msg, p.line, p.col);
  }

  // What both arithmetic recipes come down to, once: an i32/u32 result gets
  // its Wrap, an f32 result its ToFloat32, and i64/f64 already are what
  // they claim to be (README's Fixed-width integers and `float` sections).
  // A binary op and a negation owe exactly this same choice, and the same
  // two refusals differing only in the verb they name, so a seventh
  // numeric type is one arm here rather than one arm in each of two
  // switches that have to be remembered together.
  NodeId normalize(Type t, NodeId raw, const Ast& at, const char* verb) {
    Builder b(m);
    const SrcPos p = pos_of(at);
    switch (t) {
      case Type::I32: return b.unary(UnOp::WrapI32, raw, p);
      case Type::U32: return b.unary(UnOp::WrapU32, raw, p);
      case Type::F32: return b.intrinsic(IntrinsicId::ToFloat32, {raw}, p);
      case Type::I64: case Type::F64: return raw;
      case Type::Bool: fail(at, std::string("cannot ") + verb + " bool");
      case Type::Struct:
        fail(at, std::string("cannot ") + verb + " a struct");
      case Type::Chan:
        fail(at, std::string("cannot ") + verb + " a channel");
    }
    fail(at, "unreachable");
  }

  // -- Arithmetic, per README's Fixed-width integers and `float` recipes -
  TypedExpr emit_binary(BinOp op, TypedExpr l, TypedExpr r, const Ast& at) {
    if (!same_type(l.type, l.struct_id, r.type, r.struct_id)) {
      fail(at, "cannot combine " + describe_type(l.type, l.struct_id) +
                   " and " + describe_type(r.type, r.struct_id));
    }
    const SrcPos p = pos_of(at);
    Builder b(m);
    if (is_comparison(op)) {
      // Go compares two structs field by field; Eq/Ne here is ObjectObj
      // identity, which would answer a different question -- so refuse it
      // at bind time rather than let the executor's own "cannot eq object
      // and object" stand in for a diagnostic, the same way printing a
      // struct is refused in "print" below. Channels the same, and an
      // ordering of bools is not Go either.
      if (has_second_half(l.type) ||
          (l.type == Type::Bool && op != BinOp::Eq && op != BinOp::Ne)) {
        fail(at, "cannot compare " + describe_type(l.type, l.struct_id) +
                     " values");
      }
      // A normalized operand compares correctly at every width without a
      // wrap or an unsigned form (README's Fixed-width integers section).
      return {b.binary(op, l.node, r.node, p), Type::Bool};
    }
    const NodeId raw = b.binary(op, l.node, r.node, p);
    // Add/Sub/Mul can leave u32's range; Div cannot (both operands are
    // already non-negative, so the quotient is too) -- README's own point
    // about a normalized operand needing no wrap there.
    if (l.type == Type::U32 && op == BinOp::Div) return {raw, Type::U32};
    return {normalize(l.type, raw, at, "do arithmetic on"), l.type};
  }

  TypedExpr emit_neg(TypedExpr v, const Ast& at) {
    Builder b(m);
    const NodeId raw = b.unary(UnOp::Neg, v.node, pos_of(at));
    return {normalize(v.type, raw, at, "negate"), v.type};
  }

  // The one refusal every arm of emit_convert owes. type_name already holds
  // each target's own Go spelling, so the five arms do not each retype
  // theirs into a string literal that could drift from the word the rest of
  // this front end prints.
  [[noreturn]] void fail_convert(Type to, const TypedExpr& v, const Ast& at) {
    fail(at, "cannot convert " + describe_type(v.type, v.struct_id) + " to " +
                 type_name(to));
  }

  // Go's own conversion syntax is a call -- `int32(x)`, `float64(x)` -- so
  // this is what `call` resolves to when its callee names a type rather
  // than a func. float32 -> float64 is a no-op here: this VM already
  // stores a float32 value as the double it promotes to (README's `float`
  // section), so widening it changes no bits.
  TypedExpr emit_convert(Type to, TypedExpr v, const Ast& at) {
    const SrcPos p = pos_of(at);
    Builder b(m);
    if (v.type == to) return v;
    switch (to) {
      case Type::I32:
        switch (v.type) {
          case Type::I64: case Type::U32:
            return {b.unary(UnOp::WrapI32, v.node, p), Type::I32};
          case Type::F32: case Type::F64:
            return {b.unary(UnOp::WrapI32,
                            b.intrinsic(IntrinsicId::ToInt, {v.node}, p), p),
                   Type::I32};
          default: fail_convert(to, v, at);
        }
      case Type::I64:
        switch (v.type) {
          case Type::I32: case Type::U32: return {v.node, Type::I64};
          case Type::F32: case Type::F64:
            return {b.intrinsic(IntrinsicId::ToInt, {v.node}, p), Type::I64};
          default: fail_convert(to, v, at);
        }
      case Type::U32:
        switch (v.type) {
          case Type::I32: case Type::I64:
            return {b.unary(UnOp::WrapU32, v.node, p), Type::U32};
          case Type::F32: case Type::F64:
            return {b.unary(UnOp::WrapU32,
                            b.intrinsic(IntrinsicId::ToInt, {v.node}, p), p),
                   Type::U32};
          default: fail_convert(to, v, at);
        }
      case Type::F32:
        switch (v.type) {
          case Type::F64:
            return {b.intrinsic(IntrinsicId::ToFloat32, {v.node}, p),
                   Type::F32};
          case Type::I32: case Type::I64: case Type::U32:
            return {b.intrinsic(IntrinsicId::ToFloat32,
                                {b.intrinsic(IntrinsicId::ToDouble,
                                            {v.node}, p)},
                                p),
                   Type::F32};
          default: fail_convert(to, v, at);
        }
      case Type::F64:
        switch (v.type) {
          case Type::F32: return {v.node, Type::F64};  // see above
          case Type::I32: case Type::I64: case Type::U32:
            return {b.intrinsic(IntrinsicId::ToDouble, {v.node}, p),
                   Type::F64};
          default: fail_convert(to, v, at);
        }
      case Type::Bool:
        fail(at, "cannot convert to bool");
      case Type::Struct:
        fail(at, "cannot convert to a struct");
      case Type::Chan:
        fail(at, "cannot convert to a channel");
    }
    fail(at, "unreachable");
  }

  // -- Expressions ----------------------------------------------------
  //
  // `hint`, when set, is the type a literal should take on at this
  // position (a var's declared type, a call argument's parameter type) --
  // Go's own rule for an untyped constant. Every other node knows its type
  // from its operands or declaration and ignores the hint.
  TypedExpr emit_expr(const Ast& a, FuncCtx& ctx, std::optional<Type> hint) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    switch (a.tag) {
      case "number"_: {
        const std::string tok(a.token);
        if (tok.find('.') != std::string::npos) {
          // A float literal is already a double; only F32 needs the extra
          // rounding, and only int syntax has a width to be narrowed into.
          const NodeId lit =
              b.double_literal(std::strtod(tok.c_str(), nullptr), p);
          if (hint == Type::F32) {
            return {b.intrinsic(IntrinsicId::ToFloat32, {lit}, p), Type::F32};
          }
          return {lit, Type::F64};
        }
        return literal_as(parse_int(tok, a), hint.value_or(Type::I64), a, p);
      }
      case "ident"_: {
        const LocalInfo& li = local_of(a, ctx, std::string(a.token));
        return {b.varref(VarKind::Local, li.slot, p), li.type, li.struct_id};
      }
      case "neg"_: {
        TypedExpr v = emit_expr(*a.nodes[0], ctx, hint);
        return emit_neg(v, a);
      }
      // Point{X: 1, Y: 2} -- children: the struct name (an "ident" the
      // grammar happens to share with plain variable references and calls,
      // told apart here only by this node's own tag) and a "fieldinits"
      // list of (field ident, value expr) pairs. Every field must be
      // given exactly once; ObjectLit fills props in the order they are
      // pushed here, which is why that order is StructDef's own
      // declaration order rather than the literal's -- a struct is exactly
      // an Object a front end has promised to index by slot (the top-level
      // README's Struct fields section), and this is where that promise is
      // kept.
      case "structlit"_: {
        const std::string sname(find_child(a, "ident")->token);
        auto sit = struct_ids.find(sname);
        if (sit == struct_ids.end()) fail(a, "unknown struct '" + sname + "'");
        const int32_t sid = sit->second;
        const StructDef& sd = structs[static_cast<size_t>(sid)];
        std::vector<NodeId> values(sd.fields.size());
        std::vector<bool> seen(sd.fields.size(), false);
        for (const auto& fiPtr : find_child(a, "fieldinits")->nodes) {
          const Ast& fi = *fiPtr;
          const std::string fname(find_child(fi, "ident")->token);
          const int32_t idx = find_field_index(sd, fname);
          if (idx < 0) fail(fi, sname + " has no field '" + fname + "'");
          if (seen[static_cast<size_t>(idx)]) {
            fail(fi, "field '" + fname + "' initialized twice");
          }
          const FieldDef& fd = sd.fields[static_cast<size_t>(idx)];
          values[static_cast<size_t>(idx)] =
              emit_field_value(*fi.nodes.back(), ctx, fd, fi, p);
          seen[static_cast<size_t>(idx)] = true;
        }
        for (size_t i = 0; i < sd.fields.size(); ++i) {
          if (!seen[i]) {
            fail(a, sname + " literal is missing field '" + sd.fields[i].name +
                        "'");
          }
        }
        return make_struct(sid, values, p);
      }
      // p.X, or p.From.X for a field that is itself a struct -- children:
      // the receiver ident, then one ident per '.'. Each step resolves a
      // field to a slot the way structlit resolved it to one when the
      // struct was built, and reads it in O(1) through FieldGet rather
      // than ObjectLit's key comparison; a chain is just FieldGet applied
      // to the previous FieldGet's own result, which is as valid a
      // receiver as a local's VarRef -- FieldGet's "children: receiver"
      // never required that receiver to be one.
      case "fieldaccess"_:
        return walk_fields(a, ctx, a.nodes.size(), p);
      // <-ch: the element the channel hands over, through $chan_recv. Its
      // type is the channel's element type, which is what makes
      // `var v int32 = <-in` check and `fmt.Println(<-out)` print an int.
      case "recv"_: {
        const LocalInfo& li = channel_local(*a.nodes[0], ctx);
        const NodeId recv = b.varref(VarKind::Cell, ctx.cell_for("$chan_recv"), p);
        return {b.call_value(recv, {b.varref(VarKind::Local, li.slot, p)}, p),
                static_cast<Type>(li.struct_id)};
      }
      // make(chan T): a fresh channel object. Its two queues start empty;
      // emit_channel_runtime's two funcs are the only things that read or
      // write them.
      case "makechan"_: {
        const TypeRef t = resolve_type(std::string(a.nodes[0]->token), a);
        if (t.type != Type::Chan) fail(a, "make takes a channel type here");
        const NodeId ch = b.object_lit(
            {{b.str_literal("recvq", p), b.array_lit({}, p)},
             {b.str_literal("sendq", p), b.array_lit({}, p)}},
            p);
        return {ch, Type::Chan, t.struct_id};
      }
      case "call"_: {
        const std::string callee(find_child(a, "ident")->token);
        if (auto conv = type_from_name(callee)) {
          const Ast* argsNode = find_child(a, "args");
          if (argsNode == nullptr || argsNode->nodes.size() != 1) {
            fail(a, "conversion takes exactly one argument");
          }
          TypedExpr v = emit_expr(*argsNode->nodes[0], ctx, std::nullopt);
          return emit_convert(*conv, v, a);
        }
        const FuncInfo& info = func_of(a, callee);
        const std::vector<NodeId> args = emit_call_args(a, ctx, info, callee);
        if (!info.has_ret) fail(a, callee + " does not return a value");
        // The recipe: read the closure this function's preamble built once
        // (the cell claimed here, filled in build()), rather than a fresh
        // MakeClosure at every call site.
        const NodeId closure = b.varref(VarKind::Cell, ctx.cell_for(callee), p);
        return {b.call_value(closure, args, p), info.ret.type,
                info.ret.struct_id};
      }
      case "equality"_:
      case "relational"_:
      case "additive"_:
      case "multiplicative"_: {
        const auto& ns = a.nodes;
        TypedExpr acc = emit_expr(*ns[0], ctx, hint);
        for (size_t i = 1; i + 1 < ns.size(); i += 2) {
          const Ast& op = *ns[i];
          TypedExpr rhs = emit_expr(*ns[i + 1], ctx, acc.type);
          acc = emit_binary(binop_of(op.token, op), acc, rhs, op);
        }
        return acc;
      }
      default:
        fail(a, "cannot evaluate " + a.name);
    }
  }

  // A user func by name, or the diagnostic for not finding one -- `$`
  // names are the runtime's own and not the program's to call.
  const FuncInfo& func_of(const Ast& at, const std::string& callee) {
    auto fit = funcs.find(callee);
    if (fit == funcs.end() || callee[0] == '$') fail(at, "undefined: " + callee);
    return fit->second;
  }

  // The arguments of a "call" node, each emitted with its parameter's type
  // as the hint and checked against it -- what a call expression and a
  // `go` statement both owe before they differ in what they do with the
  // callee.
  std::vector<NodeId> emit_call_args(const Ast& a, FuncCtx& ctx,
                                     const FuncInfo& info,
                                     const std::string& callee) {
    const Ast* argsNode = find_child(a, "args");
    std::vector<const Ast*> argAsts;
    if (argsNode != nullptr) {
      for (const auto& n : argsNode->nodes) argAsts.push_back(n.get());
    }
    if (argAsts.size() != info.param_types.size()) {
      fail(a, "wrong argument count calling " + callee);
    }
    std::vector<NodeId> args;
    args.reserve(argAsts.size());
    for (size_t i = 0; i < argAsts.size(); ++i) {
      const TypeRef& pt = info.param_types[i];
      TypedExpr v = emit_expr(*argAsts[i], ctx, hint_for(pt.type));
      if (!same_type(v.type, v.struct_id, pt.type, pt.struct_id)) {
        fail(*argAsts[i], "argument type mismatch calling " + callee);
      }
      args.push_back(v.node);
    }
    return args;
  }

  // The local an ident names, required to be a channel -- what both ends
  // of a channel operation need before anything else.
  const LocalInfo& channel_local(const Ast& ident, FuncCtx& ctx) {
    const LocalInfo& li = local_of(ident, ctx, std::string(ident.token));
    if (li.type != Type::Chan) {
      fail(ident, "'" + std::string(ident.token) + "' (" +
                      describe_type(li.type, li.struct_id) +
                      ") is not a channel");
    }
    return li;
  }

  // The statements of a "stmts" node, as one Block at `at`'s position.
  NodeId emit_block(const Ast& stmts, FuncCtx& ctx, const Ast& at) {
    std::vector<NodeId> out;
    for (const auto& s : stmts.nodes) out.push_back(emit_stmt(*s, ctx));
    Builder b(m);
    return b.block(out, pos_of(at));
  }

  // -- Statements -------------------------------------------------------
  NodeId emit_stmt(const Ast& a, FuncCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    switch (a.tag) {
      // go f(args): the arguments are evaluated now, into cells of this
      // frame (fresh ones -- CellFresh -- so a `go` inside a loop gives
      // each goroutine its own), and a wrapper func capturing the callee's
      // closure and those cells is spawned as a coroutine through the
      // scheduler: Enqueue(CoroCreate(wrapper)). The wrapper takes the one
      // argument the scheduler passes (nil) leniently and makes the real
      // call with the captured values; the callee itself is any declared
      // func, with or without a result (a goroutine's result goes
      // nowhere, as in Go).
      case "gostmt"_: {
        const Ast& call = *a.nodes[0];
        const std::string callee(find_child(call, "ident")->token);
        if (type_from_name(callee)) fail(a, "go needs a function call");
        const FuncInfo& info = func_of(call, callee);
        const std::vector<NodeId> args = emit_call_args(call, ctx, info, callee);

        std::vector<NodeId> stmts;
        std::vector<CaptureSrc> cmap{{VarKind::Cell, ctx.cell_for(callee)}};
        for (const NodeId arg : args) {
          const int32_t c = ctx.next_cell++;
          stmts.push_back(b.cell_fresh(c, p));
          stmts.push_back(b.assign(VarKind::Cell, c, arg, p));
          cmap.push_back({VarKind::Cell, c});
        }

        Func w;
        w.name = "go " + callee;
        w.num_captures = static_cast<int32_t>(cmap.size());
        w.capture_names.push_back(callee);
        w.lenient_arity = true;
        std::vector<NodeId> wargs;
        for (size_t i = 0; i < args.size(); ++i) {
          w.capture_names.push_back("arg" + std::to_string(i));
          wargs.push_back(
              b.varref(VarKind::Capture, static_cast<int32_t>(i + 1), p));
        }
        w.body = b.call_value(b.varref(VarKind::Capture, 0, p), wargs, p);
        const int32_t widx = static_cast<int32_t>(m.funcs.size());
        m.funcs.push_back(w);
        const int32_t cm = static_cast<int32_t>(m.capture_maps.size());
        m.capture_maps.push_back(cmap);

        stmts.push_back(b.intrinsic(
            IntrinsicId::Enqueue,
            {b.intrinsic(IntrinsicId::CoroCreate,
                         {b.make_closure(widx, cm, p)}, p)},
            p));
        return b.block(stmts, p);
      }
      // ch <- v, through $chan_send; the value takes the channel's element
      // type as its hint and must match it.
      case "sendstmt"_: {
        const LocalInfo& li = channel_local(*a.nodes[0], ctx);
        const Type elem = static_cast<Type>(li.struct_id);
        TypedExpr v = emit_expr(*a.nodes[1], ctx, elem);
        if (v.type != elem) {
          fail(a, "cannot send " + describe_type(v.type, v.struct_id) +
                      " on " + describe_type(li.type, li.struct_id));
        }
        const NodeId send = b.varref(VarKind::Cell, ctx.cell_for("$chan_send"), p);
        return b.call_value(send, {b.varref(VarKind::Local, li.slot, p), v.node},
                            p);
      }
      // <-ch as a statement: receive and discard -- the same call the
      // expression form makes, with nothing reading its value.
      case "recvstmt"_: {
        const LocalInfo& li = channel_local(*a.nodes[0], ctx);
        const NodeId recv = b.varref(VarKind::Cell, ctx.cell_for("$chan_recv"), p);
        return b.call_value(recv, {b.varref(VarKind::Local, li.slot, p)}, p);
      }
      // for cond { ... } -- Go's while. A `var` inside the body declares
      // into this function's one flat local table, the same as anywhere
      // else in this front end: it is bound once, and the slot is
      // reassigned on each iteration.
      case "forstmt"_: {
        TypedExpr cond = emit_expr(*a.nodes[0], ctx, std::nullopt);
        if (cond.type != Type::Bool) fail(a, "for condition must be bool");
        return b.make_while(cond.node, emit_block(*a.nodes[1], ctx, a), p);
      }
      case "ifstmt"_: {
        TypedExpr cond = emit_expr(*a.nodes[0], ctx, std::nullopt);
        if (cond.type != Type::Bool) fail(a, "if condition must be bool");
        const NodeId then_ = emit_block(*a.nodes[1], ctx, a);
        NodeId els;
        if (a.nodes.size() > 2) els = emit_block(*a.nodes[2], ctx, a);
        return b.make_if(cond.node, then_, els, p);
      }
      case "vardecl"_: {
        const std::string name(find_child(a, "ident")->token);
        if (ctx.locals.count(name)) {
          fail(a, "'" + name + "' is already declared");
        }
        const TypeRef ty =
            resolve_type(std::string(find_child(a, "type")->token), a);
        // The value is emitted before the slot exists, so `var x int32 = x`
        // still resolves the right-hand `x` against whatever was declared
        // before this statement (or fails "undeclared"), never against the
        // slot this statement is itself about to create.
        const NodeId value = emit_local_value(*a.nodes.back(), ctx, ty.type,
                                              ty.struct_id, a, p);
        const int32_t slot = ctx.next_local++;
        ctx.locals[name] = {slot, ty.type, ty.struct_id};
        ctx.local_names.push_back(name);
        return b.assign(VarKind::Local, slot, value, p);
      }
      // Either a plain `x = v` (2 children: ident, expr) or a field
      // `p.X = v` / `p.From.X = v` (that same 2, plus however many
      // '.ident' the grammar's `('.' ident)*` matched -- receiver, those
      // fields, then the expr) -- read positionally, since find_child
      // would not tell the receiver's ident from a field's. A chain walks
      // every field but the last with FieldGet (walk_fields -- the same
      // walk fieldaccess does, reading rather than writing), landing on
      // the struct the last field actually lives on, then FieldSets that
      // one -- so `p.From.X = v` writes through a FieldGet of `p`, not a
      // copy of `From`, which is what makes the write visible through `p`
      // afterward rather than only through a value read out of it.
      case "assign"_: {
        const size_t nfields = a.nodes.size() - 2;
        if (nfields == 0) {
          const LocalInfo& li =
              local_of(a, ctx, std::string(find_child(a, "ident")->token));
          return b.assign(VarKind::Local, li.slot,
                         emit_local_value(*a.nodes.back(), ctx, li.type,
                                          li.struct_id, a, p),
                         p);
        }
        const TypedExpr recv = walk_fields(a, ctx, nfields, p);
        const FieldDef& fd = field_of(a, recv, nfields);
        return b.field_set(recv.node, fd.slot, fd.name,
                           emit_field_value(*a.nodes.back(), ctx, fd, a, p), p);
      }
      case "ret"_: {
        if (!ctx.ret_type) fail(a, "this func does not return a value");
        TypedExpr val = emit_expr(*a.nodes[0], ctx, hint_for(ctx.ret_type->type));
        if (!same_type(val.type, val.struct_id, ctx.ret_type->type,
                       ctx.ret_type->struct_id)) {
          fail(a, "return type does not match the func's declared type");
        }
        return b.make_return(val.node, p);
      }
      case "print"_: {
        TypedExpr val = emit_expr(*a.nodes[0], ctx, std::nullopt);
        // Print has no struct rendering, and Go's own `{3 4}` is not
        // something a slot-indexed Object could reproduce without field
        // names at runtime -- so refuse it rather than print `<object>`
        // and quietly stop agreeing with `go run`, the only thing this
        // front end's samples are checked against. A channel prints as an
        // address in Go, which nothing could reproduce either.
        if (has_second_half(val.type)) {
          fail(a, "cannot print a " + describe_type(val.type, val.struct_id));
        }
        return b.intrinsic(IntrinsicId::Print, {val.node}, p);
      }
      // switch subject { case k1, k2: stmts ... [default: stmts] } --
      // README's Switch recipe, concretely: this front end has no string
      // type, so every key is Int, and Go's own case syntax already means
      // "no fallthrough", which is exactly coreir::Switch's own semantics,
      // needing no extra lowering to get there. A `case a, b:` shares one
      // compiled body NodeId across both keys (see the top-level README's
      // Switch section) rather than compiling the statements twice.
      case "switchstmt"_: {
        TypedExpr subj = emit_expr(*a.nodes[0], ctx, std::nullopt);
        if (subj.type != Type::I32 && subj.type != Type::I64 &&
            subj.type != Type::U32) {
          fail(a, "switch subject must be int32, int64 or uint32, not " +
                      describe_type(subj.type, subj.struct_id));
        }
        std::vector<std::pair<NodeId, NodeId>> arms;
        std::set<int64_t> seen_keys;
        NodeId default_body;
        for (size_t i = 1; i < a.nodes.size(); ++i) {
          const Ast& c = *a.nodes[i];
          // Both arm kinds carry their statements identically -- a "stmts"
          // child compiled into one Block at the arm's own position -- so
          // the body is built once here and the tag only decides where it
          // gets hung: on the default, or on every key in this arm's
          // intlist, all sharing this one NodeId (the top-level README's
          // `case a, b:` point).
          std::vector<NodeId> body_stmts;
          for (const auto& s : find_child(c, "stmts")->nodes) {
            body_stmts.push_back(emit_stmt(*s, ctx));
          }
          const NodeId body = b.block(body_stmts, pos_of(c));
          if (c.tag != "case"_) {  // "defaultcase"_
            default_body = body;
            continue;
          }
          for (const auto& numAst : find_child(c, "intlist")->nodes) {
            const int64_t key = parse_int(std::string(numAst->token), *numAst);
            if (!seen_keys.insert(key).second) {
              fail(*numAst, "duplicate case value " + std::to_string(key));
            }
            arms.emplace_back(b.literal(key, pos_of(*numAst)), body);
          }
        }
        return b.make_switch(subj.node, arms, default_body, p);
      }
      default:
        fail(a, "cannot execute " + a.name);
    }
  }

  Module build(const Ast& program) {
    register_structs(program);
    // funcs[0] is vm::run's entry point and, here, the bootstrap that
    // spawns main as the first goroutine (emit_bootstrap) -- reserved now
    // so that register_funcs numbers main and the rest from 1.
    m.funcs.push_back({});
    register_funcs(program);
    emit_channel_runtime();

    for (const auto& fnPtr : program.nodes) {
      if (fnPtr->tag != "func"_) continue;  // a structdecl, already handled
      const Ast& fn = *fnPtr;
      const std::string name(find_child(fn, "ident")->token);
      const FuncInfo& info = funcs.at(name);
      // By index, not reference: a `go` statement in the body appends its
      // wrapper func to m.funcs, which may move every Func in it.
      const size_t fidx = static_cast<size_t>(info.index);

      FuncCtx ctx;
      ctx.ret_type = info.has_ret ? std::optional<TypeRef>(info.ret)
                                  : std::nullopt;

      const Ast* paramsNode = find_child(fn, "params");
      for (const auto& pn : paramsNode->nodes) {
        const std::string pname(find_child(*pn, "ident")->token);
        // resolve_sig_type, as register_funcs used for the signature, so
        // what a param's local slot ends up typed as cannot differ from
        // what the callee's FuncInfo recorded.
        const TypeRef pty =
            resolve_sig_type(std::string(find_child(*pn, "type")->token), *pn);
        ctx.locals[pname] = {ctx.next_local, pty.type, pty.struct_id};
        ctx.local_names.push_back(pname);
        ++ctx.next_local;
      }

      // The body first: each call it emits claims a cell for its callee
      // (FuncCtx::cell_for), and each `go` claims cells for its arguments.
      const Ast* stmtsNode = find_child(fn, "stmts");
      std::vector<NodeId> stmts;
      for (const auto& s : stmtsNode->nodes) {
        stmts.push_back(emit_stmt(*s, ctx));
      }

      // Then the preamble ahead of it: one MakeClosure per distinct callee
      // into the cell the body reads -- README's Static calls recipe,
      // concretely -- and the channel runtime's own two funcs reached the
      // same way when the body used a channel.
      Builder b(m);
      std::vector<NodeId> body_stmts;
      for (const auto& [callee, cell] : ctx.call_cells) {
        const int32_t cmap = static_cast<int32_t>(m.capture_maps.size());
        m.capture_maps.push_back({});
        body_stmts.push_back(b.assign(
            VarKind::Cell, cell,
            b.make_closure(funcs.at(callee).index, cmap, pos_of(fn)),
            pos_of(fn)));
      }
      body_stmts.insert(body_stmts.end(), stmts.begin(), stmts.end());

      Func& f = m.funcs[fidx];
      f.num_cells = ctx.next_cell;
      f.num_locals = ctx.next_local;
      f.local_names = ctx.local_names;
      f.body = b.scope(0, f.num_locals, b.block(body_stmts, pos_of(fn)),
                       pos_of(fn));
    }
    emit_bootstrap();
    return std::move(m);
  }
};

}  // namespace

Module bind_source(const std::string& source) {
  parser p;
  p.set_logger([](size_t line, size_t col, const std::string& msg,
                  const std::string&) {
    coreir_rt::fail(msg, static_cast<uint32_t>(line),
                    static_cast<uint32_t>(col));
  });
  if (!p.load_grammar(kGrammar)) coreir_rt::fail("invalid grammar", 0, 0);
  p.enable_ast();

  std::shared_ptr<Ast> ast;
  if (!p.parse(source, ast)) coreir_rt::fail("syntax error", 0, 0);
  ast = p.optimize_ast(ast);

  Binder b;
  Module m = b.build(*ast);

  if (auto err = verify(m)) {
    coreir_rt::fail("internal error: malformed IR: " + *err, 0, 0);
  }
  return m;
}

}  // namespace go_mini
