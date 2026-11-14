// The converter's "code" stage implementation (ADR 0009): links the
// translator (LLVM + LLD) and the reference core's state layout into the
// converter programs that are built with them.
#include "code_builder.h"

#include "hp2/convert.h"
#include "hp2/package.h"
#include "reference_layout.h"

namespace hp2 {

namespace {
constexpr const char* kCpuLevel = "x86-64-v3";  // AVX2 generation (2013 and later); checked by the launcher
}

void register_translator() {
  CodeBuilder b;
  b.identity = [](ByteView elf) {
    Sha256 h;
    h.update(elf);
    return code_package_identity(h.finish(), hp2ref::ReferenceCodeLayout(), kCpuLevel);
  };
  b.build = [](ByteView elf, const fs::path& dll, const std::function<void(std::string_view)>& log) {
    CodePackageOptions options;
    options.cpu = kCpuLevel;
    options.log = log;
    build_code_package(elf, hp2ref::ReferenceCodeLayout(), dll, options);
  };
  register_code_builder(std::move(b));
}

}  // namespace hp2
