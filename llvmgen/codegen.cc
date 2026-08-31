#include "llvmgen/codegen.h"

#include <cstdlib>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include "coreir/ir.h"

using namespace llvm;

namespace llvmgen {
namespace {

std::string chunk_symbol(size_t i) { return "pl0_chunk_" + std::to_string(i); }

struct Gen {
  const vm::Program& p;
  LLVMContext& ctx;
  std::unique_ptr<Module> mod;
  IRBuilder<> b;

  StructType* slot_ty = nullptr;
  FunctionCallee rt_out, rt_in, rt_fail;
  std::vector<Function*> fns;

  // Per-chunk state.
  Function* fn = nullptr;
  Value* locals = nullptr;      // ptr to [N x %Slot]
  ArrayType* locals_ty = nullptr;
  std::vector<Value*> regs;     // one alloca i64 each; mem2reg flattens them

  Gen(const vm::Program& program, LLVMContext& c)
      : p(program), ctx(c), mod(std::make_unique<Module>("pl0", c)), b(c) {}

  Type* i64() { return Type::getInt64Ty(ctx); }
  Type* i8() { return Type::getInt8Ty(ctx); }
  Type* ptr() { return PointerType::getUnqual(ctx); }
  Constant* k64(int64_t v) { return ConstantInt::get(i64(), v, true); }

  void declare_runtime() {
    slot_ty = StructType::create(ctx, {i64(), i8()}, "Slot");
    rt_out = mod->getOrInsertFunction(
        "pl0_rt_out", FunctionType::get(Type::getVoidTy(ctx), {i64()}, false));
    rt_in = mod->getOrInsertFunction(
        "pl0_rt_in", FunctionType::get(i64(), {i64(), i64()}, false));
    auto* fail_ty =
        FunctionType::get(Type::getVoidTy(ctx), {ptr(), i64(), i64()}, false);
    rt_fail = mod->getOrInsertFunction("pl0_rt_fail", fail_ty);
    if (auto* f = dyn_cast<Function>(rt_fail.getCallee())) {
      f->addFnAttr(Attribute::NoReturn);
    }
  }

  void declare_chunks() {
    fns.resize(p.chunks.size());
    for (size_t i = 0; i < p.chunks.size(); ++i) {
      std::vector<Type*> params(
          static_cast<size_t>(p.chunks[i].num_captures), ptr());
      auto* ty = FunctionType::get(Type::getVoidTy(ctx), params, false);
      fns[i] = Function::Create(ty, Function::ExternalLinkage,
                                chunk_symbol(i), mod.get());
    }
  }

  Value* slot_ptr(int32_t kind, int32_t index) {
    if (static_cast<coreir::VarKind>(kind) == coreir::VarKind::Local) {
      return b.CreateInBoundsGEP(locals_ty, locals,
                                 {k64(0), k64(index)}, "slot");
    }
    return fn->getArg(static_cast<unsigned>(index));
  }

  void trap(const char* msg, coreir::SrcPos sp) {
    Value* s = b.CreateGlobalString(msg, "msg");
    b.CreateCall(rt_fail, {s, k64(sp.line), k64(sp.col)});
    b.CreateUnreachable();
  }

  Value* load_reg(int32_t r) { return b.CreateLoad(i64(), regs[r]); }
  void store_reg(int32_t r, Value* v) { b.CreateStore(v, regs[r]); }

