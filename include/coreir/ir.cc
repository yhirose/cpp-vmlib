#include "coreir/ir.h"

#include <cstdlib>
#include <vector>

#include <sstream>

#include "coreir/value.h"

namespace coreir {

// coreir/value.h's out-of-line members. They live here rather than in a
// value.cc of their own so that no new translation unit appears -- culebra's
// CMakeLists names cpp-vmlib's sources one by one, in two separate places.

thread_local Runtime* Runtime::current_ = nullptr;

void Runtime::link(HeapObj* o) {
  o->prev = nullptr;
  o->next = head_;
  if (head_) head_->prev = o;
  head_ = o;
  ++live_;
}

void Runtime::unlink(HeapObj* o) {
  if (o->prev) o->prev->next = o->next;
  else head_ = o->next;
  if (o->next) o->next->prev = o->prev;
  o->prev = o->next = nullptr;
  --live_;
}

// A Runtime outliving its objects is the normal case -- everything a program
// allocated and released is already gone. What can remain is a reference
// cycle, which counting cannot collect; those are freed here, so the heap
// really does end empty and a leak checker does not report as a bug the one
// thing this design openly cannot do on its own.
//
// Freeing a cycle cannot be a single pass. Destroying one member releases its
// references, which frees the next member, whose destructor releases the
// reference back to the first -- which is mid-destruction. So: pin everything
// first, drop every outgoing reference while nothing can be freed, and only
// then free the shells, which by that point refer to nothing. culebra's
// collector separates the same two steps for the same reason (memory.md's
// finalize-then-sweep), and this is where a tracing collector would hook in.
Runtime::Runtime() {
  const char* s = std::getenv("COREIR_GC_STRESS");
  stress_ = s && *s && *s != '0';
}

namespace {

// One place that knows each kind's outgoing references, shared by the edge
// count and the mark. New heap kinds add their arm here in the same commit
// that adds the type.
template <typename Fn>
void visit_children(HeapObj* o, Fn fn) {
  auto visit = [&](const Value& v) {
    if (v.is_heap()) fn(reinterpret_cast<HeapObj*>(v.raw_data()));
  };
  switch (o->kind) {
    case ValueTag::Array:
      for (const Value& v : static_cast<ArrayObj*>(o)->items) visit(v);
      break;
    case ValueTag::Object:
      for (const auto& kv : static_cast<ObjectObj*>(o)->props) {
        visit(kv.second);
      }
      break;
    case ValueTag::Cell:
      visit(static_cast<CellObj*>(o)->v);
      break;
    case ValueTag::Func:
      for (const Value& v : static_cast<ClosureObj*>(o)->cells) visit(v);
      break;
    case ValueTag::Generator: {
      const GenFrame& gf = static_cast<GeneratorObj*>(o)->frame;
      for (const Value& v : gf.locals) visit(v);
      for (const Value& v : gf.regs) visit(v);
      for (const Value& v : gf.cells) visit(v);
      for (const Value& v : gf.captures) visit(v);
      for (const Value& v : gf.defers) visit(v);
      break;
    }
    default:
      break;  // a string refers to nothing
  }
}

}  // namespace

// Marks everything reachable from an externally-held object. An object is
// held externally when its refcount exceeds the references other heap
// objects hold on it -- less the collection's own pin, on an object it has
// condemned. A worklist, not recursion: a deep structure must not become a
// C++ stack problem.
void Runtime::mark() {
  for (HeapObj* o = head_; o; o = o->next) {
    o->gc_refs = 0;
    o->gc_marked = false;
  }
  for (HeapObj* o = head_; o; o = o->next) {
    visit_children(o, [](HeapObj* c) { ++c->gc_refs; });
  }
  std::vector<HeapObj*> work;
  for (HeapObj* o = head_; o; o = o->next) {
    if (o->rc - (o->gc_condemned ? 1 : 0) > o->gc_refs) {
      o->gc_marked = true;
      work.push_back(o);
    }
  }
  while (!work.empty()) {
    HeapObj* o = work.back();
    work.pop_back();
    visit_children(o, [&](HeapObj* c) {
      if (!c->gc_marked) {
        c->gc_marked = true;
        work.push_back(c);
      }
    });
  }
}

int64_t Runtime::collect() {
  if (in_collect_) return 0;
  in_collect_ = true;

  // Condemn what nothing reaches, and pin it: a destructor about to run
  // may drop the last reference between two condemned objects, and that
  // must not free one of them out from under the sweep.
  mark();
  std::vector<HeapObj*> dead;
  for (HeapObj* o = head_; o; o = o->next) {
    if (!o->gc_marked) {
      o->gc_condemned = true;
      ++o->rc;
      dead.push_back(o);
    }
  }

  // Destructors, while every condemned object is still whole: newest first
  // (the heap list's order), the way a scope releases its locals in reverse.
  // One may store a condemned object somewhere reachable -- so if any ran,
  // reachability is recomputed and what it resurrected is unpinned and
  // spared. Its destructor runs again when it next dies, like the refcount
  // path's. Allocation here is ordinary: a nested collect is refused by
  // in_collect_, and an object dying by refcount gets its own destructor.
  bool ran = false;
  if (drop_) {
    for (HeapObj* o : dead) {
      if (o->kind != ValueTag::Object) continue;
      if (!static_cast<ObjectObj*>(o)->find("\x01" "drop")) continue;
      drop_(drop_ctx_, o);
      ran = true;
    }
  }
  if (ran) {
    mark();
    std::vector<HeapObj*> still;
    for (HeapObj* o : dead) {
      if (o->gc_marked) {
        o->gc_condemned = false;
        --o->rc;  // the pin; a reachable object holds at least one more
      } else {
        still.push_back(o);
      }
    }
    dead.swap(still);
  }

  // The host's last look (while every member is still whole), then
  // ~Runtime's strip / free on just this set.
  if (finalize_) {
    for (HeapObj* o : dead) finalize_(finalize_ctx_, o);
  }
  sweeping_ = true;
  for (HeapObj* o : dead) clear_heap_object_refs(o);
  for (HeapObj* o : dead) {
    unlink(o);
    o->owner = nullptr;
    destroy_heap_object(o);
  }
  sweeping_ = false;

  next_gc_ = live_ < 2048 ? 4096 : live_ * 2;
  in_collect_ = false;
  return static_cast<int64_t>(dead.size());
}

// A walk rather than a counter kept on the allocation path: nothing here
// makes an allocation or a container growth pay for a number only a
// diagnostic asks for. Per object, its own struct plus what its containers
// have reserved -- capacity, not size, since that is what is held.
int64_t Runtime::heap_bytes() const {
  int64_t bytes = 0;
  auto vec = [](const auto& v) {
    return static_cast<int64_t>(v.capacity() * sizeof(v[0]));
  };
  for (HeapObj* o = head_; o; o = o->next) {
    switch (o->kind) {
      case ValueTag::Str:
        bytes += sizeof(StrObj) + vec(static_cast<StrObj*>(o)->s);
        break;
      case ValueTag::Array:
        bytes += sizeof(ArrayObj) + vec(static_cast<ArrayObj*>(o)->items);
        break;
      case ValueTag::Object: {
        auto* obj = static_cast<ObjectObj*>(o);
        bytes += sizeof(ObjectObj) + vec(obj->props);
        for (const auto& kv : obj->props) bytes += vec(kv.first);
        break;
      }
      case ValueTag::Cell:
        bytes += sizeof(CellObj);
        break;
      case ValueTag::Func:
        bytes += sizeof(ClosureObj) + vec(static_cast<ClosureObj*>(o)->cells);
        break;
      case ValueTag::Generator: {
        const GenFrame& gf = static_cast<GeneratorObj*>(o)->frame;
        bytes += sizeof(GeneratorObj) + vec(gf.locals) + vec(gf.regs) +
                 vec(gf.cells) + vec(gf.captures) + vec(gf.defers) +
                 vec(gf.defer_marks);
        break;
      }
      default:
        break;
    }
  }
  return bytes;
}

// The refcount just hit zero. An Object carrying the drop contract's key
// runs its destructor first, pinned so the closure can read the fields; a
// destructor that stores the object somewhere resurrects it and the free is
// skipped. Everything else -- and every non-Object -- frees directly.
void heap_release_to_zero(HeapObj* h) {
  if (h->kind == ValueTag::Object && h->owner && h->owner->drop_fn() &&
      !h->owner->sweeping()) {
    auto* o = static_cast<ObjectObj*>(h);
    if (o->find("\x01" "drop")) {
      ++h->rc;  // pin across the destructor
      h->owner->drop_fn()(h->owner->drop_ctx(), h);
      if (--h->rc != 0) return;  // resurrected
    }
  }
  destroy_heap_object(h);
}

Runtime::~Runtime() {
  for (HeapObj* o = head_; o; o = o->next) ++o->rc;
  for (HeapObj* o = head_; o; o = o->next) clear_heap_object_refs(o);
  while (head_) {
    HeapObj* o = head_;
    unlink(o);
    o->owner = nullptr;  // already unlinked; do not let ~HeapObj do it again
    destroy_heap_object(o);
  }
}

HeapObj::HeapObj(ValueTag k) : rc(1), kind(k), owner(Runtime::current()) {
  if (owner) owner->link(this);
}

HeapObj::~HeapObj() {
  if (owner) owner->unlink(this);
}

void clear_heap_object_refs(HeapObj* o) {
  switch (o->kind) {
    case ValueTag::Array:
      static_cast<ArrayObj*>(o)->items.clear();
      break;
    case ValueTag::Object:
      static_cast<ObjectObj*>(o)->props.clear();
      break;
    case ValueTag::Cell:
      static_cast<CellObj*>(o)->v = Value();
      break;
    case ValueTag::Func:
      static_cast<ClosureObj*>(o)->cells.clear();
      break;
    case ValueTag::Generator:
      static_cast<GeneratorObj*>(o)->frame = GenFrame{};
      break;
    default:
      break;  // a string refers to nothing
  }
}

void destroy_heap_object(HeapObj* o) {
  switch (o->kind) {
    case ValueTag::Str:   delete static_cast<StrObj*>(o); break;
    case ValueTag::Array: delete static_cast<ArrayObj*>(o); break;
    case ValueTag::Object: delete static_cast<ObjectObj*>(o); break;
    case ValueTag::Cell:  delete static_cast<CellObj*>(o); break;
    case ValueTag::Func:  delete static_cast<ClosureObj*>(o); break;
    case ValueTag::Generator: delete static_cast<GeneratorObj*>(o); break;
    default: break;  // no other tag names a heap object
  }
}

// Declared in ir.h and public: vm::bytecode.cc's own instruction-name table
// shares this rather than repeating the eleven arithmetic/compare names in a
// second switch of its own.
const char* name_of(BinOp op) {
  switch (op) {
    case BinOp::Add: return "add";
    case BinOp::Sub: return "sub";
    case BinOp::Mul: return "mul";
    case BinOp::Div: return "div";
    case BinOp::Mod: return "mod";
    case BinOp::Eq:  return "eq";
    case BinOp::Ne:  return "ne";
    case BinOp::Lt:  return "lt";
    case BinOp::Le:  return "le";
    case BinOp::Gt:  return "gt";
    case BinOp::Ge:  return "ge";
    case BinOp::BitAnd: return "bitand";
    case BinOp::BitOr:  return "bitor";
    case BinOp::BitXor: return "bitxor";
    case BinOp::Shl:    return "shl";
    case BinOp::Shr:    return "shr";
  }
  return "?";
}

namespace {

const char* name_of(IntrinsicId id) {
  switch (id) {
    case IntrinsicId::Print:   return "print";
    case IntrinsicId::ReadInt: return "readint";
    case IntrinsicId::Len:     return "len";
    case IntrinsicId::ToStr:   return "tostr";
    case IntrinsicId::TypeOf:  return "typeof";
    case IntrinsicId::ToInt:   return "toint";
    case IntrinsicId::ToDouble: return "todouble";
    case IntrinsicId::FMod:    return "fmod";
    case IntrinsicId::Pow:     return "pow";
    case IntrinsicId::PrintRaw: return "printraw";
    case IntrinsicId::ArrayPush: return "arraypush";
    case IntrinsicId::ArrayPop: return "arraypop";
    case IntrinsicId::ObjectHas: return "objecthas";
    case IntrinsicId::ObjectKeys: return "objectkeys";
    case IntrinsicId::ObjectRemove: return "objectremove";
    case IntrinsicId::ArgCount: return "argcount";
    case IntrinsicId::Same: return "same";
    case IntrinsicId::FnArity: return "fnarity";
    case IntrinsicId::Collect: return "collect";
    case IntrinsicId::HeapStats: return "heapstats";
    case IntrinsicId::GenResume: return "genresume";
    case IntrinsicId::GenReturn: return "genreturn";
  }
  return "?";
}

const char* name_of(VarKind k) {
  switch (k) {
    case VarKind::Local:   return "local";
    case VarKind::Capture: return "capture";
    case VarKind::Cell:    return "cell";
  }
  return "?";
}

struct Verifier {
  const Module& m;
  std::string err;

