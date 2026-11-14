// See hp2/package.h.
#include "hp2/package.h"

#include <chrono>
#include <fstream>

#include "hp2/discover.h"
#include "hp2/lift.h"
#include "hp2/vu.h"
#include "hp2/vulift.h"
#include "lift_internal.h"

#pragma warning(push, 0)
#include <lld/Common/Driver.h>
#pragma warning(pop)

LLD_HAS_DRIVER(coff)

namespace hp2 {

using namespace detail;

namespace {

double seconds_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

StateLayout state_layout(const Hp2CodeLayout& c) {
  StateLayout l;
  l.pc = c.pc;
  l.gpr = c.gpr;
  l.gpr_stride = c.gpr_stride;
  l.hi = c.hi;
  l.lo = c.lo;
  l.hi1 = c.hi1;
  l.lo1 = c.lo1;
  l.fpr = c.fpr;
  l.fcsr = c.fcsr;
  l.fpacc = c.fpacc;
  l.vf = c.vf;
  l.cycle_quota = c.cycle_quota;
  l.has_exception = c.has_exception;
  l.vacc = c.vacc;
  l.vi = c.vi;
  l.vq = c.vq;
  l.vi_i = c.vi_i;
  l.vr = c.vr;
  l.vcf = c.vcf;
  l.vmf = c.vmf;
  l.vdf = c.vdf;
  l.vt = c.vt;
  l.pipe_mac = c.pipe_mac;
  l.pipe_sticky = c.pipe_sticky;
  l.pipe_clip = c.pipe_clip;
  l.pipe_time = c.pipe_time;
  l.pipe_q = c.pipe_q;
  return l;
}

// The module holding the table the launcher reads, and the two pieces of
// runtime support the generated code may refer to on Windows without a C
// runtime: _fltused (marks floating-point use) and __chkstk (stack probe for
// frames over 4 KB; the usual implementation, as in LLVM's compiler-rt).
class TableBuilder {
 public:
  explicit TableBuilder(llvm::LLVMContext& ctx) : ctx_(ctx), m_(std::make_unique<llvm::Module>("hp2_table", ctx)) {
    ptr_ = llvm::PointerType::get(ctx_, 0);
    i32_ = llvm::Type::getInt32Ty(ctx_);
    entry_ = llvm::StructType::create(ctx_, {i32_, i32_, ptr_}, "Hp2CodeEntry");
    image_ = llvm::StructType::create(ctx_, {i32_, i32_, ptr_, ptr_, i32_, i32_, ptr_, ptr_}, "Hp2VuImageTable");
  }

  llvm::Module& module() { return *m_; }
  std::unique_ptr<llvm::Module> take() { return std::move(m_); }

