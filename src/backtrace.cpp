#include "tracer.hpp"
#include "opcodes.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>

#if defined _WIN32
	#include <windows.h>
#endif

namespace tracer {

namespace {

constexpr size_t MaximumFrames = 64;
constexpr size_t MaximumWalk = 100000;
constexpr int MaximumArguments = 12;
constexpr size_t MaximumString = 80;
constexpr int MaximumCells = 8;

struct SourceFile {
	bool found = false;
	std::vector<std::string> lines;
};

std::unordered_map<std::string, SourceFile> sources;
std::mutex cacheLock;

std::string serverRoot() {
	static std::string root;
	static bool known = false;
	if (!known) {
		known = true;
#if defined _WIN32
		char buffer[MAX_PATH];
		DWORD length = GetCurrentDirectoryA(MAX_PATH, buffer);
		if (length != 0 && length < MAX_PATH) {
			root.assign(buffer, length);
		}
#endif
		std::replace(root.begin(), root.end(), '\\', '/');
		std::transform(root.begin(), root.end(), root.begin(), [](unsigned char character) { return char(std::tolower(character)); });
		if (!root.empty() && root.back() != '/') {
			root += '/';
		}
	}
	return root;
}

unsigned char* dataOf(AMX* amx) {
	auto* header = reinterpret_cast<AMX_HEADER*>(amx->base);
	return amx->data != nullptr ? amx->data : amx->base + header->dat;
}

cell codeSize(AMX* amx) {
	auto* header = reinterpret_cast<AMX_HEADER*>(amx->base);
	return header->dat - header->cod;
}

const cell* cellsAt(AMX* amx, cell address, cell count) {
	if (address < 0 || (address & (sizeof(cell) - 1)) != 0 || count < 0 || address > amx->stp - count * cell(sizeof(cell))) {
		return nullptr;
	}
	return reinterpret_cast<const cell*>(dataOf(amx) + address);
}

std::string formatFloat(cell value) {
	float number;
	std::memcpy(&number, &value, sizeof(number));
	if (std::isnan(number)) {
		return "nan";
	}
	if (std::isinf(number)) {
		return number > 0 ? "inf" : "-inf";
	}
	char buffer[64];
	double magnitude = std::fabs(double(number));
	if (magnitude != 0.0 && (magnitude >= 1e9 || magnitude < 1e-4)) {
		std::snprintf(buffer, sizeof(buffer), "%g", double(number));
		return buffer;
	}
	std::snprintf(buffer, sizeof(buffer), "%.4f", double(number));
	std::string text = buffer;
	while (text.size() > 2 && text.back() == '0' && text[text.size() - 2] != '.') {
		text.pop_back();
	}
	return text;
}

std::string formatValue(const std::string& tag, cell value) {
	if (tag == "Float") {
		return formatFloat(value);
	}
	if (tag == "bool") {
		return value == 0 ? "false" : (value == 1 ? "true" : std::to_string(value));
	}
	return std::to_string(value);
}

bool printable(cell character) {
	return (character >= 32 && character < 127) || character == '\t' || (character >= 160 && character < 256);
}

bool readString(AMX* amx, cell address, cell size, std::string& text) {
	const cell* first = cellsAt(amx, address, 1);
	if (first == nullptr) {
		return false;
	}
	cell available = (amx->stp - address) / cell(sizeof(cell));
	cell cells = size > 0 ? std::min(size, available) : std::min(cell(256), available);
	const cell* data = first;
	text.clear();
	if (ucell(data[0]) > UNPACKEDMAX) {
		for (cell index = 0; index != cells; ++index) {
			ucell value = ucell(data[index]);
			for (int shift = 24; shift >= 0; shift -= 8) {
				cell character = cell((value >> shift) & 0xFF);
				if (character == 0) {
					return true;
				}
				if (!printable(character)) {
					return false;
				}
				text += char(character);
			}
		}
		return size > 0;
	}
	for (cell index = 0; index != cells; ++index) {
		cell character = data[index];
		if (character == 0) {
			return true;
		}
		if (!printable(character)) {
			return false;
		}
		text += char(character);
	}
	return size > 0;
}

std::string quote(const std::string& text) {
	std::string quoted = "\"";
	size_t length = std::min(text.size(), MaximumString);
	for (size_t index = 0; index != length; ++index) {
		char character = text[index];
		if (character == '"' || character == '\\') {
			quoted += '\\';
		}
		quoted += character == '\t' ? ' ' : character;
	}
	if (text.size() > MaximumString) {
		quoted += "...";
	}
	quoted += '"';
	return quoted;
}

std::string formatCells(AMX* amx, cell address, cell size, const std::string& tag) {
	cell count = size > 0 ? std::min(size, cell(MaximumCells)) : 4;
	const cell* data = cellsAt(amx, address, count);
	if (data == nullptr) {
		return "?";
	}
	std::string text = "{";
	for (cell index = 0; index != count; ++index) {
		if (index != 0) {
			text += ", ";
		}
		text += formatValue(tag, data[index]);
	}
	if (size <= 0 || size > count) {
		text += ", ...";
	}
	return text + "}";
}

std::string formatArgument(Script& script, const DebugInfo::Symbol& symbol, cell value) {
	AMX* amx = script.amx;
	const DebugInfo& debug = *script.debug;
	const std::string& tag = debug.tagName(symbol.tag);
	std::string text;
	if (symbol.ident == DebugInfo::IdentReference) {
		text += '&';
	}
	if (!tag.empty()) {
		text += tag + ":";
	}
	text += symbol.name;
	if (symbol.ident == DebugInfo::IdentArray || symbol.ident == DebugInfo::IdentReferenceArray) {
		for (const DebugInfo::Dimension& dimension : symbol.dimensions) {
			if (dimension.size == 0) {
				text += "[]";
			} else {
				const std::string& dimensionTag = debug.tagName(dimension.tag);
				text += "[" + (dimensionTag.empty() ? std::string() : dimensionTag + ":") + std::to_string(dimension.size) + "]";
			}
		}
	}
	text += '=';
	switch (symbol.ident) {
	case DebugInfo::IdentVariable:
		text += formatValue(tag, value);
		break;
	case DebugInfo::IdentReference: {
		const cell* target = cellsAt(amx, value, 1);
		text += target != nullptr ? formatValue(tag, *target) : "?";
		break;
	}
	case DebugInfo::IdentArray:
	case DebugInfo::IdentReferenceArray: {
		if (symbol.dimensions.size() != 1) {
			text += "{...}";
			break;
		}
		cell size = cell(symbol.dimensions[0].size);
		std::string string;
		if (tag.empty() && debug.tagName(symbol.dimensions[0].tag).empty() && readString(amx, value, size, string)) {
			text += quote(string);
		} else {
			text += formatCells(amx, value, size, tag);
		}
		break;
	}
	default:
		text += std::to_string(value);
		break;
	}
	return text;
}

std::string formatVariable(AMX* amx, cell address) {
	std::string string;
	if (readString(amx, address, 0, string) && string.size() >= 2) {
		return quote(string);
	}
	const cell* target = cellsAt(amx, address, 1);
	return target != nullptr ? std::to_string(*target) : "?";
}

std::string publicAt(Script& script, cell entry) {
	if (!script.loaded || entry <= 0) {
		return std::string();
	}
	auto* header = reinterpret_cast<AMX_HEADER*>(script.amx->base);
	for (int index = 0; index != script.publicCount; ++index) {
		auto* stub = GETENTRY(header, publics, index);
		if (cell(reinterpret_cast<AMX_FUNCSTUB*>(stub)->address) == entry) {
			return GETENTRYNAME(header, stub);
		}
	}
	return std::string();
}

cell calleeOf(AMX* amx, cell returnAddress) {
	cell size = codeSize(amx);
	if (returnAddress < cell(2 * sizeof(cell)) || returnAddress > size) {
		return -1;
	}
	const unsigned char* code = amx->base + reinterpret_cast<AMX_HEADER*>(amx->base)->cod;
	if (*reinterpret_cast<const cell*>(code + returnAddress - 2 * sizeof(cell)) != OpCall) {
		return -1;
	}
	cell target = *reinterpret_cast<const cell*>(code + returnAddress - sizeof(cell));
	if (ucell(target) >= ucell(size)) {
		target = cell(ucell(target) - ucell(uintptr_t(code)));
	}
	return target >= 0 && target < size ? target : -1;
}

struct FrameName {
	int function;
	std::string name;
	bool isPublic;
};

FrameName nameFrame(Script& script, cell place, cell entry) {
	FrameName result = { -1, std::string(), false };
	if (entry > 0) {
		if (script.debug) {
			result.function = script.debug->functionStartingAt(ucell(entry));
		}
		std::string table = publicAt(script, entry);
		result.isPublic = !table.empty();
		result.name = result.function != -1 ? script.debug->functions()[result.function].name : table;
	} else if (script.debug) {
		result.function = script.debug->functionAt(ucell(place));
		if (result.function != -1) {
			result.name = script.debug->functions()[result.function].name;
			result.isPublic = !publicAt(script, cell(script.debug->functions()[result.function].start)).empty();
		}
	}
	if (result.name.empty()) {
		result.name = "??";
	}
	return result;
}

std::string moduleOf(const void* address) {
#if defined _WIN32
	static std::unordered_map<HMODULE, std::string> names;
	std::lock_guard<std::mutex> guard(cacheLock);
	HMODULE module = nullptr;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(address), &module) || module == nullptr) {
		return std::string();
	}
	auto found = names.find(module);
	if (found != names.end()) {
		return found->second;
	}
	char buffer[MAX_PATH];
	DWORD length = GetModuleFileNameA(module, buffer, MAX_PATH);
	std::string name(buffer, length);
	size_t slash = name.find_last_of("/\\");
	if (slash != std::string::npos) {
		name = name.substr(slash + 1);
	}
	names.emplace(module, name);
	return name;
#else
	(void)address;
	return std::string();
#endif
}

