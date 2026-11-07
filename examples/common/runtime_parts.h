// Runtime parts the front ends in examples/ share: what two or more of them
// had each written in IR -- the same rule, written the same way -- kept here
// once, so that the next front end does not write it again.
//
// Only what is *the same* is here. vmlib.h leaves a language's rules to the
// language (truthiness, how a value prints, what `==` asks of an object), and
// where the languages here really differ on one, each front end keeps its
// own; the note at the end of this file says, for each part left out, why.
// That is also why this is a header of the examples and not part of
// vmlib.h: a class layout or an iteration protocol is a decision, and the
// library deliberately makes none. (The table these helpers are reserved in
// and called through, coreir::HelperTable, decides nothing, and is
// vmlib.h's.)
//
// The parts are FuncWriter-level: a function that writes a helper's body
// into a writer the front end made, or a statement it adds to one. The
// front end still lists the helper's name and finishes it, so each keeps
// its own name and its own place in its runtime -- all but the promise's
// helpers, which call one another and so come with their names.

#pragma once

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vmlib.h"

namespace runtime_parts {

using coreir::BinOp;
using coreir::Fallback;
using coreir::FuncWriter;
using coreir::HelperTable;
using coreir::HelperWriter;
using coreir::IntrinsicId;
using coreir::NodeId;
using coreir::UnOp;

// ---- Hidden state ----------------------------------------------------------
//
// What the implementation keeps on an object a program holds -- an
// instance's class, the mark that makes an object a tuple or a range, a
// promise's state -- is the object's meta (vmlib.h's MetaGet/MetaSet), the
// slot no key reaches: a program can neither read, forge nor strip it by
// spelling a key, and nothing that lists, counts or compares keys has
// anything to step over. The head of a meta is always the object's type,
// and what follows it is the state the implementation keeps beside that,
// the way CPython's struct follows ob_type:
//
//   * an Object: a class's method table (Classes, below), and no state --
//     an instance of a program's class, its fields all its own;
//   * an Array, [type, state...], at positions the front end numbers: a
//     *kind record*, whose type is a kind's name (a string) for a kind of
//     the implementation's own -- a tuple, a Map, a promise -- or a class's
//     method table for an instance of a program's class that keeps state
//     beside its fields, which only an exception does here (its message).
//
// So a meta is never taken for what it is not: a class is an Object, and a
// kind record an Array. Only an Object carries a kind record. No program
// holds a meta, so a position in one needs no prefix to keep it apart
// from anything -- and nothing of the implementation's is under a key,
// so every key of an object is the program's own name. A namespace of the
// program's that would share an object with another gets an object of its
// own: a class's methods and its static fields are two (Classes, below).
// mini-csharp once kept a class's name and constructor beside its methods,
// and then a method named `name` overwrote the class's name and one named
// `ctor` was the constructor.

// `obj` with [type, state...] as its meta -- the object, answered, as
// MetaSet answers it; of_kind for a kind of the implementation's own.
inline NodeId with_meta(coreir::Builder::At& b, NodeId obj, NodeId type,
                        const std::vector<NodeId>& state) {
  std::vector<NodeId> rec{type};
  rec.insert(rec.end(), state.begin(), state.end());
  return b.intrinsic(IntrinsicId::MetaSet, {obj, b.array_lit(rec)});
}
inline NodeId of_kind(coreir::Builder::At& b, NodeId obj,
                      const std::string& kind,
                      const std::vector<NodeId>& state) {
  return with_meta(b, obj, b.str_literal(kind), state);
}
inline NodeId of_kind(FuncWriter& w, NodeId obj, const std::string& kind,
                      const std::vector<NodeId>& state) {
  return of_kind(w.b, obj, kind, state);
}

// Whether `v` carries a kind record of `kind`: any value can be asked, and
// one that is no Object, or whose meta is no kind record of a kind's name,
// is of none -- anything but an Object is answered by its first test,
// which on the paths that ask of every number (`$add`'s "is this a
// tuple") is the whole cost. The meta, and its head, are each read once,
// into the two locals `m` and `h` the caller lends.
inline NodeId is_kind(coreir::Builder::At& b, NodeId v, int32_t m, int32_t h,
                      const std::string& kind) {
  using coreir::VarKind;
  const auto is = [&](NodeId x, const char* type) {
    return b.binary(BinOp::Eq, b.intrinsic(IntrinsicId::TypeOf, {x}),
                    b.str_literal(type));
  };
  const auto L = [&](int32_t i) { return b.varref(VarKind::Local, i); };
  const NodeId no = b.bool_literal(false);
  return b.make_if(
      is(v, "object"),
      b.block(
          {b.assign(VarKind::Local, m, b.intrinsic(IntrinsicId::MetaGet, {v})),
           b.make_if(is(L(m), "array"),
                     b.block({b.assign(VarKind::Local, h,
                                       b.index(L(m), b.literal(0))),
                              b.make_if(is(L(h), "string"),
                                        b.binary(BinOp::Eq, L(h),
                                                 b.str_literal(kind)),
                                        b.bool_literal(false))}),
                     b.bool_literal(false))}),
      no);
}
inline NodeId is_kind(FuncWriter& w, FuncWriter::Slot v,
                      const std::string& kind) {
  return is_kind(w.b, w.L(v), w.local("$meta").i, w.local("$type").i, kind);
}

// Element `i` of the kind record `v` carries, read and written.
inline NodeId kind_field(coreir::Builder::At& b, NodeId v, int64_t i) {
  return b.index(b.intrinsic(IntrinsicId::MetaGet, {v}), b.literal(i));
}
inline NodeId kind_field(FuncWriter& w, NodeId v, int64_t i) {
  return kind_field(w.b, v, i);
}
inline NodeId set_kind_field(coreir::Builder::At& b, NodeId v, int64_t i,
                             NodeId x) {
  return b.set_index(b.intrinsic(IntrinsicId::MetaGet, {v}), b.literal(i), x);
}
inline NodeId set_kind_field(FuncWriter& w, NodeId v, int64_t i, NodeId x) {
  return set_kind_field(w.b, v, i, x);
}

// The same questions, asked in C++ by a host function. The kind record `v`
// carries when it is of `kind`, or null -- of an Object only, as is_kind.
inline const std::vector<coreir::Value>* kind_record(const coreir::Value& v,
                                                     std::string_view kind) {
  if (!v.is_object() || !v.as_object()->meta.is_array()) return nullptr;
  const std::vector<coreir::Value>& rec = v.as_object()->meta.as_array()->items;
  if (rec.empty() || !rec[0].is_str() || rec[0].as_str() != kind) {
    return nullptr;
  }
  return &rec;
}

// ---- Classes ---------------------------------------------------------------
//
// There is no class in the IR, and none was needed. A class is an object --
// its table -- holding its methods by name, with its name and its base's
// table in its kind record; an instance is an object of the program's
// fields whose meta is its class:
//
//   table:    { m: <m>, ... }       ["class", "C", <the base's table>, ...]
//   instance: { <fields> }          meta: <table>
//
// So the meta of an instance is an Object and every kind record is an
// Array: nothing is taken for a class that is not one. A front end adds
// what else its classes need to the record, from kClassOwn on -- a
// constructor, an identity to compare -- and every question a class system
// asks is a walk up kClassBase: which method a name means, whether a value
// is an instance of a class.
inline constexpr char kClassKind[] = "class";
inline constexpr int64_t kClassName = 1;
inline constexpr int64_t kClassBase = 2;
inline constexpr int64_t kClassOwn = 3;

// A class table: `methods` (an ObjectLit) with its kind record.
inline NodeId class_table(coreir::Builder::At& b, NodeId methods,
                          const std::string& name, NodeId base,
                          const std::vector<NodeId>& own = {}) {
  std::vector<NodeId> state{b.str_literal(name), base};
  state.insert(state.end(), own.begin(), own.end());
  return of_kind(b, methods, kClassKind, state);
}

// A fresh instance of the class `cls`, with no fields yet.
inline NodeId new_instance(coreir::Builder::At& b, NodeId cls) {
  return b.intrinsic(IntrinsicId::MetaSet, {b.object_lit({}), cls});
}

// The class of `v`: the method table at the head of its meta when it is
// an instance -- the meta itself, or the type of a kind record that keeps
// state beside its fields -- and nil for anything else. Only an Object
// is an instance, as only an Object has a kind record (is_kind), so any
// other value -- a receiver of a builtin type, the common case in a
// method call -- is answered by its one type test. The meta is read once,
// into a local of the writer's.
inline NodeId instance_class(FuncWriter& w, FuncWriter::Slot v) {
  const auto m = w.local("$meta");
  return w.iff(
      w.is(w.typ(v), "object"),
      w.blk({w.set(m, w.meta(w.L(v))),
             w.iff(w.is(w.typ(m), "object"), w.L(m),
                   w.iff(w.both(w.is(w.typ(m), "array"),
                                w.is(w.typ(w.idx(w.L(m), w.I(0))), "object")),
                         w.idx(w.L(m), w.I(0)), w.Nil()))}),
      w.Nil());
}

// A class's name, from its table.
inline NodeId class_name(FuncWriter& w, NodeId table) {
  return kind_field(w, table, kClassName);
}

// The same, asked in C++ by a host function: the class of `v`, or null
// when `v` is no instance; a class's name, from its table, or null when
// `table` is none; and the name of `v`'s class.
inline const coreir::Value* instance_class(const coreir::Value& v) {
  if (!v.is_object()) return nullptr;
  const coreir::Value* m = &v.as_object()->meta;
  if (m->is_object()) return m;
  if (m->is_array() && !m->as_array()->items.empty() &&
      m->as_array()->items[0].is_object()) {
    return &m->as_array()->items[0];
  }
  return nullptr;
}
inline const std::string* class_name(const coreir::Value& table) {
  const auto* rec = kind_record(table, kClassKind);
  constexpr auto at = static_cast<size_t>(kClassName);
  if (!rec || rec->size() <= at || !(*rec)[at].is_str()) return nullptr;
  return &(*rec)[at].as_str();
}
inline const std::string* instance_class_name(const coreir::Value& v) {
  const coreir::Value* cls = instance_class(v);
  return cls ? class_name(*cls) : nullptr;
}

// The walk, as the While it is: from the table in `c`, each table up the
// chain in turn, nearest first, until one has no base -- whose kClassBase
// is nil, which ends the loop. `visit` runs at each and returns from the
// helper when it finds what it is after; a walk that falls off the end
// found nothing.
inline NodeId walk_bases(FuncWriter& w, FuncWriter::Slot c, NodeId visit) {
  return w.wh(w.is(w.typ(c), "object"),
              w.blk({visit, w.set(c, kind_field(w, w.L(c), kClassBase))}));
}

// Method lookup: what `key` names in the table in `c` or in its nearest
// ancestor that has it, returned from the helper -- one has() per table
// walked -- or nothing, when the walk falls off the end. As a helper of
// its own, `(t, key)` answering nil for a miss, or a statement in one
// that does something else on a miss.
inline NodeId find_in_chain(FuncWriter& w, FuncWriter::Slot c,
                            FuncWriter::Slot key) {
  return walk_bases(w, c,
                    w.iff(w.has(w.L(c), w.L(key)),
                          w.ret(w.idx(w.L(c), w.L(key)))));
}
inline void find_in_chain(FuncWriter& w) {
  const auto [t, key] = w.params("t", "key");
  w.add(find_in_chain(w, t, key));
  w.add(w.ret(w.Nil()));
}

// ---- Exceptions ------------------------------------------------------------
//
// An exception the implementation raises or a program builds is an object
// whose meta is [type, name, message]: the type kExcKind for a builtin
// one, or -- in a language whose programs derive their own -- the class
// of one of the program's, which keeps its message beside its fields (the
// one instance here with state of its own; see Hidden state). The name is
// the one a report gives it, its class's for one of the program's; the
// message is nil until one is given. What a report says, and in which
// shape, is each language's (RunOptions::describe).
inline constexpr char kExcKind[] = "exc";
inline constexpr int64_t kExcName = 1;
inline constexpr int64_t kExcMessage = 2;

// A fresh exception of `type` (a kind's name node, or a class's table).
inline NodeId exception(coreir::Builder::At& b, NodeId type, NodeId name,
                        NodeId message) {
  return with_meta(b, b.object_lit({}), type, {name, message});
}
inline NodeId exception(FuncWriter& w, NodeId type, NodeId name,
                        NodeId message) {
  return exception(w.b, type, name, message);
}

// Whether `v` is an exception: a builtin one, or an instance of a
// program's class that keeps a message. The meta and its head are each
// read once, into locals of the writer's.
inline NodeId is_exception(FuncWriter& w, FuncWriter::Slot v) {
  const auto m = w.local("$meta");
  const auto h = w.local("$type");
  return w.both(
      w.is(w.typ(v), "object"),
      w.blk({w.set(m, w.meta(w.L(v))),
             w.both(w.is(w.typ(m), "array"),
                    w.blk({w.set(h, w.idx(w.L(m), w.I(0))),
                           w.iff(w.is(w.typ(h), "string"),
                                 w.bin(BinOp::Eq, w.L(h), w.S(kExcKind)),
                                 w.is(w.typ(h), "object"))}))}));
}

// The same, asked in C++ by a host function -- a report's: the exception's
// [type, name, message], or null when `v` is none.
inline const std::vector<coreir::Value>* exception_record(
    const coreir::Value& v) {
  if (const auto* rec = kind_record(v, kExcKind)) return rec;
  if (!v.is_object() || !v.as_object()->meta.is_array()) return nullptr;
  const std::vector<coreir::Value>& rec = v.as_object()->meta.as_array()->items;
  return rec.size() > static_cast<size_t>(kExcMessage) && rec[0].is_object()
             ? &rec
             : nullptr;
}

// ---- Iteration -------------------------------------------------------------
//
// A for-each loop drives a cursor: the front end's `$iter(v)` builds one --
// what is iterable, and how, is its language's -- and `$iternext(it)` steps
// it, answering what GenResume answers for a generator, {value, done}. So a
// generator is stepped by GenResume itself, and every other cursor answers
// in the same shape, and the loop that reads `done` and then `value` is one
// loop whatever it walks:
//
//   {k: "g", v: <generator>}               generator_cursor, by GenResume
//   {k: "a", v: <array or string>, i: 0}   sequence_cursor, by index
//
// A front end with a kind of its own -- mini-culebra's custom iterator
// protocol -- builds it with cursor() and steps it itself, ahead of
// iter_step.

inline NodeId cursor(FuncWriter& w, const std::string& kind, NodeId v) {
  return w.obj({{"k", w.S(kind)}, {"v", v}});
}
inline NodeId generator_cursor(FuncWriter& w, NodeId gen) {
  return cursor(w, "g", gen);
}
inline NodeId sequence_cursor(FuncWriter& w, NodeId seq) {
  return w.obj({{"k", w.S("a")}, {"v", seq}, {"i", w.I(0)}});
}

// Whether the cursor in `it` is of `kind`, and what it walks.
inline NodeId cursor_is(FuncWriter& w, FuncWriter::Slot it,
                        const std::string& kind) {
  return w.is(w.idx(w.L(it), "k"), kind);
}
inline NodeId cursor_source(FuncWriter& w, FuncWriter::Slot it) {
  return w.idx(w.L(it), "v");
}

// One step's answer, in GenResume's shape: the end, or the next value.
inline NodeId step_done(FuncWriter& w) {
  return w.obj({{"value", w.Nil()}, {"done", w.Bo(true)}});
}
inline NodeId step(FuncWriter& w, NodeId value) {
  return w.obj({{"value", value}, {"done", w.Bo(false)}});
}

// The next step of the cursor in `it`, the parameter the front end declared
// for its `$iternext(it)` -- after any statements of its own, for cursor
// kinds of its own.
inline void iter_step(FuncWriter& w, FuncWriter::Slot it) {
  const auto [a, i] = w.locals("a", "i");
  w.add(w.iff(cursor_is(w, it, "g"),
              w.ret(w.in(IntrinsicId::GenResume,
                         {cursor_source(w, it), w.Nil()}))));
  w.add(w.set(a, cursor_source(w, it)));
  w.add(w.set(i, w.idx(w.L(it), "i")));
  w.add(w.iff(w.bin(BinOp::Ge, w.L(i), w.len(w.L(a))), w.ret(step_done(w))));
  w.add(w.sidx(w.L(it), "i", w.bin(BinOp::Add, w.L(i), w.I(1))));
  w.add(w.ret(step(w, w.idx(w.L(a), w.L(i)))));
}

// ---- A for-each loop -------------------------------------------------------
//
// The loop a for-each lowers to, whether a binder's emit pass writes it or
// a helper does: the cursor in `it`, each step in `st`, until a step is
// done --
//
//   it = cursor
//   while (true) { st = $iternext(it); if (st.done) break; body... }
//
// `it` and `st` are local slots the caller allocated, `cursor` is what its
// `$iter` built (or a cursor it already holds), and `body` is what one
// element runs, written over value_of(st). The step is the front end's
// helper named "$iternext". A caller builds `body` before calling, in
// whatever order its own slots and scopes need.
inline NodeId done_of(coreir::Builder::At& b, int32_t st) {
  return b.index(b.varref(coreir::VarKind::Local, st), b.str_literal("done"));
}
inline NodeId value_of(coreir::Builder::At& b, int32_t st) {
  return b.index(b.varref(coreir::VarKind::Local, st), b.str_literal("value"));
}
namespace detail {
// The loop's two statements: the cursor stored, and the while.
inline std::pair<NodeId, NodeId> for_each(HelperTable& rt, coreir::SrcPos p,
                                          int32_t it, int32_t st,
                                          NodeId cursor,
                                          const std::vector<NodeId>& body) {
  using coreir::VarKind;
  auto b = coreir::Builder(rt.module()).at(p);
  std::vector<NodeId> loop{
      b.assign(VarKind::Local, st,
               rt.call("$iternext", {b.varref(VarKind::Local, it)}, p)),
      b.make_if(done_of(b, st), b.make_break(), NodeId{})};
  loop.insert(loop.end(), body.begin(), body.end());
  return {b.assign(VarKind::Local, it, cursor),
          b.make_while(b.bool_literal(true), b.block(loop))};
}
}  // namespace detail

inline NodeId for_each(HelperTable& rt, coreir::SrcPos p, int32_t it,
                       int32_t st, NodeId cursor,
                       const std::vector<NodeId>& body) {
  const auto [start, loop] = detail::for_each(rt, p, it, st, cursor, body);
  return coreir::Builder(rt.module()).at(p).block({start, loop});
}

// The same, in a helper's own slots, added to its body as two statements
// rather than one block around them, as a helper's loop always stands.
inline NodeId done_of(FuncWriter& w, FuncWriter::Slot st) {
  return done_of(w.b, st.i);
}
inline NodeId value_of(FuncWriter& w, FuncWriter::Slot st) {
  return value_of(w.b, st.i);
}
inline void add_for_each(HelperWriter& r, FuncWriter::Slot it,
                         FuncWriter::Slot st, NodeId cursor,
                         const std::vector<NodeId>& body) {
  const auto [start, loop] =
      detail::for_each(r.table, r.pos, it.i, st.i, cursor, body);
  r.add(start);
  r.add(loop);
}

// ---- Operators -------------------------------------------------------------
//
// A dynamic language's `+`, `==` and truthiness are the VM's own operation
// on some values and the language's own runtime helper on the rest, which
// is what an operation with a fallback says (vmlib.h's Tag::Unary): the
// VM's answer when every operand's tag is in `tags` -- and, `exact`, an
// int result did not wrap -- and the helper's, called with the operands,
// otherwise. Which tags is each language's to say, beside the helper they
// stand in for: where the two give the same answer. The helper is the one
// the front end called before, so no rule changes, and the call is made
// only where it is needed.

// The sets on which languages here draw the same line.
//
// Numbers: two of them add, subtract, multiply and compare as the VM's
// operators do, an int and a double meeting as numbers.
inline uint32_t number_tags() { return coreir::tags_named({"int", "double"}); }
// `+`: two numbers add and two strings concatenate, which is Add -- in
// JavaScript, C#, Ruby and Python; a string beside anything else is each
// language's own.
inline uint32_t add_tags() {
  return coreir::tags_named({"int", "double", "string"});
}
// `==`: two values of these types compare as Eq compares them, an int
// and a double as numbers; two types Eq refuses are unequal, which is the
// helper's to say -- and `!=` is Not of `==`. Lua, Ruby, C# and culebra.
inline uint32_t equality_tags() {
  return coreir::tags_named({"nil", "bool", "int", "double", "string"});
}

// The VM's operation that stands in for a helper: which one -- a UnOp, a
// BinOp, or Index -- the tags it answers on, and whether exactly.
struct VmOp {
  enum class Kind : uint8_t { Unary, Binary, Index };
  Kind kind;
  uint8_t op;  // the UnOp or the BinOp; nothing for Index
  uint32_t tags;
  bool exact;

