#include "tracer.hpp"

#include "report_template.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace tracer {

namespace {

void appendText(std::string& out, const std::string& text) {
	out += '"';
	for (unsigned char character : text) {
		switch (character) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '<':
			out += "\\u003c";
			break;
		case '>':
			out += "\\u003e";
			break;
		case '&':
			out += "\\u0026";
			break;
		default:
			if (character < 0x20) {
				char escaped[8];
				std::snprintf(escaped, sizeof escaped, "\\u%04x", character);
				out += escaped;
			} else {
				out += char(character);
			}
		}
	}
	out += '"';
}

void appendNumber(std::string& out, double value) {
	char buffer[48];
	std::snprintf(buffer, sizeof buffer, "%.1f", value);
	out += buffer;
}

void appendInteger(std::string& out, uint64_t value) {
	out += std::to_string(value);
}

void appendLines(std::string& out, const std::vector<std::string>& lines) {
	out += '[';
	for (size_t index = 0; index != lines.size(); ++index) {
		if (index != 0) {
			out += ',';
		}
		std::string line = lines[index];
		if (line.compare(0, 9, "[Tracer] ") == 0) {
			line.erase(0, 9);
		}
		appendText(out, line);
	}
	out += ']';
}

double secondsSinceStart(uint64_t at) {
	State& s = state();
	uint64_t zero = s.sessionStarted ? s.clockZero : s.sessionStart;
	if (at < zero) {
		return -s.clock.microseconds(zero - at) / 1000000.0;
	}
	return s.clock.microseconds(at - zero) / 1000000.0;
}

void appendStat(std::string& out, const Stat& stat) {
	State& s = state();
	appendInteger(out, stat.calls);
	out += ',';
	appendNumber(out, s.clock.microseconds(stat.total));
	out += ',';
	appendNumber(out, s.clock.microseconds(stat.self));
	out += ',';
	appendNumber(out, s.clock.microseconds(stat.worst));
	out += ',';
	appendNumber(out, secondsSinceStart(stat.worstAt));
	for (uint64_t bucket : stat.buckets) {
		out += ',';
		appendInteger(out, bucket);
	}
}

void appendScript(std::string& out, Script& script) {
	script.captureNames();
	State& s = state();
	out += "{\"name\":";
	appendText(out, script.name);
	out += ",\"main\":";
	out += s.pawn != nullptr && script.api == s.pawn->mainScript() ? "true" : "false";

	out += ",\"publics\":[";
	bool first = true;
	for (int slot = 0; slot != int(script.publics.size()); ++slot) {
		const Stat& stat = script.publics[slot];
		if (stat.calls == 0) {
			continue;
		}
		out += first ? "[" : ",[";
		first = false;
		appendText(out, script.slotName(slot));
		out += ',';
		appendStat(out, stat);
		out += ']';
	}

	out += "],\"natives\":[";
	first = true;
	for (int index = 0; index != int(script.natives.size()); ++index) {
		const Stat& stat = script.natives[index];
		if (stat.calls == 0) {
			continue;
		}
		out += first ? "[" : ",[";
		first = false;
		appendText(out, script.nativeName(index));
		out += ',';
		appendStat(out, stat);
		out += ']';
	}

	out += "],\"functions\":[";
	first = true;
	if (script.debug) {
		const auto& functions = script.debug->functions();
		for (size_t index = 0; index != script.functions.size(); ++index) {
			const Stat& stat = script.functions[index];
			if (stat.calls == 0) {
				continue;
			}
			out += first ? "[" : ",[";
			first = false;
			appendText(out, functions[index].name);
			out += ',';
			appendStat(out, stat);
			out += ']';
		}
	}

	out += "],\"debug\":";
	out += script.debug ? "true" : "false";
	out += ",\"pairs\":[";
	first = true;
	for (const auto& entry : script.pairs) {
		out += first ? "[" : ",[";
		first = false;
		appendText(out, script.slotName(int(entry.first >> 16)));
		out += ',';
		appendText(out, script.nativeName(int(entry.first & 0xFFFF)));
		out += ',';
		appendStat(out, entry.second);
		out += ']';
	}

	out += "],\"memory\":[";
	if (script.memorySeen && script.loaded) {
		AMX* amx = script.amx;
		appendInteger(out, uint64_t(amx->stp - script.lowestStack));
		out += ',';
		appendInteger(out, uint64_t(script.highestHeap - amx->hlw));
		out += ',';
		appendInteger(out, uint64_t(amx->stp - amx->hlw));
	}
	out += "]}";
}

std::string localTime(int64_t seconds, const char* format) {
	std::time_t value = std::time_t(seconds);
	std::tm parts;
#if defined _WIN32
	localtime_s(&parts, &value);
#else
	localtime_r(&value, &parts);
#endif
	char buffer[64];
	std::strftime(buffer, sizeof buffer, format, &parts);
	return buffer;
}