std::string nativeModule(Script& script, int index) {
	if (!script.loaded || index < 0 || index >= script.nativeCount) {
		return std::string();
	}
	auto* header = reinterpret_cast<AMX_HEADER*>(script.amx->base);
	ucell address = (GETENTRY(header, natives, index))->address;
	return address != 0 ? moduleOf(reinterpret_cast<const void*>(uintptr_t(address))) : std::string();
}

const SourceFile& sourceFile(Script& script, const std::string& name) {
	auto found = sources.find(name);
	if (found != sources.end()) {
		return found->second;
	}
	SourceFile& file = sources[name];
	std::vector<std::string> candidates;
	std::string normal = name;
	std::replace(normal.begin(), normal.end(), '\\', '/');
	candidates.push_back(normal);
	bool absolute = normal.size() > 1 && (normal[0] == '/' || normal[1] == ':');
	if (!absolute) {
		size_t slash = script.path.find_last_of("/\\");
		if (slash != std::string::npos) {
			candidates.push_back(script.path.substr(0, slash + 1) + normal);
		}
		candidates.push_back("gamemodes/" + normal);
		candidates.push_back("filterscripts/" + normal);
	}
	for (const std::string& candidate : candidates) {
		std::ifstream stream(candidate, std::ios::binary);
		if (!stream) {
			continue;
		}
		std::string line;
		while (std::getline(stream, line)) {
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			file.lines.push_back(std::move(line));
		}
		file.found = true;
		break;
	}
	return file;
}