  void gen_chunk(size_t ci) {
    const vm::Chunk& ch = p.chunks[ci];
    fn = fns[ci];

    BasicBlock* entry = BasicBlock::Create(ctx, "entry", fn);
    b.SetInsertPoint(entry);

    locals_ty = ArrayType::get(slot_ty, static_cast<uint64_t>(ch.num_locals));
    locals = b.CreateAlloca(locals_ty, nullptr, "locals");
    // Zeroing clears every inited flag, so an unassigned variable is caught
    // rather than read as whatever the stack held.
    b.CreateStore(Constant::getNullValue(locals_ty), locals);

    regs.clear();
    for (int32_t i = 0; i < ch.num_regs; ++i) {
      regs.push_back(b.CreateAlloca(i64(), nullptr, "r" + std::to_string(i)));
    }

    // Block leaders: the entry of the code proper, every jump target, and
    // whatever follows a transfer of control.
    std::set<int32_t> leaders{0};
    for (size_t i = 0; i < ch.code.size(); ++i) {
      const vm::Insn& in = ch.code[i];
      const int32_t next = static_cast<int32_t>(i) + 1;
      if (in.op == vm::Op::Jump) {
        leaders.insert(in.a);
        leaders.insert(next);
      } else if (in.op == vm::Op::JumpIfFalse) {
        leaders.insert(in.b);
        leaders.insert(next);
      } else if (in.op == vm::Op::Ret) {
        leaders.insert(next);
      }
    }
    std::vector<BasicBlock*> bbs(ch.code.size(), nullptr);
    for (int32_t l : leaders) {
      if (l >= 0 && static_cast<size_t>(l) < ch.code.size()) {
        bbs[l] = BasicBlock::Create(ctx, "L" + std::to_string(l), fn);
      }
    }

    if (ch.code.empty()) {
      b.CreateRetVoid();
      return;
    }
    b.CreateBr(bbs[0]);
    b.SetInsertPoint(bbs[0]);

    for (size_t i = 0; i < ch.code.size(); ++i) {
      if (i > 0 && bbs[i]) {
        if (!b.GetInsertBlock()->getTerminator()) b.CreateBr(bbs[i]);
        b.SetInsertPoint(bbs[i]);
      }
      gen_insn(ch, i, bbs);
    }
    if (!b.GetInsertBlock()->getTerminator()) b.CreateRetVoid();

    // Leaders nothing branches to (dead code after an unconditional jump)
    // still need a terminator for the verifier.
    for (BasicBlock& bb : *fn) {
      if (!bb.getTerminator()) {
        b.SetInsertPoint(&bb);
        b.CreateRetVoid();
      }
    }

    std::string err;
    raw_string_ostream os(err);
    if (verifyFunction(*fn, &os)) {
      errs() << "llvmgen: invalid function " << chunk_symbol(ci) << ":\n"
             << err;
      std::exit(70);
    }
  }

