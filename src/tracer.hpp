#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

#include <sdk.hpp>
#include <Server/Components/Console/console.hpp>
#include <Server/Components/Pawn/pawn.hpp>

#include "debuginfo.hpp"
#include "pawnjit.hpp"

namespace tracer {

/**
 * @brief      Upper limits of the duration buckets, in microseconds.
 *
 * The last bucket has no limit.
 */
constexpr double BucketLimits[] = { 10.0, 100.0, 1000.0, 10000.0, 100000.0 };
constexpr int BucketCount = 6;

/**
 * @brief      What was measured for a public, a native or a function, or for a native called inside a public.
 *
 * Durations are in clock ticks.
 */
struct Stat {
	uint64_t calls = 0;
	uint64_t total = 0;
	uint64_t self = 0;
	uint64_t worst = 0;
	uint64_t worstAt = 0;
	uint64_t buckets[BucketCount] = {};
};

/**
 * @brief      A loaded script.
 */
struct Script {
	AMX* amx = nullptr;
	IPawnScript* api = nullptr;
	std::string name;
	std::string path;
	bool loaded = false;
	bool traced = true;
	AMX_CALLBACK previousCallback = nullptr;
	AMX_DEBUG previousDebug = nullptr;
	bool debugHooked = false;
	bool numericOpcodes = true;
	bool hinted = false;
	int publicCount = 0;
	int nativeCount = 0;
	std::unique_ptr<DebugInfo> debug;
	std::vector<std::string> nativeNames;

	/**
	 * The last place the script was seen at, a statement it started or a native it called.
	 */
	cell seenCip = 0;
	cell seenFrm = 0;
	uint32_t seenCount = 0;

	/**
	 * The native that returned an error, and where it was called.
	 */
	int raisedNative = -1;
	int raisedError = AMX_ERR_NONE;
	cell raisedCip = 0;
	cell raisedFrm = 0;

	std::deque<Stat> publics;
	std::deque<std::string> slotNames;
	std::vector<int> indexSlot;
	std::vector<uint32_t> indexName;
	std::unordered_map<std::string, int> nameSlot;
	std::vector<Stat> natives;
	std::unordered_map<uint32_t, Stat> pairs;
	bool memorySeen = false;
	cell lowestStack = 0;
	cell highestHeap = 0;
	std::vector<Stat> functions;
	std::unordered_map<cell, int> entries;
	cell procOpcode = 0;

	std::vector<uint64_t> callNativeTime;
	std::vector<uint32_t> callNativeCalls;

	explicit Script(AMX* amx);

	/**
	 * @brief      The slot of a public about to run.
	 *
	 * The name is read at the call, because a library can rename and sort the table of publics while the script runs.
	 *
	 * @param      index  Index of the public in the table of publics.
	 *
	 * @return     - the slot
	 *             - `-1` the index is not timed
	 */
	int slotFor(int index);

	/**
	 * @brief      Copy the names of the natives, so a report can be written after the script is gone.
	 */
	void captureNames();

	const std::string& slotName(int slot) const;
	const std::string& nativeName(int index) const;

	/**
	 * @brief      Where the name of a public is in the name table, read when it is called.
	 *
	 * A library can sort the table of publics and cut their names while a call runs, so the index of a call can
	 * name another public by the time the call ends.
	 *
	 * @param      index  Index of the public.
	 *
	 * @return     - the offset of the name
	 *             - `0` the name is not in a name table
	 */
	uint32_t nameOffset(int index) const;

	/**
	 * @brief      The address of the code of a public, read when it is called.
	 *
	 * @param      index  Index of the public, or `AMX_EXEC_MAIN`.
	 */
	cell entryOf(int index) const;

	/**
	 * @brief      The name of a public.
	 *
	 * @param      index   Index of the public, or `AMX_EXEC_MAIN`.
	 * @param      offset  Offset of the name read when the public was called, or `0` to read it now.
	 */
	std::string publicName(int index, uint32_t offset = 0) const;

