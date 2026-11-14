// Optimisation and object-code generation shared by the translators: see lift_internal.h.
#include <atomic>
#include <thread>

#include "lift_internal.h"

#pragma warning(push, 0)
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>
#pragma warning(pop)

namespace hp2::detail {

void initialise_llvm() {
  static const bool initialised = [] {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();
    return true;
  }();
  (void)initialised;
}

std::unique_ptr<llvm::TargetMachine> make_target(const std::string& cpu) {
  initialise_llvm();
  if (cpu.empty()) {
    auto builder = check(llvm::orc::JITTargetMachineBuilder::detectHost(), "detecting the host CPU");
    builder.setCodeGenOptLevel(llvm::CodeGenOptLevel::Aggressive);
    return check(builder.createTargetMachine(), "creating the target machine");
  }
  const llvm::Triple triple(llvm::sys::getProcessTriple());
  std::string error;
  const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, error);
  if (!target) fail("LLVM: no target for {}: {}", triple.str(), error);
  llvm::TargetOptions options;
  std::unique_ptr<llvm::TargetMachine> tm(target->createTargetMachine(triple, cpu, "", options, llvm::Reloc::PIC_,
                                                                      std::nullopt, llvm::CodeGenOptLevel::Aggressive));
  if (!tm) fail("LLVM: cannot create a target machine for CPU {}", cpu);
  return tm;
}

void optimise(llvm::Module& module, llvm::TargetMachine* target) {
  llvm::PassBuilder passes(target);
  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;
  passes.registerModuleAnalyses(mam);
  passes.registerCGSCCAnalyses(cgam);
  passes.registerFunctionAnalyses(fam);
  passes.registerLoopAnalyses(lam);
  passes.crossRegisterProxies(lam, fam, cgam, mam);
  passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2).run(module, mam);
}

std::vector<std::vector<char>> compile_modules(const std::vector<llvm::Module*>& modules, const std::string& cpu,
                                               unsigned threads) {
  std::vector<std::vector<char>> objects(modules.size());
  std::vector<std::string> errors(modules.size());
  std::atomic<std::size_t> next{0};
  auto worker = [&] {
    for (std::size_t i; (i = next.fetch_add(1)) < modules.size();) {
      try {
        // A target machine is not shared between threads.
        const std::unique_ptr<llvm::TargetMachine> tm = make_target(cpu);
        llvm::Module& m = *modules[i];
        m.setDataLayout(tm->createDataLayout());
        m.setTargetTriple(tm->getTargetTriple());
        if (llvm::verifyModule(m, &llvm::errs())) fail("a translated module does not verify");
        optimise(m, tm.get());
        llvm::SmallVector<char, 0> buffer;
        llvm::raw_svector_ostream out(buffer);
        llvm::legacy::PassManager pm;
        if (tm->addPassesToEmitFile(pm, out, nullptr, llvm::CodeGenFileType::ObjectFile))
          fail("LLVM: the target cannot emit object code");
        pm.run(m);
        objects[i].assign(buffer.begin(), buffer.end());
      } catch (const std::exception& e) {
        errors[i] = e.what();
      }
    }
  };
  if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
  threads = std::min<unsigned>(threads, static_cast<unsigned>(modules.size()));
  std::vector<std::thread> pool;
  for (unsigned t = 1; t < threads; ++t) pool.emplace_back(worker);
  worker();
  for (std::thread& t : pool) t.join();
  for (const std::string& e : errors)
    if (!e.empty()) fail("{}", e);
  return objects;
}

}  // namespace hp2::detail
