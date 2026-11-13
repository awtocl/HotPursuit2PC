// HotPursuit2.exe - the launcher that runs the game (Milestone 1).
//
// What this is, stated plainly (ADR 0001): an EMULATOR-BACKED build. The
// game's own PS2 code runs inside the Play! core (BSD 2-Clause, linked in);
// this file supplies the window, the OpenGL context, audio, the controller
// and the rule that every file written stays inside the game folder (C6).
// It is not the native build.
//
//   HotPursuit2.exe [--rom <dir>] [--data <dir>] [--res <1..8>] [--size <W>x<H>] [--fullscreen] [--mute]
//
//   --rom   the ROM folder (default: ..\rom next to this program, i.e. <game>\rom).
//           It is presented to the core as a virtual disc; no image file is needed.
//   --data  where settings, memory cards and the log go (default: .\data)
//   Alt+Enter or F11 toggles full screen.
//
// Developer/automation switches (used for the acceptance runs):
//   --disc <image.iso>    boot a disc image file instead of the ROM folder
//   --frames <n>          exit after n emulated frames
//   --shots <dir>         save screenshots there ...
//   --shot-every <n>      ... every n frames (default 600)
//   --script <spec>       timed pad input: "<frame>:<button>[:<frames held>],..."
//                         buttons: up down left right start select cross circle square triangle l1 l2 r1 r2
//   --unlimited           no frame pacing at all (measures emulation speed; never the default, C7)
//   --core-limiter        use the core's own frame limiter instead of our pacer (comparison only)
//   --frametimes <csv>    record the instant of every emulated refresh and every presented image
//                         (analyse with tools/frametimes.py)
//   --trace <dir>         reference measurements for the recompiler (ref_trace.h): every block of
//                         game code the core executed, time per refresh, a copy of main memory
//   --trace-counts        with --trace: also count how often each block runs (slower, counting runs only)
//   --lift-check <list>   with --trace: translate the listed game functions (translator spike) and
//                         compare them with the reference core while the game runs (builds with LLVM only)
#include <windows.h>

#include <dbghelp.h>
#include <shellapi.h>
#include <xinput.h>

#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "AppConfig.h"
#include "OpticalMedia.h"
#include "PH_Generic.h"
#include "PS2VM.h"
#include "PS2VM_Preferences.h"
#include "Stream.h"
#include "gs/GSH_OpenGL/GSH_OpenGL.h"
#include "iop/IopBios.h"
#include "iop/Iop_Cdvdfsv.h"
#include "iop/Iop_Cdvdman.h"
#include "sound/SH_WaveOut/SH_WaveOut.h"

#include <GL/wglew.h>

#include "ref_trace.h"
#include "virtual_disc.h"

namespace
{
	fs::path g_dataDir;  // everything the core writes: settings, memory cards, states
	std::atomic<uint32_t> g_vmFrames{0};       // emulated vertical blanks (59.94 per second at full speed)
	std::atomic<uint32_t> g_presentedFrames{0}; // buffers shown on screen
	FILE* g_log = nullptr;

	void Log(const char* format, ...)
	{
		if(!g_log) return;
		SYSTEMTIME t;
		GetLocalTime(&t);
		fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
		va_list args;
		va_start(args, format);
		vfprintf(g_log, format, args);
		va_end(args);
		fputc('\n', g_log);
		fflush(g_log);
	}

	// Frame-time recorder (--frametimes <csv>): the exact moment of every
	// emulated vertical blank ('V') and every presented image ('P').
	// Per-second averages hide stutter; this does not.
	struct FrameTimeRecorder
	{
		bool enabled = false;
		std::mutex mutex;
		std::vector<std::pair<char, int64_t>> events;  // kind, QueryPerformanceCounter ticks
		void Mark(char kind)
		{
			if(!enabled) return;
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			std::lock_guard<std::mutex> lock(mutex);
			events.emplace_back(kind, now.QuadPart);
		}
		void Save(const fs::path& path)
		{
			if(!enabled || events.empty()) return;
			LARGE_INTEGER frequency;
			QueryPerformanceFrequency(&frequency);
			if(FILE* file = _wfopen(path.c_str(), L"w"))
			{
				fprintf(file, "kind,ms\n");
				const int64_t origin = events.front().second;
				for(const auto& event : events)
					fprintf(file, "%c,%.3f\n", event.first, (event.second - origin) * 1000.0 / frequency.QuadPart);
				fclose(file);
			}
		}
	};
	FrameTimeRecorder g_frameTimes;

