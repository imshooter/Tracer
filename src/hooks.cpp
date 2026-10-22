#include "tracer.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>

#include <subhook.h>

#if defined _WIN32
	#include <intrin.h>
	#include <windows.h>
#else
	#include <pthread.h>
	#include <x86intrin.h>
#endif

namespace tracer {

namespace {

subhook::Hook execHook;

const std::string unknownName = "?";

bool onMainThread() {
#if defined _WIN32
	return GetCurrentThreadId() == state().mainThread;
#else
	return (unsigned long)pthread_self() == state().mainThread;
#endif
}

void record(Stat& stat, uint64_t total, uint64_t self, uint64_t end) {
	State& s = state();
	++stat.calls;
	stat.total += total;
	stat.self += self;
	if (total > stat.worst) {
		stat.worst = total;
		stat.worstAt = end;
	}
	int bucket = 0;
	while (bucket != BucketCount - 1 && total >= s.bucketLimits[bucket]) {
		++bucket;
	}
	++stat.buckets[bucket];
}

void pop(uint64_t end) {
	State& s = state();
	Call call = s.calls.back();
	s.calls.pop_back();
	uint64_t total = end - call.start;
	uint64_t self = total > call.children ? total - call.children : 0;
	if (call.stat != nullptr) {
		record(*call.stat, total, self, end);
		++s.events;
	}

	switch (call.kind) {
	case CallKind::Public:
		s.topPublic = call.previousTop;
		s.watch.amx.store(s.topPublic == -1 ? nullptr : s.calls[s.topPublic].script->amx, std::memory_order_relaxed);
		break;
	case CallKind::Native:
		if (call.stat != nullptr) {
			++s.second.natives;
			if (s.settings.profileCallers && call.slot != -1) {
				uint32_t key = (uint32_t(call.slot) << 16) | uint32_t(call.index & 0xFFFF);
				record(call.script->pairs[key], total, self, end);
			}
		}
		break;
	case CallKind::Function:
		break;
	}

	if (s.calls.empty()) {
		if (s.profiling) {
			s.pawnTotal += total;
			s.pendingTick += total;
			++s.second.callbacks;
			if (call.kind == CallKind::Public && call.slot != -1) {
				s.tickEntries.push_back({ call.script, call.slot, total });
			}
		}
	} else {
		s.calls.back().children += total;
	}
}

void popReturned(Script& script, cell frame, bool same) {
	State& s = state();
	uint64_t now = 0;
	while (!s.calls.empty()) {
		const Call& top = s.calls.back();
		if (top.kind != CallKind::Function || top.script != &script || (same ? top.frm > frame : top.frm >= frame)) {
			break;
		}
		if (now == 0) {
			now = Clock::now();
		}
		pop(now);
	}
}

void rememberSlowTick(uint64_t pawn) {
	State& s = state();
	SlowTick tick;
	tick.at = Clock::now();
	tick.pawn = pawn;
	tick.callbacks = uint32_t(s.tickEntries.size());
	struct Group {
		Script* script;
		int slot;
		uint32_t calls;
		uint64_t total;
	};
	std::vector<Group> groups;
	for (const TickEntry& entry : s.tickEntries) {
		bool found = false;
		for (Group& group : groups) {
			if (group.script == entry.script && group.slot == entry.slot) {
				++group.calls;
				group.total += entry.total;
				found = true;
				break;
			}
		}
		if (!found) {
			groups.push_back({ entry.script, entry.slot, 1, entry.total });
		}
	}
	std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return a.total > b.total; });
	if (groups.size() > SlowTickGroups) {
		groups.resize(SlowTickGroups);
	}
	for (const Group& group : groups) {
		TickGroup named;
		named.script = group.script->name;
		named.name = group.script->slotName(group.slot);
		named.calls = group.calls;
		named.total = group.total;
		tick.groups.push_back(std::move(named));
	}
	auto at = std::upper_bound(s.slowTicks.begin(), s.slowTicks.end(), tick, [](const SlowTick& a, const SlowTick& b) { return a.pawn > b.pawn; });
	s.slowTicks.insert(at, std::move(tick));
	if (s.slowTicks.size() > SlowTickCount) {
		s.slowTicks.resize(SlowTickCount);
	}
}

