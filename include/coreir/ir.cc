#include "coreir/ir.h"

#include <sstream>

#include "coreir/value.h"

namespace coreir {

// SPIKE: see coreir/value.h. Defined here rather than in a value.cc of its
// own so the spike adds no new translation unit -- culebra's CMakeLists names
// cpp-vmlib's sources one by one, in two separate places.
int64_t g_live_heap_objects = 0;

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
  }
  return "?";
}

namespace {

const char* name_of(IntrinsicId id) {
  switch (id) {
    case IntrinsicId::Print:   return "print";
    case IntrinsicId::ReadInt: return "readint";
  }
  return "?";
}

const char* name_of(VarKind k) {
  return k == VarKind::Local ? "local" : "capture";
}

struct Verifier {
  const Module& m;
  std::string err;

  bool fail(const std::string& msg) {
    if (err.empty()) err = msg;
    return false;
  }

  bool check_node(NodeId id, const Func& f) {
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
        const auto k = static_cast<VarKind>(n.op);
        const int32_t limit =
            k == VarKind::Local ? f.num_locals : f.num_captures;
        if (n.a < 0 || n.a >= limit) return fail("var index out of range");
        break;
      }
      case Tag::If:
        if (n.num_children != 2 && n.num_children != 3) {
          return fail("If takes 2 or 3 children");
        }
        break;
      case Tag::Call: {
        if (n.a < 0 || static_cast<size_t>(n.a) >= m.funcs.size()) {
          return fail("call func index out of range");
        }
        if (n.b < 0 || static_cast<size_t>(n.b) >= m.capture_maps.size()) {
          return fail("call capture map index out of range");
        }
        const auto& cmap = m.capture_maps[n.b];
        const Func& callee = m.funcs[n.a];
        if (cmap.size() != static_cast<size_t>(callee.num_captures)) {
          return fail("capture map length does not match callee");
        }
        // Every forwarding entry must be resolvable in this frame.
        for (const CaptureSrc& src : cmap) {
          const int32_t limit = src.from == VarKind::Local ? f.num_locals
                                                           : f.num_captures;
          if (src.index < 0 || src.index >= limit) {
            return fail("capture map entry out of range in caller frame");
          }
        }
        break;
      }
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
      case Tag::Call:  // args..., empty for PL/0 but part of the documented
                       // shape (a future frontend with call arguments should
                       // not need this switch touched to be checked).
        for (uint32_t i = 0; i < n.num_children; ++i) {
          if (!operand(i)) return false;
        }
        break;
      case Tag::Assign:
      case Tag::If:
      case Tag::While:
        if (!operand(0)) return false;
        break;
      default:
        break;
    }

    for (uint32_t i = 0; i < n.num_children; ++i) {
      if (!check_node(m.child(id, i), f)) return false;
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
        out << "neg";
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
      case Tag::While:  out << "while"; break;
      case Tag::Block:  out << "block"; break;
      case Tag::Call: {
        auto v = view_call(m, id);
        out << "call " << m.funcs[v.func].name << " #" << v.func
            << " cmap=" << v.capture_map;
        break;
      }
      case Tag::Intrinsic:
        out << name_of(static_cast<IntrinsicId>(n.op));
        break;
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
    if (!v.check_node(f.body, f)) return v.err;
  }
  if (!m.funcs[0].capture_names.empty()) {
    return std::string("entry function must capture nothing");
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