  bool fail(const std::string& msg) {
    if (err.empty()) err = msg;
    return false;
  }

  bool check_node(NodeId id, const Func& f, int loop_depth, int scope_depth) {
    if (!id.valid() || id.v >= m.nodes.size()) return fail("dangling NodeId");
    const Node& n = m.at(id);

    if (n.pos >= m.positions.size()) return fail("node pos out of range");
    if (n.first_child + n.num_children > m.child_ids.size()) {
      return fail("node children out of range");
    }

    const int want = arity_of(n.tag);
    if (want >= 0 && n.num_children != static_cast<uint32_t>(want)) {
      return fail("wrong arity for tag");
    }

    switch (n.tag) {
      case Tag::Literal:
        if (n.a < 0 || static_cast<size_t>(n.a) >= m.consts.size()) {
          return fail("literal const index out of range");
        }
        break;
      case Tag::VarRef:
      case Tag::Assign: {
        if (n.a < 0 || n.a >= slot_limit(f, static_cast<VarKind>(n.op))) {
          return fail("var index out of range");
        }
        break;
      }
      case Tag::If:
        if (n.num_children != 2 && n.num_children != 3) {
          return fail("If takes 2 or 3 children");
        }
        break;
      // A closure's captures are cells, which outlive the frame that
      // built it. A plain Local cannot be one -- it dies with the frame -- so
      // naming one here is the mistake this rejects, and the front end's cue
      // that the variable needed promoting to a Cell first.
      case Tag::MakeClosure: {
        if (n.a < 0 || static_cast<size_t>(n.a) >= m.funcs.size()) {
          return fail("closure func index out of range");
        }
        if (n.b < 0 || static_cast<size_t>(n.b) >= m.capture_maps.size()) {
          return fail("closure capture map index out of range");
        }
        const auto& cmap = m.capture_maps[n.b];
        if (cmap.size() != static_cast<size_t>(m.funcs[n.a].num_captures)) {
          return fail("capture map length does not match closure");
        }
        for (const CaptureSrc& src : cmap) {
          if (src.from == VarKind::Local) {
            return fail("a closure cannot capture a local; promote it to a cell");
          }
          if (src.index < 0 || src.index >= slot_limit(f, src.from)) {
            return fail("capture map entry out of range in caller frame");
          }
        }
        break;
      }
      case Tag::CallValue:
        if (n.num_children < 1) return fail("CallValue needs a callee");
        break;
      case Tag::Scope:
        if (n.a < 0 || n.a > n.b || n.b > f.num_locals) {
          return fail("scope local range out of range");
        }
        break;
      case Tag::Return:
        if (n.num_children > 1) return fail("Return takes 0 or 1 children");
        break;
      case Tag::Break:
        if (loop_depth == 0) return fail("Break outside a loop body");
        break;
      case Tag::Continue:
        if (loop_depth == 0) return fail("Continue outside a loop body");
        break;
      case Tag::TryCatch:
        if (n.a < 0 || n.a >= f.num_locals) {
          return fail("caught local slot out of range");
        }
        break;
      case Tag::Defer:
        if (scope_depth == 0) {
          return fail("Defer outside a Scope; wrap the region in one");
        }
        break;
      case Tag::CellFresh:
        if (n.a < 0 || n.a >= f.num_cells) {
          return fail("cell index out of range");
        }
        break;
      case Tag::Yield:
        if (!f.is_generator) {
          return fail("Yield outside a generator function");
        }
        break;
      case Tag::ObjectLit:
        if (n.num_children % 2 != 0) {
          return fail("ObjectLit takes key/value pairs");
        }
        break;
      case Tag::Intrinsic: {
        const auto id = static_cast<IntrinsicId>(n.op);
        if (n.num_children != intrinsic_arity(id)) {
          return fail("wrong arity for intrinsic");
        }
        break;
      }
      default:
        break;
    }

    // Operand positions must hold value-producing nodes.
    auto operand = [&](uint32_t i) {
      NodeId c = m.child(id, i);
      if (!c.valid() || c.v >= m.nodes.size()) return fail("dangling child");
      if (!yields_value(m.at(c).tag)) return fail("statement in value position");
      return true;
    };

    switch (n.tag) {
      case Tag::Unary:
      case Tag::Binary:
      case Tag::Intrinsic:
      case Tag::ArrayLit:
      case Tag::ObjectLit:
      case Tag::Index:
      case Tag::SetIndex:
      case Tag::CallValue:  // callee, then args -- all value positions
        for (uint32_t i = 0; i < n.num_children; ++i) {
          if (!operand(i)) return false;
        }
        break;
      case Tag::Assign:
      case Tag::If:
      case Tag::While:
      case Tag::Throw:
      case Tag::Defer:
      case Tag::Yield:
        if (!operand(0)) return false;
        break;
      default:
        break;
    }

    for (uint32_t i = 0; i < n.num_children; ++i) {
      // A While's body is the one edge that opens a loop; its condition is
      // not part of the body, so a Break there names the loop outside it.
      const int child_depth =
          (n.tag == Tag::While && i == 1) ? loop_depth + 1 : loop_depth;
      const int child_scope =
          n.tag == Tag::Scope ? scope_depth + 1 : scope_depth;
      if (!check_node(m.child(id, i), f, child_depth, child_scope)) {
        return false;
      }
    }
    return true;
  }
};

struct Dumper {
  const Module& m;
  std::ostringstream out;

