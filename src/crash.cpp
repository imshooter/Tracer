#include "tracer.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>

#if defined _WIN32
	#include <windows.h>
	#include <dbghelp.h>
	#include <psapi.h>
#endif

namespace tracer {

namespace {

constexpr int MaximumNativeFrames = 32;

#if defined _WIN32

LPTOP_LEVEL_EXCEPTION_FILTER previousFilter = nullptr;
bool installed = false;
bool crashing = false;
std::mutex symbolLock;
bool symbolsReady = false;

void prepareSymbols() {
	if (symbolsReady) {
		return;
	}
	SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
	SymInitialize(GetCurrentProcess(), nullptr, TRUE);
	symbolsReady = true;
}

std::string hex(uint32_t value) {
	char buffer[16];
	std::snprintf(buffer, sizeof(buffer), "%08X", value);
	return buffer;
}

std::string symbolName(DWORD64 address) {
	alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
	auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
	symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
	symbol->MaxNameLen = 255;
	DWORD64 displacement = 0;
	if (!SymFromAddr(GetCurrentProcess(), address, &displacement, symbol)) {
		return std::string();
	}
	IMAGEHLP_MODULE64 module = {};
	module.SizeOfStruct = sizeof(module);
	bool real = SymGetModuleInfo64(GetCurrentProcess(), address, &module) && (module.SymType == SymPdb || module.SymType == SymDia || module.SymType == SymCoff || module.SymType == SymCv);
	if (!real && displacement > 0x40) {
		return std::string();
	}
	char offset[32];
	std::snprintf(offset, sizeof(offset), "+0x%X", unsigned(displacement));
	return std::string(symbol->Name) + offset;
}

std::string generatedCode(DWORD64 address) {
	State& s = state();
	AMX* amx = s.watch.amx.load(std::memory_order_relaxed);
	cell cip, frm;
	if (s.jit == nullptr || amx == nullptr || !s.jit->locate(amx, uintptr_t(address), 0, cip, frm)) {
		return std::string();
	}
	Script* script = findScript(amx);
	if (script == nullptr) {
		return std::string();
	}
	std::string where = location(*script, cip);
	return "code PawnJIT generated for " + script->name + (where.empty() ? std::string() : ", at " + where);
}

std::string describeAddress(DWORD64 address) {
	std::string text;
	HMODULE module = nullptr;
	char path[MAX_PATH] = "";
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(uintptr_t(address)), &module) && module != nullptr) {
		GetModuleFileNameA(module, path, MAX_PATH);
		std::string name = path;
		size_t slash = name.find_last_of("/\\");
		if (slash != std::string::npos) {
			name = name.substr(slash + 1);
		}
		char offset[32];
		std::snprintf(offset, sizeof(offset), "+0x%X", unsigned(address - DWORD64(uintptr_t(module))));
		text = name + offset;
		std::string symbol = symbolName(address);
		if (!symbol.empty()) {
			text += " (" + symbol + ")";
		}
		return text;
	}
	text = "0x" + hex(uint32_t(address));
	std::string generated = generatedCode(address);
	if (!generated.empty()) {
		text += " (" + generated + ")";
	}
	return text;
}

std::vector<std::string> walk(CONTEXT context) {
	std::vector<std::string> lines;
	std::lock_guard<std::mutex> guard(symbolLock);
	prepareSymbols();
	STACKFRAME64 frame = {};
	frame.AddrPC.Offset = context.Eip;
	frame.AddrPC.Mode = AddrModeFlat;
	frame.AddrFrame.Offset = context.Ebp;
	frame.AddrFrame.Mode = AddrModeFlat;
	frame.AddrStack.Offset = context.Esp;
	frame.AddrStack.Mode = AddrModeFlat;
	HANDLE process = GetCurrentProcess();
	HANDLE thread = GetCurrentThread();
	for (int number = 0; number != MaximumNativeFrames; ++number) {
		if (!StackWalk64(IMAGE_FILE_MACHINE_I386, process, thread, &frame, &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0) {
			break;
		}
		lines.push_back("#" + std::to_string(number) + " " + describeAddress(frame.AddrPC.Offset));
	}
	return lines;
}

std::string exceptionText(const EXCEPTION_RECORD& record) {
	switch (record.ExceptionCode) {
	case EXCEPTION_ACCESS_VIOLATION:
		if (record.NumberParameters >= 2) {
			const char* kind = record.ExceptionInformation[0] == 1 ? "writing" : (record.ExceptionInformation[0] == 8 ? "executing" : "reading");
			return std::string("access violation ") + kind + " address 0x" + hex(uint32_t(record.ExceptionInformation[1]));
		}
		return "access violation";
	case EXCEPTION_STACK_OVERFLOW:
		return "stack overflow";
	case EXCEPTION_ILLEGAL_INSTRUCTION:
		return "illegal instruction";
	case EXCEPTION_PRIV_INSTRUCTION:
		return "privileged instruction";
	case EXCEPTION_INT_DIVIDE_BY_ZERO:
		return "integer division by zero";
	case EXCEPTION_INT_OVERFLOW:
		return "integer overflow";
	case EXCEPTION_FLT_DIVIDE_BY_ZERO:
		return "floating point division by zero";
	case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
		return "array bounds exceeded";
	case EXCEPTION_DATATYPE_MISALIGNMENT:
		return "misaligned data";
	case EXCEPTION_IN_PAGE_ERROR:
		return "page error";
	default:
		return "exception 0x" + hex(uint32_t(record.ExceptionCode));
	}
}

std::vector<std::string> serverModules() {
	std::vector<std::string> lines;
	HMODULE modules[1024];
	DWORD needed = 0;
	if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
		return lines;
	}
	char directory[MAX_PATH];
	DWORD length = GetCurrentDirectoryA(MAX_PATH, directory);
	std::string root(directory, length);
	std::transform(root.begin(), root.end(), root.begin(), [](unsigned char character) { return char(std::tolower(character)); });
	for (DWORD index = 0; index != std::min<DWORD>(needed / sizeof(HMODULE), 1024); ++index) {
		char path[MAX_PATH];
		DWORD size = GetModuleFileNameA(modules[index], path, MAX_PATH);
		std::string name(path, size);
		std::string lower = name;
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char character) { return char(std::tolower(character)); });
		if (lower.compare(0, root.size(), root) != 0) {
			continue;
		}
		MODULEINFO info = {};
		K32GetModuleInformation(GetCurrentProcess(), modules[index], &info, sizeof(info));
		uint32_t base = uint32_t(uintptr_t(info.lpBaseOfDll));
		lines.push_back(hex(base) + " - " + hex(base + info.SizeOfImage) + " " + shortPath(name.substr(root.size() + 1)));
	}
	return lines;
}