void appendBacktrace(std::vector<std::string>& lines, int& number, Script& origin, cell place, cell frame, size_t from) {
	State& s = state();
	Script* script = &origin;
	for (int segment = 0; segment != 64 && lines.size() < MaximumFrames; ++segment) {
		AMX* amx = script->amx;
		int publicCall = -1;
		for (size_t index = std::min(from, s.calls.size()); index-- > 0;) {
			if (s.calls[index].kind == CallKind::Public && s.calls[index].script == script) {
				publicCall = int(index);
				break;
			}
		}
		struct Step {
			cell place;
			cell frame;
			cell entry;
			bool last;
		};
		std::vector<Step> steps;
		cell publicEntry = publicCall != -1 ? s.calls[publicCall].entry : 0;
		while (steps.size() < MaximumWalk) {
			const cell* link = cellsAt(amx, frame, 2);
			cell returnAddress = link != nullptr ? link[1] : 0;
			bool last = returnAddress <= 0 || returnAddress >= codeSize(amx);
			steps.push_back({ place, frame, last ? publicEntry : calleeOf(amx, returnAddress), last });
			if (last || link[0] <= frame) {
				break;
			}
			place = returnAddress - cell(sizeof(cell));
			frame = link[0];
		}
		auto emit = [&](const Step& step) {
			FrameName named = nameFrame(*script, step.place, step.entry);
			if (named.function == -1 && step.last && publicCall != -1) {
				named.name = script->publicName(s.calls[publicCall].index, s.calls[publicCall].name);
				named.isPublic = true;
			}
			std::string text = "#" + std::to_string(number) + " " + (named.isPublic ? "public " : "") + named.name + formatArguments(*script, named.function, step.frame);
			std::string where = location(*script, step.place);
			if (!where.empty()) {
				text += " at " + where;
			} else if (!script->debug) {
				char address[32];
				std::snprintf(address, sizeof(address), " at 0x%08X", unsigned(step.place));
				text += address;
			}
			if (&step == &steps.front() && script != &origin) {
				text += " in " + script->name;
			}
			lines.push_back(std::move(text));
			++number;
		};
		for (size_t index = 0; index < steps.size() && lines.size() < MaximumFrames;) {
			size_t run = 1;
			while (steps[index].entry > 0 && index + run < steps.size() && steps[index + run].entry == steps[index].entry) {
				++run;
			}
			if (run <= 4) {
				emit(steps[index]);
				++index;
				continue;
			}
			emit(steps[index]);
			emit(steps[index + 1]);
			size_t skipped = run - 3;
			lines.push_back("   ... " + std::to_string(skipped) + " more calls of " + nameFrame(*script, steps[index].place, steps[index].entry).name);
			number += int(skipped);
			emit(steps[index + run - 1]);
			index += run;
		}
		if (publicCall <= 0) {
			break;
		}
		const Call& caller = s.calls[publicCall - 1];
		if (caller.kind != CallKind::Native || lines.size() >= MaximumFrames) {
			break;
		}
		std::string text = "#" + std::to_string(number++) + " native " + caller.script->nativeName(caller.index) + "()";
		std::string module = nativeModule(*caller.script, caller.index);
		if (!module.empty()) {
			text += " in " + module;
		}
		lines.push_back(std::move(text));
		script = caller.script;
		place = caller.cip - cell(sizeof(cell));
		frame = caller.frm;
		from = size_t(publicCall - 1);
	}
}

}