	// Frame pacer. A console shows a new image at a fixed instant every
	// refresh, however long the frame took to compute. The core's own limiter
	// instead sleeps by "average work time of the last 4 frames" and presents
	// whenever a frame's work happens to finish, which measured as +-3..5 ms
	// of jitter on every frame. Here both the end of each emulated frame (VM
	// thread) and each presentation (GS thread) are held to one absolute time
	// grid of exactly 1/60 s (the rate the core's audio clock assumes).
	class FramePacer
	{
	public:
		void Start(double hz)
		{
			LARGE_INTEGER frequency;
			QueryPerformanceFrequency(&frequency);
			m_ticksPerSecond = frequency.QuadPart;
			m_period = static_cast<int64_t>(frequency.QuadPart / hz + 0.5);
			m_anchor.store(Now());
			m_enabled = true;
		}
		bool Enabled() const
		{
			return m_enabled;
		}
		// VM thread, once per emulated frame.
		void WaitFrameEnd()
		{
			if(!m_enabled) return;
			static thread_local HANDLE timer = CreateTimer();
			const int64_t now = Now();
			int64_t deadline = m_nextFrameEnd;
			if(deadline == 0 || now - deadline > m_period)
			{
				// First frame, or more than one refresh behind after a load/hitch:
				// restart the grid here instead of racing to catch up.
				// The limit must not be larger. The core lets the VM thread run at
				// most two frames ahead of presentation, and each presentation waits
				// for its instant (WaitPresent). A VM more than one period late never
				// waits here, so with 60 fps content (videos, menus) it then advances
				// two refreshes at once every 33 ms and can never catch up - measured
				// with a limit of four periods (docs/evidence/m1-acceptance.md §4b).
				deadline = now;
				m_anchor.store(now);
			}
			WaitUntil(deadline, timer);
			m_nextFrameEnd = deadline + m_period;
		}
		// GS thread, immediately before the buffer swap: hold the image until the
		// next presentation point, so presentations are exactly one (or two, ...)
		// periods apart. Presentation points sit half a period after the frame-end
		// grid: the game does a 30 fps frame's work inside one refresh, and that
		// busy refresh sometimes takes a few ms more than 1/60 s to emulate. With
		// the half-period slack such a frame is still shown on time instead of a
		// whole refresh late (measured: 50 ms + 17 ms pairs). Cost: up to 8 ms of
		// added display latency.
		void WaitPresent()
		{
			if(!m_enabled) return;
			static thread_local HANDLE timer = CreateTimer();
			const int64_t first = m_anchor.load() + m_period / 2;
			const int64_t now = Now();
			const int64_t periods = now <= first ? 0 : (now - first + m_period - 1) / m_period;
			WaitUntil(first + periods * m_period, timer);
		}

	private:
		static int64_t Now()
		{
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			return now.QuadPart;
		}
		static HANDLE CreateTimer()
		{
			// High-resolution timers exist since Windows 10 1803; fall back to a normal one.
			HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
			return timer ? timer : CreateWaitableTimerW(nullptr, TRUE, nullptr);
		}
		void WaitUntil(int64_t deadline, HANDLE timer) const
		{
			const int64_t spin = m_ticksPerSecond / 2000;  // finish the last 0.5 ms by spinning
			int64_t remaining = deadline - Now();
			if(timer && remaining > spin)
			{
				LARGE_INTEGER due;
				due.QuadPart = -((remaining - spin) * 10000000 / m_ticksPerSecond);  // 100 ns units, relative
				if(due.QuadPart < 0 && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
					WaitForSingleObject(timer, 100);
			}
			while(Now() < deadline) YieldProcessor();
		}

		bool m_enabled = false;
		int64_t m_ticksPerSecond = 1;
		int64_t m_period = 1;
		int64_t m_nextFrameEnd = 0;       // VM thread only
		std::atomic<int64_t> m_anchor{0};  // origin of the grid, shared with the GS thread
	};
	FramePacer g_pacer;

	fs::path ExeDir()
	{
		std::wstring buffer(32768, L'\0');
		buffer.resize(GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size())));
		return fs::path(buffer).parent_path();
	}

	// Last-chance handler: record what crashed and where, so a failure in the
	// field leaves evidence in launcher.log instead of a silent exit.
	LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info)
	{
		const EXCEPTION_RECORD* record = info->ExceptionRecord;
		Log("CRASH: exception 0x%08lX at %p (thread %lu)", record->ExceptionCode, record->ExceptionAddress,
		    GetCurrentThreadId());
		if(record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
			Log("  access violation %s address %p", record->ExceptionInformation[0] ? "writing" : "reading",
			    reinterpret_cast<void*>(record->ExceptionInformation[1]));

		const HANDLE process = GetCurrentProcess();
		SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
		SymInitialize(process, nullptr, TRUE);
		CONTEXT context = *info->ContextRecord;
		STACKFRAME64 frame = {};
		frame.AddrPC.Offset = context.Rip;
		frame.AddrFrame.Offset = context.Rbp;
		frame.AddrStack.Offset = context.Rsp;
		frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
		for(int i = 0; i < 48; i++)
		{
			if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context, nullptr,
			                SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
			   frame.AddrPC.Offset == 0)
				break;
			char buffer[sizeof(SYMBOL_INFO) + 512] = {};
			auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
			symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
			symbol->MaxNameLen = 511;
			DWORD64 displacement = 0;
			IMAGEHLP_LINE64 line = {};
			line.SizeOfStruct = sizeof(line);
			DWORD lineDisplacement = 0;
			const bool hasSymbol = SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol) != FALSE;
			const bool hasLine = SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement, &line) != FALSE;
			char module[MAX_PATH] = "?";
			if(const DWORD64 base = SymGetModuleBase64(process, frame.AddrPC.Offset))
				GetModuleFileNameA(reinterpret_cast<HMODULE>(base), module, MAX_PATH);
			const char* moduleName = strrchr(module, '\\');
			Log("  #%02d %p %s!%s+0x%llx %s:%lu", i, reinterpret_cast<void*>(frame.AddrPC.Offset),
			    moduleName ? moduleName + 1 : module, hasSymbol ? symbol->Name : "?", displacement,
			    hasLine ? line.FileName : "", hasLine ? line.LineNumber : 0);
		}
		if(g_log) fflush(g_log);
		return EXCEPTION_EXECUTE_HANDLER;
	}
}