bool traceMatches(const std::string& line) {
	State& s = state();
	return s.traceFilter == nullptr || std::regex_search(line, *s.traceFilter);
}

void countNative(Script& script, const Call& call, uint64_t self, uint64_t end) {
	State& s = state();
	int index = call.index;
	if (script.callNativeCalls[index]++ == 0) {
		s.callNatives.touched.emplace_back(&script, index);
	}
	script.callNativeTime[index] += self;
	if (self > s.callNatives.slowest) {
		s.callNatives.slowest = self;
		s.callNatives.slowestScript = &script;
		s.callNatives.slowestNative = index;
		s.callNatives.slowestCip = call.cip;
		s.callNatives.slowestFrm = call.frm;
	}
	if (end >= s.longCallLimitAt) {
		passLongCallLimit(call);
	}
}

int AMXAPI execHookFunction(AMX* amx, cell* retval, int index) {
	State& s = state();
	auto original = reinterpret_cast<int(AMXAPI*)(AMX*, cell*, int)>(s.execTrampoline);
	if (!s.settings.enable || (amx->flags & AMX_FLAG_BROWSE) || !onMainThread()) {
		return original(amx, retval, index);
	}
	Script* script = findOrAddScript(amx);
	if (!script->traced || s.calls.size() >= CallDepth) {
		return original(amx, retval, index);
	}
	size_t depth = s.calls.size();
	int slot = s.profiling ? script->slotFor(index) : -1;
	if (s.traceFlags & TracePublics) {
		std::string line = "public " + formatPublicCall(*script, index);
		if (traceMatches(line)) {
			print(LogLevel::Message, "[Tracer] trace: " + line);
		}
	}
	uint64_t start = Clock::now();
	if (depth == 0) {
		beginTopCall(start);
	}
	Call call;
	call.stat = slot != -1 ? &script->publics[slot] : nullptr;
	call.script = script;
	call.start = start;
	call.children = 0;
	call.kind = CallKind::Public;
	call.index = index;
	call.slot = slot;
	call.previousTop = s.topPublic;
	call.frm = amx->frm;
	call.cip = amx->cip;
	call.own = 0;
	call.name = script->nameOffset(index);
	call.entry = script->entryOf(index);
	s.calls.push_back(call);
	s.topPublic = int(depth);
	s.watch.amx.store(amx, std::memory_order_relaxed);
	uint32_t jitErrors = s.jit != nullptr ? s.jit->errorCount() : 0;
	script->raisedNative = -1;
	script->seenCip = 0;
	script->seenFrm = 0;
	cell entryStk = amx->stk;
	cell entryHea = amx->hea;

	int error = original(amx, retval, index);

	uint64_t end = Clock::now();
	if (error != AMX_ERR_NONE && error != AMX_ERR_SLEEP) {
		amx->stk = entryStk;
		amx->hea = entryHea;
		uint32_t jitNow = s.jit != nullptr ? s.jit->errorCount() : 0;
		bool fromJit = jitNow != jitErrors && jitNow != s.jitConsumed;
		s.jitConsumed = jitNow;
		if (s.settings.errors && !s.reporting) {
			Failure failure = locateFailure(*script, error, index, fromJit, depth);
			failure.publicName = call.name;
			reportFailure(failure);
		}
	}
	while (s.calls.size() > depth) {
		pop(end);
	}
	if (depth == 0) {
		endTopCall(*script, index, call.name, end);
	}
	return error;
}

