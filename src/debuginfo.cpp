#include "debuginfo.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace tracer {

namespace {

constexpr uint16_t DebugMagic = 0xF1EF;

template <typename T>
bool readValue(const std::vector<char>& data, size_t& at, T& value) {
	if (at + sizeof(T) > data.size()) {
		return false;
	}
	std::memcpy(&value, data.data() + at, sizeof(T));
	at += sizeof(T);
	return true;
}

bool readName(const std::vector<char>& data, size_t& at, std::string& name) {
	if (at >= data.size()) {
		return false;
	}
	const char* start = data.data() + at;
	const char* end = static_cast<const char*>(std::memchr(start, 0, data.size() - at));
	if (end == nullptr) {
		return false;
	}
	name.assign(start, end);
	at += (end - start) + 1;
	return true;
}

const std::string noName;

}

bool DebugInfo::load(const std::string& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return false;
	}
	std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

	AMX_HEADER header;
	if (data.size() < sizeof(header)) {
		return false;
	}
	std::memcpy(&header, data.data(), sizeof(header));
	if (header.size <= 0 || size_t(header.size) >= data.size()) {
		return false;
	}
	codeSize_ = ucell(header.dat - header.cod);

	size_t at = size_t(header.size);
	int32_t size;
	uint16_t magic;
	char fileVersion, amxVersion;
	int16_t flags, files, lines, symbols, tags, automatons, states;
	if (!readValue(data, at, size) || !readValue(data, at, magic) || magic != DebugMagic) {
		return false;
	}
	readValue(data, at, fileVersion);
	readValue(data, at, amxVersion);
	readValue(data, at, flags);
	readValue(data, at, files);
	readValue(data, at, lines);
	readValue(data, at, symbols);
	readValue(data, at, tags);
	readValue(data, at, automatons);
	if (!readValue(data, at, states)) {
		return false;
	}

	for (int index = 0; index != uint16_t(files); ++index) {
		File entry;
		if (!readValue(data, at, entry.address) || !readName(data, at, entry.name)) {
			return false;
		}
		files_.push_back(std::move(entry));
	}
	for (int index = 0; index != uint16_t(lines); ++index) {
		Line entry;
		if (!readValue(data, at, entry.address) || !readValue(data, at, entry.line)) {
			return false;
		}
		lines_.push_back(entry);
	}
	for (int index = 0; index != uint16_t(symbols); ++index) {
		Symbol symbol;
		ucell address;
		int16_t dimensions;
		if (!readValue(data, at, address) || !readValue(data, at, symbol.tag) || !readValue(data, at, symbol.codeStart) || !readValue(data, at, symbol.codeEnd)
			|| !readValue(data, at, symbol.ident) || !readValue(data, at, symbol.scope) || !readValue(data, at, dimensions) || !readName(data, at, symbol.name)) {
			return false;
		}
		symbol.address = cell(address);
		for (int dimension = 0; dimension != uint16_t(dimensions); ++dimension) {
			Dimension entry;
			if (!readValue(data, at, entry.tag) || !readValue(data, at, entry.size)) {
				return false;
			}
			symbol.dimensions.push_back(entry);
		}
		symbols_.push_back(std::move(symbol));
	}
	for (int index = 0; index != uint16_t(tags); ++index) {
		int16_t id;
		std::string name;
		if (!readValue(data, at, id) || !readName(data, at, name)) {
			break;
		}
		tags_.emplace(id, name == "_" ? std::string() : std::move(name));
	}

	for (int index = 0; index != int(symbols_.size()); ++index) {
		const Symbol& symbol = symbols_[index];
		if (symbol.ident == IdentFunction) {
			functions_.push_back({ symbol.name, symbol.codeStart, symbol.codeEnd, symbol.tag, {} });
		}
	}
	std::sort(functions_.begin(), functions_.end(), [](const Function& a, const Function& b) {
		return a.start != b.start ? a.start < b.start : a.end - a.start < b.end - b.start;
	});
	for (int index = 0; index != int(symbols_.size()); ++index) {
		const Symbol& symbol = symbols_[index];
		if (symbol.scope != ScopeLocal || symbol.address < cell(3 * sizeof(cell)) || symbol.ident == IdentFunction) {
			continue;
		}
		int function = functionStartingAt(symbol.codeStart);
		if (function != -1) {
			functions_[function].parameters.push_back(index);
		}
	}
	for (Function& function : functions_) {
		std::sort(function.parameters.begin(), function.parameters.end(), [this](int a, int b) { return symbols_[a].address < symbols_[b].address; });
	}
	std::stable_sort(lines_.begin(), lines_.end(), [](const Line& a, const Line& b) { return a.address < b.address; });
	std::stable_sort(files_.begin(), files_.end(), [](const File& a, const File& b) { return a.address < b.address; });
	return !functions_.empty();
}

int DebugInfo::functionAt(ucell address) const {
	auto after = std::upper_bound(functions_.begin(), functions_.end(), address, [](ucell value, const Function& function) { return value < function.start; });
	int best = -1;
	for (int steps = 0; after != functions_.begin() && steps != 64; ++steps) {
		--after;
		if (best != -1 && after->start != functions_[best].start) {
			break;
		}
		if (address < after->end) {
			best = int(after - functions_.begin());
		}
	}
	return best;
}

int DebugInfo::functionStartingAt(ucell address) const {
	auto found = std::lower_bound(functions_.begin(), functions_.end(), address, [](const Function& function, ucell value) { return function.start < value; });
	if (found == functions_.end() || found->start != address) {
		return -1;
	}
	return int(found - functions_.begin());
}

long DebugInfo::lineAt(ucell address) const {
	auto after = std::upper_bound(lines_.begin(), lines_.end(), address, [](ucell value, const Line& line) { return value < line.address; });
	if (after == lines_.begin()) {
		return 0;
	}
	return long(std::prev(after)->line) + 1;
}

bool DebugInfo::statementAt(ucell address, ucell& start, ucell& end) const {
	auto after = std::upper_bound(lines_.begin(), lines_.end(), address, [](ucell value, const Line& line) { return value < line.address; });
	if (after == lines_.begin()) {
		return false;
	}
	start = std::prev(after)->address;
	end = after == lines_.end() ? codeSize_ : after->address;
	return true;
}

const std::string& DebugInfo::fileAt(ucell address) const {
	auto after = std::upper_bound(files_.begin(), files_.end(), address, [](ucell value, const File& file) { return value < file.address; });
	if (after == files_.begin()) {
		return noName;
	}
	return std::prev(after)->name;
}

const std::string& DebugInfo::tagName(int16_t tag) const {
	auto found = tags_.find(tag);
	return found == tags_.end() ? noName : found->second;
}

const DebugInfo::Symbol* DebugInfo::variableAt(cell address, bool local, ucell position) const {
	const Symbol* best = nullptr;
	for (const Symbol& symbol : symbols_) {
		if (symbol.ident == IdentFunction || symbol.address != address) {
			continue;
		}
		bool frame = symbol.scope == ScopeLocal;
		if (frame != local) {
			continue;
		}
		if ((frame || symbol.scope == ScopeStatic) && (position < symbol.codeStart || position >= symbol.codeEnd)) {
			continue;
		}
		if (best == nullptr || symbol.codeEnd - symbol.codeStart < best->codeEnd - best->codeStart) {
			best = &symbol;
		}
	}
	return best;
}

}
