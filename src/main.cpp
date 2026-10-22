#include "tracer.hpp"
#include "opcodes.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>

#if defined _WIN32
	#include <windows.h>
#else
	#include <pthread.h>
#endif

using namespace tracer;

namespace {

constexpr int AmxExportCallback = 4;
constexpr int AmxExportExec = 7;

std::vector<std::string> readStrings(IConfig& config, const char* key) {
	std::vector<StringView> views(config.getStringsCount(key));
	config.getStrings(key, Span<StringView>(views.data(), views.size()));
	std::vector<std::string> strings;
	for (const StringView& view : views) {
		strings.emplace_back(view.data(), view.size());
	}
	return strings;
}

std::string configuredName(IConfig& config, const char* key, size_t index) {
	std::vector<std::string> names = readStrings(config, key);
	if (index >= names.size()) {
		return std::string();
	}
	std::string name = names[index];
	size_t space = name.find(' ');
	if (space != std::string::npos) {
		name.resize(space);
	}
	return name;
}

void setDefault(IEarlyConfig& config, bool defaults, const char* key, bool value) {
	if (defaults || config.getType(key) == ConfigOptionType_None) {
		config.setBool(key, value);
	}
}

void setDefault(IEarlyConfig& config, bool defaults, const char* key, int value) {
	if (defaults || config.getType(key) == ConfigOptionType_None) {
		config.setInt(key, value);
	}
}

void setDefault(IEarlyConfig& config, bool defaults, const char* key, const char* value) {
	if (defaults || config.getType(key) == ConfigOptionType_None) {
		config.setString(key, value);
	}
}

std::string describeSettings() {
	State& s = state();
	const Settings& settings = s.settings;
	if (!settings.enable) {
		return "is off (tracer.enable in config.json)";
	}
	std::vector<std::string> parts;
	if (settings.errors) {
		parts.push_back("run time errors");
	}
	if (settings.longCallTime > 0) {
		parts.push_back("calls longer than " + formatTime(double(settings.longCallTime)));
	}
	if (settings.hangTime > 0) {
		parts.push_back("calls stuck for " + formatTime(double(settings.hangTime) * 1000.0));
	}
	if (settings.crash) {
		parts.push_back("crashes");
	}
	std::string text = "watches ";
	for (size_t index = 0; index != parts.size(); ++index) {
		text += index == 0 ? "" : (index + 1 == parts.size() ? " and " : ", ");
		text += parts[index];
	}
	if (parts.empty()) {
		text = "watches nothing";
	}
	text += s.profiling ? ", and profiles" : ". Profiling is off, tracer.profile in config.json turns it on without a restart";
	return text;
}

}

class TracerComponent final : public IComponent, public PawnEventHandler, public CoreEventHandler, public ConsoleEventHandler {
public:
	PROVIDE_UID(0x7d3a19c4b25e8f61);

	StringView componentName() const override {
		return "Tracer";
	}

	SemanticVersion componentVersion() const override {
		return SemanticVersion(TRACER_VERSION_MAJOR, TRACER_VERSION_MINOR, TRACER_VERSION_PATCH, 0);
	}

	void provideConfiguration(ILogger& logger, IEarlyConfig& config, bool defaults) override {
		Settings settings;
		setDefault(config, defaults, "tracer.enable", settings.enable);
		if (defaults || config.getType("tracer.scripts") == ConfigOptionType_None) {
			config.setStrings("tracer.scripts", Span<const StringView>());
		}
		setDefault(config, defaults, "tracer.errors", settings.errors);
		setDefault(config, defaults, "tracer.error_repeat", settings.errorRepeat);
		setDefault(config, defaults, "tracer.source", settings.source);
		setDefault(config, defaults, "tracer.long_call_time", settings.longCallTime);
		setDefault(config, defaults, "tracer.hang_time", settings.hangTime);
		setDefault(config, defaults, "tracer.crash", settings.crash);
		setDefault(config, defaults, "tracer.trace", "");
		setDefault(config, defaults, "tracer.trace_filter", "");
		setDefault(config, defaults, "tracer.log", "");
		setDefault(config, defaults, "tracer.profile", settings.profile);
		setDefault(config, defaults, "tracer.profile_natives", settings.profileNatives);
		setDefault(config, defaults, "tracer.profile_callers", settings.profileCallers);
		setDefault(config, defaults, "tracer.profile_functions", settings.profileFunctions);
		setDefault(config, defaults, "tracer.output", settings.output.c_str());
	}