  llvm::Constant* function(const std::string& name) {
    if (llvm::Function* f = m_->getFunction(name)) return f;
    return llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {}, false),
                                  llvm::Function::ExternalLinkage, name, *m_);
  }
  llvm::Constant* entries(const std::vector<std::pair<std::uint32_t, std::string>>& list, const std::string& name) {
    std::vector<llvm::Constant*> items;
    for (const auto& [address, symbol] : list)
      items.push_back(llvm::ConstantStruct::get(entry_, {c32(address), c32(0), function(symbol)}));
    return array(entry_, items, name);
  }
  llvm::Constant* words(const std::vector<std::uint32_t>& list, const std::string& name) {
    std::vector<llvm::Constant*> items;
    for (std::uint32_t w : list) items.push_back(c32(w));
    return array(i32_, items, name);
  }
  llvm::Constant* image(std::uint32_t address, const std::vector<std::uint32_t>& microcode, const std::string& code,
                        const std::vector<std::uint32_t>& starts,
                        const std::vector<std::pair<std::uint32_t, std::string>>& entry_list, std::size_t index) {
    return llvm::ConstantStruct::get(
        image_, {c32(address), c32(static_cast<std::uint32_t>(microcode.size())),
                 words(microcode, std::format("vu_words_{}", index)), function(code),
                 c32(static_cast<std::uint32_t>(starts.size())), c32(static_cast<std::uint32_t>(entry_list.size())),
                 words(starts, std::format("vu_starts_{}", index)), entries(entry_list, std::format("vu_entries_{}", index))});
  }
  void table(const std::array<std::uint8_t, 32>& sha, std::uint64_t fingerprint, const std::string& cpu,
             std::uint32_t function_count, llvm::Constant* functions, std::uint32_t entry_count,
             llvm::Constant* entry_table, const std::vector<llvm::Constant*>& images) {
    auto* i8 = llvm::Type::getInt8Ty(ctx_);
    std::vector<llvm::Constant*> hash, name;
    for (std::uint8_t b : sha) hash.push_back(llvm::ConstantInt::get(i8, b));
    for (std::size_t i = 0; i < 16; ++i) name.push_back(llvm::ConstantInt::get(i8, i < cpu.size() && i < 15 ? cpu[i] : 0));
    auto* hash_t = llvm::ArrayType::get(i8, 32);
    auto* name_t = llvm::ArrayType::get(i8, 16);
    auto* table_t = llvm::StructType::create(
        ctx_, {i32_, i32_, hash_t, llvm::Type::getInt64Ty(ctx_), name_t, i32_, i32_, ptr_, ptr_, i32_, i32_, ptr_, ptr_},
        "Hp2CodeTable");
    llvm::Constant* value = llvm::ConstantStruct::get(
        table_t, {c32(kCodeTableMagic), c32(kCodeTableVersion), llvm::ConstantArray::get(hash_t, hash),
                  llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), fingerprint), llvm::ConstantArray::get(name_t, name),
                  c32(function_count), c32(entry_count), functions, entry_table,
                  c32(static_cast<std::uint32_t>(images.size())), c32(0), array(image_, images, "vu_images"),
                  function("hp2_dispatch")});
    auto* g = new llvm::GlobalVariable(*m_, table_t, true, llvm::GlobalValue::ExternalLinkage, value, kCodeTableSymbol);
    g->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
    new llvm::GlobalVariable(*m_, i32_, false, llvm::GlobalValue::ExternalLinkage, c32(0x9875), "_fltused");
    m_->appendModuleInlineAsm(std::string(
        ".text\n"
        ".globl __chkstk\n"
        "__chkstk:\n"
        "  pushq %rcx\n"
        "  pushq %rax\n"
        "  cmpq $0x1000, %rax\n"
        "  leaq 24(%rsp), %rcx\n"
        "  jb 1f\n"
        "2:\n"
        "  subq $0x1000, %rcx\n"
        "  testq %rcx, (%rcx)\n"
        "  subq $0x1000, %rax\n"
        "  cmpq $0x1000, %rax\n"
        "  ja 2b\n"
        "1:\n"
        "  subq %rax, %rcx\n"
        "  testq %rcx, (%rcx)\n"
        "  popq %rax\n"
        "  popq %rcx\n"
        "  retq\n"));
  }

 private:
  llvm::LLVMContext& ctx_;
  std::unique_ptr<llvm::Module> m_;
  llvm::PointerType* ptr_ = nullptr;
  llvm::Type* i32_ = nullptr;
  llvm::StructType *entry_ = nullptr, *image_ = nullptr;

  llvm::Constant* c32(std::uint32_t v) { return llvm::ConstantInt::get(i32_, v); }
  llvm::Constant* array(llvm::Type* type, const std::vector<llvm::Constant*>& items, const std::string& name) {
    if (items.empty()) return llvm::ConstantPointerNull::get(ptr_);
    auto* t = llvm::ArrayType::get(type, items.size());
    return new llvm::GlobalVariable(*m_, t, true, llvm::GlobalValue::PrivateLinkage, llvm::ConstantArray::get(t, items),
                                    name);
  }
};

void write_bytes(const fs::path& path, const std::vector<char>& bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!out) fail("cannot write '{}'", to_utf8(path));
}

}  // namespace

std::string code_package_identity(const std::array<std::uint8_t, 32>& elf_sha256, const Hp2CodeLayout& layout,
                                  std::string_view cpu) {
  return std::format("format {} elf {} layout {:016X} cpu {}", kCodeTableVersion, Sha256::hex(elf_sha256),
                     code_layout_fingerprint(layout), cpu);
}