  void indent(int d) { out << std::string(static_cast<size_t>(d) * 2, ' '); }

  void node(NodeId id, int d) {
    const Node& n = m.at(id);
    const SrcPos p = m.pos_of(id);
    indent(d);
    switch (n.tag) {
      case Tag::Literal:
        out << "literal " << m.int_const(id);
        break;
      case Tag::VarRef: {
        auto v = view_varref(m, id);
        out << "varref " << name_of(v.kind) << "[" << v.index << "]";
        break;
      }
      case Tag::Unary:
        out << (static_cast<UnOp>(n.op) == UnOp::BitNot ? "bitnot" : "neg");
        break;
      case Tag::Binary:
        out << name_of(static_cast<BinOp>(n.op));
        break;
      case Tag::Assign: {
        auto v = view_assign(m, id);
        out << "assign " << name_of(v.kind) << "[" << v.index << "]";
        break;
      }
      case Tag::If:     out << "if"; break;
      case Tag::Scope:
        out << "scope local[" << n.a << ".." << n.b << ")";
        break;
      case Tag::Return:   out << "return"; break;
      case Tag::Break:    out << "break"; break;
      case Tag::Continue: out << "continue"; break;
      case Tag::Throw:    out << "throw"; break;
      case Tag::Defer:    out << "defer"; break;
      case Tag::CellFresh:
        out << "cellfresh cell[" << n.a << "]";
        break;
      case Tag::Yield: out << "yield"; break;
      case Tag::TryCatch:
        out << "try caught=local[" << n.a << "]";
        break;
      case Tag::While:  out << "while"; break;
      case Tag::Block:  out << "block"; break;
      case Tag::Intrinsic:
        out << name_of(static_cast<IntrinsicId>(n.op));
        break;
      case Tag::MakeClosure:
        out << "makeclosure " << m.funcs[n.a].name << " #" << n.a
            << " cmap=" << n.b;
        break;
      case Tag::CallValue:
        out << "callvalue";
        break;
      case Tag::ArrayLit:  out << "arraylit"; break;
      case Tag::ObjectLit: out << "objectlit"; break;
      case Tag::Index:    out << "index"; break;
      case Tag::SetIndex: out << "setindex"; break;
    }
    out << "  @" << p.line << ":" << p.col << "\n";
    for (uint32_t i = 0; i < n.num_children; ++i) node(m.child(id, i), d + 1);
  }
};

}  // namespace

std::optional<std::string> verify(const Module& m) {
  if (m.funcs.empty()) return std::string("module has no entry function");
  Verifier v{m, {}};
  for (const Func& f : m.funcs) {
    if (static_cast<size_t>(f.num_locals) != f.local_names.size() ||
        static_cast<size_t>(f.num_captures) != f.capture_names.size()) {
      return std::string("func name table does not match its slot count");
    }
    if (!v.check_node(f.body, f, 0, 0)) return v.err;
  }
  if (!m.funcs[0].capture_names.empty()) {
    return std::string("entry function must capture nothing");
  }
  if (m.funcs[0].is_generator) {
    return std::string("entry function cannot be a generator");
  }
  return std::nullopt;
}

std::string to_string(const Module& m) {
  Dumper d{m, {}};
  for (size_t i = 0; i < m.funcs.size(); ++i) {
    const Func& f = m.funcs[i];
    d.out << "func #" << i << " " << f.name << "  locals=" << f.num_locals
          << " captures=" << f.num_captures;
    if (!f.capture_names.empty()) {
      d.out << " [";
      for (size_t j = 0; j < f.capture_names.size(); ++j) {
        d.out << (j ? " " : "") << f.capture_names[j];
      }
      d.out << "]";
    }
    d.out << "\n";
    d.node(f.body, 1);
  }
  for (size_t i = 0; i < m.capture_maps.size(); ++i) {
    d.out << "cmap " << i << ":";
    for (const CaptureSrc& s : m.capture_maps[i]) {
      d.out << " " << name_of(s.from) << "[" << s.index << "]";
    }
    d.out << "\n";
  }
  return d.out.str();
}

}  // namespace coreir