int profiledNative(Script& script, AMX* amx, AMX_CALLBACK previous, cell index, cell* result, const cell* params) {
	State& s = state();
	if (s.profiling) {
		if (!script.memorySeen || amx->stk < script.lowestStack) {
			script.lowestStack = amx->stk;
		}
		if (!script.memorySeen || amx->hea > script.highestHeap) {
			script.highestHeap = amx->hea;
		}
		script.memorySeen = true;
	}
	if (s.functionFrames && script.debug) {
		popReturned(script, amx->frm, false);
	}
	if (s.traceFlags & TraceNatives) {
		std::string line = "native " + script.nativeName(int(index)) + "()";
		if (traceMatches(line)) {
			print(LogLevel::Message, "[Tracer] trace: " + line);
		}
	}
	int slot = -1;
	if (s.topPublic != -1 && s.calls[s.topPublic].script == &script) {
		slot = s.calls[s.topPublic].slot;
	}
	Call call;
	call.stat = s.profiling && s.settings.profileNatives ? &script.natives[index] : nullptr;
	call.script = &script;
	call.children = 0;
	call.kind = CallKind::Native;
	call.index = int(index);
	call.slot = slot;
	call.previousTop = -1;
	call.frm = amx->frm;
	call.cip = amx->cip;
	call.own = 0;
	call.name = 0;
	call.entry = 0;
	size_t depth = s.calls.size();
	call.start = Clock::now();
	if (call.start >= s.longCallLimitAt) {
		passLongCallLimit(call);
	}
	s.calls.push_back(call);

	int error = previous(amx, index, result, params);

	uint64_t end = Clock::now();
	uint64_t children = s.calls.size() > depth ? s.calls[depth].children : 0;
	while (s.calls.size() > depth) {
		pop(end);
	}
	uint64_t total = end - call.start;
	uint64_t self = total > children ? total - children : 0;
	if (s.longCallTicks != 0) {
		countNative(script, call, self, end);
	}
	return error;
}

int AMXAPI callbackHookFunction(AMX* amx, cell index, cell* result, const cell* params) {
	State& s = state();
	Script* script = findScript(amx);
	AMX_CALLBACK previous = script != nullptr && script->previousCallback != nullptr ? script->previousCallback : s.exportedCallback;
	size_t depth = s.calls.size();
	if (script == nullptr || !script->traced || !s.settings.enable || ucell(index) >= ucell(script->nativeCount) || depth == 0 || depth >= CallDepth) {
		return previous(amx, index, result, params);
	}
	cell frm = amx->frm;
	cell cip = amx->cip;
	int error;
	if (s.profiling || s.traceFlags != 0) {
		error = profiledNative(*script, amx, previous, index, result, params);
	} else {
		Call& call = s.calls.emplace_back();
		call.stat = nullptr;
		call.script = script;
		call.children = 0;
		call.kind = CallKind::Native;
		call.index = int(index);
		call.slot = -1;
		call.previousTop = -1;
		call.frm = frm;
		call.cip = cip;
		call.own = 0;
		call.name = 0;
		call.entry = 0;
		bool timed = s.longCallTicks != 0;
		call.start = timed ? Clock::now() : 0;
		if (call.start >= s.longCallLimitAt) {
			passLongCallLimit(call);
		}

		error = previous(amx, index, result, params);

		if (s.calls.size() > depth + 1) {
			uint64_t now = Clock::now();
			while (s.calls.size() > depth + 1) {
				pop(now);
			}
		}
		if (timed) {
			uint64_t end = Clock::now();
			Call done = s.calls.back();
			s.calls.pop_back();
			uint64_t total = end - done.start;
			s.calls.back().children += total;
			countNative(*script, done, total > done.children ? total - done.children : 0, end);
		} else {
			s.calls.pop_back();
		}
	}
	script->seenCip = cip;
	script->seenFrm = frm;
	++script->seenCount;
	if (error != AMX_ERR_NONE) {
		script->raisedNative = int(index);
		script->raisedError = error;
		script->raisedCip = cip;
		script->raisedFrm = frm;
	}
	return error;
}