  static VmOp unary(UnOp op, uint32_t tags, bool exact = false) {
    return {Kind::Unary, static_cast<uint8_t>(op), tags, exact};
  }
  static VmOp binary(BinOp op, uint32_t tags, bool exact = false) {
    return {Kind::Binary, static_cast<uint8_t>(op), tags, exact};
  }
  static VmOp index(uint32_t tags) { return {Kind::Index, 0, tags, false}; }
};

// A front end's operations by the helper each stands in for: one table,
// written once, so the line between the VM's answer and the helper's is
// drawn once per helper rather than at each place a source operator maps
// to it. A helper the table does not name is called; one it does is its
// operation, with the helper as the fallback. Every name is checked
// against the helpers reserved at the first call, so a misspelled one
// fails the bind rather than quietly losing its operation.
class Operators {
 public:
  using Table = std::map<std::string, VmOp>;

  Operators(HelperTable& rt, Table table)
      : rt_(rt), table_(std::move(table)) {}

  NodeId call(const std::string& helper, const std::vector<NodeId>& args,
              coreir::SrcPos p) {
    if (!checked_) {
      for (const auto& entry : table_) rt_.func(entry.first);
      checked_ = true;
    }
    const auto it = table_.find(helper);
    if (it == table_.end()) return rt_.call(helper, args, p);
    const VmOp& o = it->second;
    const size_t operands = o.kind == VmOp::Kind::Unary ? 1 : 2;
    if (args.size() != operands) {
      coreir_rt::fail("Operators: " + helper + " takes " +
                          std::to_string(operands) + " operands",
                      0, 0);
    }
    const Fallback f{rt_.ref(helper, p), o.tags, o.exact};
    auto b = coreir::Builder(rt_.module()).at(p);
    switch (o.kind) {
      case VmOp::Kind::Unary:
        return b.unary(static_cast<UnOp>(o.op), args[0], f);
      case VmOp::Kind::Binary:
        return b.binary(static_cast<BinOp>(o.op), args[0], args[1], f);
      case VmOp::Kind::Index:
        return b.index(args[0], args[1], f);
    }
    return NodeId{};
  }

