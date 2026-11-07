// The slot no key reaches (ObjectObj::meta, MapObj::meta; MetaGet and
// MetaSet): where an implementation keeps what a program spelling a key
// must not read, forge or strip. Pinned here: it is no key -- an object's
// or a map's keys, its count and its removals all pass it by; it reads as
// nil where it was never set and on a value that carries none, and only an
// object or a map can be given one; it is a reference the collector
// follows -- a cycle through it is garbage, an object held only through it
// survives a collection intact, and the owned stack's analysis at a
// scope's exit sees through it; and a freed object releases it, after
// everything else the object held.

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "vmlib.h"

using namespace coreir;

namespace {

struct Failure : std::runtime_error {
  Failure(std::string msg) : std::runtime_error(std::move(msg)) {}
};

std::vector<std::string> g_out;
int g_failures = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
  }
}

void check_eq(const std::string& got, const std::string& want,
              const std::string& what) {
  if (got != want) {
    std::fprintf(stderr, "FAIL: %s\n  want [%s]\n  got  [%s]\n", what.c_str(),
                 want.c_str(), got.c_str());
    ++g_failures;
  }
}

std::string joined() {
  std::string s;
  for (const auto& line : g_out) {
    s += line;
    s += "|";
  }
  return s;
}

struct RunResult {
  std::string failure;
  int64_t left = 0;  // live objects after the run, before ~Runtime
};

RunResult run_module(const Module& m, const std::string& what) {
  g_out.clear();
  RunResult r;
  if (auto err = verify(m)) {
    std::fprintf(stderr, "FAIL: %s: malformed IR: %s\n", what.c_str(),
                 err->c_str());
    ++g_failures;
    return r;
  }
  {
    Runtime rt;
    const vm::Program p = vm::compile(m);
    try {
      vm::run(p, rt);
    } catch (const Failure& e) {
      r.failure = e.what();
    }
    r.left = rt.live_objects();
  }
  return r;
}

void expect_clean(const RunResult& r, const std::string& what) {
  check_eq(r.failure, "", what + ": unexpected failure");
  check(r.left == 0, what + ": leaked " + std::to_string(r.left) +
                         " heap object(s)");
}

NodeId print(FuncWriter& w, NodeId v) {
  return w.in(IntrinsicId::Print, {v});
}

// What the destructor hook below records: the "name" of each object it is
// handed, in the order it is handed them.
void record_name(void* ctx, HeapObj* h) {
  auto* order = static_cast<std::string*>(ctx);
  const Value* name = static_cast<ObjectObj*>(h)->find("name");
  *order += (name ? name->as_str() : std::string("?")) + "|";
}

// {name: <name>}, with a destructor: an object the hook above is handed
// when it dies.
Value named_resource(const char* name) {
  Value o = Value::make_object();
  o.as_object()->set("name", Value::make_str(name));
  o.as_object()->set_drop(Value::make_int(0));
  return o;
}

}  // namespace

extern "C" {
void coreir_rt_out(int64_t v) { g_out.push_back(std::to_string(v)); }
void coreir_rt_out_str(const char* bytes, int64_t len) {
  g_out.emplace_back(bytes, static_cast<size_t>(len));
}
void coreir_rt_out_raw(const char* bytes, int64_t len) {
  g_out.emplace_back(bytes, static_cast<size_t>(len));
}
int64_t coreir_rt_in(int64_t, int64_t) { return 0; }
void coreir_rt_poll(void) {}
[[noreturn]] void coreir_rt_fail(const char* msg, int64_t, int64_t) {
  throw Failure(msg);
}
}