CodePackageResult build_code_package(ByteView elf_bytes, const Hp2CodeLayout& layout, const fs::path& dll,
                                     const CodePackageOptions& options) {
  auto log = [&](const std::string& line) {
    if (options.log) options.log(line);
  };
  CodePackageResult r;
  const auto t0 = std::chrono::steady_clock::now();
  const ElfImage elf = ElfImage::parse(Bytes(elf_bytes.begin(), elf_bytes.end()));
  std::array<std::uint8_t, 32> sha{};
  {
    Sha256 h;
    h.update(elf_bytes);
    sha = h.finish();
  }
  const Discovery d = discover(elf);
  const StateLayout state = state_layout(layout);

  // ---- translation ----
  Translator ee(elf, d, state, layout.ram_size, options.threads);
  r.functions_found = d.functions.size();
  for (const Function& f : d.functions)
    if (auto why = ee.add_function(f.start)) ++r.refusals[*why];
  r.functions_translated = ee.size();
  r.ee_entries = ee.entries();

  VuLayout vl;
  vl.delayed_jump = layout.delayed_jump;
  vl.fmac_write = layout.fmac_write;
  vl.saved_next_index = layout.saved_next_index;
  vl.saved_next_value = layout.saved_next_value;
  vl.xgkick_address = layout.xgkick_address;
  VuConfig vc;
  vc.micro_size = layout.micro_size;
  vc.data_mask = layout.data_mask;
  vc.addr_top = layout.addr_top;
  vc.addr_xgkick = layout.addr_xgkick;
  vc.addr_itop = layout.addr_itop;
  vc.exception_dbit = layout.exception_dbit;
  vc.exception_tbit = layout.exception_tbit;
  vc.exception_ebit = layout.exception_ebit;
  vc.quota_done = layout.quota_done;
  vc.invalid_pc = layout.invalid_pc;
  VuTranslator vu(state, vl, vc);
  for (vu::MicroImage& im : vu::microcode_images(elf, layout.micro_size)) vu.add_image(im.vu_address, std::move(im.words));
  r.translate_seconds = seconds_since(t0);
  log(std::format("Translated {} of {} game functions and {} microcode image(s) in {:.1f} s; compiling for {} ...",
                  r.functions_translated, r.functions_found, vu.images(), r.translate_seconds, options.cpu));

  // ---- compilation (parallel) ----
  const auto t1 = std::chrono::steady_clock::now();
  std::vector<std::vector<char>> objects = ee.emit_objects(options.cpu);
  for (auto& o : vu.emit_objects(options.cpu, options.threads)) objects.push_back(std::move(o));
  r.vu_images = vu.images();
  r.vu_block_starts = vu.stats().blocks;
  r.vu_entries = vu.entry_functions();
  {
    llvm::LLVMContext ctx;
    TableBuilder tb(ctx);
    std::vector<std::pair<std::uint32_t, std::string>> functions, entries;
    for (std::uint32_t start : ee.functions()) functions.emplace_back(start, Translator::symbol(start));
    for (const auto& [address, start] : ee.entry_functions()) entries.emplace_back(address, Translator::symbol(start));
    std::vector<llvm::Constant*> images;
    for (std::size_t i = 0; i < vu.images(); ++i) {
      std::vector<std::pair<std::uint32_t, std::string>> list(vu.entry_symbols(i).begin(), vu.entry_symbols(i).end());
      images.push_back(tb.image(vu.image_address(i), vu.image_words(i), VuTranslator::image_symbol(i), vu.block_starts(i),
                                list, i));
    }
    tb.table(sha, code_layout_fingerprint(layout), options.cpu, static_cast<std::uint32_t>(functions.size()),
             tb.entries(functions, "functions"), static_cast<std::uint32_t>(entries.size()), tb.entries(entries, "entries"),
             images);
    std::unique_ptr<llvm::Module> table = tb.take();
    objects.push_back(compile_modules({table.get()}, options.cpu, 1)[0]);
  }
  r.compile_seconds = seconds_since(t1);
  log(std::format("Compiled {} object files in {:.1f} s; linking ...", objects.size(), r.compile_seconds));

  // ---- link ----
  const auto t2 = std::chrono::steady_clock::now();
  const fs::path dir = dll.parent_path();
  const fs::path work = dir / "hp2code-objects";  // our own temporary files, removed after the link
  fs::create_directories(work);
  std::vector<std::string> args = {"lld-link", "/dll", "/noentry", "/nodefaultlib", "/machine:x64", "/opt:ref",
                                   "/opt:noicf", "/Brepro", "/nologo", "/noimplib"};
  const fs::path partial = fs::path(dll).concat(".partial");
  args.push_back("/out:" + to_utf8(partial));
  for (std::size_t i = 0; i < objects.size(); ++i) {
    const fs::path obj = work / std::format("part{}.obj", i);
    write_bytes(obj, objects[i]);
    args.push_back(to_utf8(obj));
  }
  std::vector<const char*> argv;
  for (const std::string& a : args) argv.push_back(a.c_str());
  std::string out_text, err_text;
  llvm::raw_string_ostream out(out_text), err(err_text);
  const lld::Result linked = lld::lldMain(argv, out, err, {{lld::WinLink, &lld::coff::link}});
  out.flush();
  err.flush();
  if (linked.retCode != 0) fail("linking the translated code failed: {}{}", err_text, out_text);
  std::error_code ec;
  fs::remove_all(work, ec);
  fs::rename(partial, dll);
  r.link_seconds = seconds_since(t2);
  r.dll_bytes = fs::file_size(dll);

  std::size_t refused = 0;
  for (const auto& [why, n] : r.refusals) refused += n;
  r.summary = std::format(
      "{}\n"
      "game functions: {} found, {} translated, {} not translated (the reference core runs those); {} re-entry places\n"
      "VU1 microcode: {} image(s), {} block starts, {} other entries\n"
      "time: translation {:.1f} s, compilation {:.1f} s ({} threads), link {:.1f} s; {} bytes\n",
      code_package_identity(sha, layout, options.cpu), r.functions_found, r.functions_translated, refused, r.ee_entries,
      r.vu_images, r.vu_block_starts, r.vu_entries, r.translate_seconds, r.compile_seconds,
      options.threads ? options.threads : std::max(1u, std::thread::hardware_concurrency()), r.link_seconds, r.dll_bytes);
  {
    fs::path txt = dll;
    txt.replace_extension(".txt");
    std::ofstream(txt) << r.summary;
  }
  log(std::format("Wrote '{}' ({:.1f} MB) in {:.1f} s.", to_utf8(dll), r.dll_bytes / 1048576.0, seconds_since(t0)));
  return r;
}

}  // namespace hp2
