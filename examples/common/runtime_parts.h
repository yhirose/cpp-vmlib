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
// its own name and its own place in its runtime.

#pragma once

#include <functional>
#include <string>

#include "vmlib.h"

namespace runtime_parts {

using coreir::BinOp;
using coreir::FuncWriter;
using coreir::IntrinsicId;
using coreir::NodeId;

// ---- Keys, by who chose the name -------------------------------------------
//
// An object a front end builds holds names from two choosers, and two
// prefixes keep them apart. The implementation's own names -- an instance's
// class, a class's base and name, the runtime's destructor key (vmlib.h's
// kDropKey) -- start with 0x01. The program's names that have to sit beside
// them or beside its own fields -- its methods on a class table, the field
// behind a C# auto property -- start with 0x02. Neither byte can
// begin a name any of these languages lets a program write, so the
// program's own fields, unprefixed, meet neither; and the two never meet
// each other, so a method named `drop`, `init`, `name` or `ctor` is not the
// implementation's key of the same name. mini-culebra arrived at this rule
// (its methods once sat under the runtime's own prefix, which made a class
// with a `drop` method a resource), and mini-csharp, whose class tables kept
// their name and constructor under the methods' prefix, had the bug from
// the other side until it took the rule from here: a method named `name`
// overwrote the class's name, and one named `ctor` was the constructor.
inline constexpr char kImplPrefix = '\x01';
inline constexpr char kProgramPrefix = '\x02';

// The key for a name the program chose, kept apart from the
// implementation's keys and from the program's fields.
inline std::string program_key(const std::string& name) {
  return kProgramPrefix + name;
}

// The same, for a name only known at run time: `name` is a string value.
inline NodeId program_key(FuncWriter& w, NodeId name) {
  return w.bin(BinOp::Add, w.S(std::string(1, kProgramPrefix)), name);
}

// ---- Classes ---------------------------------------------------------------
//
// There is no class in the IR, and none was needed. A class is an object --
// its table -- holding its methods and its base's table, and an instance is
// an object pointing back at its class:
//
//   table:    { kNameKey: "C", kBaseKey: <the base's table>,
//               program_key("m"): <m>, ... }
//   instance: { kClassKey: <table>, <the program's fields> }
//
// A front end adds what else its classes need under kImplPrefix -- a
// constructor, an identity to compare -- and every question a class system
// asks is a walk up kBaseKey: which method a name means, whether a value is
// an instance of a class.
inline constexpr char kClassKey[] = "\x01" "c";
inline constexpr char kBaseKey[] = "\x01" "p";
inline constexpr char kNameKey[] = "\x01" "n";
static_assert(kClassKey[0] == kImplPrefix && kBaseKey[0] == kImplPrefix &&
              kNameKey[0] == kImplPrefix);

// The walk, as the While it is: from the table in `c`, each table up the
// chain in turn, nearest first, until one has no base. `visit` runs at each
// and returns from the helper when it finds what it is after; a walk that
// falls off the end found nothing. `c` is left at the last table looked at.
inline NodeId walk_bases(FuncWriter& w, FuncWriter::Slot c, NodeId visit) {
  return w.wh(
      w.is(w.typ(c), "object"),
      w.blk({visit,
             w.iff(w.bin(BinOp::Eq, w.has(w.L(c), w.S(kBaseKey)), w.Bo(false)),
                   w.b.make_break()),
             w.set(c, w.idx(w.L(c), kBaseKey))}));
}

// `(t, key)`: what `key` names in the table `t` or in its nearest ancestor
// that has it, or nil -- method lookup, with `key` already a program_key.
inline void find_in_chain(FuncWriter& w) {
  const auto [t, key] = w.params("t", "key");
  const auto [c] = w.locals("c");
  w.add(w.set(c, w.L(t)));
  w.add(walk_bases(w, c,
                   w.iff(w.has(w.L(c), w.L(key)),
                         w.ret(w.idx(w.L(c), w.L(key))))));
  w.add(w.ret(w.Nil()));
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
// protocol -- builds it with cursor() and steps it in iter_step's `own`.

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

// `(it)`: the next step of a cursor. `own(it, a)` adds the statements for a
// front end's own kinds of cursor, tested after a generator's and before a
// sequence's; `a` is a local it may use, and it may declare more.
inline void iter_step(
    FuncWriter& w,
    const std::function<void(FuncWriter::Slot it, FuncWriter::Slot a)>& own =
        {}) {
  const auto [it] = w.params("it");
  const auto [a, i] = w.locals("a", "i");
  w.add(w.iff(cursor_is(w, it, "g"),
              w.ret(w.in(IntrinsicId::GenResume,
                         {cursor_source(w, it), w.Nil()}))));
  if (own) own(it, a);
  w.add(w.set(a, cursor_source(w, it)));
  w.add(w.set(i, w.idx(w.L(it), "i")));
  w.add(w.iff(w.bin(BinOp::Ge, w.L(i), w.len(w.L(a))), w.ret(step_done(w))));
  w.add(w.sidx(w.L(it), "i", w.bin(BinOp::Add, w.L(i), w.I(1))));
  w.add(w.ret(step(w, w.idx(w.L(a), w.L(i)))));
}

// ---- Left per language, and why -------------------------------------------
//
// * `$truthy`, past nil and false. JavaScript's falsy set has "" and NaN and
//   not [], Python's has "", [] and {} and not NaN: the same decision
//   Value::truthy() refuses to make, made differently by each.
// * `$str`. Every front end spells nil, a bool and a container its own way
//   ("None", "nil", "", "undefined"; "True", "true"), and finds a class's own
//   to_s/__str__/ToString/__tostring by its own protocol.
// * `$eq` as a whole. Where numbers meet (Python's bignums), what an object
//   is asked (Ruby's ==, Python's __eq__, Lua's __eq, C# identity, culebra's
//   structural comparison) and in which order differ in every language.
// * `$add`. JavaScript and C# agree that a string on either side makes `+`
//   a concatenation, but that is two front ends and five lines; Python and
//   Ruby refuse the mixed case, and Lua turns the string into a number.
// * mini-ruby's iteration. Its cursor walks a Hash too, and a Hash's step
//   answers a key beside the value; it has no generator to step. The
//   shared step could take that as an `own` kind, but at the cost of an
//   extra test in every `.each`, a hot path, for no other front end's use.
// * An `async` function's lowering into three funcs (mini-js, mini-csharp):
//   it is the emit pass's, built from each binder's own frames and
//   captures, not a helper.

}  // namespace runtime_parts