// Play! leaves this for the application to define. Ours keeps all data in
// the game folder instead of the user's Documents folder.
fs::path CAppConfig::GetBasePath() const
{
	return g_dataDir;
}

namespace
{
	// OpenGL output on our window; modelled on Play!'s GSH_OpenGLWin32.
	class CGSH_OpenGL_Hp2 : public CGSH_OpenGL
	{
	public:
		explicit CGSH_OpenGL_Hp2(HWND window)
		    : m_window(window)
		{
		}

		static FactoryFunction GetFactoryFunction(HWND window)
		{
			return [window]() { return new CGSH_OpenGL_Hp2(window); };
		}

		void InitializeImpl() override
		{
			static const PIXELFORMATDESCRIPTOR pfd = {
			    sizeof(PIXELFORMATDESCRIPTOR), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
			    PFD_TYPE_RGBA, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 24, 8, 0, PFD_MAIN_PLANE, 0, 0, 0, 0};
			m_dc = GetDC(m_window);
			SetPixelFormat(m_dc, ChoosePixelFormat(m_dc, &pfd), &pfd);
			m_context = wglCreateContext(m_dc);
			wglMakeCurrent(m_dc, m_context);

			auto createContextAttribs =
			    reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARBPROC>(wglGetProcAddress("wglCreateContextAttribsARB"));
			if(createContextAttribs)
			{
				static const int attributes[] = {
				    WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
				    WGL_CONTEXT_MINOR_VERSION_ARB, 2,
				    WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
				    0};
				if(HGLRC core = createContextAttribs(m_dc, nullptr, attributes))
				{
					wglMakeCurrent(m_dc, core);
					wglDeleteContext(m_context);
					m_context = core;
				}
			}

			glewExperimental = GL_TRUE;  // needed for core profiles
			const GLenum glewResult = glewInit();
			glGetError();
			const auto text = [](GLenum name) {
				const GLubyte* s = glGetString(name);
				return s ? reinterpret_cast<const char*>(s) : "(none)";
			};
			Log("OpenGL: context %p | %s | %s | glew %s", m_context, text(GL_VERSION), text(GL_RENDERER),
			    glewResult == GLEW_OK ? "ok" : "FAILED");
			if(wglSwapIntervalEXT) wglSwapIntervalEXT(0);  // pacing is done by the core's 60 fps limiter

			CGSH_OpenGL::InitializeImpl();
		}

		void ReleaseImpl() override
		{
			CGSH_OpenGL::ReleaseImpl();
			wglMakeCurrent(nullptr, nullptr);
			if(m_context) wglDeleteContext(m_context);
			if(m_dc) ReleaseDC(m_window, m_dc);
		}

		// Saves the next presented frame as a 32-bit BMP (Play!'s own
		// GetScreenshot is a stub in the OpenGL renderer).
		void RequestScreenshot(const fs::path& path)
		{
			std::lock_guard<std::mutex> lock(m_shotMutex);
			m_shotPath = path;
			m_shotRequested = true;
		}

	protected:
		void PresentBackbuffer() override
		{
			if(m_shotRequested.exchange(false)) SaveBackbuffer();
			hp2ref::TracePresent();
			g_pacer.WaitPresent();
			SwapBuffers(m_dc);
			g_frameTimes.Mark('P');
			++g_presentedFrames;
		}