 private:
  HelperTable& rt_;
  Table table_;
  bool checked_ = false;
};

// ---- Truthiness ------------------------------------------------------------
//
// `(v)`: false for nil and for false, true for everything else -- 0, "",
// an empty container all included. Lua's rule and Ruby's (and Elixir's and
// Clojure's), which Value::truthy() does not follow: it calls 0 false. The
// one truthiness written the same way twice here; JavaScript's and
// Python's differ from it and from each other (see below).
inline void truthy_nil_false(FuncWriter& w) {
  const auto [v] = w.params("v");
  w.add(w.iff(w.is(w.typ(v), "nil"), w.ret(w.Bo(false))));
  w.add(w.iff(w.is(w.typ(v), "bool"), w.ret(w.L(v))));
  w.add(w.ret(w.Bo(true)));
}

// The tags on which Value::truthy() already is a language's truthiness:
// every one but those `differ` names, the ones the language decides for
// itself. A truth test (UnOp::Truth with the language's helper behind it)
// takes the VM's answer on these and calls the helper only for the rest --
// so a condition on a table, a function or a list where the language,
// like the VM, calls every one true asks no helper.
inline uint32_t truthy_tags_except(
    std::initializer_list<std::string_view> differ) {
  return coreir::kAllTags & ~coreir::tags_named(differ);
}

// Where nil-and-false truthiness is Value::truthy()'s: everywhere but a
// number, whose 0 the VM calls false.
inline uint32_t nil_false_tags() {
  return truthy_tags_except({"int", "double"});
}

// ---- How a float prints ----------------------------------------------------
//
// `(d)`: a double as shortest round trip -- ToStr, which is to_display --
// with ".0" forced onto a whole one, so that 4.0 prints "4.0" and not "4".
// to_display's comment calls that a language's decision, and Python, Ruby,
// Guile and culebra all make it the same way. -0.0 is whole, and its
// integer has lost the sign, so it is spelled out before the whole-number
// path. How the three doubles that are no number print is each language's
// own -- "nan", "NaN", "+nan.0"; "inf", "Infinity", "+inf.0" -- and they
// are tested after the whole numbers, which none of them is.
struct NonFinite {
  std::string nan, inf, neg_inf;
};
inline void float_str(FuncWriter& w, const NonFinite& spell) {
  const double lim = 9007199254740992.0;  // 2^53: every whole double below
                                          // it has an int64 of its own
  const double inf = std::numeric_limits<double>::infinity();
  const auto [d] = w.params("d");
  const auto [i] = w.locals("i");
  w.add(w.iff(w.in(IntrinsicId::Same, {w.L(d), w.D(-0.0)}),
              w.ret(w.S("-0.0"))));
  w.add(w.iff(
      w.bin(BinOp::Gt, w.L(d), w.D(-lim)),
      w.iff(w.bin(BinOp::Lt, w.L(d), w.D(lim)),
            w.blk({w.set(i, w.in(IntrinsicId::ToInt, {w.L(d)})),
                   w.iff(w.bin(BinOp::Eq,
                               w.in(IntrinsicId::ToDouble, {w.L(i)}), w.L(d)),
                         w.ret(w.bin(BinOp::Add,
                                     w.in(IntrinsicId::ToStr, {w.L(i)}),
                                     w.S(".0"))))}))));
  w.add(w.iff(w.bin(BinOp::Ne, w.L(d), w.L(d)), w.ret(w.S(spell.nan))));
  w.add(w.iff(w.bin(BinOp::Eq, w.L(d), w.D(inf)), w.ret(w.S(spell.inf))));
  w.add(w.iff(w.bin(BinOp::Eq, w.L(d), w.D(-inf)),
              w.ret(w.S(spell.neg_inf))));
  w.add(w.ret(w.in(IntrinsicId::ToStr, {w.L(d)})));
}

// ---- Equality, of containers -----------------------------------------------
//
// Ruby, Python and culebra compare containers by what they hold: two
// arrays are equal when they are as long and equal element by element, two
// keyed containers when they hold the same keys with equal values -- equal
// by the language's own `==`, the helper `eq` names, which is how nested
// containers compare. What each `$eq` asks first, of numbers and of
// objects, and in which order, is its own (see below); these are the two
// loops all three wrote, as the block each case returns from. `a` and `b`
// are the operands, already known to be the same kind; the rest are locals
// the loop may use.
inline NodeId equal_elements(HelperWriter& r, FuncWriter::Slot a,
                             FuncWriter::Slot b, FuncWriter::Slot i,
                             const std::string& eq) {
  return r.blk(
      {r.iff(r.bin(BinOp::Ne, r.len(r.L(a)), r.len(r.L(b))),
             r.ret(r.Bo(false))),
       r.set(i, r.I(0)),
       r.wh(r.bin(BinOp::Lt, r.L(i), r.len(r.L(a))),
            r.blk({r.iff(r.bin(BinOp::Eq,
                               r.call(eq, {r.idx(r.L(a), r.L(i)),
                                           r.idx(r.L(b), r.L(i))}),
                               r.Bo(false)),
                         r.ret(r.Bo(false))),
                   r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})),
       r.ret(r.Bo(true))});
}
inline NodeId equal_entries(HelperWriter& r, FuncWriter::Slot a,
                            FuncWriter::Slot b, FuncWriter::Slot i,
                            FuncWriter::Slot ks, FuncWriter::Slot k,
                            const std::string& eq) {
  return r.blk(
      {r.set(ks, r.in(IntrinsicId::ObjectKeys, {r.L(a)})),
       r.iff(r.bin(BinOp::Ne, r.len(r.L(ks)), r.len(r.L(b))),
             r.ret(r.Bo(false))),
       r.set(i, r.I(0)),
       r.wh(r.bin(BinOp::Lt, r.L(i), r.len(r.L(ks))),
            r.blk({r.set(k, r.idx(r.L(ks), r.L(i))),
                   r.iff(r.bin(BinOp::Eq, r.has(r.L(b), r.L(k)), r.Bo(false)),
                         r.ret(r.Bo(false))),
                   r.iff(r.bin(BinOp::Eq,
                               r.call(eq, {r.idx(r.L(a), r.L(k)),
                                           r.idx(r.L(b), r.L(k))}),
                               r.Bo(false)),
                         r.ret(r.Bo(false))),
                   r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})),
       r.ret(r.Bo(true))});
}

// ---- Promises --------------------------------------------------------------
//
// A promise -- JavaScript's Promise, C#'s Task -- settled once and waited
// on, over the scheduler's primitives (CoroCurrent, CoroYield, Enqueue).
// mini-js and mini-csharp wrote the same one, and found that the dynamic
// language and the static one wanted the same thing from the scheduler:
//
//   {}, with the kind record
//   [kPromiseKind, kPending | kFulfilled | kRejected  (kPromiseState),
//                  the value, or the reason           (kPromiseValue),
//                  [what settling it enqueues]        (kPromiseWaiting)]
//
// A promise is a value the program holds, and none of its state is a
// property of it -- so a program that writes properties on one, or
// builds an object, reaches none of it and forges none.
//
// The helpers call one another, so their names are fixed: a front end
// reserves promise_helpers() beside its own, writes them once with
// write_promise_helpers, and calls them from its emit pass --
//
//   $pnew()             a pending promise
//   $psettle(p, st, v)  settles p, once -- a second settle is ignored --
//                       and enqueues what was waiting on it
//   $pon(p, cb)         enqueues cb when p settles, now if it has
//   $topromise(v)       v if it is a promise, else one fulfilled with v
//   $presolve(q, v)     resolves q with v: adopts v's state if v is a
//                       promise (so one returned from an async function
//                       flattens), else fulfils q with it
//   $await(v)           parks the running coroutine until v, as a promise,
//                       settles: its value, or its reason thrown
//
// -- and $adopt, the reaction $presolve hangs on a promise it adopts. What a
// language builds on them (JavaScript's `then` and `new Promise`, C#'s
// `.Result`) is its own.
inline constexpr char kPromiseKind[] = "promise";
inline constexpr int64_t kPromiseState = 1;
inline constexpr int64_t kPromiseValue = 2;
inline constexpr int64_t kPromiseWaiting = 3;
inline constexpr int64_t kPending = 0;
inline constexpr int64_t kFulfilled = 1;
inline constexpr int64_t kRejected = 2;

// `then`, when `v` holds a promise.
inline NodeId if_promise(FuncWriter& w, FuncWriter::Slot v, NodeId then) {
  return w.iff(is_kind(w, v, kPromiseKind), then);
}
inline NodeId promise_state(FuncWriter& w, NodeId p) {
  return kind_field(w, p, kPromiseState);
}
inline NodeId promise_value(FuncWriter& w, NodeId p) {
  return kind_field(w, p, kPromiseValue);
}

inline const std::vector<std::string>& promise_helpers() {
  static const std::vector<std::string> names = {
      "$pnew", "$psettle", "$pon", "$topromise", "$presolve", "$adopt",
      "$await"};
  return names;
}

inline void write_promise_helpers(HelperTable& rt) {
  {
    HelperWriter r(rt);
    r.add(r.ret(of_kind(r, r.obj({}), kPromiseKind,
                        {r.I(kPending), r.Nil(), r.arr({})})));
    r.finish("$pnew");
  }
  {
    HelperWriter r(rt);
    const auto [p, st, v] = r.params("p", "st", "v");
    const auto [waiting, i] = r.locals("waiting", "i");
    r.add(r.iff(r.bin(BinOp::Ne, promise_state(r, r.L(p)), r.I(kPending)),
                r.ret(r.Nil())));
    r.add(set_kind_field(r, r.L(p), kPromiseState, r.L(st)));
    r.add(set_kind_field(r, r.L(p), kPromiseValue, r.L(v)));
    r.add(r.set(waiting, kind_field(r, r.L(p), kPromiseWaiting)));
    r.add(set_kind_field(r, r.L(p), kPromiseWaiting, r.arr({})));
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(i), r.len(r.L(waiting))),
               r.blk({r.in(IntrinsicId::Enqueue, {r.idx(r.L(waiting), r.L(i))}),
                      r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.Nil()));
    r.finish("$psettle");
  }
  {
    HelperWriter r(rt);
    const auto [p, cb] = r.params("p", "cb");
    r.add(r.iff(r.bin(BinOp::Ne, promise_state(r, r.L(p)), r.I(kPending)),
                r.blk({r.in(IntrinsicId::Enqueue, {r.L(cb)}), r.ret(r.Nil())})));
    r.add(r.push(kind_field(r, r.L(p), kPromiseWaiting), r.L(cb)));
    r.add(r.ret(r.Nil()));
    r.finish("$pon");
  }
  {
    HelperWriter r(rt);
    const auto [v] = r.params("v");
    const auto [p] = r.locals("p");
    r.add(if_promise(r, v, r.ret(r.L(v))));
    r.add(r.set(p, r.call("$pnew", {})));
    r.add(r.call("$psettle", {r.L(p), r.I(kFulfilled), r.L(v)}));
    r.add(r.ret(r.L(p)));
    r.finish("$topromise");
  }
  {
    // cells: 0 = q, 1 = v, for $adopt to close over.
    HelperWriter r(rt);
    const auto [q, v] = r.params("q", "v");
    r.add(if_promise(
        r, v,
        r.blk({r.setc(0, r.L(q)), r.setc(1, r.L(v)),
               r.call("$pon", {r.L(v), r.clos("$adopt", {0, 1})}),
               r.ret(r.Nil())})));
    r.add(r.call("$psettle", {r.L(q), r.I(kFulfilled), r.L(v)}));
    r.add(r.ret(r.Nil()));
    r.finish("$presolve", 2);
  }
  {
    // captures: 0 = the promise to settle, 1 = the one it adopts.
    HelperWriter r(rt);
    r.add(r.call("$psettle", {r.P(0), promise_state(r, r.P(1)),
                              promise_value(r, r.P(1))}));
    r.finish("$adopt", 0, 2);
  }
  {
    HelperWriter r(rt);
    const auto [v] = r.params("v");
    const auto [p] = r.locals("p");
    r.add(r.set(p, r.call("$topromise", {r.L(v)})));
    r.add(r.call("$pon", {r.L(p), r.in(IntrinsicId::CoroCurrent, {})}));
    r.add(r.in(IntrinsicId::CoroYield, {r.Nil()}));
    r.add(r.iff(r.bin(BinOp::Eq, promise_state(r, r.L(p)), r.I(kRejected)),
                r.b.make_throw(promise_value(r, r.L(p)))));
    r.add(r.ret(promise_value(r, r.L(p))));
    r.finish("$await");
  }
}

