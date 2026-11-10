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
#include <iterator>
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

// Whether `v` carries a kind record of `kind`: any value can be asked,
// and one that is no Object, or whose meta is no kind record of a kind's
// name, is of none -- anything but an Object is answered by its first
// test, which on the paths that ask of every number (`$add`'s "is this a
// tuple") is the whole cost. The value, its meta and the meta's head are
// each read once, each held by a Let for the tests that read it.
inline NodeId is_kind(coreir::Builder::At& b, NodeId v,
                      const std::string& kind) {
  const auto is = [&](NodeId x, const char* type) {
    return b.binary(BinOp::Eq, b.intrinsic(IntrinsicId::TypeOf, {x}),
                    b.str_literal(type));
  };
  const auto no = [&] { return b.bool_literal(false); };
  return b.let(v, [&](auto t) {
    return b.make_if(
        is(t(), "object"),
        b.let(b.intrinsic(IntrinsicId::MetaGet, {t()}), [&](auto m) {
          return b.make_if(
              is(m(), "array"),
              b.let(b.index(m(), b.literal(0)), [&](auto h) {
                return b.make_if(is(h(), "string"),
                                 b.binary(BinOp::Eq, h(), b.str_literal(kind)),
                                 no());
              }),
              no());
        }),
        no());
  });
}
inline NodeId is_kind(FuncWriter& w, FuncWriter::Slot v,
                      const std::string& kind) {
  return is_kind(w.b, w.L(v), kind);
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

// The same questions, asked in C++ by a host function, each a twin of a
// part above or below that test_runtime_parts.cc holds to the same
// answers. They start from the one walk to a kind record: the items of
// `v`'s meta when `v` is an Object whose meta is an array, read once, or
// null.
inline const std::vector<coreir::Value>* meta_items(const coreir::Value& v) {
  if (!v.is_object()) return nullptr;
  const coreir::Value& m = v.as_object()->meta;
  return m.is_array() ? &m.as_array()->items : nullptr;
}

// The kind record `v` carries when it is of `kind`, or null: is_kind's
// twin.
inline const std::vector<coreir::Value>* kind_record(const coreir::Value& v,
                                                     std::string_view kind) {
  const auto* rec = meta_items(v);
  if (!rec || rec->empty() || !(*rec)[0].is_str() ||
      (*rec)[0].as_str() != kind) {
    return nullptr;
  }
  return rec;
}

// ---- Classes ---------------------------------------------------------------
//
// There is no class in the IR, and none was needed. A class's methods are
// an object -- its method table, JavaScript's C.prototype -- holding them
// by name, with the class's name and its base's table in the table's kind
// record; an instance is an object of the program's fields whose meta is
// the method table:
//
//   method table: { m: <m>, ... }    ["class", "C", <the base's table>, ...]
//   instance:     { <fields> }       meta: <the method table>
//
// A front end adds what else its classes need to the record, from
// kClassOwn on -- a constructor, an identity to compare -- and every
// question a class system asks is a walk up kClassBase: which method a
// name means (find_in_chain), whether a value is an instance of a class.
// The class's value, what its name evaluates to, is the program's: where
// a class holds nothing else it is the method table itself (mini-ruby's),
// or no object at all (mini-python's, its constructor), and where it holds
// what the program puts on it -- JavaScript's statics, C#'s static fields,
// culebra's `new` and `name` -- it is an object of its own, so that none
// of that meets a method.
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
// method call -- is answered by its one type test. The meta and its head
// are each read once, each held by a Let.
inline NodeId instance_class(FuncWriter& w, FuncWriter::Slot v) {
  return w.iff(
      w.is(w.typ(v), "object"),
      w.let(w.meta(w.L(v)), [&](auto m) {
        return w.iff(
            w.is(w.typ(m()), "object"), m(),
            w.iff(w.is(w.typ(m()), "array"),
                  w.let(w.idx(m(), w.I(0)), [&](auto h) {
                    return w.iff(w.is(w.typ(h()), "object"), h(), w.Nil());
                  }),
                  w.Nil()));
      }),
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
  const coreir::Value& m = v.as_object()->meta;
  if (m.is_object()) return &m;
  const auto* rec = meta_items(v);
  return rec && !rec->empty() && (*rec)[0].is_object() ? &(*rec)[0]
                                                        : nullptr;
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

// ---- Held values --------------------------------------------------------
//
// What a binder writes over Builder::let for the two shapes most of them
// share.

// A call that reads fewer of its arguments than it is given -- a library
// function lowered by name to an intrinsic or a helper, which takes only
// the ones it reads -- in a language that runs every argument anyway, in
// order, before the call. The first `reads` are held by Lets, the rest run
// for their effects alone, and then `call(read)` over the values the first
// ones held. Given no more than it reads, it is `call(args)`, built as it
// would have been without this.
template <class F>
inline NodeId reading(coreir::Builder::At& b, const std::vector<NodeId>& args,
                      size_t reads, F call) {
  if (args.size() <= reads) return call(args);
  const std::vector<NodeId> first(args.begin(),
                                  args.begin() + static_cast<long>(reads));
  return b.lets(first, [&](const std::vector<coreir::Builder::Temp>& t) {
    std::vector<NodeId> out(args.begin() + static_cast<long>(reads),
                            args.end());
    out.push_back(call(coreir::Builder::read_all(t)));
    return b.block(out);
  });
}

// `store(value)`, and with `as_value` an assignment that is an expression:
// `value` held by a Let, stored, and read again after, so that its value is
// what was assigned and not what the store answers (a SetIndex answers
// none).
template <class Store>
inline NodeId stored_value(coreir::Builder::At& b, NodeId value, Store store,
                           bool as_value = true) {
  if (!as_value) return store(value);
  return b.let(value, [&](auto v) { return b.block({store(v()), v()}); });
}

// `recv[key] = combine(recv[key])`, the receiver and the key each run once
// and held by a Let for the read and for the write: a compound assignment
// to a field, an element or a property. The binder supplies how to read
// one (`get(recv, key)`), how to write one (`set(recv, key, value)`), and
// `combine(read)`, the new value over a callable that makes the present
// one -- never called for a plain `=`, which has no use for it.
//
// A SetIndex is a statement: it has no value, whatever its fallback
// answers. A store that is itself an expression whose value is the one
// stored -- Ruby's, JavaScript's, C#'s, culebra's, `x = (h[k] = v)` --
// asks for `as_value`, and the new value is held by a Let and read after
// the store (stored_value, below).
template <class Get, class Set, class Combine>
inline NodeId read_modify_write(coreir::Builder::At& b, NodeId recv,
                                NodeId key, Get get, Set set, Combine combine,
                                bool as_value = false) {
  return b.lets({recv, key}, [&](const std::vector<coreir::Builder::Temp>& t) {
    return stored_value(
        b, combine([&] { return get(t[0](), t[1]()); }),
        [&](NodeId v) { return set(t[0](), t[1](), v); }, as_value);
  });
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
// Optional, and a front end's own: the state a language that keeps more than
// a message (Python's `args`) puts after the three above, through the
// builders' trailing `extra`. The readers below look no further than
// kExcMessage, and an origin stamp (coreir::stamp_origin) goes after it.
inline constexpr int64_t kExcArgs = 3;

// A fresh exception of `type` (a kind's name node, or a class's table), with
// the front end's `extra` state after the message.
inline NodeId exception(coreir::Builder::At& b, NodeId type, NodeId name,
                        NodeId message,
                        const std::vector<NodeId>& extra = {}) {
  std::vector<NodeId> state{name, message};
  state.insert(state.end(), extra.begin(), extra.end());
  return with_meta(b, b.object_lit({}), type, state);
}
inline NodeId exception(FuncWriter& w, NodeId type, NodeId name,
                        NodeId message,
                        const std::vector<NodeId>& extra = {}) {
  return exception(w.b, type, name, message, extra);
}

// Whether `v` is an exception: an Object whose meta is an exception's
// record, all three of its fields there, headed by kExcKind for a builtin
// one or by the class of a program's that keeps a message. The meta and
// its head are each read once, each held by a Let.
inline NodeId is_exception(FuncWriter& w, FuncWriter::Slot v) {
  return w.both(
      w.is(w.typ(v), "object"),
      w.let(w.meta(w.L(v)), [&](auto m) {
        return w.both(
            w.both(w.is(w.typ(m()), "array"),
                   w.bin(BinOp::Gt, w.len(m()), w.I(kExcMessage))),
            w.let(w.idx(m(), w.I(0)), [&](auto h) {
              return w.iff(w.is(w.typ(h()), "string"),
                           w.bin(BinOp::Eq, h(), w.S(kExcKind)),
                           w.is(w.typ(h()), "object"));
            }));
      }));
}

// The same, asked in C++ by a host function -- a report's: the exception's
// [type, name, message], or null when `v` is none, by is_exception's tests
// in is_exception's order, the meta read once.
inline const std::vector<coreir::Value>* exception_record(
    const coreir::Value& v) {
  const auto* rec = meta_items(v);
  if (!rec || rec->size() <= static_cast<size_t>(kExcMessage)) return nullptr;
  const coreir::Value& type = (*rec)[0];
  return (type.is_str() && type.as_str() == kExcKind) || type.is_object()
             ? rec
             : nullptr;
}

// A fresh builtin exception, built by a host function: the object
// `exception()` builds in IR, for a front end that makes one outside a
// helper -- the conversion of an executor's trap at a catch
// (coreir::trap_kind), which has no IR to run. exception_record reads
// it back, and test_runtime_parts holds the two builders to one shape.
inline coreir::Value make_exception(
    const std::string& name, const coreir::Value& message,
    const std::vector<coreir::Value>& extra = {}) {
  coreir::Value e = coreir::Value::make_object();
  std::vector<coreir::Value> rec{coreir::Value::make_str(kExcKind),
                                 coreir::Value::make_str(name), message};
  rec.insert(rec.end(), extra.begin(), extra.end());
  e.as_object()->meta = coreir::Value::make_array(rec);
  return e;
}

// ---- Executor traps at a catch ---------------------------------------------
//
// A trap the executor raised is {message, line, col} with a kind and facts
// (coreir::trap_kind, trap_fact): its own words, not a front end's. A
// front end that has words for it converts it where a handler can observe
// it -- the head of a catch that reads the exception -- by `converted()`,
// one native call whose answer is the caught value unchanged unless it is a
// trap, and then the front end's exception for it. One conversion per
// trap: what comes back is not a trap, so a second catch passes it by.
//
// A handler that only re-raises (a `finally`, a `rescue` that matched
// nothing) does not convert, and the raw trap goes on. A converted exception
// carries the trap's position (coreir::stamp_origin), so raising it again
// reports where it first failed.

inline constexpr char kTrapConvert[] = "trapcvt";

inline coreir::NodeId converted(coreir::Builder::At& b, coreir::NodeId caught) {
  return b.call_value(b.native_ref(b.declare_native(kTrapConvert)), {caught});
}

// `Convert(trap, ctx)` makes the front end's exception for a trap; ctx is the
// native's own (a style, a chunk name), as double_format's styles are.
// converted_or_self is the one place it is applied: a value that is no trap
// is its own answer, and a converted exception is stamped with the place the
// trap was raised (coreir::stamp_origin), so raising it again reports that
// place. The front end's `describe` runs a raised value through it too, so
// that describe never sees a trap.
template <coreir::Value (*Convert)(const coreir::Value& trap, const void* ctx)>
coreir::Value converted_or_self(const coreir::Value& v,
                                const void* ctx = nullptr) {
  if (coreir::trap_kind(v) == coreir::TrapKind::None) return v;
  coreir::Value e = Convert(v, ctx);
  if (const auto d = coreir::trap_diagnostic(v)) {
    coreir::stamp_origin(e, d->pos);
  }
  return e;
}

// The def is registered with the run's natives.
template <coreir::Value (*Convert)(const coreir::Value& trap, const void* ctx)>
bool trap_convert_native(coreir::NativeCall& c) {
  c.result = converted_or_self<Convert>(c.arg(0), c.ctx);
  return true;
}
template <coreir::Value (*Convert)(const coreir::Value& trap, const void* ctx)>
vm::NativeDef trap_convert_def(const void* ctx = nullptr) {
  return {kTrapConvert, 1, &trap_convert_native<Convert>,
          const_cast<void*>(ctx)};
}

// ---- Operators -------------------------------------------------------------

// `<`, `<=`, `>` and `>=`: the operators a message words as a comparison.
inline bool is_ordering(coreir::BinOp op) {
  using B = coreir::BinOp;
  return op == B::Lt || op == B::Le || op == B::Gt || op == B::Ge;
}
// `&`, `|`, `^`, `<<` and `>>`: the operators that take two ints only.
inline bool is_bitwise(coreir::BinOp op) {
  using B = coreir::BinOp;
  return op == B::BitAnd || op == B::BitOr || op == B::BitXor ||
         op == B::Shl || op == B::Shr;
}

// The spelling of an operator in the C family's notation, which Python,
// Ruby and culebra share for every one of these: a diagnostic that names
// the operator ("unsupported operand type(s) for +") gets it here. FloorDiv
// is `//` (Python's, and the only language here that spells it), and Mod
// and FloorMod are both `%`. An operator no language here spells, the
// unsigned ones, gets its name.
inline const char* binop_symbol(coreir::BinOp op) {
  using B = coreir::BinOp;
  switch (op) {
    case B::Add:      return "+";
    case B::Sub:      return "-";
    case B::Mul:      return "*";
    case B::Div:      return "/";
    case B::Mod:
    case B::FloorMod: return "%";
    case B::FloorDiv: return "//";
    case B::Eq:       return "==";
    case B::Ne:       return "!=";
    case B::Lt:       return "<";
    case B::Le:       return "<=";
    case B::Gt:       return ">";
    case B::Ge:       return ">=";
    case B::BitAnd:   return "&";
    case B::BitOr:    return "|";
    case B::BitXor:   return "^";
    case B::Shl:      return "<<";
    case B::Shr:      return ">>";
    default:          return coreir::name_of(op);
  }
}
inline const char* unop_symbol(coreir::UnOp op) {
  using U = coreir::UnOp;
  switch (op) {
    case U::Neg:    return "-";
    case U::BitNot: return "~";
    case U::Not:    return "not";
    default:        return coreir::name_of(op);
  }
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
// The same line asked of one value inside a helper's body: whether `v` is
// an int or a double -- `v` evaluated once, and each test a TypeIn of it.
inline NodeId is_number(FuncWriter& w, NodeId v) {
  return w.let(v, [&](auto t) {
    return w.either(w.is(w.typ(t()), "int"), w.is(w.typ(t()), "double"));
  });
}
// Whether `v`, a divisor, is a zero, an int's or a double's -- what an
// exact division leaves its helper to raise for (vmlib.h's kExact).
// Anything that is no number is not, and goes on to the operator, which
// refuses it in its own words.
inline NodeId is_zero_divisor(FuncWriter& w, NodeId v) {
  return w.both(is_number(w, v), w.bin(coreir::BinOp::Eq, v, w.I(0)));
}
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
// BinOp, Index or SetIndex -- the tags it answers on, and its flags. An
// Index or a SetIndex names a set for each operand: `tags` is the
// receiver's, `key_tags` the key's (0: the same), `value_tags` a stored
// value's (0: any). The flags are vmlib.h's: kExact, and for Index and
// SetIndex kMissNil, and for SetIndex kAppend.
struct VmOp {
  enum class Kind : uint8_t { Unary, Binary, Index, SetIndex };
  Kind kind;
  uint8_t op;  // the UnOp or the BinOp; nothing for Index and SetIndex
  uint32_t tags;
  int32_t flags;
  uint32_t key_tags = 0;
  uint32_t value_tags = 0;

  static VmOp unary(UnOp op, uint32_t tags, int32_t flags = 0) {
    return {Kind::Unary, static_cast<uint8_t>(op), tags, flags};
  }
  static VmOp binary(BinOp op, uint32_t tags, int32_t flags = 0) {
    return {Kind::Binary, static_cast<uint8_t>(op), tags, flags};
  }
  static VmOp index(uint32_t tags, int32_t flags = 0, uint32_t key_tags = 0) {
    return {Kind::Index, 0, tags, flags, key_tags};
  }
  static VmOp set_index(uint32_t tags, int32_t flags = 0,
                        uint32_t key_tags = 0, uint32_t value_tags = 0) {
    return {Kind::SetIndex, 0, tags, flags, key_tags, value_tags};
  }
  size_t operands() const {
    return kind == Kind::Unary ? 1 : kind == Kind::SetIndex ? 3 : 2;
  }
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
    if (args.size() != o.operands()) {
      coreir_rt::fail("Operators: " + helper + " takes " +
                          std::to_string(o.operands()) + " operands",
                      0, 0);
    }
    const Fallback f{rt_.ref(helper, p), o.tags, o.flags, o.key_tags,
                     o.value_tags};
    auto b = coreir::Builder(rt_.module()).at(p);
    switch (o.kind) {
      case VmOp::Kind::Unary:
        return b.unary(static_cast<UnOp>(o.op), args[0], f);
      case VmOp::Kind::Binary:
        return b.binary(static_cast<BinOp>(o.op), args[0], args[1], f);
      case VmOp::Kind::Index:
        return b.index(args[0], args[1], f);
      case VmOp::Kind::SetIndex:
        return b.set_index(args[0], args[1], args[2], f);
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
// * How a double prints. It is not the same decision twice: the fixed-versus-
//   exponent thresholds, the exponent's spelling, ".0", inf, nan and -0 are
//   each language's own, so they are a table of styles, not a part of IR:
//   double_format.h, which a front end calls as a host function.
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