	void onLoad(ICore* core) override {
		State& s = state();
		s.core = core;
		s.players = &core->getPlayers();
#if defined _WIN32
		s.mainThread = GetCurrentThreadId();
#else
		s.mainThread = (unsigned long)pthread_self();
#endif
		pinModule();
		s.clock.calibrate();
		for (int index = 0; index != BucketCount - 1; ++index) {
			s.bucketLimits[index] = s.clock.ticksFor(BucketLimits[index]);
		}
		measureOverhead();
		s.calls.reserve(CallDepth + 1);
		applySettings(readServerSettings(core->getConfig()), false);
		reloadConfig(true);
		core->getEventDispatcher().addEventHandler(this);
		startWatchdog();
		core->printLn("Tracer " TRACER_VERSION " %s.", describeSettings().c_str());
	}

	void onInit(IComponentList* components) override {
		State& s = state();
		s.pawn = components->queryComponent<IPawnComponent>();
		if (s.pawn != nullptr) {
			s.pawn->getEventDispatcher().addEventHandler(this);
			s.exportedCallback = reinterpret_cast<AMX_CALLBACK>(s.pawn->getAmxFunctions()[AmxExportCallback]);
		}
		s.console = components->queryComponent<IConsoleComponent>();
		if (s.console != nullptr) {
			s.console->getEventDispatcher().addEventHandler(this);
		}
		IComponent* jit = components->queryComponent(PawnJITComponentUID);
		if (jit != nullptr) {
			IExtension* extension = jit->getExtension(IPawnJITDiagnostics::ExtensionIID);
			s.jit = extension != nullptr ? static_cast<IPawnJITDiagnostics*>(extension) : nullptr;
			if (s.jit != nullptr) {
				s.core->printLn("[Tracer] PawnJIT gives the exact place and registers of errors in its generated code.");
			} else {
				s.core->printLn("[Tracer] This build of PawnJIT does not report its errors, so errors in its generated code are found by statement.");
			}
		}
		updateDebugHookUse();
	}

	void onAmxLoad(IPawnScript& api) override {
		State& s = state();
		if (s.pawn == nullptr) {
			return;
		}
		registerNatives(api);
		if (!s.hooked) {
			if (!installExecHook(s.pawn->getAmxFunctions()[AmxExportExec])) {
				s.core->logLn(LogLevel::Error, "[Tracer] Could not hook amx_Exec, Tracer is off.");
				return;
			}
			startSession();
			warnAboutCrashdetect();
		}
		AMX* amx = api.GetAMX();
		Script* script = findOrAddScript(amx);
		script->api = &api;
		script->loaded = true;
		script->name = nameOf(api);
		script->path = filePath(api, script->name);
		script->captureNames();
		script->traced = isTraced(script->name);
		auto* header = reinterpret_cast<AMX_HEADER*>(amx->base);
		ucell entry = script->publicCount != 0 ? (GETENTRY(header, publics, 0))->address : ucell(header->cip);
		script->numericOpcodes = entry < ucell(header->dat - header->cod) && *reinterpret_cast<const cell*>(amx->base + header->cod + entry) == OpProc;
		if ((header->flags & AMX_FLAG_DEBUG) && !script->debug) {
			auto debug = std::make_unique<DebugInfo>();
			if (debug->load(script->path)) {
				script->debug = std::move(debug);
			} else {
				s.core->logLn(LogLevel::Warning, "[Tracer] %s has debug information, but %s could not be read, so its errors show no files or lines.", script->name.c_str(), script->path.c_str());
			}
		}
		hookCallback(*script);
		hookDebug(*script);
		updateDebugHookUse();
		if (script->traced) {
			s.core->printLn("[Tracer] Watching %s%s.", script->name.c_str(),
				script->debug ? ", with its debug information" : ", which has no debug information, -d3 gives files, lines and names");
		}
	}

	void onAmxUnload(IPawnScript& api) override {
		State& s = state();
		Script* script = findScript(api.GetAMX());
		if (script == nullptr) {
			return;
		}
		unhookCallback(*script);
		unhookDebug(*script);
		bool main = s.pawn != nullptr && &api == s.pawn->mainScript();
		if (main) {
			flushRepeatedErrors(true);
			flushLongCalls(true);
		}
		if (main && script->traced && s.profiling && s.events != s.reportedEvents) {
			report("unload");
		}
		script->loaded = false;
		forgetScript(*script);
		delete script;
	}

	void onTick(Microseconds elapsed, TimePoint now) override {
		State& s = state();
		tracer::onTick(now);
		if (now - lastSecond_ >= std::chrono::seconds(1)) {
			lastSecond_ = now;
			reloadConfig(false);
			flushRepeatedErrors(false);
			flushLongCalls(false);
		}
	}

