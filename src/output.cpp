#include "tracer.hpp"

#include <ctime>
#include <mutex>

namespace tracer {

namespace {

std::mutex outputLock;
std::string openedLog;

}

void print(LogLevel level, const std::string& line) {
	State& s = state();
	std::lock_guard<std::mutex> guard(outputLock);
	if (s.core != nullptr) {
		s.core->logLn(level, "%s", line.c_str());
	}
	if (s.logFile != nullptr) {
		char stamp[32];
		std::time_t now = std::time(nullptr);
		std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
		std::fprintf(s.logFile, "[%s] %s\n", stamp, line.c_str());
		std::fflush(s.logFile);
	}
}

void printBlock(LogLevel level, const std::vector<std::string>& lines) {
	for (const std::string& line : lines) {
		print(level, line);
	}
}

void openLog() {
	State& s = state();
	std::lock_guard<std::mutex> guard(outputLock);
	if (s.settings.log == openedLog) {
		return;
	}
	if (s.logFile != nullptr) {
		std::fclose(s.logFile);
		s.logFile = nullptr;
	}
	openedLog = s.settings.log;
	if (!openedLog.empty()) {
		s.logFile = std::fopen(openedLog.c_str(), "a");
	}
}

std::string formatTime(double microseconds) {
	char buffer[32];
	if (microseconds < 1000.0) {
		std::snprintf(buffer, sizeof(buffer), microseconds < 10.0 ? "%.2f us" : "%.1f us", microseconds);
	} else if (microseconds < 1000000.0) {
		std::snprintf(buffer, sizeof(buffer), microseconds < 10000.0 ? "%.2f ms" : "%.1f ms", microseconds / 1000.0);
	} else {
		std::snprintf(buffer, sizeof(buffer), "%.2f s", microseconds / 1000000.0);
	}
	return buffer;
}

}