int AMXAPI debugHookFunction(AMX* amx) {
	State& s = state();
	Script* script = findScript(amx);
	if (script == nullptr) {
		return AMX_ERR_NONE;
	}
	if (script->traced && s.settings.enable && !s.calls.empty()) {
		script->seenCip = amx->cip;
		script->seenFrm = amx->frm;
		++script->seenCount;
		if ((s.functionFrames || (s.traceFlags & TraceFunctions)) && script->debug && s.calls.back().script == script) {
			cell frm = amx->frm;
			Call& top = s.calls.back();
			if (top.kind == CallKind::Public && top.own == 0) {
				top.own = frm;
			} else {
				if (top.kind == CallKind::Function && frm > top.frm) {
					popReturned(*script, frm, false);
				}
				const unsigned char* code = amx->base + reinterpret_cast<AMX_HEADER*>(amx->base)->cod;
				if (amx->cip >= cell(2 * sizeof(cell)) && *reinterpret_cast<const cell*>(code + amx->cip - 2 * sizeof(cell)) == script->procOpcode) {
					popReturned(*script, frm, true);
					const Call& below = s.calls.back();
					bool own = below.kind == CallKind::Public && below.script == script && below.own == frm;
					int function = own ? -1 : script->functionAtEntry(amx->cip);
					if (function != -1) {
						if (s.traceFlags & TraceFunctions) {
							std::string line = "function " + formatCall(*script, function, frm);
							if (traceMatches(line)) {
								print(LogLevel::Message, "[Tracer] trace: " + line);
							}
						}
						if (s.functionFrames && s.calls.size() < CallDepth) {
							Call call;
							call.stat = &script->functions[function];
							call.script = script;
							call.children = 0;
							call.kind = CallKind::Function;
							call.index = function;
							call.slot = -1;
							call.previousTop = -1;
							call.frm = frm;
							call.cip = amx->cip;
							call.own = frm;
							call.name = 0;
							call.entry = 0;
							call.start = Clock::now();
							s.calls.push_back(call);
						}
					}
				}
			}
		}
	}
	return script->previousDebug != nullptr ? script->previousDebug(amx) : AMX_ERR_NONE;
}

}

State* stateInstance = new State();

uint64_t Clock::now() {
	return __rdtsc();
}

void Clock::calibrate() {
#if defined _WIN32
	LARGE_INTEGER frequency, start, current;
	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&start);
	uint64_t ticks = __rdtsc();
	do {
		QueryPerformanceCounter(&current);
	} while ((current.QuadPart - start.QuadPart) * 1000 < frequency.QuadPart * 25);
	uint64_t elapsed = __rdtsc() - ticks;
	double seconds = double(current.QuadPart - start.QuadPart) / double(frequency.QuadPart);
	perSecond_ = double(elapsed) / seconds;
	counterStart_ = start.QuadPart;
	counterFrequency_ = frequency.QuadPart;
	ticksStart_ = ticks;
#else
	auto start = std::chrono::steady_clock::now();
	uint64_t ticks = __rdtsc();
	while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(25)) {
	}
	auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	perSecond_ = double(__rdtsc() - ticks) / elapsed;
	counterStart_ = std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch()).count();
	counterFrequency_ = 1000000000;
	ticksStart_ = ticks;
#endif
}

void Clock::refine() {
#if defined _WIN32
	LARGE_INTEGER current;
	QueryPerformanceCounter(&current);
	int64_t counter = current.QuadPart;
#else
	int64_t counter = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
	uint64_t ticks = __rdtsc();
	double seconds = double(counter - counterStart_) / double(counterFrequency_);
	if (seconds > 1.0) {
		perSecond_ = double(ticks - ticksStart_) / seconds;
	}
}

Script::Script(AMX* amx)
	: amx(amx) {
	auto* header = reinterpret_cast<AMX_HEADER*>(amx->base);
	publicCount = int(NUMENTRIES(header, publics, natives));
	nativeCount = int(NUMENTRIES(header, natives, libraries));
	publics.resize(2);
	slotNames.emplace_back("main");
	slotNames.emplace_back("(continued after sleep)");
	indexSlot.assign(publicCount, -1);
	indexName.assign(publicCount, 0);
	natives.resize(nativeCount);
	callNativeTime.assign(nativeCount, 0);
	callNativeCalls.assign(nativeCount, 0);
}