	/**
	 * @brief      The function that a statement right after a `proc` belongs to.
	 *
	 * @param      cip   Address of the statement.
	 *
	 * @return     - the index of the function in the debug information
	 *             - `-1` no function starts there
	 */
	int functionAtEntry(cell cip);

	void resetStatistics();
};

/**
 * @brief      The time stamp counter of the CPU, turned into time with a rate measured against the system clock.
 */
class Clock {
public:
	void calibrate();
	void refine();
	double microseconds(uint64_t ticks) const { return double(ticks) * 1000000.0 / perSecond_; }
	uint64_t ticksFor(double microseconds) const { return uint64_t(microseconds * perSecond_ / 1000000.0); }
	static uint64_t now();

private:
	double perSecond_ = 1000000000.0;
	int64_t counterStart_ = 0;
	int64_t counterFrequency_ = 1;
	uint64_t ticksStart_ = 0;
};

/**
 * @brief      Pawn time and activity of one second.
 */
struct Second {
	uint64_t pawn = 0;
	uint64_t worstTick = 0;
	uint32_t ticks = 0;
	uint32_t callbacks = 0;
	uint32_t natives = 0;
	uint32_t errors = 0;
	uint32_t players = 0;
};

/**
 * @brief      A public the server called in the current tick.
 */
struct TickEntry {
	Script* script;
	int slot;
	uint64_t total;
};

/**
 * @brief      The publics of a slow tick that share a name.
 */
struct TickGroup {
	std::string script;
	std::string name;
	uint32_t calls = 0;
	uint64_t total = 0;
};

/**
 * @brief      One of the slowest ticks and the publics that ran in it.
 */
struct SlowTick {
	uint64_t at = 0;
	uint64_t pawn = 0;
	uint32_t callbacks = 0;
	std::vector<TickGroup> groups;
};

constexpr size_t SlowTickCount = 10;

/**
 * @brief      The deepest the call stack is followed.
 */
constexpr size_t CallDepth = 4096;
constexpr size_t SlowTickGroups = 6;

enum class CallKind : uint8_t {
	Public,
	Native,
	Function
};

/**
 * @brief      A call in progress.
 *
 * For a public, `frm` and `cip` are the registers of the script when it was called, which are those of the native
 * that called it, and `own` is its frame once its first statement ran. For a native, they are the frame of the code
 * that called it and the address it returns to. For a function, `frm` is its own frame.
 */
struct Call {
	Stat* stat;
	Script* script;
	uint64_t start;
	uint64_t children;
	CallKind kind;
	int index;
	int slot;
	int previousTop;
	cell frm;
	cell cip;
	cell own;
	uint32_t name;
	cell entry;
};

/**
 * @brief      A run time error, found where the script was when it failed.
 */
struct Failure {
	Script* script = nullptr;
	int code = AMX_ERR_NONE;
	int publicIndex = 0;
	uint32_t publicName = 0;
	size_t depth = 0;
	cell cip = 0;
	cell frm = 0;
	cell stk = 0;
	cell hea = 0;
	cell pri = 0;
	cell alt = 0;
	bool located = false;
	bool exact = false;
	bool start = false;
	bool registers = false;
	bool fault = false;
	int native = -1;

