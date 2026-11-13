// Is the LLVM built by tools/build-llvm.ps1 usable from this project (MSVC,
// static C runtime)? Builds a function in LLVM's intermediate form, optimises
// it, compiles it to x64 in memory and calls it.
#include "minitest.h"

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/TargetSelect.h>

TEST(llvm_compiles_and_runs_a_function) {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();

  auto context = std::make_unique<llvm::LLVMContext>();
  auto module = std::make_unique<llvm::Module>("smoke", *context);
  llvm::IRBuilder<> b(*context);
  // int sum_to(int n) { int s = 0; for (int i = 1; i <= n; ++i) s += i; return s; }
  auto* i32 = b.getInt32Ty();
  auto* fn = llvm::Function::Create(llvm::FunctionType::get(i32, {i32}, false), llvm::Function::ExternalLinkage,
                                    "sum_to", *module);
  auto* entry = llvm::BasicBlock::Create(*context, "entry", fn);
  auto* loop = llvm::BasicBlock::Create(*context, "loop", fn);
  auto* done = llvm::BasicBlock::Create(*context, "done", fn);
  b.SetInsertPoint(entry);
  b.CreateCondBr(b.CreateICmpSLT(fn->getArg(0), b.getInt32(1)), done, loop);
  b.SetInsertPoint(loop);
  auto* i = b.CreatePHI(i32, 2);
  auto* s = b.CreatePHI(i32, 2);
  auto* s2 = b.CreateAdd(s, i);
  auto* i2 = b.CreateAdd(i, b.getInt32(1));
  i->addIncoming(b.getInt32(1), entry);
  i->addIncoming(i2, loop);
  s->addIncoming(b.getInt32(0), entry);
  s->addIncoming(s2, loop);
  b.CreateCondBr(b.CreateICmpSGT(i2, fn->getArg(0)), done, loop);
  b.SetInsertPoint(done);
  auto* result = b.CreatePHI(i32, 2);
  result->addIncoming(b.getInt32(0), entry);
  result->addIncoming(s2, loop);
  b.CreateRet(result);
  REQUIRE(!llvm::verifyFunction(*fn, &llvm::errs()));

  // The standard -O2 pipeline.
  llvm::PassBuilder passes;
  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;
  passes.registerModuleAnalyses(mam);
  passes.registerCGSCCAnalyses(cgam);
  passes.registerFunctionAnalyses(fam);
  passes.registerLoopAnalyses(lam);
  passes.crossRegisterProxies(lam, fam, cgam, mam);
  passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2).run(*module, mam);

  auto jit = llvm::orc::LLJITBuilder().create();
  REQUIRE(static_cast<bool>(jit));
  REQUIRE(!(*jit)->addIRModule(llvm::orc::ThreadSafeModule(std::move(module), std::move(context))));
  auto address = (*jit)->lookup("sum_to");
  REQUIRE(static_cast<bool>(address));
  auto* sum_to = address->toPtr<int (*)(int)>();
  CHECK_EQ(sum_to(100), 5050);
  CHECK_EQ(sum_to(0), 0);
}

MINITEST_MAIN()