// An async function, lowered over the promise above. Calling one has to
// return its promise before its body has finished, so it is three funcs:
//
//   f       what the source declared. Builds the promise, packages the
//           arguments and the body closure into cells, starts the body's
//           coroutine, and answers the promise.
//   f$body  the body as the front end built it, with f's own captures.
//   f$run   the coroutine's entry: calls f$body and settles the promise
//           with what it returned ($presolve, which adopts a promise it is
//           handed) or with what it threw.
//
// The coroutine is started with CoroResume, not Enqueue: JavaScript's and
// C#'s async bodies alike run synchronously up to their first `await`, so
// the call returns only once the body has suspended or finished, and from
// then on whoever settles what it awaits enqueues it. The arguments are
// copied into fresh cells at the call, so two calls in flight do not share
// them.
//
// `body` is the function the front end built for the source's declaration
// -- its name, parameters, cells and captures already set -- and `params`
// names its parameters, one per num_params. A rest parameter
// (body.rest_param) arrives as the array and is spread back out to f$body,
// whose own rest parameter collects it afresh. `index` is the
// Module::funcs slot f takes; the other two are added.
inline void write_async(HelperTable& rt, int32_t index, coreir::Func body,
                        std::vector<std::string> params, coreir::SrcPos p) {
  using coreir::CaptureSrc;
  using coreir::Func;
  using coreir::VarKind;
  coreir::Module& m = rt.module();
  auto b = coreir::Builder(m).at(p);
  const std::string name = body.name;
  const int32_t nparams = body.num_params;
  const int32_t ncaps = body.num_captures;
  const bool rest = body.rest_param;
  const std::vector<std::string> capnames = body.capture_names;
  body.name = name + "$body";
  const int32_t body_idx = static_cast<int32_t>(m.funcs.size());
  m.funcs.push_back(std::move(body));
  const int32_t run_idx = static_cast<int32_t>(m.funcs.size());
  m.funcs.push_back({});

  // f: cells 0 = the promise, 1 = the body closure, 2.. = the arguments.
  std::vector<NodeId> launch;
  launch.push_back(b.cell_fresh(0));
  launch.push_back(b.assign(VarKind::Cell, 0, rt.call("$pnew", {}, p)));
  std::vector<CaptureSrc> fwd;
  fwd.reserve(static_cast<size_t>(ncaps));
  for (int32_t i = 0; i < ncaps; ++i) fwd.push_back({VarKind::Capture, i});
  const int32_t fwd_cmap = static_cast<int32_t>(m.capture_maps.size());
  m.capture_maps.push_back(fwd);
  launch.push_back(b.cell_fresh(1));
  launch.push_back(
      b.assign(VarKind::Cell, 1, b.make_closure(body_idx, fwd_cmap)));
  std::vector<CaptureSrc> runcap{{VarKind::Cell, 1}, {VarKind::Cell, 0}};
  for (int32_t i = 0; i < nparams; ++i) {
    launch.push_back(b.cell_fresh(2 + i));
    launch.push_back(
        b.assign(VarKind::Cell, 2 + i, b.varref(VarKind::Local, i)));
    runcap.push_back({VarKind::Cell, 2 + i});
  }
  const int32_t run_cmap = static_cast<int32_t>(m.capture_maps.size());
  m.capture_maps.push_back(runcap);
  launch.push_back(
      b.intrinsic(IntrinsicId::CoroResume,
                  {b.intrinsic(IntrinsicId::CoroCreate,
                               {b.make_closure(run_idx, run_cmap)}),
                   b.nil_literal()}));
  launch.push_back(b.make_return(b.varref(VarKind::Cell, 0)));

  Func f;
  f.name = name;
  f.num_params = nparams;
  f.num_locals = nparams;
  f.local_names = params;
  f.num_cells = 2 + nparams;
  f.num_captures = ncaps;
  f.capture_names = capnames;
  f.lenient_arity = true;
  f.rest_param = rest;
  f.body = b.scope(0, nparams, b.block(launch));
  m.funcs[static_cast<size_t>(index)] = std::move(f);

  // f$run: captures 0 = the body closure, 1 = the promise, 2.. = the
  // arguments; local 0 = what the body threw.
  std::vector<NodeId> callargs;
  callargs.reserve(static_cast<size_t>(nparams));
  for (int32_t i = 0; i < nparams; ++i) {
    callargs.push_back(b.varref(VarKind::Capture, 2 + i));
  }
  NodeId call_body;
  if (rest) {
    const NodeId spread = callargs.back();
    callargs.pop_back();
    call_body = b.call_spread(b.varref(VarKind::Capture, 0), callargs, spread);
  } else {
    call_body = b.call_value(b.varref(VarKind::Capture, 0), callargs);
  }
  Func run;
  run.name = name + "$run";
  run.num_params = 0;
  run.num_locals = 1;
  run.local_names = {"$exc"};
  run.num_captures = 2 + nparams;
  run.capture_names = {"$body", "$promise"};
  run.capture_names.insert(run.capture_names.end(), params.begin(),
                           params.end());
  run.lenient_arity = true;  // the scheduler resumes it as f(nil)
  run.body = b.scope(
      0, 1,
      b.make_try(0,
                 rt.call("$presolve", {b.varref(VarKind::Capture, 1),
                                       call_body},
                         p),
                 rt.call("$psettle", {b.varref(VarKind::Capture, 1),
                                      b.literal(kRejected),
                                      b.varref(VarKind::Local, 0)},
                         p)));
  m.funcs[static_cast<size_t>(run_idx)] = std::move(run);
}