  void gen_insn(const vm::Chunk& ch, size_t i,
                const std::vector<BasicBlock*>& bbs) {
    const vm::Insn& in = ch.code[i];
    const coreir::SrcPos sp = p.positions[ch.code_pos[i]];

    switch (in.op) {
      case vm::Op::LoadConst:
        store_reg(in.a, k64(p.consts[in.b].bits));
        break;

      case vm::Op::Neg:
        // No nsw: the value model wraps, and nsw would make INT64_MIN poison
        // in this lane alone.
        store_reg(in.a, b.CreateNeg(load_reg(in.b)));
        break;

      case vm::Op::Add:
        store_reg(in.a, b.CreateAdd(load_reg(in.b), load_reg(in.c)));
        break;
      case vm::Op::Sub:
        store_reg(in.a, b.CreateSub(load_reg(in.b), load_reg(in.c)));
        break;
      case vm::Op::Mul:
        store_reg(in.a, b.CreateMul(load_reg(in.b), load_reg(in.c)));
        break;

      case vm::Op::Div:
      case vm::Op::Mod: {
        Value* l = load_reg(in.b);
        Value* r = load_reg(in.c);
        BasicBlock* zero = BasicBlock::Create(ctx, "div.zero", fn);
        BasicBlock* chk = BasicBlock::Create(ctx, "div.chk", fn);
        b.CreateCondBr(b.CreateICmpEQ(r, k64(0)), zero, chk);
        b.SetInsertPoint(zero);
        trap("divide by zero", sp);
        b.SetInsertPoint(chk);
        // INT64_MIN / -1 is poison in LLVM exactly as it is UB in C++, and
        // wrapping arithmetic puts INT64_MIN in reach.
        Value* ovf = b.CreateAnd(
            b.CreateICmpEQ(l, k64(INT64_MIN)), b.CreateICmpEQ(r, k64(-1)));
        BasicBlock* over = BasicBlock::Create(ctx, "div.ovf", fn);
        BasicBlock* ok = BasicBlock::Create(ctx, "div.ok", fn);
        b.CreateCondBr(ovf, over, ok);
        b.SetInsertPoint(over);
        trap("division overflow", sp);
        b.SetInsertPoint(ok);
        store_reg(in.a, in.op == vm::Op::Div ? b.CreateSDiv(l, r)
                                             : b.CreateSRem(l, r));
        break;
      }

      case vm::Op::Eq: case vm::Op::Ne: case vm::Op::Lt:
      case vm::Op::Le: case vm::Op::Gt: case vm::Op::Ge: {
        Value* l = load_reg(in.b);
        Value* r = load_reg(in.c);
        Value* c = nullptr;
        switch (in.op) {
          case vm::Op::Eq: c = b.CreateICmpEQ(l, r); break;
          case vm::Op::Ne: c = b.CreateICmpNE(l, r); break;
          case vm::Op::Lt: c = b.CreateICmpSLT(l, r); break;
          case vm::Op::Le: c = b.CreateICmpSLE(l, r); break;
          case vm::Op::Gt: c = b.CreateICmpSGT(l, r); break;
          default:         c = b.CreateICmpSGE(l, r); break;
        }
        // Comparisons are i64 0/1 like every other value, not i1.
        store_reg(in.a, b.CreateZExt(c, i64()));
        break;
      }

      case vm::Op::LoadVar: {
        Value* s = slot_ptr(in.b, in.c);
        Value* flag = b.CreateLoad(i8(), b.CreateStructGEP(slot_ty, s, 1));
        BasicBlock* bad = BasicBlock::Create(ctx, "var.uninit", fn);
        BasicBlock* good = BasicBlock::Create(ctx, "var.ok", fn);
        b.CreateCondBr(b.CreateICmpNE(flag, ConstantInt::get(i8(), 0)), good,
                       bad);
        b.SetInsertPoint(bad);
        {
          const auto& names =
              static_cast<coreir::VarKind>(in.b) == coreir::VarKind::Local
                  ? ch.local_names
                  : ch.capture_names;
          const std::string msg =
              "uninitialized variable '" + names[in.c] + "'";
          trap(msg.c_str(), sp);
        }
        b.SetInsertPoint(good);
        store_reg(in.a,
                  b.CreateLoad(i64(), b.CreateStructGEP(slot_ty, s, 0)));
        break;
      }

      case vm::Op::StoreVar: {
        Value* s = slot_ptr(in.a, in.b);
        b.CreateStore(load_reg(in.c), b.CreateStructGEP(slot_ty, s, 0));
        b.CreateStore(ConstantInt::get(i8(), 1),
                      b.CreateStructGEP(slot_ty, s, 1));
        break;
      }

      case vm::Op::Jump:
        b.CreateBr(bbs[in.a]);
        break;

      case vm::Op::JumpIfFalse: {
        Value* c = b.CreateICmpNE(load_reg(in.a), k64(0));
        b.CreateCondBr(c, bbs[i + 1], bbs[in.b]);
        break;
      }

      case vm::Op::Call: {
        // The caller resolves the callee's captures out of its own frame --
        // the forwarding table belongs to the call site, so a self-recursive
        // call forwards its own captures through unchanged.
        std::vector<Value*> args;
        for (const coreir::CaptureSrc& src : p.capture_maps[in.b]) {
          args.push_back(slot_ptr(static_cast<int32_t>(src.from), src.index));
        }
        b.CreateCall(fns[in.a], args);
        break;
      }

      case vm::Op::Out:
        b.CreateCall(rt_out, {load_reg(in.a)});
        break;

      case vm::Op::In:
        store_reg(in.a, b.CreateCall(rt_in, {k64(sp.line), k64(sp.col)}));
        break;

      case vm::Op::Ret:
        b.CreateRetVoid();
        break;
    }
  }

  void gen_all() {
    declare_runtime();
    declare_chunks();
    for (size_t i = 0; i < p.chunks.size(); ++i) gen_chunk(i);
  }
};

}  // namespace

std::string emit_ir(const vm::Program& p) {
  LLVMContext ctx;
  Gen g(p, ctx);
  g.gen_all();
  std::string out;
  raw_string_ostream os(out);
  g.mod->print(os, nullptr);
  return out;
}

void run(const vm::Program& p) {
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();

  auto ctx = std::make_unique<LLVMContext>();
  Gen g(p, *ctx);
  g.gen_all();

  auto jit = cantFail(orc::LLJITBuilder().create());
  // pl0_rt_out / pl0_rt_in / pl0_rt_fail resolve straight out of this process,
  // so the JIT lane calls the very same functions the other two lanes call.
  jit->getMainJITDylib().addGenerator(
      cantFail(orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
          jit->getDataLayout().getGlobalPrefix())));
  cantFail(jit->addIRModule(
      orc::ThreadSafeModule(std::move(g.mod), std::move(ctx))));

  auto addr = cantFail(jit->lookup(chunk_symbol(0)));
  addr.toPtr<void (*)()>()();
}

}  // namespace llvmgen