LONG WINAPI crashFilter(EXCEPTION_POINTERS* pointers) {
	State& s = state();
	if (!crashing && s.settings.crash && pointers != nullptr) {
		crashing = true;
		const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
		const CONTEXT& context = *pointers->ContextRecord;
		std::vector<std::string> lines;
		lines.push_back("[Tracer] The server crashed: " + exceptionText(record));
		lines.push_back("[Tracer]   in " + describeAddress(DWORD64(uintptr_t(record.ExceptionAddress))));
		bool mainThread = GetCurrentThreadId() == s.mainThread;
		if (mainThread && !s.calls.empty()) {
			const Call& top = s.calls.back();
			if (top.kind == CallKind::Native) {
				std::string where = location(*top.script, top.cip - cell(sizeof(cell)));
				lines.push_back("[Tracer]   while " + top.script->name + " ran native " + top.script->nativeName(top.index) + (where.empty() ? std::string() : ", called at " + where));
			} else {
				lines.push_back("[Tracer]   while " + top.script->name + " ran Pawn code");
			}
			lines.push_back("[Tracer]   Pawn backtrace:");
			for (const std::string& frame : currentBacktrace(true)) {
				lines.push_back("[Tracer]     " + frame);
			}
		} else if (!mainThread) {
			lines.push_back("[Tracer]   on a thread other than the server thread");
		}
		lines.push_back("[Tracer]   native backtrace:");
		for (const std::string& frame : walk(context)) {
			lines.push_back("[Tracer]     " + frame);
		}
		lines.push_back("[Tracer]   registers:");
		lines.push_back("[Tracer]     EAX " + hex(context.Eax) + "  EBX " + hex(context.Ebx) + "  ECX " + hex(context.Ecx) + "  EDX " + hex(context.Edx));
		lines.push_back("[Tracer]     ESI " + hex(context.Esi) + "  EDI " + hex(context.Edi) + "  EBP " + hex(context.Ebp) + "  ESP " + hex(context.Esp));
		lines.push_back("[Tracer]     EIP " + hex(context.Eip) + "  EFLAGS " + hex(context.EFlags));
		std::vector<std::string> modules = serverModules();
		if (!modules.empty()) {
			lines.push_back("[Tracer]   modules of the server:");
			for (const std::string& module : modules) {
				lines.push_back("[Tracer]     " + module);
			}
		}
		printBlock(LogLevel::Error, lines);
	}
	return previousFilter != nullptr ? previousFilter(pointers) : EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI consoleHandler(DWORD type) {
	State& s = state();
	if ((type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) && s.settings.enable && s.watch.callStart.load() != 0) {
		reportRunningCall(true);
	}
	return FALSE;
}

#endif

}

std::vector<std::string> nativeBacktrace(const void* context) {
#if defined _WIN32
	CONTEXT copy;
	if (context != nullptr) {
		copy = *static_cast<const CONTEXT*>(context);
	} else {
		RtlCaptureContext(&copy);
	}
	return walk(copy);
#else
	(void)context;
	return {};
#endif
}

void installCrashHandlers() {
#if defined _WIN32
	if (installed) {
		return;
	}
	installed = true;
	previousFilter = SetUnhandledExceptionFilter(crashFilter);
	SetConsoleCtrlHandler(consoleHandler, TRUE);
#endif
}

void removeCrashHandlers() {
#if defined _WIN32
	if (!installed) {
		return;
	}
	installed = false;
	SetUnhandledExceptionFilter(previousFilter);
	SetConsoleCtrlHandler(consoleHandler, FALSE);
#endif
}

}