int Script::slotFor(int index) {
	if (index == AMX_EXEC_MAIN) {
		return 0;
	}
	if (index == AMX_EXEC_CONT) {
		return 1;
	}
	if (index < 0 || index >= publicCount) {
		return -1;
	}
	auto* header = reinterpret_cast<AMX_HEADER*>(amx->base);
	auto* entry = GETENTRY(header, publics, index);
	uint32_t name = USENAMETABLE(header) ? reinterpret_cast<AMX_FUNCSTUBNT*>(entry)->nameofs : uint32_t(index) + 1;
	if (indexSlot[index] != -1 && indexName[index] == name) {
		return indexSlot[index];
	}
	std::string text = GETENTRYNAME(header, entry);
	int slot;
	auto found = nameSlot.find(text);
	if (found != nameSlot.end()) {
		slot = found->second;
	} else {
		slot = int(publics.size());
		publics.emplace_back();
		slotNames.push_back(text);
		nameSlot.emplace(std::move(text), slot);
	}
	indexSlot[index] = slot;
	indexName[index] = name;
	return slot;
}

void Script::captureNames() {
	if (!loaded) {
		return;
	}
	auto* header = reinterpret_cast<AMX_HEADER*>(amx->base);
	nativeNames.clear();
	for (int index = 0; index != nativeCount; ++index) {
		nativeNames.emplace_back(GETENTRYNAME(header, GETENTRY(header, natives, index)));
	}
}

const std::string& Script::slotName(int slot) const {
	if (slot >= 0 && slot < int(slotNames.size())) {
		return slotNames[slot];
	}
	return unknownName;
}

const std::string& Script::nativeName(int index) const {
	if (index >= 0 && index < int(nativeNames.size())) {
		return nativeNames[index];
	}
	if (loaded) {
		const_cast<Script*>(this)->captureNames();
		if (index >= 0 && index < int(nativeNames.size())) {
			return nativeNames[index];
		}
	}
	return unknownName;
}

cell Script::entryOf(int index) const {
	auto* header = reinterpret_cast<AMX_HEADER*>(amx->base);
	if (index == AMX_EXEC_MAIN) {
		return header->cip;
	}
	if (index < 0 || index >= publicCount) {
		return 0;
	}
	return cell((GETENTRY(header, publics, index))->address);
}

uint32_t Script::nameOffset(int index) const {
	if (!loaded || index < 0 || index >= publicCount) {
		return 0;
	}
	auto* header = reinterpret_cast<AMX_HEADER*>(amx->base);
	if (!USENAMETABLE(header)) {
		return 0;
	}
	return reinterpret_cast<AMX_FUNCSTUBNT*>(GETENTRY(header, publics, index))->nameofs;
}

std::string Script::publicName(int index, uint32_t offset) const {
	if (index == AMX_EXEC_MAIN) {
		return "main";
	}
	if (index == AMX_EXEC_CONT) {
		return "(continued after sleep)";
	}
	if (!loaded || index < 0 || index >= publicCount) {
		return "public " + std::to_string(index);
	}
	auto* header = reinterpret_cast<AMX_HEADER*>(amx->base);
	if (offset != 0 && offset < uint32_t(header->size)) {
		return reinterpret_cast<const char*>(amx->base) + offset;
	}
	return GETENTRYNAME(header, GETENTRY(header, publics, index));
}

int Script::functionAtEntry(cell cip) {
	auto found = entries.find(cip);
	if (found != entries.end()) {
		return found->second;
	}
	int function = debug ? debug->functionAt(ucell(cip)) : -1;
	entries.emplace(cip, function);
	return function;
}

void Script::resetStatistics() {
	for (Stat& stat : publics) {
		stat = Stat();
	}
	for (Stat& stat : natives) {
		stat = Stat();
	}
	pairs.clear();
	memorySeen = false;
	for (Stat& stat : functions) {
		stat = Stat();
	}
}

Script* findScript(AMX* amx) {
	State& s = state();
	if (amx == s.lastAmx) {
		return s.lastScript;
	}
	auto found = s.scripts.find(amx);
	if (found == s.scripts.end()) {
		return nullptr;
	}
	s.lastAmx = amx;
	s.lastScript = found->second;
	return found->second;
}