int main() {
  using I = IntrinsicId;

  // --- 1. No key reaches it. An object's keys and count leave it out, and
  //        removing every key it lists leaves it set; a map's the same.
  // o = {a: 1}; metaset(o, "o's"); print(len(objectkeys(o))); print(len(o))
  // for k in objectkeys(o): objectremove(o, k); print(metaget(o))
  // mp = mapnew(); mp[1] = 2; metaset(mp, "mp's")
  // print(len(objectkeys(mp))); print(len(mp)); objectremove(mp, 1)
  // print(metaget(mp))
  {
    Module m;
    m.capture_maps.push_back({});
    m.funcs.resize(1);
    FuncWriter w(m);
    const auto [o, ks, i, mp] = w.locals("o", "ks", "i", "mp");
    w.add(w.set(o, w.obj({{"a", w.I(1)}})));
    w.add(w.set_meta(w.L(o), w.S("o's")));
    w.add(print(w, w.len(w.in(I::ObjectKeys, {w.L(o)}))));
    w.add(print(w, w.len(w.L(o))));
    w.add(w.set(ks, w.in(I::ObjectKeys, {w.L(o)})));
    w.add(w.set(i, w.I(0)));
    w.add(w.wh(w.bin(BinOp::Lt, w.L(i), w.len(w.L(ks))),
               w.blk({w.in(I::ObjectRemove, {w.L(o), w.idx(w.L(ks), w.L(i))}),
                      w.set(i, w.bin(BinOp::Add, w.L(i), w.I(1)))})));
    w.add(print(w, w.meta(w.L(o))));
    w.add(w.set(mp, w.in(I::MapNew, {})));
    w.add(w.sidx(w.L(mp), w.I(1), w.I(2)));
    w.add(w.set_meta(w.L(mp), w.S("mp's")));
    w.add(print(w, w.len(w.in(I::ObjectKeys, {w.L(mp)}))));
    w.add(print(w, w.len(w.L(mp))));
    w.add(w.in(I::ObjectRemove, {w.L(mp), w.I(1)}));
    w.add(print(w, w.meta(w.L(mp))));
    w.write(m.funcs[0], "main");
    expect_clean(run_module(m, "no key"), "no key");
    check_eq(joined(), "1|1|o's|1|1|mp's|", "no key: output");
  }

  // --- 2. Nil where it was never set, and on every value that carries
  //        none. A write answers the object it wrote; a read into the
  //        register that held the object's only reference still gets the
  //        slot's value.
  // print(metaget({})); print(metaget(mapnew())); print(metaget(1))
  // print(metaget("s")); print(metaget([]))
  // o = metaset({}, [7]); o = metaget(o); print(o[0])
  {
    Module m;
    m.capture_maps.push_back({});
    m.funcs.resize(1);
    FuncWriter w(m);
    const auto [o] = w.locals("o");
    w.add(print(w, w.meta(w.obj({}))));
    w.add(print(w, w.meta(w.in(I::MapNew, {}))));
    w.add(print(w, w.meta(w.I(1))));
    w.add(print(w, w.meta(w.S("s"))));
    w.add(print(w, w.meta(w.arr({}))));
    w.add(w.set(o, w.set_meta(w.obj({}), w.arr({w.I(7)}))));
    w.add(w.set(o, w.meta(w.L(o))));
    w.add(print(w, w.idx(w.L(o), w.I(0))));
    w.write(m.funcs[0], "main");
    expect_clean(run_module(m, "unset"), "unset");
    check_eq(joined(), "nil|nil|nil|nil|nil|7|", "unset: output");
  }

  // --- 3. Only an object or a map can be given one.
  // metaset([], 1)
  {
    Module m;
    m.capture_maps.push_back({});
    m.funcs.resize(1);
    FuncWriter w(m);
    w.add(w.set_meta(w.arr({}), w.I(1)));
    w.write(m.funcs[0], "main");
    const RunResult r = run_module(m, "set on an array");
    check_eq(r.failure, "cannot set the meta of array",
             "set on an array: the trap");
  }

  // --- 4. A cycle through the slot is garbage: an object or a map that is
  //        its own meta, and a two-object cycle closed by one meta. The
  //        sweep strips every condemned object before it frees any, so a
  //        slot left for the C++ destructor to release would be released
  //        into an object already freed -- which the sanitizer lane sees.
  {
    Runtime rt;
    Runtime::Scope scope(rt);
    {
      Value o = Value::make_object();
      o.as_object()->meta = o;
      Value mp = Value::make_map();
      mp.as_map()->meta = mp;
      Value a = Value::make_object();
      Value b = Value::make_object();
      a.as_object()->set("b", b);
      b.as_object()->meta = a;
    }
    check(rt.live_objects() == 4, "meta cycles survive counting");
    rt.collect();
    check(rt.live_objects() == 0, "meta cycles collected");
  }

  // --- 5. An object held only through the slot survives a collection
  //        intact; once its holder goes, the cycle it closes back to the
  //        holder is garbage.
  {
    Runtime rt;
    Runtime::Scope scope(rt);
    Value holder = Value::make_object();
    {
      Value c = Value::make_object();
      c.as_object()->set("back", holder);
      holder.as_object()->meta = c;
    }
    rt.collect();
    check(rt.live_objects() == 2, "held through meta: survives a collect");
    const Value& c = holder.as_object()->meta;
    const Value* back = c.is_object() ? c.as_object()->find("back") : nullptr;
    check(back && back->raw_data() == holder.raw_data(),
          "held through meta: intact after the collect");
    holder = Value();
    rt.collect();
    check(rt.live_objects() == 0, "held through meta: goes with its holder");
  }

  // --- 6. A freed object or map releases its meta, and only after
  //        everything else it held.
  for (const bool map : {false, true}) {
    Runtime rt;
    Runtime::Scope scope(rt);
    std::string order;
    rt.set_drop_fn(&order, record_name);
    {
      Value holder = map ? Value::make_map() : Value::make_object();
      Value& slot = map ? holder.as_map()->meta : holder.as_object()->meta;
      slot = named_resource("meta");
      if (map) {
        holder.as_map()->set(Value::make_int(1), named_resource("held"));
      } else {
        holder.as_object()->set("held", named_resource("held"));
      }
    }
    rt.set_drop_fn(nullptr, nullptr);
    const std::string what = map ? "map" : "object";
    check_eq(order, "held|meta|", what + " freed: meta released last");
    check(rt.live_objects() == 0, what + " freed: nothing left");
  }

  // --- 6b. An object's destructor, which its runtime keeps rather than
  //         the object, is released after the meta: the props, then the
  //         meta, then the destructor. (The destructor below is no
  //         callable; what the hook records is each object it is handed,
  //         the destructor among them when it dies.)
  {
    Runtime rt;
    Runtime::Scope scope(rt);
    std::string order;
    rt.set_drop_fn(&order, record_name);
    {
      Value holder = named_resource("holder");
      holder.as_object()->set_drop(named_resource("dtor"));
      holder.as_object()->meta = named_resource("meta");
      holder.as_object()->set("held", named_resource("held"));
    }
    rt.set_drop_fn(nullptr, nullptr);
    check_eq(order, "holder|held|meta|dtor|",
             "destructor released after the meta");
    check(rt.live_objects() == 0, "destructor released: nothing left");
  }

  // --- 6c. Neither the meta nor a destructor makes an Object larger than
  //         its header, its props and its meta: the destructor is its
  //         runtime's, and an Object's flags share the header's padding.
  {
    struct Bare : HeapObj {
      std::vector<std::pair<std::string, Value>> props;
      Value meta;
    };
    check(sizeof(ObjectObj) == sizeof(Bare),
          "an Object is its header, its props and its meta");
  }

  // --- 7. The owned stack's analysis sees through the slot: a resource
  //        whose cycle closes through its own meta drops at the exit of
  //        the scope that bound it, not at some later collection.
  // d = fn (self) { print(self.name) }
  // { r = dropset({name: "r"}, d); metaset(r, {back: r}) }; print("after")
  {
    Module m;
    m.capture_maps.push_back({});
    m.funcs.resize(2);
    {
      FuncWriter w(m);
      w.lenient_arity = false;
      w.singleton = false;
      const auto [self] = w.params("self");
      w.add(print(w, w.idx(w.L(self), "name")));
      w.write(m.funcs[1], "drop");
    }
    FuncWriter w(m);
    w.lenient_arity = false;
    w.singleton = false;
    const auto [d, r] = w.locals("d", "r");
    w.add(w.set(d, w.b.make_closure(1, 0)));
    w.add(w.b.scope(
        r.i, r.i + 1,
        w.blk({w.set(r, w.set_drop(w.obj({{"name", w.S("r")}}), w.L(d))),
               w.set_meta(w.L(r), w.obj({{"back", w.L(r)}}))})));
    w.add(print(w, w.S("after")));
    w.write(m.funcs[0], "main");
    expect_clean(run_module(m, "owned through meta"), "owned through meta");
    check_eq(joined(), "r|after|", "owned through meta: output");
  }

  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("meta: all checks passed");
  return 0;
}
