// See translated.h.
#include "translated.h"

#include <windows.h>

#include <bcrypt.h>
#include <intrin.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "MemoryUtils.h"
#include "PS2VM.h"
#include "Ps2Const.h"
#include "ee/EeExecutor.h"
#include "ee/VuExecutor.h"
#include "reference_layout.h"
#include "string_format.h"

namespace hp2rt
{
	namespace
	{
		namespace fs = std::filesystem;

		using EeCode = void (*)(uint8_t* state, uint8_t* ram, uint8_t* spr, const void* runtime);
		using VuCode = void (*)(uint8_t* state, uint8_t* vuMem, const void* runtime);

		const hp2::Hp2CodeTable* g_table = nullptr;
		CMIPS* g_ee = nullptr;
		CMIPS* g_vu1 = nullptr;
		uint8_t* g_ram = nullptr;
		uint8_t* g_spr = nullptr;
		uint8_t* g_microMem1 = nullptr;
		uint8_t* g_vuMem1 = nullptr;
		uint64_t g_eeCalls = 0, g_vuCalls = 0, g_vuReferenceBlocks = 0;

		// ---- runtime services the translated code calls ----
		struct EeRuntime
		{
			uint64_t (*read)(uint8_t*, uint32_t, uint32_t);
			void (*write)(uint8_t*, uint32_t, uint64_t, uint32_t);
		};
		uint64_t EeRead(uint8_t*, uint32_t address, uint32_t size)
		{
			switch(size)
			{
			case 1: return MemoryUtils_GetByteProxy(g_ee, address);
			case 2: return MemoryUtils_GetHalfProxy(g_ee, address);
			case 4: return MemoryUtils_GetWordProxy(g_ee, address);
			default: return MemoryUtils_GetDoubleProxy(g_ee, address);
			}
		}
		void EeWrite(uint8_t*, uint32_t address, uint64_t value, uint32_t size)
		{
			switch(size)
			{
			case 1: MemoryUtils_SetByteProxy(g_ee, static_cast<uint32>(value), address); break;
			case 2: MemoryUtils_SetHalfProxy(g_ee, static_cast<uint32>(value), address); break;
			case 4: MemoryUtils_SetWordProxy(g_ee, static_cast<uint32>(value), address); break;
			default: MemoryUtils_SetDoubleProxy(g_ee, value, address); break;
			}
		}
		const EeRuntime g_eeRuntime{&EeRead, &EeWrite};

		struct VuRuntime
		{
			uint32_t (*read)(uint8_t*, uint32_t);
			void (*write)(uint8_t*, uint32_t, uint32_t);
		};
		uint32_t VuRead(uint8_t*, uint32_t address)
		{
			return MemoryUtils_GetWordProxy(g_vu1, address);
		}
		void VuWrite(uint8_t*, uint32_t address, uint32_t value)
		{
			MemoryUtils_SetWordProxy(g_vu1, value, address);
		}
		const VuRuntime g_vuRuntime{&VuRead, &VuWrite};

		void* Find(const hp2::Hp2CodeEntry* list, uint32_t count, uint32_t address)
		{
			const auto* end = list + count;
			const auto* it = std::lower_bound(list, end, address, [](const hp2::Hp2CodeEntry& e, uint32_t a) { return e.address < a; });
			return it != end && it->address == address ? it->code : nullptr;
		}

		// ---- the game program ----
		// A block at a place where translated code can be entered: its code runs
		// the translated function (and everything it calls) until it returns or
		// hands over; the block's epilogue continues wherever that left the
		// program counter. Not linked: where it continues is known only at run time.
		struct EeEntry
		{
			uint32_t begin;
			EeCode code;
		};
		std::vector<EeEntry> g_eeEntries;

		void RunEe(CMIPS* context, uint32 slot)
		{
			const EeEntry& entry = g_eeEntries[slot];
			auto& state = context->m_State;
			state.nPC = entry.begin;
			++g_eeCalls;
			entry.code(reinterpret_cast<uint8_t*>(&state), g_ram, g_spr, &g_eeRuntime);
			state.nDelayedJumpAddr = state.nPC;
			state.nPC = entry.begin;
		}

		class CTranslatedEeBlock : public CBasicBlock
		{
		public:
			CTranslatedEeBlock(CMIPS& context, uint32 begin, uint32 end, BLOCK_CATEGORY category, uint32 slot)
			    : CBasicBlock(context, begin, end, category)
			    , m_slot(slot)
			{
			}

			void CompileRange(CMipsJitter* jitter) override
			{
				jitter->PushCtx();
				jitter->PushCst(m_slot);
				jitter->Call(reinterpret_cast<void*>(&RunEe), 2, Jitter::CJitter::RETURN_VALUE_NONE);
				CBasicBlock::CompileEpilog(jitter, false);
			}

