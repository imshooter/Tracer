#include "tracer.hpp"

namespace tracer {

namespace {

std::string join(const std::vector<std::string>& lines) {
	std::string text;
	for (const std::string& line : lines) {
		if (!text.empty()) {
			text += '\n';
		}
		text += line;
	}
	return text;
}

bool setString(AMX* amx, cell address, cell size, const std::string& text) {
	Script* script = findScript(amx);
	if (script == nullptr || script->api == nullptr || size <= 0) {
		return false;
	}
	cell* physical;
	if (script->api->GetAddr(address, &physical) != AMX_ERR_NONE) {
		return false;
	}
	return script->api->SetString(physical, StringView(text.data(), text.size()), false, false, size_t(size)) == AMX_ERR_NONE;
}

void printLines(const char* title, const std::vector<std::string>& lines) {
	std::vector<std::string> block;
	block.push_back(std::string("[Tracer] ") + title);
	for (const std::string& line : lines) {
		block.push_back("[Tracer]   " + line);
	}
	printBlock(LogLevel::Message, block);
}

cell AMX_NATIVE_CALL PrintBacktrace(AMX* amx, const cell* params) {
	(void)amx;
	(void)params;
	printLines("Backtrace:", currentBacktrace(false));
	return 1;
}

cell AMX_NATIVE_CALL GetBacktrace(AMX* amx, const cell* params) {
	return setString(amx, params[1], params[2], join(currentBacktrace(false)));
}

cell AMX_NATIVE_CALL PrintNativeBacktrace(AMX* amx, const cell* params) {
	(void)amx;
	(void)params;
	printLines("Native backtrace:", nativeBacktrace(nullptr));
	return 1;
}

cell AMX_NATIVE_CALL GetNativeBacktrace(AMX* amx, const cell* params) {
	return setString(amx, params[1], params[2], join(nativeBacktrace(nullptr)));
}

cell AMX_NATIVE_CALL Tracer_SetLongCallTime(AMX* amx, const cell* params) {
	(void)amx;
	setLongCallTime(params[1]);
	return 1;
}

cell AMX_NATIVE_CALL Tracer_GetLongCallTime(AMX* amx, const cell* params) {
	(void)amx;
	(void)params;
	return state().longCallCurrent;
}

cell AMX_NATIVE_CALL Tracer_GetDefaultLongCallTime(AMX* amx, const cell* params) {
	(void)amx;
	(void)params;
	return state().settings.longCallTime;
}

cell AMX_NATIVE_CALL Tracer_ResetLongCallTime(AMX* amx, const cell* params) {
	(void)amx;
	(void)params;
	setLongCallTime(state().settings.longCallTime);
	return 1;
}

cell AMX_NATIVE_CALL Tracer_EnableLongCall(AMX* amx, const cell* params) {
	(void)amx;
	(void)params;
	state().longCallEnabled = true;
	return 1;
}

cell AMX_NATIVE_CALL Tracer_DisableLongCall(AMX* amx, const cell* params) {
	(void)amx;
	(void)params;
	State& s = state();
	s.longCallEnabled = false;
	s.longCallLimitAt = UINT64_MAX;
	return 1;
}

cell AMX_NATIVE_CALL Tracer_IsLongCallEnabled(AMX* amx, const cell* params) {
	(void)amx;
	(void)params;
	State& s = state();
	return s.longCallEnabled && s.longCallCurrent != 0;
}

cell AMX_NATIVE_CALL Tracer_RestartLongCall(AMX* amx, const cell* params) {
	(void)amx;
	(void)params;
	restartLongCall();
	return 1;
}

#if defined TRACER_TEST_NATIVES
cell AMX_NATIVE_CALL Tracer_TestCrash(AMX* amx, const cell* params) {
	(void)amx;
	(void)params;
	volatile int* address = nullptr;
	*address = 1;
	return 0;
}
#endif

const AMX_NATIVE_INFO natives[] = {
	{ "PrintBacktrace", PrintBacktrace },
	{ "GetBacktrace", GetBacktrace },
	{ "PrintNativeBacktrace", PrintNativeBacktrace },
	{ "GetNativeBacktrace", GetNativeBacktrace },
	{ "PrintAmxBacktrace", PrintBacktrace },
	{ "GetAmxBacktrace", GetBacktrace },
	{ "Tracer_SetLongCallTime", Tracer_SetLongCallTime },
	{ "Tracer_GetLongCallTime", Tracer_GetLongCallTime },
	{ "Tracer_GetDefaultLongCallTime", Tracer_GetDefaultLongCallTime },
	{ "Tracer_ResetLongCallTime", Tracer_ResetLongCallTime },
	{ "Tracer_EnableLongCall", Tracer_EnableLongCall },
	{ "Tracer_DisableLongCall", Tracer_DisableLongCall },
	{ "Tracer_IsLongCallEnabled", Tracer_IsLongCallEnabled },
	{ "Tracer_RestartLongCall", Tracer_RestartLongCall },
#if defined TRACER_TEST_NATIVES
	{ "Tracer_TestCrash", Tracer_TestCrash },
#endif
};

}

void registerNatives(IPawnScript& script) {
	script.Register(natives, int(sizeof natives / sizeof natives[0]));
}

}