	private:
		void SaveBackbuffer()
		{
			fs::path path;
			{
				std::lock_guard<std::mutex> lock(m_shotMutex);
				path = m_shotPath;
			}
			RECT rc;
			GetClientRect(m_window, &rc);
			const int width = rc.right, height = rc.bottom;
			if(width <= 0 || height <= 0) return;
			std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
			GLint previousRead = 0;
			glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
			glReadBuffer(GL_BACK);
			glPixelStorei(GL_PACK_ALIGNMENT, 1);
			glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, pixels.data());  // bottom-up, like BMP
			glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousRead));

			BITMAPFILEHEADER fileHeader = {};
			BITMAPINFOHEADER infoHeader = {};
			infoHeader.biSize = sizeof(infoHeader);
			infoHeader.biWidth = width;
			infoHeader.biHeight = height;
			infoHeader.biPlanes = 1;
			infoHeader.biBitCount = 32;
			fileHeader.bfType = 0x4D42;
			fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
			fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(pixels.size());
			if(FILE* file = _wfopen(path.c_str(), L"wb"))
			{
				fwrite(&fileHeader, sizeof(fileHeader), 1, file);
				fwrite(&infoHeader, sizeof(infoHeader), 1, file);
				fwrite(pixels.data(), 1, pixels.size(), file);
				fclose(file);
			}
			else
			{
				Log("screenshot: cannot write %ls", path.c_str());
			}
		}

		HWND m_window = nullptr;
		HDC m_dc = nullptr;
		HGLRC m_context = nullptr;
		std::mutex m_shotMutex;
		fs::path m_shotPath;
		std::atomic<bool> m_shotRequested{false};
	};

	// The ROM folder as a read-only stream of disc sectors.
	class CVirtualDiscStream : public Framework::CStream
	{
	public:
		explicit CVirtualDiscStream(std::shared_ptr<hp2::VirtualDisc> disc)
		    : m_disc(std::move(disc))
		{
		}
		void Seek(int64 position, Framework::STREAM_SEEK_DIRECTION direction) override
		{
			switch(direction)
			{
			case Framework::STREAM_SEEK_SET: m_position = static_cast<uint64>(position); break;
			case Framework::STREAM_SEEK_CUR: m_position = static_cast<uint64>(static_cast<int64>(m_position) + position); break;
			case Framework::STREAM_SEEK_END: m_position = static_cast<uint64>(static_cast<int64>(m_disc->Size()) + position); break;
			}
		}
		uint64 Tell() override
		{
			return m_position;
		}
		uint64 Read(void* buffer, uint64 size) override
		{
			const uint64 got = m_disc->Read(m_position, buffer, size);
			m_position += got;
			return got;
		}
		uint64 Write(const void*, uint64) override
		{
			throw std::runtime_error("The disc is read-only.");
		}
		bool IsEOF() override
		{
			return m_position >= m_disc->Size();
		}
		uint64 GetLength() override
		{
			return m_disc->Size();
		}

	private:
		std::shared_ptr<hp2::VirtualDisc> m_disc;
		uint64 m_position = 0;
	};

	std::shared_ptr<hp2::VirtualDisc> g_disc;

	// Puts the virtual disc into the core's drive. Play! only knows how to do
	// this from a file path, so the same three steps are done here through
	// its public members. Must run while the VM is paused or on the VM thread.
	void MountVirtualDisc(CPS2VM* vm)
	{
		vm->m_cdrom0 = COpticalMedia::CreateAuto(std::make_shared<CVirtualDiscStream>(g_disc));
		auto* iopBios = dynamic_cast<CIopBios*>(vm->m_iop->m_bios.get());
		if(!iopBios) throw std::runtime_error("The core has no IOP BIOS to attach the disc to.");
		iopBios->GetCdvdfsv()->SetOpticalMedia(vm->m_cdrom0.get());
		iopBios->GetCdvdman()->SetOpticalMedia(vm->m_cdrom0.get());
	}

	// Play!'s WaveOut output, plus a count of what the game produced so the
	// log can show that sound is really flowing.
	std::atomic<uint64_t> g_audioSamples{0};
	class CSH_Hp2Sound : public CSH_WaveOut
	{
	public:
		static CSoundHandler* Factory()
		{
			return new CSH_Hp2Sound();
		}
		void Write(int16* samples, unsigned int sampleCount, unsigned int sampleRate) override
		{
			g_audioSamples += sampleCount;
			CSH_WaveOut::Write(samples, sampleCount, sampleRate);
		}
	};

	struct Options
	{
		fs::path rom;   // ROM folder -> virtual disc (the normal way)
		fs::path disc;  // or a disc image file (development)
		fs::path shots;
		fs::path frameTimes;  // CSV of every vblank and present (development)
		fs::path trace;       // folder for reference measurements (development, ref_trace.h)
		bool traceCounts = false;  // with --trace: count how often each block of game code runs
		fs::path liftCheck;        // with --trace: list of functions to translate and compare (lift_check.h)
		int resolutionFactor = 2;
		int width = 1280, height = 896;  // 2x the PS2's 640x448
		bool fullscreen = false;
		bool mute = false;
		bool unlimited = false;
		bool coreLimiter = false;  // use the core's frame limiter instead of our pacer (comparison only)
		uint32_t frames = 0;
		uint32_t shotEvery = 600;
		struct ScriptEvent
		{
			uint32_t frame, hold;
			PS2::CControllerInfo::BUTTON button;
		};
		std::vector<ScriptEvent> script;
	};

	const std::map<std::wstring, PS2::CControllerInfo::BUTTON> kButtonNames = {
	    {L"up", PS2::CControllerInfo::DPAD_UP}, {L"down", PS2::CControllerInfo::DPAD_DOWN},
	    {L"left", PS2::CControllerInfo::DPAD_LEFT}, {L"right", PS2::CControllerInfo::DPAD_RIGHT},
	    {L"start", PS2::CControllerInfo::START}, {L"select", PS2::CControllerInfo::SELECT},
	    {L"cross", PS2::CControllerInfo::CROSS}, {L"circle", PS2::CControllerInfo::CIRCLE},
	    {L"square", PS2::CControllerInfo::SQUARE}, {L"triangle", PS2::CControllerInfo::TRIANGLE},
	    {L"l1", PS2::CControllerInfo::L1}, {L"l2", PS2::CControllerInfo::L2},
	    {L"r1", PS2::CControllerInfo::R1}, {L"r2", PS2::CControllerInfo::R2}};

	bool ParseScript(const std::wstring& spec, Options& options)
	{
		size_t pos = 0;
		while(pos < spec.size())
		{
			size_t end = spec.find(L',', pos);
			if(end == std::wstring::npos) end = spec.size();
			const std::wstring item = spec.substr(pos, end - pos);
			pos = end + 1;
			const size_t c1 = item.find(L':');
			if(c1 == std::wstring::npos) return false;
			const size_t c2 = item.find(L':', c1 + 1);
			const std::wstring name = item.substr(c1 + 1, c2 == std::wstring::npos ? c2 : c2 - c1 - 1);
			const auto button = kButtonNames.find(name);
			if(button == kButtonNames.end()) return false;
			Options::ScriptEvent event;
			event.frame = static_cast<uint32_t>(_wtoi(item.c_str()));
			event.hold = c2 == std::wstring::npos ? 6 : static_cast<uint32_t>(_wtoi(item.c_str() + c2 + 1));
			event.button = button->second;
			options.script.push_back(event);
		}
		return true;
	}

	bool ParseOptions(Options& options, std::wstring& error)
	{
		int argc = 0;
		wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
		bool ok = true;
		for(int i = 1; argv && i < argc && ok; i++)
		{
			const std::wstring a = argv[i];
			const bool hasValue = i + 1 < argc;
			if(a == L"--disc" && hasValue) options.disc = argv[++i];
			else if(a == L"--rom" && hasValue) options.rom = argv[++i];
			else if(a == L"--fullscreen") options.fullscreen = true;
			else if(a == L"--data" && hasValue) g_dataDir = argv[++i];
			else if(a == L"--shots" && hasValue) options.shots = argv[++i];
			else if(a == L"--res" && hasValue) options.resolutionFactor = _wtoi(argv[++i]);
			else if(a == L"--frames" && hasValue) options.frames = static_cast<uint32_t>(_wtoi(argv[++i]));
			else if(a == L"--shot-every" && hasValue) options.shotEvery = static_cast<uint32_t>(_wtoi(argv[++i]));
			else if(a == L"--size" && hasValue)
			{
				if(swscanf_s(argv[++i], L"%dx%d", &options.width, &options.height) != 2) ok = false;
			}
			else if(a == L"--frametimes" && hasValue)
			{
				options.frameTimes = argv[++i];
				g_frameTimes.enabled = true;
				g_frameTimes.events.reserve(1 << 18);
			}
			else if(a == L"--trace" && hasValue) options.trace = argv[++i];
			else if(a == L"--trace-counts") options.traceCounts = true;
			else if(a == L"--lift-check" && hasValue) options.liftCheck = argv[++i];
			else if(a == L"--script" && hasValue) ok = ParseScript(argv[++i], options);
			else if(a == L"--mute") options.mute = true;
			else if(a == L"--unlimited") options.unlimited = true;
			else if(a == L"--core-limiter") options.coreLimiter = true;
			else ok = false;
			if(!ok) error = L"Unknown or malformed option: " + a;
		}
		if(argv) LocalFree(argv);
		if(options.resolutionFactor < 1 || options.resolutionFactor > 8 || options.shotEvery == 0 ||
		   options.width < 320 || options.height < 224)
		{
			ok = false;
			error = L"Option value out of range.";
		}
		return ok;
	}

	CPS2VM* g_vm = nullptr;
	bool g_quit = false;

	void UpdatePresentation(HWND window)
	{
		if(!g_vm || !g_vm->m_ee || !g_vm->m_ee->m_gs) return;
		RECT rc;
		GetClientRect(window, &rc);
		CGSHandler::PRESENTATION_PARAMS params;
		params.windowWidth = static_cast<uint32>(rc.right > 0 ? rc.right : 1);
		params.windowHeight = static_cast<uint32>(rc.bottom > 0 ? rc.bottom : 1);
		params.mode = CGSHandler::PRESENTATION_MODE_FIT;  // keep the PS2 picture's 4:3 shape
		g_vm->m_ee->m_gs->SetPresentationParams(params);
	}

	// Borderless full screen on the monitor the window is on, and back.
	void ToggleFullscreen(HWND window)
	{
		static WINDOWPLACEMENT windowed = {sizeof(WINDOWPLACEMENT)};
		const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
		if(style & WS_OVERLAPPEDWINDOW)
		{
			MONITORINFO monitor = {sizeof(MONITORINFO)};
			if(GetWindowPlacement(window, &windowed) &&
			   GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor))
			{
				SetWindowLongPtrW(window, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
				SetWindowPos(window, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
				             monitor.rcMonitor.right - monitor.rcMonitor.left,
				             monitor.rcMonitor.bottom - monitor.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
			}
		}
		else
		{
			SetWindowLongPtrW(window, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
			SetWindowPlacement(window, &windowed);
			SetWindowPos(window, nullptr, 0, 0, 0, 0,
			             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
		}
	}

	LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
	{
		switch(message)
		{
		case WM_SIZE:
			UpdatePresentation(window);
			return 0;
		case WM_SYSKEYDOWN:
			if(wparam == VK_RETURN && !(lparam & (1 << 30)))  // Alt+Enter, not auto-repeat
			{
				ToggleFullscreen(window);
				return 0;
			}
			break;
		case WM_KEYDOWN:
			if(wparam == VK_F11 && !(lparam & (1 << 30)))
			{
				ToggleFullscreen(window);
				return 0;
			}
			break;
		case WM_ERASEBKGND:
			return 1;
		case WM_CLOSE:
			g_quit = true;
			return 0;
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		}
		return DefWindowProcW(window, message, wparam, lparam);
	}

	// Keyboard + first XInput controller -> PS2 pad.
	void PollInput(CPH_Generic* pad, HWND window, const Options& options, uint32_t frame)
	{
		using B = PS2::CControllerInfo;
		bool down[B::MAX_BUTTONS] = {};
		float axis[4] = {0, 0, 0, 0};

		if(GetForegroundWindow() == window)
		{
			static const std::pair<int, B::BUTTON> keys[] = {
			    {VK_UP, B::DPAD_UP}, {VK_DOWN, B::DPAD_DOWN}, {VK_LEFT, B::DPAD_LEFT}, {VK_RIGHT, B::DPAD_RIGHT},
			    {VK_RETURN, B::START}, {VK_BACK, B::SELECT}, {'X', B::CROSS}, {'C', B::CIRCLE},
			    {'Z', B::SQUARE}, {'S', B::TRIANGLE}, {'Q', B::L1}, {'W', B::R1}, {'1', B::L2}, {'2', B::R2}};
			for(const auto& key : keys)
				if(GetAsyncKeyState(key.first) & 0x8000) down[key.second] = true;
		}

		XINPUT_STATE state = {};
		if(XInputGetState(0, &state) == ERROR_SUCCESS)
		{
			const WORD b = state.Gamepad.wButtons;
			static const std::pair<WORD, B::BUTTON> buttons[] = {
			    {XINPUT_GAMEPAD_DPAD_UP, B::DPAD_UP}, {XINPUT_GAMEPAD_DPAD_DOWN, B::DPAD_DOWN},
			    {XINPUT_GAMEPAD_DPAD_LEFT, B::DPAD_LEFT}, {XINPUT_GAMEPAD_DPAD_RIGHT, B::DPAD_RIGHT},
			    {XINPUT_GAMEPAD_START, B::START}, {XINPUT_GAMEPAD_BACK, B::SELECT},
			    {XINPUT_GAMEPAD_A, B::CROSS}, {XINPUT_GAMEPAD_B, B::CIRCLE},
			    {XINPUT_GAMEPAD_X, B::SQUARE}, {XINPUT_GAMEPAD_Y, B::TRIANGLE},
			    {XINPUT_GAMEPAD_LEFT_SHOULDER, B::L1}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, B::R1},
			    {XINPUT_GAMEPAD_LEFT_THUMB, B::L3}, {XINPUT_GAMEPAD_RIGHT_THUMB, B::R3}};
			for(const auto& button : buttons)
				if(b & button.first) down[button.second] = true;
			if(state.Gamepad.bLeftTrigger > 64) down[B::L2] = true;
			if(state.Gamepad.bRightTrigger > 64) down[B::R2] = true;
			const auto stick = [](SHORT v, SHORT deadzone) {
				return (v > -deadzone && v < deadzone) ? 0.0f : (v < 0 ? v / 32768.0f : v / 32767.0f);
			};
			axis[0] = stick(state.Gamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
			axis[1] = -stick(state.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
			axis[2] = stick(state.Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
			axis[3] = -stick(state.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
		}

		for(const auto& event : options.script)
			if(frame >= event.frame && frame < event.frame + event.hold) down[event.button] = true;

		for(unsigned int i = 0; i < B::MAX_BUTTONS; i++)
		{
			const auto button = static_cast<B::BUTTON>(i);
			if(B::IsAxis(button)) pad->SetAxisState(i, axis[i]);
			else pad->SetButtonState(i, down[i]);
		}
	}

	int Fail(const std::wstring& message)
	{
		Log("FATAL: %ls", message.c_str());
		MessageBoxW(nullptr, message.c_str(), L"Hot Pursuit 2", MB_OK | MB_ICONERROR);
		return 1;
	}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
	Options options;
	std::wstring optionError;
	g_dataDir = ExeDir() / L"data";
	if(!ParseOptions(options, optionError)) return Fail(optionError);
	if(options.rom.empty()) options.rom = ExeDir().parent_path() / L"rom";

	std::error_code ec;
	fs::create_directories(g_dataDir, ec);
	g_log = _wfopen((g_dataDir / L"launcher.log").c_str(), L"w");
	SetUnhandledExceptionFilter(CrashFilter);
	Log("HotPursuit2 launcher - emulator-backed build (Play! core %s). Data folder: %ls", PLAY_VERSION,
	    g_dataDir.c_str());
	if(!options.disc.empty())
	{
		if(!fs::is_regular_file(options.disc, ec))
			return Fail(L"The disc image was not found:\n" + options.disc.wstring());
	}
	else
	{
		std::string why;
		g_disc = hp2::VirtualDisc::Open(options.rom.wstring(), why);
		if(!g_disc)
		{
			Log("ROM folder rejected: %s", why.c_str());
			std::wstring wide(static_cast<size_t>(MultiByteToWideChar(CP_UTF8, 0, why.c_str(), -1, nullptr, 0)), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, why.c_str(), -1, wide.data(), static_cast<int>(wide.size()));
			return Fail(L"The game files cannot be used:\n\n" + wide +
			            L"\nRun the converter again on this game folder to repair it.");
		}
		Log("Virtual disc: %s", g_disc->Description().c_str());
	}
	if(!options.shots.empty()) fs::create_directories(options.shots, ec);

	// ---- window ------------------------------------------------------------------------
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.style = CS_OWNDC;
	wc.lpfnWndProc = WindowProc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
	wc.lpszClassName = L"HotPursuit2PC.Launcher";
	RegisterClassExW(&wc);
	const DWORD style = WS_OVERLAPPEDWINDOW;
	RECT rc = {0, 0, options.width, options.height};
	AdjustWindowRect(&rc, style, FALSE);
	const wchar_t* kTitle = L"Need for Speed: Hot Pursuit 2 (PS2) - emulator-backed build";
	HWND window = CreateWindowExW(0, wc.lpszClassName, kTitle, style, CW_USEDEFAULT, CW_USEDEFAULT,
	                              rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, instance, nullptr);
	if(!window) return Fail(L"The game window could not be created.");
	ShowWindow(window, SW_SHOWNORMAL);
	if(options.fullscreen) ToggleFullscreen(window);

	int exitCode = 0;
	try
	{
		// ---- core ----------------------------------------------------------------------
		auto& config = CAppConfig::GetInstance();
		Log("step: config ready");
		config.RegisterPreferenceInteger(PREF_CGSH_OPENGL_RESOLUTION_FACTOR, 1);
		g_vm = new CPS2VM();
		Log("step: VM constructed");
		g_vm->Initialize();
		Log("step: VM initialised");
		// With the virtual disc the path stays empty: the core must not open a file of its own.
		config.SetPreferencePath(PREF_PS2_CDROM0_PATH, options.disc);
		config.SetPreferenceInteger(PREF_CGSH_OPENGL_RESOLUTION_FACTOR, options.resolutionFactor);
		// 60 Hz baseline (C7). By default our pacer does it; --core-limiter keeps
		// the core's own limiter instead (for comparison); --unlimited uses neither.
		const bool ownPacer = !options.unlimited && !options.coreLimiter;
		config.SetPreferenceBoolean(PREF_PS2_LIMIT_FRAMERATE, !options.unlimited && options.coreLimiter);
		g_vm->ReloadFrameRateLimit();
		if(ownPacer) g_pacer.Start(60.0);

		if(!options.mute) g_vm->CreateSoundHandler(&CSH_Hp2Sound::Factory);
		g_vm->CreatePadHandler(CPH_Generic::GetFactoryFunction());
		Log("step: sound and pad handlers created");
		g_vm->CreateGSHandler(CGSH_OpenGL_Hp2::GetFactoryFunction(window));
		Log("step: GS handler created");
		UpdatePresentation(window);
		auto frameConnection = g_vm->OnNewFrame.Connect([]() {
			hp2ref::TraceVmFrameWorkDone(g_vmFrames.load());
			g_pacer.WaitFrameEnd();  // on the VM thread: this is what holds the game to 60 Hz
			hp2ref::TraceVmFrameWaitDone();
			++g_vmFrames;
			g_frameTimes.Mark('V');
		});

		g_vm->Pause();
		if(!options.trace.empty())
		{
			const std::string traceError = hp2ref::TraceInstall(*g_vm, options.trace, options.traceCounts, options.liftCheck,
			                                                    options.rom / L"SLUS_203.62");
			if(!traceError.empty()) throw std::runtime_error("--lift-check: " + traceError);
			Log("step: reference tracing installed");
		}
		g_vm->Reset();
		Log("step: VM reset");
		if(g_disc)
		{
			MountVirtualDisc(g_vm);
			// If the game reloads its executable the core resets the drive; put the disc back.
			g_vm->BeforeExecutableReloaded = [](CPS2VM* vm) {
				Log("executable reload: remounting the virtual disc");
				MountVirtualDisc(vm);
			};
		}
		g_vm->m_ee->m_os->BootFromCDROM();
		Log("step: boot from disc requested");
		g_vm->Resume();
		Log("Booted from %ls (resolution x%d, pacing: %s, sound %s)",
		    g_disc ? options.rom.c_str() : options.disc.c_str(), options.resolutionFactor,
		    options.unlimited ? "none" : (options.coreLimiter ? "core limiter" : "own 60 Hz grid"),
		    options.mute ? "off" : "on");

		// ---- main loop -----------------------------------------------------------------
		auto* pad = static_cast<CPH_Generic*>(g_vm->GetPadHandler());
		uint32_t nextShot = options.shots.empty() ? UINT32_MAX : options.shotEvery;
		uint32_t lastFrames = 0, lastPresented = 0, seconds = 0;
		uint32_t minFps = UINT32_MAX, maxFps = 0;
		uint64_t lastAudio = 0;
		ULONGLONG lastTick = GetTickCount64();
		const ULONGLONG startTick = lastTick;

		while(!g_quit)
		{
			MSG message;
			while(PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
			{
				if(message.message == WM_QUIT) g_quit = true;
				TranslateMessage(&message);
				DispatchMessageW(&message);
			}
			const uint32_t frame = g_vmFrames.load();
			PollInput(pad, window, options, frame);

			if(frame >= nextShot)
			{
				wchar_t name[40];
				swprintf_s(name, L"frame_%06u.bmp", frame);
				static_cast<CGSH_OpenGL_Hp2*>(g_vm->m_ee->m_gs)->RequestScreenshot(options.shots / name);
				nextShot = frame + options.shotEvery;
			}

			const ULONGLONG now = GetTickCount64();
			if(now - lastTick >= 1000)
			{
				const uint32_t presented = g_presentedFrames.load();
				const uint32_t fps = static_cast<uint32_t>((frame - lastFrames) * 1000ull / (now - lastTick));
				const uint32_t shown = static_cast<uint32_t>((presented - lastPresented) * 1000ull / (now - lastTick));
				if(++seconds > 2)  // ignore start-up
				{
					if(fps < minFps) minFps = fps;
					if(fps > maxFps) maxFps = fps;
				}
				const uint64_t audioNow = g_audioSamples.load();
				// stereo interleaved: two samples per audio frame
				const uint32_t audioHz = static_cast<uint32_t>((audioNow - lastAudio) * 500ull / (now - lastTick));
				lastAudio = audioNow;
				Log("t=%us frame=%u vm=%u fps shown=%u fps audio=%u Hz", seconds, frame, fps, shown, audioHz);
				wchar_t title[200];
				swprintf_s(title, L"%s - %u fps", kTitle, fps);
				SetWindowTextW(window, title);
				lastFrames = frame;
				lastPresented = presented;
				lastTick = now;
			}
			if(options.frames && frame >= options.frames) g_quit = true;
			Sleep(1);
		}

		const double elapsed = (GetTickCount64() - startTick) / 1000.0;
		Log("Stopping after %u emulated frames in %.1f s (average %.1f fps, per-second min %u max %u), %u presented",
		    g_vmFrames.load(), elapsed, elapsed > 0 ? g_vmFrames.load() / elapsed : 0.0,
		    minFps == UINT32_MAX ? 0 : minFps, maxFps, g_presentedFrames.load());

		g_vm->Pause();
		frameConnection.reset();
		g_frameTimes.Save(options.frameTimes);
		hp2ref::TraceFinish(*g_vm);
		g_vm->DestroyPadHandler();
		g_vm->DestroyGSHandler();
		g_vm->DestroySoundHandler();
		g_vm->Destroy();
		delete g_vm;
		g_vm = nullptr;
		config.Save();
	}
	catch(const std::exception& e)
	{
		std::string what = e.what();
		exitCode = Fail(L"The game stopped because of an error:\n" + std::wstring(what.begin(), what.end()));
	}
	DestroyWindow(window);
	Log("Exit code %d", exitCode);
	if(g_log) fclose(g_log);
	return exitCode;
}