std::string shortPath(const std::string& path) {
	std::string normal = path;
	std::replace(normal.begin(), normal.end(), '\\', '/');
	while (normal.compare(0, 2, "./") == 0) {
		normal.erase(0, 2);
	}
	const std::string& root = serverRoot();
	if (!root.empty() && normal.size() > root.size()) {
		std::string head = normal.substr(0, root.size());
		std::transform(head.begin(), head.end(), head.begin(), [](unsigned char character) { return char(std::tolower(character)); });
		if (head == root) {
			normal.erase(0, root.size());
		}
	}
	return normal;
}

std::string location(Script& script, cell cip) {
	if (!script.debug) {
		return std::string();
	}
	const std::string& file = script.debug->fileAt(ucell(cip));
	long line = script.debug->lineAt(ucell(cip));
	if (file.empty() || line == 0) {
		return std::string();
	}
	return shortPath(file) + ":" + std::to_string(line);
}

std::string sourceLine(Script& script, cell cip) {
	if (!script.debug) {
		return std::string();
	}
	const std::string& name = script.debug->fileAt(ucell(cip));
	long line = script.debug->lineAt(ucell(cip));
	if (name.empty() || line == 0) {
		return std::string();
	}
	std::string text;
	{
		std::lock_guard<std::mutex> guard(cacheLock);
		const SourceFile& file = sourceFile(script, name);
		if (!file.found || size_t(line) > file.lines.size()) {
			return std::string();
		}
		text = file.lines[size_t(line) - 1];
	}
	size_t start = text.find_first_not_of(" \t");
	if (start == std::string::npos) {
		return std::string();
	}
	size_t end = text.find_last_not_of(" \t");
	std::string trimmed = text.substr(start, end - start + 1);
	std::replace(trimmed.begin(), trimmed.end(), '\t', ' ');
	if (trimmed.size() > 160) {
		trimmed = trimmed.substr(0, 157) + "...";
	}
	return trimmed;
}

std::string formatCall(Script& script, int function, cell frm) {
	std::string name = function != -1 && script.debug ? script.debug->functions()[function].name : std::string("??");
	return name + formatArguments(script, function, frm);
}