	bool onConsoleText(StringView command, StringView parameters, const ConsoleCommandSenderData& sender) override {
		if (command != StringView("tracer")) {
			return false;
		}
		State& s = state();
		std::string argument(parameters.data(), parameters.size());
		if (argument.empty() || argument == "status") {
			reply(sender, "[Tracer] Tracer " TRACER_VERSION " " + describeSettings() + ".");
			reply(sender, "[Tracer] " + std::to_string(s.errorTotal) + " run time errors in " + std::to_string(s.errors.size()) + " places, " + std::to_string(s.longCalls.size())
				+ " long calls kept. Commands: tracer report, tracer reset, tracer reload, tracer errors.");
		} else if (argument == "report") {
			std::string path = report("command");
			reply(sender, path.empty() ? "[Tracer] The report could not be written." : "[Tracer] Report written to " + path);
		} else if (argument == "reset") {
			resetEverything();
			reply(sender, "[Tracer] Statistics, errors and long calls cleared.");
		} else if (argument == "reload") {
			if (!reloadConfig(true)) {
				reply(sender, "[Tracer] " + s.configPath + " could not be read.");
			} else {
				reply(sender, "[Tracer] " + s.configPath + " read again. Tracer " + describeSettings() + ".");
			}
		} else if (argument == "errors") {
			if (s.errors.empty()) {
				reply(sender, "[Tracer] No run time error so far.");
			}
			for (const ErrorRecord& error : s.errors) {
				reply(sender, "[Tracer] " + std::to_string(error.count) + " x run time error " + std::to_string(error.code) + " (" + errorText(error.code) + ") in " + error.script
					+ (error.where.empty() ? ", public " + error.publicName : " at " + error.where));
			}
		} else {
			reply(sender, "[Tracer] Usage: tracer [status | report | reset | reload | errors]");
		}
		return true;
	}

	void onConsoleCommandListRequest(FlatHashSet<StringView>& commands) override {
		commands.emplace("tracer");
	}

	void onFree(IComponent* component) override {
		State& s = state();
		if (component == s.console && s.console != nullptr) {
			s.console->getEventDispatcher().removeEventHandler(this);
			s.console = nullptr;
			return;
		}
		if (s.jit != nullptr && component != nullptr && component->getUID() == PawnJITComponentUID) {
			s.jit = nullptr;
			return;
		}
		if (component != s.pawn || s.pawn == nullptr) {
			return;
		}
		bool loaded = false;
		for (auto& entry : s.scripts) {
			loaded = loaded || (entry.second->loaded && entry.second->traced);
		}
		if (loaded && s.profiling && s.events != s.reportedEvents) {
			report("exit");
		}
		flushRepeatedErrors(true);
		flushLongCalls(true);
		s.settings.enable = false;
		s.pawn->getEventDispatcher().removeEventHandler(this);
		s.pawn = nullptr;
	}

	void reset() override {
	}

	void free() override {
		State& s = state();
		flushRepeatedErrors(true);
		flushLongCalls(true);
		stopWatchdog();
		removeCrashHandlers();
		s.settings.enable = false;
		if (s.console != nullptr) {
			s.console->getEventDispatcher().removeEventHandler(this);
			s.console = nullptr;
		}
		if (s.core != nullptr) {
			s.core->getEventDispatcher().removeEventHandler(this);
		}
		delete this;
	}

private:
	TimePoint lastSecond_ {};

	std::string nameOf(IPawnScript& api) {
		State& s = state();
		IConfig& config = s.core->getConfig();
		std::string name;
		if (&api == s.pawn->mainScript()) {
			name = configuredName(config, "pawn.main_scripts", 0);
		} else {
			Span<IPawnScript*> sides = s.pawn->sideScripts();
			for (size_t index = 0; index != sides.size(); ++index) {
				if (sides[index] == &api) {
					name = configuredName(config, "pawn.side_scripts", index);
				}
			}
		}
		if (name.empty()) {
			name = "script " + std::to_string(api.GetID());
		}
		return name;
	}

	std::string filePath(IPawnScript& api, const std::string& name) {
		State& s = state();
		std::string path = &api == s.pawn->mainScript() ? "gamemodes/" + name : name;
		if (path.size() < 4 || path.compare(path.size() - 4, 4, ".amx") != 0) {
			path += ".amx";
		}
		return path;
	}

	void warnAboutCrashdetect() {
#if defined _WIN32
		if (GetModuleHandleA("crashdetect.dll") != nullptr) {
			state().core->logLn(LogLevel::Warning,
				"[Tracer] crashdetect is loaded too. It runs every script in its own interpreter, so PawnJIT stays idle and errors are reported twice. Remove it from pawn.legacy_plugins.");
		}
#endif
	}

	std::string report(const char* reason) {
		std::string path = writeReport(reason);
		if (!path.empty()) {
			printSummary(path);
		}
		return path;
	}

	void reply(const ConsoleCommandSenderData& sender, const std::string& text) {
		State& s = state();
		if (s.console != nullptr) {
			s.console->sendMessage(sender, StringView(text.data(), text.size()));
		} else {
			s.core->printLn("%s", text.c_str());
		}
	}
};

COMPONENT_ENTRY_POINT() {
	return new TracerComponent();
}