void forgetScript(Script& script) {
	State& s = state();
	s.tickEntries.erase(std::remove_if(s.tickEntries.begin(), s.tickEntries.end(), [&](const TickEntry& entry) { return entry.script == &script; }), s.tickEntries.end());
	s.callNatives.touched.erase(std::remove_if(s.callNatives.touched.begin(), s.callNatives.touched.end(), [&](const std::pair<Script*, int>& entry) { return entry.first == &script; }),
		s.callNatives.touched.end());
	if (s.callNatives.slowestScript == &script) {
		s.callNatives.slowestScript = nullptr;
		s.callNatives.slowest = 0;
	}
	s.scripts.erase(script.amx);
	s.lastAmx = nullptr;
	s.lastScript = nullptr;
}

Script* findOrAddScript(AMX* amx) {
	Script* script = findScript(amx);
	if (script != nullptr) {
		return script;
	}
	State& s = state();
	script = new Script(amx);
	s.scripts.emplace(amx, script);
	s.lastAmx = amx;
	s.lastScript = script;
	return script;
}

bool installExecHook(void* exec) {
	State& s = state();
	if (s.hooked) {
		return true;
	}
	if (!execHook.Install(exec, reinterpret_cast<void*>(execHookFunction))) {
		return false;
	}
	s.execTrampoline = execHook.GetTrampoline();
	s.hooked = s.execTrampoline != nullptr;
	return s.hooked;
}

void pinModule() {
#if defined _WIN32
	HMODULE module = nullptr;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCSTR>(&execHookFunction), &module);
#endif
}

void hookCallback(Script& script) {
	AMX* amx = script.amx;
	if (amx->callback == callbackHookFunction) {
		return;
	}
	script.previousCallback = amx->callback;
	amx->callback = callbackHookFunction;
}

void unhookCallback(Script& script) {
	if (script.amx->callback == callbackHookFunction) {
		script.amx->callback = script.previousCallback;
	}
}

void hookDebug(Script& script) {
	AMX* amx = script.amx;
	if (script.debug) {
		auto* header = reinterpret_cast<AMX_HEADER*>(amx->base);
		if (script.publicCount != 0) {
			const unsigned char* code = amx->base + header->cod;
			ucell address = (GETENTRY(header, publics, 0))->address;
			script.procOpcode = *reinterpret_cast<const cell*>(code + address);
		}
		script.functions.assign(script.debug->functions().size(), Stat());
		script.entries.clear();
	}
	if (amx->debug != debugHookFunction) {
		script.previousDebug = amx->debug;
		amx->debug = debugHookFunction;
	}
	script.debugHooked = true;
}

void unhookDebug(Script& script) {
	if (script.amx->debug == debugHookFunction) {
		script.amx->debug = script.previousDebug;
	}
	script.debugHooked = false;
}

void updateDebugHookUse() {
	State& s = state();
	s.functionFrames = s.profiling && s.settings.profileFunctions;
	if (s.jit != nullptr) {
		bool generated = s.functionFrames || (s.traceFlags & TraceFunctions);
		s.jit->setInterpreterDebugHook(generated ? nullptr : debugHookFunction);
	}
}

void startSession() {
	State& s = state();
	s.sessionStart = Clock::now();
	s.sessionStartTime = int64_t(std::time(nullptr));
	s.pawnTotal = 0;
	s.pendingTick = 0;
	s.ticks = 0;
	s.worstTick = 0;
	s.worstTickAt = 0;
	s.second = Second();
	s.secondStarted = false;
	s.seconds.clear();
	s.events = 0;
	s.reportedEvents = 0;
	s.tickEntries.clear();
	s.slowTicks.clear();
	s.sessionStarted = s.ticking;
	s.clockZero = s.sessionStart;
	s.startupPawn = 0;
	s.startupWall = 0;
}

void resetStatistics() {
	State& s = state();
	for (auto& entry : s.scripts) {
		entry.second->resetStatistics();
	}
	startSession();
}

void resetEverything() {
	State& s = state();
	resetStatistics();
	s.errors.clear();
	s.errorIndex.clear();
	s.errorTotal = 0;
	s.longCalls.clear();
}