		private:
			uint32 m_slot;
		};

		class CTranslatedEeExecutor : public CEeExecutor
		{
		public:
			using CEeExecutor::CEeExecutor;

			BasicBlockPtr BlockFactory(CMIPS& context, uint32 start, uint32 end) override
			{
				void* code = Find(g_table->functions, g_table->function_count, start);
				if(!code) code = Find(g_table->entries, g_table->entry_count, start);
				if(!code) return CEeExecutor::BlockFactory(context, start, end);
				g_eeEntries.push_back({start, reinterpret_cast<EeCode>(code)});
				auto block = std::make_shared<CTranslatedEeBlock>(context, start, end, m_blockCategory,
				                                                  static_cast<uint32>(g_eeEntries.size() - 1));
				block->Compile();
				block->SetRecycleCount(RECYCLE_NOLINK_THRESHOLD);
				return block;
			}
		};

		// ---- VU1 microcode ----
		// An image is used while microcode memory holds exactly its words; the
		// core invalidates a range before an upload changes it.
		enum class ImageState : uint8_t
		{
			Unknown,
			Valid,
			Invalid
		};
		std::vector<ImageState> g_imageState;

		VuCode VuCodeAt(uint32_t pc)
		{
			for(uint32_t i = 0; i < g_table->vu_image_count; i++)
			{
				const hp2::Hp2VuImageTable& image = g_table->vu_images[i];
				if(pc < image.address || pc >= image.address + image.word_count * 4) continue;
				if(g_imageState[i] == ImageState::Unknown)
					g_imageState[i] = memcmp(g_microMem1 + image.address, image.words, image.word_count * 4) == 0 ? ImageState::Valid
					                                                                                               : ImageState::Invalid;
				if(g_imageState[i] != ImageState::Valid) continue;
				if(std::binary_search(image.block_starts, image.block_starts + image.block_count, pc))
					return reinterpret_cast<VuCode>(image.code);
				return reinterpret_cast<VuCode>(Find(image.entries, image.entry_count, pc));
			}
			return nullptr;
		}

		class CTranslatedVuExecutor : public CVuExecutor
		{
		public:
			using CVuExecutor::CVuExecutor;

			// CGenericMipsExecutor::Execute, with translated code where there is some.
			int Execute(int quota) override
			{
				auto& s = m_context.m_State;
				s.cycleQuota = quota;
				while(s.nHasException == 0)
				{
					if(const VuCode code = VuCodeAt(s.nPC))
					{
						code(reinterpret_cast<uint8_t*>(&s), g_vuMem1, &g_vuRuntime);
						++g_vuCalls;
						continue;  // each call runs at least one block
					}
					++g_vuReferenceBlocks;
					m_blockLookup.FindBlockAt(s.nPC & m_addressMask)->Execute();
				}
				s.nHasException &= ~MIPS_EXCEPTION_STATUS_QUOTADONE;
				return s.cycleQuota;
			}

			void Reset() override
			{
				CVuExecutor::Reset();
				std::fill(g_imageState.begin(), g_imageState.end(), ImageState::Unknown);
			}

			void ClearActiveBlocksInRange(uint32 start, uint32 end, bool executing) override
			{
				CVuExecutor::ClearActiveBlocksInRange(start, end, executing);
				for(uint32_t i = 0; i < g_table->vu_image_count; i++)
				{
					const auto& image = g_table->vu_images[i];
					if(start < image.address + image.word_count * 4 && end > image.address) g_imageState[i] = ImageState::Unknown;
				}
			}
		};

		// ---- checks ----
		bool Sha256OfFile(const fs::path& path, uint8_t digest[32])
		{
			FILE* file = _wfopen(path.c_str(), L"rb");
			if(!file) return false;
			std::vector<uint8_t> data;
			uint8_t buffer[1 << 16];
			for(size_t n; (n = fread(buffer, 1, sizeof buffer, file)) > 0;) data.insert(data.end(), buffer, buffer + n);
			fclose(file);
			return BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, data.data(), static_cast<ULONG>(data.size()), digest, 32) == 0;
		}

