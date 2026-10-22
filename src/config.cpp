#include "tracer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace tracer {

namespace {

struct Json {
	enum Type {
		Null,
		Bool,
		Number,
		String,
		Array,
		Object
	};

	Type type = Null;
	bool boolean = false;
	double number = 0.0;
	std::string text;
	std::vector<Json> items;
	std::vector<std::pair<std::string, Json>> members;

	const Json* member(const std::string& name) const {
		for (const auto& entry : members) {
			if (entry.first == name) {
				return &entry.second;
			}
		}
		return nullptr;
	}
};

class JsonParser {
public:
	explicit JsonParser(const std::string& text)
		: text_(text) {
	}

	bool parse(Json& value) {
		skip();
		if (!parseValue(value, 0)) {
			return false;
		}
		skip();
		return at_ == text_.size();
	}

	int line() const {
		return int(std::count(text_.begin(), text_.begin() + std::min(at_, text_.size()), '\n')) + 1;
	}

private:
	void skip() {
		while (at_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[at_]))) {
			++at_;
		}
	}

	bool literal(const char* word) {
		size_t length = std::char_traits<char>::length(word);
		if (text_.compare(at_, length, word) != 0) {
			return false;
		}
		at_ += length;
		return true;
	}

	void append(std::string& out, unsigned code) {
		if (code < 0x80) {
			out += char(code);
		} else if (code < 0x800) {
			out += char(0xC0 | (code >> 6));
			out += char(0x80 | (code & 0x3F));
		} else {
			out += char(0xE0 | (code >> 12));
			out += char(0x80 | ((code >> 6) & 0x3F));
			out += char(0x80 | (code & 0x3F));
		}
	}

	bool parseString(std::string& out) {
		if (at_ >= text_.size() || text_[at_] != '"') {
			return false;
		}
		++at_;
		while (at_ < text_.size()) {
			char character = text_[at_++];
			if (character == '"') {
				return true;
			}
			if (character != '\\') {
				out += character;
				continue;
			}
			if (at_ >= text_.size()) {
				return false;
			}
			char escape = text_[at_++];
			switch (escape) {
			case '"':
			case '\\':
			case '/':
				out += escape;
				break;
			case 'b':
				out += '\b';
				break;
			case 'f':
				out += '\f';
				break;
			case 'n':
				out += '\n';
				break;
			case 'r':
				out += '\r';
				break;
			case 't':
				out += '\t';
				break;
			case 'u': {
				if (at_ + 4 > text_.size() || !std::all_of(text_.begin() + at_, text_.begin() + at_ + 4, [](unsigned char digit) { return std::isxdigit(digit) != 0; })) {
					return false;
				}
				unsigned code = unsigned(std::stoul(text_.substr(at_, 4), nullptr, 16));
				at_ += 4;
				append(out, code);
				break;
			}
			default:
				return false;
			}
		}
		return false;
	}

	bool parseNumber(Json& value) {
		size_t start = at_;
		if (at_ < text_.size() && (text_[at_] == '-' || text_[at_] == '+')) {
			++at_;
		}
		while (at_ < text_.size() && (std::isdigit(static_cast<unsigned char>(text_[at_])) || text_[at_] == '.' || text_[at_] == 'e' || text_[at_] == 'E' || text_[at_] == '-' || text_[at_] == '+')) {
			++at_;
		}
		if (at_ == start) {
			return false;
		}
		value.type = Json::Number;
		value.number = std::strtod(text_.substr(start, at_ - start).c_str(), nullptr);
		return true;
	}

	bool parseValue(Json& value, int depth) {
		if (depth > 64 || at_ >= text_.size()) {
			return false;
		}
		char character = text_[at_];
		if (character == '{') {
			++at_;
			value.type = Json::Object;
			skip();
			if (at_ < text_.size() && text_[at_] == '}') {
				++at_;
				return true;
			}
			while (true) {
				skip();
				std::string name;
				if (!parseString(name)) {
					return false;
				}
				skip();
				if (at_ >= text_.size() || text_[at_] != ':') {
					return false;
				}
				++at_;
				skip();
				Json member;
				if (!parseValue(member, depth + 1)) {
					return false;
				}
				value.members.emplace_back(std::move(name), std::move(member));
				skip();
				if (at_ < text_.size() && text_[at_] == ',') {
					++at_;
					continue;
				}
				if (at_ < text_.size() && text_[at_] == '}') {
					++at_;
					return true;
				}
				return false;
			}
		}
		if (character == '[') {
			++at_;
			value.type = Json::Array;
			skip();
			if (at_ < text_.size() && text_[at_] == ']') {
				++at_;
				return true;
			}
			while (true) {
				skip();
				Json item;
				if (!parseValue(item, depth + 1)) {
					return false;
				}
				value.items.push_back(std::move(item));
				skip();
				if (at_ < text_.size() && text_[at_] == ',') {
					++at_;
					continue;
				}
				if (at_ < text_.size() && text_[at_] == ']') {
					++at_;
					return true;
				}
				return false;
			}
		}
		if (character == '"') {
			value.type = Json::String;
			return parseString(value.text);
		}
		if (literal("true")) {
			value.type = Json::Bool;
			value.boolean = true;
			return true;
		}
		if (literal("false")) {
			value.type = Json::Bool;
			return true;
		}
		if (literal("null")) {
			return true;
		}
		return parseNumber(value);
	}

	const std::string& text_;
	size_t at_ = 0;
};