std::string buildData(const char* reason) {
	State& s = state();
	s.clock.refine();
	uint64_t now = Clock::now();
	double duration = s.sessionStarted ? s.clock.microseconds(now - s.clockZero) / 1000000.0 : 0.0;

	std::string out;
	out.reserve(1 << 16);
	out += "{\"version\":\"" TRACER_VERSION "\",\"reason\":";
	appendText(out, reason);
	out += ",\"started\":";
	appendText(out, localTime(s.sessionStartTime, "%Y-%m-%d %H:%M:%S"));
	out += ",\"generated\":";
	appendText(out, localTime(int64_t(std::time(nullptr)), "%Y-%m-%d %H:%M:%S"));
	out += ",\"duration\":";
	appendNumber(out, duration);
	out += ",\"pawn\":";
	appendNumber(out, s.clock.microseconds(s.pawnTotal));
	out += ",\"startup\":";
	appendNumber(out, s.clock.microseconds(s.sessionStarted ? s.startupPawn : s.pawnTotal));
	out += ",\"startupWall\":";
	appendNumber(out, s.clock.microseconds(s.sessionStarted ? s.startupWall : now - s.sessionStart));
	out += ",\"ticks\":";
	appendInteger(out, s.ticks);
	out += ",\"worstTick\":";
	appendNumber(out, s.clock.microseconds(s.worstTick));
	out += ",\"worstTickAt\":";
	appendNumber(out, secondsSinceStart(s.worstTickAt));
	out += ",\"events\":";
	appendInteger(out, s.events);
	out += ",\"overhead\":";
	appendNumber(out, s.clock.microseconds(s.overheadPerEvent * s.events));
	out += ",\"settings\":{\"profile\":";
	out += s.settings.profile ? "true" : "false";
	out += ",\"natives\":";
	out += s.settings.profileNatives ? "true" : "false";
	out += ",\"callers\":";
	out += s.settings.profileCallers ? "true" : "false";
	out += ",\"functions\":";
	out += s.settings.profileFunctions ? "true" : "false";
	out += ",\"longCallTime\":";
	appendInteger(out, uint64_t(s.settings.longCallTime));
	out += "},\"buckets\":[10,100,1000,10000,100000]";

	std::vector<Second> seconds = s.seconds;
	if (s.second.ticks != 0) {
		Second current = s.second;
		if (s.players != nullptr) {
			current.players = uint32_t(s.players->entries().size());
		}
		seconds.push_back(current);
	}
	out += ",\"seconds\":[";
	for (size_t index = 0; index != seconds.size(); ++index) {
		const Second& second = seconds[index];
		out += index == 0 ? "[" : ",[";
		appendNumber(out, s.clock.microseconds(second.pawn));
		out += ',';
		appendNumber(out, s.clock.microseconds(second.worstTick));
		out += ',';
		appendInteger(out, second.ticks);
		out += ',';
		appendInteger(out, second.callbacks);
		out += ',';
		appendInteger(out, second.natives);
		out += ',';
		appendInteger(out, second.errors);
		out += ',';
		appendInteger(out, second.players);
		out += ']';
	}

	out += "],\"slowTicks\":[";
	for (size_t index = 0; index != s.slowTicks.size(); ++index) {
		const SlowTick& tick = s.slowTicks[index];
		out += index == 0 ? "[" : ",[";
		appendNumber(out, secondsSinceStart(tick.at));
		out += ',';
		appendNumber(out, s.clock.microseconds(tick.pawn));
		out += ',';
		appendInteger(out, tick.callbacks);
		out += ",[";
		for (size_t group = 0; group != tick.groups.size(); ++group) {
			const TickGroup& entry = tick.groups[group];
			out += group == 0 ? "[" : ",[";
			appendText(out, entry.script);
			out += ',';
			appendText(out, entry.name);
			out += ',';
			appendInteger(out, entry.calls);
			out += ',';
			appendNumber(out, s.clock.microseconds(entry.total));
			out += ']';
		}
		out += "]]";
	}

	out += "],\"scripts\":[";
	bool first = true;
	for (auto& entry : s.scripts) {
		if (!entry.second->traced) {
			continue;
		}
		if (!first) {
			out += ',';
		}
		first = false;
		appendScript(out, *entry.second);
	}
	out += "],\"errors\":[";
	for (size_t index = 0; index != s.errors.size(); ++index) {
		const ErrorRecord& error = s.errors[index];
		out += index == 0 ? "[" : ",[";
		appendText(out, error.script);
		out += ',';
		appendText(out, error.publicName);
		out += ',';
		appendInteger(out, uint64_t(error.code));
		out += ',';
		appendText(out, errorText(error.code));
		out += ',';
		appendInteger(out, error.count);
		out += ',';
		appendNumber(out, secondsSinceStart(error.firstAt));
		out += ',';
		appendNumber(out, secondsSinceStart(error.lastAt));
		out += ',';
		appendText(out, error.where);
		out += ',';
		appendLines(out, error.report);
		out += ']';
	}
	out += "],\"longCalls\":[";
	for (size_t index = 0; index != s.longCalls.size(); ++index) {
		const LongCall& call = s.longCalls[index];
		out += index == 0 ? "[" : ",[";
		appendText(out, call.script);
		out += ',';
		appendText(out, call.publicName);
		out += ',';
		appendNumber(out, s.clock.microseconds(call.duration));
		out += ',';
		appendNumber(out, secondsSinceStart(call.at));
		out += ',';
		appendLines(out, call.report);
		out += ']';
	}
	out += "]}";
	return out;
}