	/**
	 * @brief      An address inside the instruction that failed, for lines and functions.
	 */
	cell place() const { return start ? cip : cip - cell(sizeof(cell)); }
};

/**
 * @brief      A run time error and how often it happened.
 */
struct ErrorRecord {
	std::string key;
	std::string script;
	std::string publicName;
	std::string where;
	int code = 0;
	std::vector<std::string> report;
	bool shown = false;
	uint64_t count = 0;
	uint64_t held = 0;
	uint64_t firstAt = 0;
	uint64_t lastAt = 0;
	uint64_t lastPrintAt = 0;
};

/**
 * @brief      A call that took longer than `long_call_time`.
 */
struct LongCall {
	std::string script;
	std::string publicName;
	uint64_t duration = 0;
	uint64_t at = 0;
	std::vector<std::string> report;
};

constexpr size_t LongCallCount = 20;

/**
 * @brief      The time the natives of the current top level call took.
 */
struct CallNatives {
	std::vector<std::pair<Script*, int>> touched;
	uint64_t slowest = 0;
	Script* slowestScript = nullptr;
	int slowestNative = -1;
	cell slowestCip = 0;
	cell slowestFrm = 0;
};

struct Settings {
	bool enable = true;
	std::vector<std::string> scripts;
	bool errors = true;
	int errorRepeat = 10;
	bool source = true;
	int longCallTime = 5000;
	int hangTime = 3000;
	bool crash = true;
	std::string trace;
	std::string traceFilter;
	std::string log;
	bool profile = false;
	bool profileNatives = true;
	bool profileCallers = true;
	bool profileFunctions = false;
	std::string output = "tracer";
};

enum TraceFlags : unsigned {
	TraceNatives = 1,
	TracePublics = 2,
	TraceFunctions = 4
};

/**
 * @brief      What the watchdog thread reads while the server thread runs Pawn.
 */
struct Watch {
	std::atomic<uint64_t> callStart{ 0 };
	std::atomic<uint32_t> callSerial{ 0 };
	std::atomic<AMX*> amx{ nullptr };
	std::atomic<bool> stop{ false };
};

struct State {
	Settings settings;
	unsigned traceFlags = 0;
	std::unique_ptr<std::regex> traceFilter;
	ICore* core = nullptr;
	IPawnComponent* pawn = nullptr;
	IConsoleComponent* console = nullptr;
	IPlayerPool* players = nullptr;
	IPawnJITDiagnostics* jit = nullptr;
	bool hooked = false;
	bool profiling = false;
	bool functionFrames = false;
	unsigned long mainThread = 0;
	Clock clock;
	uint64_t bucketLimits[BucketCount - 1] = {};
	std::unordered_map<AMX*, Script*> scripts;
	AMX* lastAmx = nullptr;
	Script* lastScript = nullptr;
	std::vector<Call> calls;
	int topPublic = -1;

	bool reporting = false;
	uint32_t jitConsumed = 0;
	std::vector<ErrorRecord> errors;
	std::unordered_map<std::string, size_t> errorIndex;
	uint64_t errorTotal = 0;

	uint64_t longCallTicks = 0;
	bool longCallEnabled = true;
	int longCallCurrent = 0;
	uint64_t longCallStart = 0;
	uint64_t longCallLimitAt = UINT64_MAX;
	bool longCallPassed = false;
	std::vector<std::string> longCallTrace;
	CallNatives callNatives;
	std::vector<LongCall> longCalls;

	Watch watch;
	uint64_t hangTicks = 0;
	bool started = false;

	std::FILE* logFile = nullptr;
	std::string configPath = "config.json";
	int64_t configStamp = 0;