std::string formatArguments(Script& script, int function, cell frm) {
	AMX* amx = script.amx;
	const DebugInfo::Function* info = function != -1 && script.debug ? &script.debug->functions()[function] : nullptr;
	std::string text = "(";
	const cell* count = cellsAt(amx, frm + cell(2 * sizeof(cell)), 1);
	cell arguments = count != nullptr ? *count / cell(sizeof(cell)) : 0;
	if (arguments < 0 || arguments > 256) {
		arguments = 0;
	}
	const cell* values = cellsAt(amx, frm + cell(3 * sizeof(cell)), arguments);
	if (values == nullptr) {
		arguments = 0;
	}
	size_t parameter = 0;
	for (cell index = 0; index < arguments && index < MaximumArguments; ++index) {
		if (index != 0) {
			text += ", ";
		}
		const DebugInfo::Symbol* symbol = nullptr;
		if (info != nullptr && parameter < info->parameters.size()) {
			symbol = &script.debug->symbols()[info->parameters[parameter]];
			if (symbol->ident != DebugInfo::IdentVariableArguments) {
				++parameter;
			}
		}
		if (symbol == nullptr) {
			text += std::to_string(values[index]);
		} else if (symbol->ident == DebugInfo::IdentVariableArguments) {
			text += formatVariable(amx, values[index]);
		} else {
			text += formatArgument(script, *symbol, values[index]);
		}
	}
	if (arguments > MaximumArguments) {
		cell more = arguments - MaximumArguments;
		text += ", ... " + std::to_string(more) + " more";
	}
	return text + ')';
}

std::string formatPublicCall(Script& script, int index) {
	AMX* amx = script.amx;
	std::string text = script.publicName(index) + "(";
	cell arguments = amx->paramcount;
	const cell* values = cellsAt(amx, amx->stk, arguments);
	if (values != nullptr) {
		for (cell argument = 0; argument < arguments && argument < MaximumArguments; ++argument) {
			if (argument != 0) {
				text += ", ";
			}
			text += std::to_string(values[argument]);
		}
	}
	return text + ")";
}

std::string functionAtFrame(Script& script, cell place, cell frame, cell publicEntry) {
	const cell* link = cellsAt(script.amx, frame, 2);
	cell returnAddress = link != nullptr ? link[1] : 0;
	cell entry = returnAddress > 0 && returnAddress < codeSize(script.amx) ? calleeOf(script.amx, returnAddress) : publicEntry;
	FrameName named = nameFrame(script, place, entry);
	return named.name == "??" ? std::string() : named.name;
}

std::vector<std::string> backtrace(Script& script, cell place, cell frame, size_t from) {
	std::vector<std::string> lines;
	int number = 0;
	appendBacktrace(lines, number, script, place, frame, from);
	return lines;
}

std::vector<std::string> entryBacktrace(Script& script, cell entry, cell frame, cell stack, size_t from) {
	std::vector<std::string> lines;
	int number = 0;
	FrameName named = nameFrame(script, entry, entry);
	std::string text = "#" + std::to_string(number++) + " " + (named.isPublic ? "public " : "") + named.name + "(...)";
	std::string where = location(script, entry + cell(sizeof(cell)));
	if (!where.empty()) {
		text += " at " + where;
	}
	lines.push_back(text + ", entering it");
	const cell* back = cellsAt(script.amx, stack, 1);
	if (back != nullptr && *back > 0 && *back < codeSize(script.amx)) {
		appendBacktrace(lines, number, script, *back - cell(sizeof(cell)), frame, from);
	}
	return lines;
}

std::vector<std::string> nativeCallBacktrace(const Call& native, size_t from) {
	std::vector<std::string> lines;
	int number = 0;
	std::string text = "#" + std::to_string(number++) + " native " + native.script->nativeName(native.index) + "()";
	std::string module = nativeModule(*native.script, native.index);
	if (!module.empty()) {
		text += " in " + module;
	}
	lines.push_back(std::move(text));
	appendBacktrace(lines, number, *native.script, native.cip - cell(sizeof(cell)), native.frm, from);
	return lines;
}

std::vector<std::string> currentBacktrace(bool native) {
	State& s = state();
	std::vector<std::string> lines;
	if (s.calls.empty()) {
		return lines;
	}
	size_t top = s.calls.size() - 1;
	while (top != 0 && s.calls[top].kind == CallKind::Function) {
		--top;
	}
	const Call& call = s.calls[top];
	if (call.kind == CallKind::Native) {
		if (native) {
			return nativeCallBacktrace(call, top);
		}
		int number = 0;
		appendBacktrace(lines, number, *call.script, call.cip - cell(sizeof(cell)), call.frm, top);
	} else {
		AMX* amx = call.script->amx;
		int number = 0;
		appendBacktrace(lines, number, *call.script, amx->cip - cell(sizeof(cell)), amx->frm, top + 1);
	}
	return lines;
}

}