std::string formatDuration(double microseconds) {
	char buffer[32];
	if (microseconds < 1000.0) {
		std::snprintf(buffer, sizeof buffer, "%.1f us", microseconds);
	} else if (microseconds < 1000000.0) {
		std::snprintf(buffer, sizeof buffer, "%.1f ms", microseconds / 1000.0);
	} else {
		std::snprintf(buffer, sizeof buffer, "%.2f s", microseconds / 1000000.0);
	}
	return buffer;
}

bool writeFile(const std::filesystem::path& path, const std::string& text) {
	std::ofstream output(path, std::ios::binary);
	output << text;
	output.close();
	return !output.fail();
}

}

std::string writeReport(const char* reason) {
	State& s = state();
	try {
		std::string data = buildData(reason);
		std::string page(reinterpret_cast<const char*>(reportTemplate), reportTemplateSize);
		const std::string marker = "\"__TRACER_DATA__\"";
		size_t at = page.find(marker);
		if (at == std::string::npos) {
			s.core->logLn(LogLevel::Error, "[Tracer] The report template has no data marker.");
			return std::string();
		}
		page.replace(at, marker.size(), data);

		std::string name = "server";
		bool main = false;
		for (auto& entry : s.scripts) {
			const Script& script = *entry.second;
			if (main || !script.traced || script.name.empty()) {
				continue;
			}
			name = script.name;
			main = s.pawn != nullptr && script.api == s.pawn->mainScript();
		}
		for (char& character : name) {
			if (character == '/' || character == '\\' || character == ':' || character == ' ') {
				character = '_';
			}
		}

		std::filesystem::path folder(std::string(s.settings.output.data(), s.settings.output.size()));
		std::filesystem::create_directories(folder);
		std::string stem = name + "-" + localTime(int64_t(std::time(nullptr)), "%Y-%m-%d_%H-%M-%S");
		std::filesystem::path file = folder / (stem + ".html");
		for (int copy = 2; std::filesystem::exists(file); ++copy) {
			file = folder / (stem + "-" + std::to_string(copy) + ".html");
		}
		if (!writeFile(file, page)) {
			s.core->logLn(LogLevel::Error, "[Tracer] Could not write %s.", file.string().c_str());
			return std::string();
		}
		writeFile(folder / "latest.html", page);
		s.reportedEvents = s.events;
		return std::string(file.generic_string());
	} catch (const std::exception& error) {
		s.core->logLn(LogLevel::Error, "[Tracer] Could not write the report: %s", error.what());
		return std::string();
	}
}

void printSummary(const std::string& path) {
	State& s = state();
	double duration = s.sessionStarted ? s.clock.microseconds(Clock::now() - s.clockZero) : 0.0;
	double startup = s.clock.microseconds(s.sessionStarted ? s.startupPawn : s.pawnTotal);
	double pawn = s.clock.microseconds(s.pawnTotal) - startup;
	s.core->printLn("[Tracer] Report: %s", path.c_str());
	if (startup > 0.0) {
		s.core->printLn("[Tracer] Start up: %s of Pawn before the first server tick.", formatDuration(startup).c_str());
	}
	s.core->printLn("[Tracer] %s profiled after it, Pawn ran %.2f%% of the time (%s per second on average), worst tick %s.",
		formatDuration(duration).c_str(),
		duration > 0.0 ? pawn * 100.0 / duration : 0.0,
		formatDuration(duration > 0.0 ? pawn * 1000000.0 / duration : 0.0).c_str(),
		formatDuration(s.clock.microseconds(s.worstTick)).c_str());

	struct Row {
		const Script* script;
		int slot;
		const Stat* stat;
	};
	std::vector<Row> rows;
	uint64_t errors = s.errorTotal;
	for (auto& entry : s.scripts) {
		Script& script = *entry.second;
		if (!script.traced) {
			continue;
		}
		script.captureNames();
		for (int slot = 0; slot != int(script.publics.size()); ++slot) {
			if (script.publics[slot].calls != 0) {
				rows.push_back({ &script, slot, &script.publics[slot] });
			}
		}
	}
	std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.stat->self > b.stat->self; });
	if (rows.size() > 5) {
		rows.resize(5);
	}
	for (const Row& row : rows) {
		s.core->printLn("[Tracer]   %-32s %10llu calls  %10s own time  %10s average  %10s worst",
			row.script->slotName(row.slot).c_str(),
			static_cast<unsigned long long>(row.stat->calls),
			formatDuration(s.clock.microseconds(row.stat->self)).c_str(),
			formatDuration(s.clock.microseconds(row.stat->total) / double(row.stat->calls)).c_str(),
			formatDuration(s.clock.microseconds(row.stat->worst)).c_str());
	}
	if (errors != 0) {
		s.core->printLn("[Tracer] %llu run time errors, listed in the report.", static_cast<unsigned long long>(errors));
	}
}

}