	std::vector<TickEntry> tickEntries;
	std::vector<SlowTick> slowTicks;
	uint64_t sessionStart = 0;
	int64_t sessionStartTime = 0;
	bool ticking = false;
	bool sessionStarted = false;
	uint64_t clockZero = 0;
	uint64_t startupPawn = 0;
	uint64_t startupWall = 0;
	uint64_t pawnTotal = 0;
	uint64_t pendingTick = 0;
	uint64_t ticks = 0;
	uint64_t worstTick = 0;
	uint64_t worstTickAt = 0;
	Second second;
	TimePoint secondStart;
	bool secondStarted = false;
	std::vector<Second> seconds;
	uint64_t events = 0;
	uint64_t reportedEvents = 0;
	uint64_t overheadPerEvent = 0;
	AMX_CALLBACK exportedCallback = nullptr;
	void* execTrampoline = nullptr;
};

extern State* stateInstance;

/**
 * @brief      The state of the component.
 *
 * It lives until the process ends, so a hook that runs while the server shuts down never reads freed memory.
 */
inline State& state() {
	return *stateInstance;
}

Script* findScript(AMX* amx);
Script* findOrAddScript(AMX* amx);

/**
 * @brief      Remove an unloading script from every list that refers to it.
 *
 * @param      script  Script that unloads.
 */
void forgetScript(Script& script);

/**
 * @brief      Hook `amx_Exec`.
 *
 * It runs after the legacy plugins and PawnJIT are loaded, so the hook sits on top of theirs.
 *
 * @param      exec  Address of `amx_Exec`.
 *
 * @return     - `true` the hook is installed
 *             - `false` the hook could not be installed
 */
bool installExecHook(void* exec);

/**
 * @brief      Keep the component loaded until the process ends, because the hook of `amx_Exec` jumps into it.
 */
void pinModule();

/**
 * @brief      Put the callback of the component in front of the callback of a script, so its natives are seen.
 *
 * @param      script  Script to hook.
 */
void hookCallback(Script& script);
void unhookCallback(Script& script);

/**
 * @brief      Put the debug hook of the component in front of the hook of a script.
 *
 * The hook keeps the last statement the script reached, and times functions when `profile_functions` is on.
 *
 * @param      script  Script to hook.
 */
void hookDebug(Script& script);
void unhookDebug(Script& script);

/**
 * @brief      Tell PawnJIT whether its generated code must call the debug hook, which it must for function timing and
 *             function tracing.
 */
void updateDebugHookUse();

void startSession();

/**
 * @brief      Clear what profiling measured and start a new session.
 */
void resetStatistics();

/**
 * @brief      Clear what profiling measured, the run time errors and the long calls.
 */
void resetEverything();
void onTick(TimePoint now);
void measureOverhead();

/**
 * @brief      Apply a new set of settings, and say what changed when `announce` is set.
 */
void applySettings(const Settings& settings, bool announce);

/**
 * @brief      The settings in the configuration the server started with.
 */
Settings readServerSettings(IConfig& config);

/**
 * @brief      Whether a script is in `scripts`, or the list is empty.
 */
bool isTraced(const std::string& name);

/**
 * @brief      Read `config.json` again when it changed since it was read last.
 *
 * @param      force  Read it even when it did not change.
 *
 * @return     - `true` the file was read
 *             - `false` it did not change or could not be read
 */
bool reloadConfig(bool force);

const char* errorText(int code);

/**
 * @brief      Write a line to the server log and to the log file of the component.
 */
void print(LogLevel level, const std::string& line);
void printBlock(LogLevel level, const std::vector<std::string>& lines);
void openLog();

/**
 * @brief      Describe a run time error that ended a call, give it to `OnRuntimeError` and print it.
 *
 * @param      failure  The error and where it was found.
 */
void reportFailure(Failure& failure);

/**
 * @brief      Find where a script was when a call ended with an error.
 *
 * @param      script    The script.
 * @param      code      The error.
 * @param      index     Index of the public that was called.
 * @param      fromJit   Whether PawnJIT ended the call and kept its registers.
 * @param      depth     Depth of the call in the call stack.
 */
Failure locateFailure(Script& script, int code, int index, bool fromJit, size_t depth);

/**
 * @brief      Print the lines of repeated errors that were held back.
 *
 * @param      all  Print every held back count, as when the server exits.
 */
void flushRepeatedErrors(bool all);

/**
 * @brief      The backtrace of the Pawn code from a place in a script down to the server.
 *
 * @param      script  Script the place is in.
 * @param      place   An address inside the instruction the script is at.
 * @param      frame   Frame register at the place.
 * @param      from    Number of calls at the bottom of the call stack that lead to the place.
 *
 * @return     One line per frame.
 */
std::vector<std::string> backtrace(Script& script, cell place, cell frame, size_t from);

/**
 * @brief      The backtrace of the Pawn code that runs now.
 *
 * @param      native  Start with the native that runs, if one does.
 */
std::vector<std::string> currentBacktrace(bool native);

/**
 * @brief      The source location of an address, as `file:line`.
 */
std::string location(Script& script, cell cip);

/**
 * @brief      The text of a source line, without its indentation.
 */
std::string sourceLine(Script& script, cell cip);

/**
 * @brief      A path of a source file made short: relative to the server when it is inside it.
 */
std::string shortPath(const std::string& path);

/**
 * @brief      Format a call with the names and values of its arguments.
 *
 * @param      script    Script of the function.
 * @param      function  Index of the function in the debug information, or `-1`.
 * @param      frm       Frame of the call.
 */
std::string formatCall(Script& script, int function, cell frm);

/**
 * @brief      Format the arguments of a call, as `(name=value, ...)`.
 */
std::string formatArguments(Script& script, int function, cell frm);

/**
 * @brief      The name of the function a frame belongs to, found by the call that entered it.
 *
 * @param      script       Script of the frame.
 * @param      place        An address inside the code the frame is at.
 * @param      frame        The frame.
 * @param      publicEntry  Address of the public the frame belongs to, when it is the frame of a public.
 *
 * @return     - the name
 *             - an empty string, when no function is known there
 */
std::string functionAtFrame(Script& script, cell place, cell frame, cell publicEntry);

/**
 * @brief      Format the arguments a public is called with, before its frame exists.
 */
std::string formatPublicCall(Script& script, int index);

/**
 * @brief      The native backtrace of the current thread, or of a captured context.
 */
std::vector<std::string> nativeBacktrace(const void* context);

/**
 * @brief      Install the handlers of crashes and of the interrupt signal.
 */
void installCrashHandlers();
void removeCrashHandlers();

/**
 * @brief      Start and stop the thread that reports a call that does not return.
 */
void startWatchdog();
void stopWatchdog();

/**
 * @brief      Print where the Pawn call that runs now is, from a thread other than the server thread.
 *
 * @param      interrupted  Whether the console interrupted the server, rather than the call taking too long.
 */
void reportRunningCall(bool interrupted);

/**
 * @brief      The long call checks at the start and end of a top level call and at each native.
 */
void beginTopCall(uint64_t now);
void endTopCall(Script& script, int index, uint32_t name, uint64_t end);
void passLongCallLimit(const Call& native);

/**
 * @brief      Print the lines of repeated long calls that were held back.
 */
void flushLongCalls(bool all);

/**
 * @brief      The limit of a top level call, in microseconds, as `long_call_time` or a script sets it.
 */
void setLongCallTime(int microseconds);

/**
 * @brief      Start counting the current top level call again from now.
 */
void restartLongCall();

/**
 * @brief      The backtrace of a function that failed as it was entered, before it had a frame, as when the stack
 *             runs out in a deep recursion.
 *
 * @param      script  Script of the function.
 * @param      entry   Address of the function.
 * @param      frame   Frame of the code that called it.
 * @param      stack   Stack pointer, at the address the call returns to.
 * @param      from    Number of calls at the bottom of the call stack that lead to it.
 */
std::vector<std::string> entryBacktrace(Script& script, cell entry, cell frame, cell stack, size_t from);

/**
 * @brief      The backtrace of a native call: the native, then the Pawn code that called it.
 *
 * @param      native  The call of the native.
 * @param      from    Number of calls at the bottom of the call stack that lead to it.
 */
std::vector<std::string> nativeCallBacktrace(const Call& native, size_t from);

void registerNatives(IPawnScript& script);

/**
 * @brief      Write the report of every profiled script.
 *
 * @param      reason  What asked for it: `command`, `unload` or `exit`.
 *
 * @return     - the path of the report
 *             - an empty string, when it could not be written
 */
std::string writeReport(const char* reason);
void printSummary(const std::string& path);

/**
 * @brief      Time in microseconds, written with a unit.
 */
std::string formatTime(double microseconds);

}
