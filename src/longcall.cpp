#include "tracer.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#if defined _WIN32
	#include <windows.h>
#endif

namespace tracer {

namespace {

struct LongCallRecord {
	std::string key;
	uint64_t count = 0;
	uint64_t held = 0;
	uint64_t worst = 0;
	uint64_t lastPrintAt = 0;
	std::string script;
	std::string publicName;
};

std::vector<LongCallRecord> longCallRecords;
std::thread watchdog;
std::mutex watchdogLock;
std::condition_variable watchdogWake;

std::string nativeShare(const std::pair<Script*, int>& entry, State& s) {
	Script& script = *entry.first;
	uint32_t calls = script.callNativeCalls[entry.second];
	return formatTime(s.clock.microseconds(script.callNativeTime[entry.second])) + " in " + script.nativeName(entry.second) + " (" + std::to_string(calls)
		+ (calls == 1 ? " call)" : " calls)");
}

void keepLongCall(LongCall call) {
	State& s = state();
	auto at = std::upper_bound(s.longCalls.begin(), s.longCalls.end(), call, [](const LongCall& a, const LongCall& b) { return a.duration > b.duration; });
	s.longCalls.insert(at, std::move(call));
	if (s.longCalls.size() > LongCallCount) {
		s.longCalls.resize(LongCallCount);
	}
}

void watchdogLoop();

}

void reportRunningCall(bool interrupted) {
	State& s = state();
	AMX* amx = s.watch.amx.load(std::memory_order_relaxed);
	uint64_t start = s.watch.callStart.load(std::memory_order_relaxed);
	if (amx == nullptr || start == 0) {
		return;
	}
	cell cip = amx->cip;
	cell frm = amx->frm;
	bool located = false;
#if defined _WIN32
	HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, DWORD(s.mainThread));
	if (thread != nullptr) {
		if (SuspendThread(thread) != DWORD(-1)) {
			CONTEXT context;
			context.ContextFlags = CONTEXT_CONTROL;
			if (GetThreadContext(thread, &context) && s.jit != nullptr) {
				cell jitCip, jitFrm;
				if (s.jit->locate(amx, uintptr_t(context.Eip), uintptr_t(context.Ebp), jitCip, jitFrm)) {
					cip = jitCip;
					frm = jitFrm;
					located = true;
				}
			}
			cip = located ? cip : amx->cip;
			frm = located ? frm : amx->frm;
			ResumeThread(thread);
		}
		CloseHandle(thread);
	}
#endif
	Script* script = nullptr;
	for (auto& entry : s.scripts) {
		if (entry.first == amx) {
			script = entry.second;
		}
	}
	if (script == nullptr) {
		return;
	}
	double running = s.clock.microseconds(Clock::now() - start);
	std::string publicName;
	size_t from = s.calls.size();
	for (size_t index = from; index-- > 0;) {
		if (s.calls[index].kind == CallKind::Public && s.calls[index].script == script) {
			publicName = script->publicName(s.calls[index].index, s.calls[index].name);
			break;
		}
	}
	std::vector<std::string> lines;
	std::string who = (publicName.empty() ? std::string("a call") : "public " + publicName) + " of " + script->name;
	if (interrupted) {
		lines.push_back("[Tracer] The server was interrupted while " + who + " was running, for " + formatTime(running));
	} else {
		lines.push_back("[Tracer] " + who + " has been running for " + formatTime(running) + ", it may be stuck in a loop");
	}
	size_t top = s.calls.size();
	if (top != 0 && s.calls[top - 1].kind == CallKind::Native) {
		const Call& native = s.calls[top - 1];
		lines.push_back("[Tracer]   it was in native " + native.script->nativeName(native.index));
	}
	cell place = located ? cip : cip - cell(sizeof(cell));
	std::string where = location(*script, place);
	if (!where.empty()) {
		lines.push_back(std::string("[Tracer]   ") + (located ? "at " : "near ") + where);
		std::string text = s.settings.source ? sourceLine(*script, place) : std::string();
		if (!text.empty()) {
			lines.push_back("[Tracer]   " + std::to_string(script->debug->lineAt(ucell(place))) + " | " + text);
		}
	}
	for (const std::string& frame : backtrace(*script, place, frm, from)) {
		lines.push_back("[Tracer]   " + frame);
	}
	printBlock(LogLevel::Error, lines);
}