class Source {
public:
	virtual ~Source() = default;
	virtual bool getBool(const char* key, bool& value) = 0;
	virtual bool getInt(const char* key, int& value) = 0;
	virtual bool getString(const char* key, std::string& value) = 0;
	virtual bool getStrings(const char* key, std::vector<std::string>& value) = 0;
};

class ServerSource final : public Source {
public:
	explicit ServerSource(IConfig& config)
		: config_(config) {
	}

	bool getBool(const char* key, bool& value) override {
		bool* found = config_.getBool(name(key));
		if (found == nullptr) {
			return false;
		}
		value = *found;
		return true;
	}

	bool getInt(const char* key, int& value) override {
		int* found = config_.getInt(name(key));
		if (found == nullptr) {
			return false;
		}
		value = *found;
		return true;
	}

	bool getString(const char* key, std::string& value) override {
		std::string full = name(key);
		if (config_.getType(full) != ConfigOptionType_String) {
			return false;
		}
		StringView found = config_.getString(full);
		value.assign(found.data(), found.size());
		return true;
	}

	bool getStrings(const char* key, std::vector<std::string>& value) override {
		std::string full = name(key);
		if (config_.getType(full) != ConfigOptionType_Strings) {
			return false;
		}
		std::vector<StringView> views(config_.getStringsCount(full));
		config_.getStrings(full, Span<StringView>(views.data(), views.size()));
		value.clear();
		for (const StringView& view : views) {
			value.emplace_back(view.data(), view.size());
		}
		return true;
	}

private:
	static std::string name(const char* key) {
		return std::string("tracer.") + key;
	}

	IConfig& config_;
};

class JsonSource final : public Source {
public:
	explicit JsonSource(const Json* object)
		: object_(object) {
	}

	bool getBool(const char* key, bool& value) override {
		const Json* found = find(key);
		if (found == nullptr || found->type != Json::Bool) {
			return false;
		}
		value = found->boolean;
		return true;
	}

	bool getInt(const char* key, int& value) override {
		const Json* found = find(key);
		if (found == nullptr || found->type != Json::Number) {
			return false;
		}
		value = int(std::lround(found->number));
		return true;
	}

	bool getString(const char* key, std::string& value) override {
		const Json* found = find(key);
		if (found == nullptr || found->type != Json::String) {
			return false;
		}
		value = found->text;
		return true;
	}

	bool getStrings(const char* key, std::vector<std::string>& value) override {
		const Json* found = find(key);
		if (found == nullptr || found->type != Json::Array) {
			return false;
		}
		value.clear();
		for (const Json& item : found->items) {
			if (item.type == Json::String) {
				value.push_back(item.text);
			}
		}
		return true;
	}

private:
	const Json* find(const char* key) const {
		return object_ != nullptr && object_->type == Json::Object ? object_->member(key) : nullptr;
	}

	const Json* object_;
};

Settings readSettings(Source& source) {
	Settings settings;
	source.getBool("enable", settings.enable);
	source.getStrings("scripts", settings.scripts);
	source.getBool("errors", settings.errors);
	source.getInt("error_repeat", settings.errorRepeat);
	source.getBool("source", settings.source);
	source.getInt("long_call_time", settings.longCallTime);
	source.getInt("hang_time", settings.hangTime);
	source.getBool("crash", settings.crash);
	source.getString("trace", settings.trace);
	source.getString("trace_filter", settings.traceFilter);
	source.getString("log", settings.log);
	source.getBool("profile", settings.profile);
	source.getBool("profile_natives", settings.profileNatives);
	source.getBool("profile_callers", settings.profileCallers);
	source.getBool("profile_functions", settings.profileFunctions);
	source.getString("output", settings.output);
	settings.errorRepeat = std::max(0, settings.errorRepeat);
	settings.longCallTime = std::max(0, settings.longCallTime);
	settings.hangTime = std::max(0, settings.hangTime);
	for (std::string& name : settings.scripts) {
		std::transform(name.begin(), name.end(), name.begin(), [](unsigned char character) { return char(std::tolower(character)); });
		std::replace(name.begin(), name.end(), '\\', '/');
	}
	if (settings.output.empty()) {
		settings.output = "tracer";
	}
	return settings;
}

std::string text(bool value) {
	return value ? "true" : "false";
}

std::string text(int value) {
	return std::to_string(value);
}

std::string text(const std::string& value) {
	return "\"" + value + "\"";
}

std::string text(const std::vector<std::string>& value) {
	std::string joined = "[";
	for (size_t index = 0; index != value.size(); ++index) {
		joined += (index == 0 ? "\"" : ", \"") + value[index] + "\"";
	}
	return joined + "]";
}