void onTick(TimePoint now) {
	State& s = state();
	uint64_t pawn = s.pendingTick;
	s.pendingTick = 0;
	s.ticking = true;
	if (!s.started) {
		s.started = true;
	}
	if (!s.profiling) {
		return;
	}
	if (!s.sessionStarted) {
		s.sessionStarted = true;
		s.clockZero = Clock::now();
		s.startupPawn = pawn;
		s.startupWall = s.clockZero - s.sessionStart;
		s.secondStart = now;
		s.secondStarted = true;
		s.tickEntries.clear();
		return;
	}
	++s.ticks;
	if (pawn > s.worstTick) {
		s.worstTick = pawn;
		s.worstTickAt = Clock::now();
	}
	if (pawn != 0 && (s.slowTicks.size() < SlowTickCount || pawn > s.slowTicks.back().pawn)) {
		rememberSlowTick(pawn);
	}
	s.tickEntries.clear();
	if (!s.secondStarted) {
		s.secondStart = now;
		s.secondStarted = true;
	}
	++s.second.ticks;
	s.second.pawn += pawn;
	if (pawn > s.second.worstTick) {
		s.second.worstTick = pawn;
	}
	if (now - s.secondStart >= std::chrono::seconds(1)) {
		if (s.players != nullptr) {
			s.second.players = uint32_t(s.players->entries().size());
		}
		s.seconds.push_back(s.second);
		s.second = Second();
		s.secondStart += std::chrono::seconds(1);
		if (now - s.secondStart >= std::chrono::seconds(1)) {
			s.secondStart = now;
		}
		s.clock.refine();
	}
}

void measureOverhead() {
	State& s = state();
	Stat stat;
	const int rounds = 20000;
	uint64_t start = Clock::now();
	for (int round = 0; round != rounds; ++round) {
		Call call;
		call.stat = &stat;
		call.script = nullptr;
		call.start = Clock::now();
		call.children = 0;
		call.kind = CallKind::Native;
		call.index = 0;
		call.slot = -1;
		call.previousTop = -1;
		call.frm = 0;
		call.cip = 0;
		call.own = 0;
		call.name = 0;
		call.entry = 0;
		s.calls.push_back(call);
		Call done = s.calls.back();
		s.calls.pop_back();
		uint64_t end = Clock::now();
		record(stat, end - done.start, end - done.start, end);
	}
	s.overheadPerEvent = (Clock::now() - start) / rounds;
}

const char* errorText(int code) {
	switch (code) {
	case AMX_ERR_EXIT:
		return "forced exit";
	case AMX_ERR_ASSERT:
		return "assertion failed";
	case AMX_ERR_STACKERR:
		return "stack and heap collided";
	case AMX_ERR_BOUNDS:
		return "array index out of bounds";
	case AMX_ERR_MEMACCESS:
		return "invalid memory access";
	case AMX_ERR_INVINSTR:
		return "invalid instruction";
	case AMX_ERR_STACKLOW:
		return "stack underflow";
	case AMX_ERR_HEAPLOW:
		return "heap underflow";
	case AMX_ERR_CALLBACK:
		return "no callback, or invalid callback";
	case AMX_ERR_NATIVE:
		return "native function failed";
	case AMX_ERR_DIVIDE:
		return "divide by zero";
	case AMX_ERR_SLEEP:
		return "sleep";
	case AMX_ERR_INVSTATE:
		return "no implementation for this state";
	case AMX_ERR_MEMORY:
		return "out of memory";
	case AMX_ERR_FORMAT:
		return "invalid file format";
	case AMX_ERR_VERSION:
		return "file is for a newer version of the AMX";
	case AMX_ERR_NOTFOUND:
		return "function not found";
	case AMX_ERR_INDEX:
		return "invalid index parameter";
	case AMX_ERR_DEBUG:
		return "debugger cannot run";
	case AMX_ERR_INIT:
		return "AMX not initialized";
	case AMX_ERR_USERDATA:
		return "unable to set user data field";
	case AMX_ERR_INIT_JIT:
		return "cannot initialize the JIT";
	case AMX_ERR_PARAMS:
		return "parameter error";
	case AMX_ERR_DOMAIN:
		return "domain error, expression result does not fit in range";
	case AMX_ERR_GENERAL:
		return "general error";
	default:
		return "unknown error";
	}
}

}