namespace {

void watchdogLoop() {
	State& s = state();
	uint32_t reported = 0;
	std::unique_lock<std::mutex> lock(watchdogLock);
	while (!s.watch.stop.load()) {
		watchdogWake.wait_for(lock, std::chrono::milliseconds(100));
		if (s.watch.stop.load() || s.hangTicks == 0 || !s.started || !s.settings.enable) {
			continue;
		}
		uint64_t start = s.watch.callStart.load(std::memory_order_relaxed);
		uint32_t serial = s.watch.callSerial.load(std::memory_order_relaxed);
		if (start == 0 || serial == reported || Clock::now() - start < s.hangTicks) {
			continue;
		}
		reported = serial;
		lock.unlock();
		reportRunningCall(false);
		lock.lock();
	}
}

}

void beginTopCall(uint64_t now) {
	State& s = state();
	s.watch.callStart.store(now, std::memory_order_relaxed);
	s.watch.callSerial.fetch_add(1, std::memory_order_relaxed);
	s.longCallPassed = false;
	s.longCallTrace.clear();
	s.longCallStart = now;
	s.longCallLimitAt = s.longCallTicks != 0 && s.longCallEnabled ? now + s.longCallTicks : UINT64_MAX;
	for (const std::pair<Script*, int>& entry : s.callNatives.touched) {
		entry.first->callNativeTime[entry.second] = 0;
		entry.first->callNativeCalls[entry.second] = 0;
	}
	s.callNatives.touched.clear();
	s.callNatives.slowest = 0;
	s.callNatives.slowestScript = nullptr;
	s.callNatives.slowestNative = -1;
}

void passLongCallLimit(const Call& native) {
	State& s = state();
	s.longCallLimitAt = UINT64_MAX;
	s.longCallPassed = true;
	s.longCallTrace = nativeCallBacktrace(native, s.calls.size());
}