// ---- Left per language, and why -------------------------------------------
//
// * `$truthy` past nil and false. JavaScript's falsy set has "" and NaN and
//   not [], Python's has "", [] and {} and not NaN: the decision
//   Value::truthy() refuses to make, made differently by each, and in
//   bodies too short for a parameterized one to be anything but a table of
//   the same decisions.
// * `$str`. Every front end spells nil, a bool and a container its own way
//   ("None", "nil", "", "undefined"; "True", "true"), and finds a class's own
//   to_s/__str__/ToString/__tostring by its own protocol.
// * `$eq` as a whole, past the container loops above. Where numbers meet
//   (Python's bignums), what an object is asked (Ruby's ==, Python's
//   __eq__, Lua's __eq, C# identity, culebra's structural comparison) and
//   in which order differ in every language; the scalar cases they share
//   are four short tests, arranged differently in each.
// * `$add`. JavaScript and C# agree that a string on either side makes `+`
//   a concatenation, but that is two front ends and five lines; Python and
//   Ruby refuse the mixed case, and Lua turns the string into a number.
// * mini-js's classes. A class is an object holding its table of methods
//   in its kind record, and `new` copies them onto the instance as wrappers
//   that supply `this` -- not runtime_parts' layout, where an instance
//   points back at its class and a call walks to it. JavaScript reads a
//   method off the instance like any property, and this subset's classes
//   have no `extends` for a walk to climb.
// * mini-ruby's iteration. Its cursor walks a Hash too, and a Hash's step
//   answers a key beside the value; it has no generator to step. Its
//   $iternext builds its cursors and its steps with the shared parts, but
//   runs its own step: iter_step would add a generator test to every
//   `.each`, a hot path, for a kind Ruby does not have.
// * An error in JavaScript, culebra, Lua and Scheme. What it holds is the
//   program's to read, so none keeps it in a record: a JavaScript Error's
//   name and message are properties, a culebra error's kind and message
//   are fields, and a Lua or Scheme program raises any value, which the
//   implementation's own errors match by being strings.

}  // namespace runtime_parts