		// The instructions of the CPU level the package was compiled for.
		bool CpuSupports(const char* level)
		{
			if(strcmp(level, "x86-64-v3") != 0) return strcmp(level, "x86-64") == 0;
			int r[4];
			__cpuid(r, 1);
			const auto bit = [](int value, int b) { return (static_cast<unsigned>(value) >> b) & 1u; };
			const bool leaf1 = bit(r[2], 0) && bit(r[2], 9) && bit(r[2], 12) && bit(r[2], 19) && bit(r[2], 20) && bit(r[2], 22) &&
			                   bit(r[2], 23) && bit(r[2], 26) && bit(r[2], 27) && bit(r[2], 28) && bit(r[2], 29);
			if(!leaf1 || (_xgetbv(0) & 6) != 6) return false;
			__cpuidex(r, 7, 0);
			if(!(bit(r[1], 3) && bit(r[1], 5) && bit(r[1], 8))) return false;
			__cpuid(r, 0x80000001);
			return bit(r[2], 5);
		}
	}

	std::string InstallTranslatedCode(CPS2VM& vm, const fs::path& dll, const fs::path& elfPath)
	{
		std::error_code ec;
		// A game folder made with the ROM referenced in place holds only rom/rom-reference.tsv
		// ("source<TAB><path>"), as the virtual disc reads it.
		fs::path elf = elfPath;
		if(!fs::is_regular_file(elf, ec))
			if(FILE* file = _wfopen((elf.parent_path() / L"rom-reference.tsv").c_str(), L"rb"))
			{
				char line[4096];
				while(fgets(line, sizeof line, file))
					if(strncmp(line, "source\t", 7) == 0)
					{
						std::string source = line + 7;
						while(!source.empty() && (source.back() == '\n' || source.back() == '\r')) source.pop_back();
						const int n = MultiByteToWideChar(CP_UTF8, 0, source.c_str(), -1, nullptr, 0);
						std::wstring wide(n > 0 ? n - 1 : 0, L'\0');
						if(n > 0) MultiByteToWideChar(CP_UTF8, 0, source.c_str(), -1, wide.data(), n);
						elf = fs::path(wide) / elfPath.filename();
					}
				fclose(file);
			}
		if(!fs::is_regular_file(dll, ec)) return "no translated code (build/hp2code.dll): the reference core runs the game";
		HMODULE module = LoadLibraryW(dll.c_str());
		if(!module) return string_format("translated code not used: cannot load %ls (error %lu)", dll.c_str(), GetLastError());
		const auto* table = reinterpret_cast<const hp2::Hp2CodeTable*>(GetProcAddress(module, hp2::kCodeTableSymbol));
		auto refuse = [&](const std::string& why) {
			FreeLibrary(module);
			return "translated code not used: " + why + " - run the converter again to rebuild it";
		};
		if(!table || table->magic != hp2::kCodeTableMagic) return refuse("build/hp2code.dll is not a code package");
		if(table->version != hp2::kCodeTableVersion) return refuse("it was made by another converter version");
		if(table->layout_fingerprint != hp2::code_layout_fingerprint(hp2ref::ReferenceCodeLayout()))
			return refuse("it was made for another launcher version");
		uint8_t digest[32];
		if(!Sha256OfFile(elf, digest) || memcmp(digest, table->elf_sha256, 32) != 0)
			return refuse("it was made from a different game executable");
		char cpu[17] = {};
		memcpy(cpu, table->cpu, 16);
		if(!CpuSupports(cpu)) return "translated code not used: this processor lacks the instructions it was compiled for (" + std::string(cpu) + ")";

		g_table = table;
		g_ee = &vm.m_ee->m_EE;
		g_vu1 = &vm.m_ee->m_VU1;
		g_ram = vm.m_ee->m_ram;
		g_spr = vm.m_ee->m_spr;
		g_microMem1 = vm.m_ee->m_microMem1;
		g_vuMem1 = vm.m_ee->m_vuMem1;
		g_imageState.assign(table->vu_image_count, ImageState::Unknown);
		g_eeEntries.reserve(1 << 16);

		// Swap the executors. The core's page-fault handler keeps a pointer to the
		// executor it was registered for, so unregister the old one first.
		auto& ee = vm.m_ee->m_EE;
		static_cast<CEeExecutor*>(ee.m_executor.get())->RemoveExceptionHandler();
		ee.m_emptyBlockHandler = nullptr;  // the new executor's constructor installs its own
		ee.m_executor = std::make_unique<CTranslatedEeExecutor>(ee, vm.m_ee->m_ram);
		static_cast<CEeExecutor*>(ee.m_executor.get())->AddExceptionHandler();
		auto& vu1 = vm.m_ee->m_VU1;
		vu1.m_emptyBlockHandler = nullptr;
		vu1.m_executor = std::make_unique<CTranslatedVuExecutor>(vu1, PS2::MICROMEM1SIZE);

		return string_format("translated code in use: %u game functions, %u entry places, %u VU1 microcode image(s), compiled for %s",
		                     table->function_count, table->entry_count, table->vu_image_count, cpu);
	}

	std::string TranslatedCodeReport()
	{
		if(!g_table) return {};
		return string_format("translated code: %llu calls into the game program, %llu into VU1 microcode, %llu VU1 blocks on the reference core",
		                     static_cast<unsigned long long>(g_eeCalls), static_cast<unsigned long long>(g_vuCalls),
		                     static_cast<unsigned long long>(g_vuReferenceBlocks));
	}
}