void endTopCall(Script& script, int index, uint32_t name, uint64_t end) {
	State& s = state();
	s.watch.callStart.store(0, std::memory_order_relaxed);
	uint64_t start = s.longCallStart;
	if (s.longCallTicks == 0 || !s.longCallEnabled || end - start < s.longCallTicks || index == AMX_EXEC_MAIN) {
		return;
	}
	uint64_t duration = end - start;
	std::string publicName = script.publicName(index, name);
	std::string key = script.name + "|" + publicName;
	LongCallRecord* record = nullptr;
	for (LongCallRecord& candidate : longCallRecords) {
		if (candidate.key == key) {
			record = &candidate;
			break;
		}
	}
	bool first = record == nullptr;
	if (first) {
		longCallRecords.emplace_back();
		record = &longCallRecords.back();
		record->key = key;
		record->script = script.name;
		record->publicName = publicName;
	}
	++record->count;
	record->worst = std::max(record->worst, duration);

	std::vector<std::string> lines;
	lines.push_back("[Tracer] Long call: public " + publicName + " of " + script.name + " took " + formatTime(s.clock.microseconds(duration)) + ", the limit is "
		+ formatTime(double(s.longCallCurrent)));
	std::vector<std::pair<Script*, int>> natives = s.callNatives.touched;
	std::sort(natives.begin(), natives.end(), [](const std::pair<Script*, int>& a, const std::pair<Script*, int>& b) {
		return a.first->callNativeTime[a.second] > b.first->callNativeTime[b.second];
	});
	uint64_t nativeTime = 0;
	for (const std::pair<Script*, int>& entry : natives) {
		nativeTime += entry.first->callNativeTime[entry.second];
	}
	if (!natives.empty()) {
		std::string text = "natives took " + formatTime(s.clock.microseconds(nativeTime)) + ": ";
		size_t shown = std::min<size_t>(natives.size(), 3);
		for (size_t entry = 0; entry != shown; ++entry) {
			text += (entry == 0 ? "" : ", ") + nativeShare(natives[entry], s);
		}
		if (natives.size() > shown) {
			text += " and " + std::to_string(natives.size() - shown) + " more";
		}
		lines.push_back("[Tracer]   " + text);
	}
	lines.push_back("[Tracer]   Pawn code took " + formatTime(s.clock.microseconds(duration > nativeTime ? duration - nativeTime : 0)));
	if (s.callNatives.slowestScript != nullptr && s.clock.microseconds(s.callNatives.slowest) >= double(s.longCallCurrent) / 4.0) {
		Script& owner = *s.callNatives.slowestScript;
		std::string where = location(owner, s.callNatives.slowestCip - cell(sizeof(cell)));
		lines.push_back("[Tracer]   the slowest native call was " + owner.nativeName(s.callNatives.slowestNative) + ", " + formatTime(s.clock.microseconds(s.callNatives.slowest))
			+ (where.empty() ? std::string() : " at " + where));
	}
	if (s.longCallPassed && !s.longCallTrace.empty()) {
		lines.push_back("[Tracer]   when it passed " + formatTime(double(s.longCallCurrent)) + " it was at:");
		for (const std::string& frame : s.longCallTrace) {
			lines.push_back("[Tracer]   " + frame);
		}
	}

	LongCall call;
	call.script = script.name;
	call.publicName = publicName;
	call.duration = duration;
	call.at = end;
	call.report = lines;
	keepLongCall(std::move(call));

	if (first || s.settings.errorRepeat == 0) {
		record->lastPrintAt = end;
		printBlock(LogLevel::Warning, lines);
		return;
	}
	++record->held;
	if (s.clock.microseconds(end - record->lastPrintAt) >= double(s.settings.errorRepeat) * 1000000.0) {
		flushLongCalls(false);
	}
}

void flushLongCalls(bool all) {
	State& s = state();
	uint64_t now = Clock::now();
	for (LongCallRecord& record : longCallRecords) {
		if (record.held == 0) {
			continue;
		}
		double seconds = s.clock.microseconds(now - record.lastPrintAt) / 1000000.0;
		if (!all && seconds < double(s.settings.errorRepeat)) {
			continue;
		}
		char buffer[64];
		std::snprintf(buffer, sizeof(buffer), "%.0f s", seconds);
		print(LogLevel::Warning, "[Tracer] Long call: public " + record.publicName + " of " + record.script + " took longer than the limit " + std::to_string(record.held)
			+ (record.held == 1 ? " more time" : " more times") + " in the last " + buffer + ", the worst took " + formatTime(s.clock.microseconds(record.worst)));
		record.held = 0;
		record.lastPrintAt = now;
	}
}

void setLongCallTime(int microseconds) {
	State& s = state();
	s.longCallCurrent = std::max(0, microseconds);
	s.longCallTicks = s.longCallCurrent != 0 ? s.clock.ticksFor(double(s.longCallCurrent)) : 0;
}

void restartLongCall() {
	State& s = state();
	if (s.longCallTicks != 0 && s.longCallEnabled && !s.calls.empty()) {
		uint64_t now = Clock::now();
		s.longCallStart = now;
		s.longCallLimitAt = now + s.longCallTicks;
		s.longCallPassed = false;
		s.longCallTrace.clear();
	}
}

void startWatchdog() {
	State& s = state();
	if (watchdog.joinable()) {
		return;
	}
	s.watch.stop.store(false);
	watchdog = std::thread(watchdogLoop);
}

void stopWatchdog() {
	State& s = state();
	if (!watchdog.joinable()) {
		return;
	}
	{
		std::lock_guard<std::mutex> guard(watchdogLock);
		s.watch.stop.store(true);
	}
	watchdogWake.notify_all();
	watchdog.join();
}

}