template <typename T>
void compare(std::vector<std::string>& changes, const char* key, const T& before, const T& after) {
	if (before != after) {
		changes.push_back(std::string(key) + " " + text(before) + " to " + text(after));
	}
}

std::vector<std::string> differences(const Settings& before, const Settings& after) {
	std::vector<std::string> changes;
	compare(changes, "enable", before.enable, after.enable);
	compare(changes, "scripts", before.scripts, after.scripts);
	compare(changes, "errors", before.errors, after.errors);
	compare(changes, "error_repeat", before.errorRepeat, after.errorRepeat);
	compare(changes, "source", before.source, after.source);
	compare(changes, "long_call_time", before.longCallTime, after.longCallTime);
	compare(changes, "hang_time", before.hangTime, after.hangTime);
	compare(changes, "crash", before.crash, after.crash);
	compare(changes, "trace", before.trace, after.trace);
	compare(changes, "trace_filter", before.traceFilter, after.traceFilter);
	compare(changes, "log", before.log, after.log);
	compare(changes, "profile", before.profile, after.profile);
	compare(changes, "profile_natives", before.profileNatives, after.profileNatives);
	compare(changes, "profile_callers", before.profileCallers, after.profileCallers);
	compare(changes, "profile_functions", before.profileFunctions, after.profileFunctions);
	compare(changes, "output", before.output, after.output);
	return changes;
}

bool isListed(const Settings& settings, const std::string& name) {
	if (settings.scripts.empty()) {
		return true;
	}
	std::string full = name;
	std::transform(full.begin(), full.end(), full.begin(), [](unsigned char character) { return char(std::tolower(character)); });
	std::replace(full.begin(), full.end(), '\\', '/');
	size_t slash = full.find_last_of('/');
	std::string base = slash == std::string::npos ? full : full.substr(slash + 1);
	for (const std::string& entry : settings.scripts) {
		if (entry == full || entry == base) {
			return true;
		}
	}
	return false;
}

int64_t fileStamp(const std::string& path) {
	std::error_code error;
	auto time = std::filesystem::last_write_time(path, error);
	return error ? 0 : int64_t(time.time_since_epoch().count());
}

int64_t failedStamp = 0;
bool applied = false;

}

Settings readServerSettings(IConfig& config) {
	ServerSource source(config);
	return readSettings(source);
}

bool isTraced(const std::string& name) {
	return isListed(state().settings, name);
}

void applySettings(const Settings& next, bool announce) {
	State& s = state();
	if (announce) {
		std::vector<std::string> changes = differences(s.settings, next);
		if (changes.empty()) {
			return;
		}
		std::string line = "[Tracer] " + s.configPath + " changed: ";
		for (size_t index = 0; index != changes.size(); ++index) {
			line += (index == 0 ? "" : ", ") + changes[index];
		}
		print(LogLevel::Message, line);
	}
	Settings previous = s.settings;
	s.settings = next;

	unsigned flags = 0;
	for (char character : next.trace) {
		switch (character) {
		case 'n':
			flags |= TraceNatives;
			break;
		case 'p':
			flags |= TracePublics;
			break;
		case 'f':
			flags |= TraceFunctions;
			break;
		}
	}
	s.traceFlags = next.enable ? flags : 0;
	s.traceFilter.reset();
	if (!next.traceFilter.empty()) {
		try {
			s.traceFilter = std::make_unique<std::regex>(next.traceFilter);
		} catch (const std::regex_error&) {
			print(LogLevel::Warning, "[Tracer] trace_filter is not a valid regular expression, every traced call is printed");
		}
	}
	if (!applied || previous.longCallTime != next.longCallTime) {
		setLongCallTime(next.longCallTime);
	}
	s.hangTicks = next.hangTime > 0 ? s.clock.ticksFor(double(next.hangTime) * 1000.0) : 0;
	openLog();

	bool profile = next.enable && next.profile;
	if (profile && !s.profiling) {
		resetStatistics();
		s.profiling = true;
	} else if (!profile && s.profiling) {
		s.profiling = false;
	}
	for (auto& entry : s.scripts) {
		entry.second->traced = isListed(next, entry.second->name);
	}
	updateDebugHookUse();
	if (next.enable && next.crash) {
		installCrashHandlers();
	} else {
		removeCrashHandlers();
	}
	applied = true;
}

bool reloadConfig(bool force) {
	State& s = state();
	int64_t stamp = fileStamp(s.configPath);
	if (stamp == 0 || (!force && (stamp == s.configStamp || stamp == failedStamp))) {
		return false;
	}
	std::ifstream file(s.configPath, std::ios::binary);
	std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	Json root;
	JsonParser parser(content);
	if (!file || !parser.parse(root) || root.type != Json::Object) {
		if (failedStamp != stamp) {
			failedStamp = stamp;
			print(LogLevel::Warning, "[Tracer] " + s.configPath + " changed but is not valid JSON near line " + std::to_string(parser.line()) + ", the settings stay as they were");
		}
		return false;
	}
	s.configStamp = stamp;
	failedStamp = 0;
	JsonSource source(root.member("tracer"));
	applySettings(readSettings(source), true);
	return true;
}

}
