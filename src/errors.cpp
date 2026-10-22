#include "tracer.hpp"
#include "opcodes.hpp"

#include <algorithm>

namespace tracer {

namespace {

constexpr int AmxExecGdk = -10;
constexpr int AmxExecGdk42 = -10000;

struct Instruction {
	cell cip;
	cell opcode;
	const cell* operands;
};

const unsigned char* codeOf(AMX* amx) {
	return amx->base + reinterpret_cast<AMX_HEADER*>(amx->base)->cod;
}

cell codeSizeOf(AMX* amx) {
	auto* header = reinterpret_cast<AMX_HEADER*>(amx->base);
	return header->dat - header->cod;
}

unsigned char* dataOf(AMX* amx) {
	auto* header = reinterpret_cast<AMX_HEADER*>(amx->base);
	return amx->data != nullptr ? amx->data : amx->base + header->dat;
}

const cell* cellAt(AMX* amx, cell address) {
	if (address < 0 || (address & (sizeof(cell) - 1)) != 0 || address > amx->stp - cell(sizeof(cell))) {
		return nullptr;
	}
	return reinterpret_cast<const cell*>(dataOf(amx) + address);
}

bool opcodeAt(Script& script, cell cip, cell& opcode) {
	AMX* amx = script.amx;
	if (!script.numericOpcodes || cip < 0 || (cip & (sizeof(cell) - 1)) != 0 || cip > codeSizeOf(amx) - cell(sizeof(cell))) {
		return false;
	}
	opcode = *reinterpret_cast<const cell*>(codeOf(amx) + cip);
	return true;
}

cell operandAt(Script& script, cell cip, int index) {
	return *reinterpret_cast<const cell*>(codeOf(script.amx) + cip + cell(sizeof(cell)) * (index + 1));
}

std::vector<Instruction> statementCode(Script& script, cell place) {
	std::vector<Instruction> code;
	ucell start, end;
	if (!script.debug || !script.numericOpcodes || !script.debug->statementAt(ucell(place), start, end)) {
		return code;
	}
	AMX* amx = script.amx;
	const unsigned char* base = codeOf(amx);
	cell size = codeSizeOf(amx);
	cell cip = cell(start);
	while (cip < cell(end) && cip < size && code.size() < 512) {
		cell cells = instructionCells(base, cip, size);
		if (cells == 0) {
			break;
		}
		code.push_back({ cip, *reinterpret_cast<const cell*>(base + cip), reinterpret_cast<const cell*>(base + cip) + 1 });
		cip += cells * cell(sizeof(cell));
	}
	return code;
}

int instructionIndex(const std::vector<Instruction>& code, cell cip) {
	for (size_t index = 0; index != code.size(); ++index) {
		if (code[index].cip == cip) {
			return int(index);
		}
	}
	return -1;
}

int pushedCells(cell opcode) {
	switch (opcode) {
	case OpPushPri:
	case OpPushAlt:
	case OpPushR:
	case OpPushC:
	case OpPush:
	case OpPushS:
	case OpPushAdr:
		return 1;
	case OpPush2C:
	case OpPush2:
	case OpPush2S:
	case OpPush2Adr:
		return 2;
	case OpPush3C:
	case OpPush3:
	case OpPush3S:
	case OpPush3Adr:
		return 3;
	case OpPush4C:
	case OpPush4:
	case OpPush4S:
	case OpPush4Adr:
		return 4;
	case OpPush5C:
	case OpPush5:
	case OpPush5S:
	case OpPush5Adr:
		return 5;
	default:
		return 0;
	}
}

std::string boundsArray(Script& script, cell bounds, cell size) {
	std::vector<Instruction> code = statementCode(script, bounds);
	int at = instructionIndex(code, bounds);
	if (at == -1) {
		return std::string();
	}
	bool local = false;
	bool found = false;
	cell address = 0;
	for (int index = at + 1; index < int(code.size()) && index <= at + 4 && !found; ++index) {
		cell opcode = code[index].opcode;
		if (opcode == OpConstAlt || opcode == OpLoadAlt || opcode == OpAddrAlt || opcode == OpLoadSAlt) {
			found = true;
			local = opcode == OpAddrAlt || opcode == OpLoadSAlt;
			address = code[index].operands[0];
		} else if (opcode == OpLidx || opcode == OpLidxB || opcode == OpIdxaddr || opcode == OpIdxaddrB || opcode == OpPopAlt) {
			break;
		}
	}
	int pending = 0;
	for (int index = at - 1; index >= 0 && !found; --index) {
		const Instruction& instruction = code[index];
		cell opcode = instruction.opcode;
		int cells = pushedCells(opcode);
		if (opcode == OpStack && instruction.operands[0] > 0) {
			pending += instruction.operands[0] / cell(sizeof(cell));
		} else if (opcode == OpSysreqN) {
			pending += instruction.operands[1] / cell(sizeof(cell)) + 1;
		} else if (opcode == OpCall) {
			if (index != 0 && code[index - 1].opcode == OpPushC) {
				pending += code[index - 1].operands[0] / cell(sizeof(cell));
			}
		} else if (opcode == OpPopPri || opcode == OpPopAlt) {
			++pending;
		} else if (cells != 0) {
			if (pending >= cells) {
				pending -= cells;
				continue;
			}
			if (cells == 1 && pending == 0) {
				if (opcode == OpPushC || opcode == OpPushAdr || opcode == OpPushS || opcode == OpPush) {
					found = true;
					local = opcode == OpPushAdr || opcode == OpPushS;
					address = instruction.operands[0];
				} else if (opcode == OpPushPri && index != 0) {
					const Instruction& load = code[index - 1];
					if (load.opcode == OpConstPri || load.opcode == OpLoadPri || load.opcode == OpAddrPri || load.opcode == OpLoadSPri) {
						found = true;
						local = load.opcode == OpAddrPri || load.opcode == OpLoadSPri;
						address = load.operands[0];
					}
				}
			}
			break;
		}
	}
	if (!found) {
		return std::string();
	}
	const DebugInfo::Symbol* symbol = script.debug->variableAt(address, local, ucell(bounds));
	if (symbol == nullptr) {
		return std::string();
	}
	for (size_t dimension = 0; dimension != symbol->dimensions.size(); ++dimension) {
		if (cell(symbol->dimensions[dimension].size) == size) {
			std::string name = symbol->name;
			for (size_t inner = 0; inner != dimension; ++inner) {
				name += "[]";
			}
			return name;
		}
	}
	return std::string();
}

bool boundsIndex(Script& script, cell bounds, cell frm, cell& value) {
	std::vector<Instruction> code = statementCode(script, bounds);
	int at = instructionIndex(code, bounds);
	if (at < 1) {
		return false;
	}
	cell offset = 0;
	int load = at - 1;
	if (code[load].opcode == OpAddC && load >= 1) {
		offset = code[load].operands[0];
		--load;
	}
	const Instruction& instruction = code[load];
	const cell* source = nullptr;
	switch (instruction.opcode) {
	case OpLoadSPri:
		source = cellAt(script.amx, frm + instruction.operands[0]);
		break;
	case OpLoadPri:
		source = cellAt(script.amx, instruction.operands[0]);
		break;
	case OpLrefSPri: {
		const cell* reference = cellAt(script.amx, frm + instruction.operands[0]);
		source = reference != nullptr ? cellAt(script.amx, *reference) : nullptr;
		break;
	}
	default:
		return false;
	}
	if (source == nullptr) {
		return false;
	}
	value = *source + offset;
	return true;
}

std::string hex(cell value) {
	char buffer[16];
	std::snprintf(buffer, sizeof(buffer), "0x%08X", unsigned(value));
	return buffer;
}

std::string plural(cell count, const char* one, const char* many) {
	return std::to_string(count) + " " + (count == 1 ? one : many);
}

bool insideMemory(AMX* amx, cell address, cell hea, cell stk) {
	return address >= 0 && (address < hea || (address >= stk && address < amx->stp));
}

std::string memoryDetail(Script& script, const Failure& failure) {
	AMX* amx = script.amx;
	cell opcode;
	if (!failure.start || !opcodeAt(script, failure.cip, opcode)) {
		return std::string();
	}
	auto reference = [&](cell address) -> cell {
		const cell* value = cellAt(amx, address);
		return value != nullptr ? *value : -1;
	};
	cell address = -1;
	bool write = false;
	switch (opcode) {
	case OpLoadI:
	case OpLodbI:
	case OpIncI:
	case OpDecI:
		address = failure.pri;
		write = opcode == OpIncI || opcode == OpDecI;
		break;
	case OpStorI:
	case OpStrbI:
		address = failure.alt;
		write = true;
		break;
	case OpLidx:
		address = failure.alt + failure.pri * cell(sizeof(cell));
		break;
	case OpLrefPri:
	case OpLrefAlt:
		address = reference(operandAt(script, failure.cip, 0));
		break;
	case OpLrefSPri:
	case OpLrefSAlt:
		address = reference(failure.frm + operandAt(script, failure.cip, 0));
		break;
	case OpSrefPri:
	case OpSrefAlt:
		address = reference(operandAt(script, failure.cip, 0));
		write = true;
		break;
	case OpSrefSPri:
	case OpSrefSAlt:
		address = reference(failure.frm + operandAt(script, failure.cip, 0));
		write = true;
		break;
	case OpMovs:
	case OpCmps:
		address = insideMemory(amx, failure.pri, failure.hea, failure.stk) ? failure.alt : failure.pri;
		write = opcode == OpMovs && address == failure.alt;
		break;
	case OpFill:
		address = failure.alt;
		write = true;
		break;
	default:
		return std::string();
	}
	return std::string(write ? "writing" : "reading") + " address " + hex(address) + ", outside the memory of the script";
}

std::vector<std::string> describe(Script& script, const Failure& failure) {
	std::vector<std::string> lines;
	AMX* amx = script.amx;
	cell opcode;
	switch (failure.code) {
	case AMX_ERR_BOUNDS: {
		cell bounds = failure.cip - 2 * cell(sizeof(cell));
		if (!opcodeAt(script, bounds, opcode) || opcode != OpBounds) {
			break;
		}
		cell size = operandAt(script, bounds, 0) + 1;
		std::string array = boundsArray(script, bounds, size);
		std::string owner = array.empty() ? "the array" : array;
		cell index = failure.pri;
		if (!failure.registers && !boundsIndex(script, bounds, failure.frm, index)) {
			lines.push_back("the index went past " + owner + ", which has size " + std::to_string(size));
		} else if (index < 0) {
			lines.push_back("index " + std::to_string(index) + " is negative, " + owner + " has size " + std::to_string(size));
		} else {
			lines.push_back("index " + std::to_string(index) + " is out of bounds, " + owner + " has size " + std::to_string(size));
		}
		break;
	}
	case AMX_ERR_DIVIDE:
		if (failure.registers && failure.start && opcodeAt(script, failure.cip, opcode)) {
			cell dividend = opcode == OpSdivAlt || opcode == OpUdivAlt ? failure.alt : failure.pri;
			lines.push_back("dividing " + std::to_string(dividend) + " by zero");
		}
		break;
	case AMX_ERR_MEMACCESS:
		if (failure.registers) {
			std::string detail = memoryDetail(script, failure);
			if (!detail.empty()) {
				lines.push_back(detail);
			}
		}
		break;
	case AMX_ERR_STACKERR: {
		cell space = amx->stp - amx->hlw;
		if (failure.registers) {
			lines.push_back("the stack reached the heap, the stack pointer is at " + hex(failure.stk) + " and the heap at " + hex(failure.hea));
		}
		lines.push_back("the script has " + plural(space, "byte", "bytes") + " for its stack and heap, #pragma dynamic gives it more");
		break;
	}
	case AMX_ERR_STACKLOW:
		lines.push_back("the stack went past its top");
		break;
	case AMX_ERR_HEAPLOW:
		lines.push_back("the heap went below its bottom");
		break;
	case AMX_ERR_NATIVE:
		if (failure.native != -1) {
			lines.push_back("native " + script.nativeName(failure.native) + " reported the error");
		}
		break;
	case AMX_ERR_EXIT:
		lines.push_back("the script called exit");
		break;
	case AMX_ERR_INVINSTR:
		if (failure.start && opcodeAt(script, failure.cip, opcode)) {
			lines.push_back("instruction " + std::to_string(opcode) + " at " + hex(failure.cip) + " is not valid");
		}
		break;
	case AMX_ERR_NOTFOUND: {
		auto* header = reinterpret_cast<AMX_HEADER*>(amx->base);
		std::string missing;
		for (int index = 0; index != script.nativeCount; ++index) {
			if ((GETENTRY(header, natives, index))->address == 0) {
				missing += (missing.empty() ? "" : ", ") + std::string(GETENTRYNAME(header, GETENTRY(header, natives, index)));
			}
		}
		if (!missing.empty()) {
			lines.push_back("natives not registered: " + missing);
		}
		break;
	}
	case AMX_ERR_INDEX:
		lines.push_back("public index " + std::to_string(failure.publicIndex) + " does not exist");
		break;
	}
	if (failure.native != -1 && failure.code != AMX_ERR_NATIVE) {
		lines.push_back("native " + script.nativeName(failure.native) + " returned the error");
	}
	if (failure.fault) {
		lines.push_back("PawnJIT hit a CPU fault in its generated code and runs this script in the interpreter from now on, this is a fault of PawnJIT and not of the script");
	}
	return lines;
}

void reconcileFrame(Script& script, Failure& failure, cell seenPlace, cell seenFrame) {
	failure.frm = seenFrame;
	if (!script.debug) {
		return;
	}
	int target = script.debug->functionAt(ucell(failure.place()));
	if (target == -1) {
		return;
	}
	cell place = seenPlace;
	cell frame = seenFrame;
	for (int level = 0; level != 32; ++level) {
		if (script.debug->functionAt(ucell(place)) == target) {
			failure.frm = frame;
			return;
		}
		const cell* link = cellAt(script.amx, frame);
		const cell* back = cellAt(script.amx, frame + cell(sizeof(cell)));
		if (link == nullptr || back == nullptr || *back <= 0 || *link <= frame) {
			return;
		}
		place = *back - cell(sizeof(cell));
		frame = *link;
	}
}

void refineDivide(Script& script, Failure& failure) {
	if (!script.debug) {
		return;
	}
	cell place = failure.place();
	cell frame = failure.frm;
	cell from = failure.cip;
	for (int level = 0; level != 32; ++level) {
		std::vector<cell> divisions;
		for (const Instruction& instruction : statementCode(script, place)) {
			if (instruction.cip >= from && (instruction.opcode == OpSdiv || instruction.opcode == OpSdivAlt || instruction.opcode == OpUdiv || instruction.opcode == OpUdivAlt)) {
				divisions.push_back(instruction.cip);
			}
		}
		if (!divisions.empty()) {
			failure.frm = frame;
			if (divisions.size() == 1) {
				failure.cip = divisions[0];
				failure.start = true;
				failure.exact = true;
			} else {
				failure.cip = place + cell(sizeof(cell));
				failure.start = false;
			}
			return;
		}
		const cell* link = cellAt(script.amx, frame);
		const cell* back = cellAt(script.amx, frame + cell(sizeof(cell)));
		if (link == nullptr || back == nullptr || *back <= 0 || *link <= frame) {
			return;
		}
		place = *back - cell(sizeof(cell));
		from = *back;
		frame = *link;
	}
}

void callRuntimeError(Script& script, int code, bool& suppress) {
	State& s = state();
	IPawnScript* api = script.api;
	int index;
	if (api == nullptr || api->FindPublic("OnRuntimeError", &index) != AMX_ERR_NONE) {
		return;
	}
	AMX* amx = script.amx;
	if (amx->stk - amx->hea < cell(16 * sizeof(cell))) {
		return;
	}
	cell zero = 0;
	cell address;
	cell* physical;
	if (api->PushArray(&address, &physical, &zero, 1) != AMX_ERR_NONE) {
		return;
	}
	api->Push(code);
	s.reporting = true;
	cell result = 0;
	api->Exec(&result, index);
	s.reporting = false;
	suppress = *physical != 0;
	api->Release(address);
}

}

Failure locateFailure(Script& script, int code, int index, bool fromJit, size_t depth) {
	State& s = state();
	Failure failure;
	failure.script = &script;
	failure.code = code;
	failure.publicIndex = index;
	failure.depth = depth;
	if (code == AMX_ERR_INDEX || code == AMX_ERR_NOTFOUND || code == AMX_ERR_CALLBACK || code == AMX_ERR_INIT) {
		return failure;
	}
	AMX* amx = script.amx;
	bool raised = script.raisedNative != -1 && script.raisedError == code;
	PawnJITErrorState jit;
	if (fromJit && s.jit != nullptr && s.jit->lastError(jit) && jit.amx == amx && jit.error == code) {
		failure.cip = jit.cip;
		failure.frm = jit.frm;
		failure.stk = jit.stk;
		failure.hea = jit.hea;
		failure.pri = jit.pri;
		failure.alt = jit.alt;
		failure.located = true;
		failure.exact = true;
		failure.registers = true;
		failure.fault = jit.fault;
		failure.start = code != AMX_ERR_BOUNDS && !raised;
		if (raised) {
			failure.native = script.raisedNative;
		}
		return failure;
	}
	if (raised) {
		failure.cip = script.raisedCip;
		failure.frm = script.raisedFrm;
		failure.located = true;
		failure.exact = true;
		failure.native = script.raisedNative;
		return failure;
	}
	if (script.seenCip == 0 && code != AMX_ERR_BOUNDS) {
		return failure;
	}
	if (code == AMX_ERR_BOUNDS) {
		failure.cip = amx->cip;
		failure.located = true;
		failure.exact = true;
		reconcileFrame(script, failure, script.seenCip - cell(sizeof(cell)), script.seenFrm);
		return failure;
	}
	failure.cip = script.seenCip;
	failure.frm = script.seenFrm;
	failure.located = true;
	if (code == AMX_ERR_DIVIDE) {
		refineDivide(script, failure);
	}
	return failure;
}

void reportFailure(Failure& failure) {
	State& s = state();
	Script& script = *failure.script;
	int code = failure.code;
	if (code == AMX_ERR_INDEX && (failure.publicIndex == AmxExecGdk || failure.publicIndex <= AmxExecGdk42)) {
		return;
	}
	uint64_t now = Clock::now();
	cell opcode;
	bool proc = failure.start && failure.registers && opcodeAt(script, failure.cip, opcode) && opcode == OpProc;
	bool entering = proc && failure.frm != failure.stk;
	cell place = proc ? failure.cip + cell(sizeof(cell)) : failure.place();
	std::string where;
	std::string function;
	if (failure.located) {
		where = location(script, place);
		function = functionAtFrame(script, place, failure.frm, failure.depth < s.calls.size() ? s.calls[failure.depth].entry : 0);
		if (where.empty()) {
			where = hex(place);
		}
	}
	std::string publicName = script.publicName(failure.publicIndex, failure.publicName);
	std::string key = script.name + "|" + std::to_string(code) + "|" + (where.empty() ? "public " + publicName : where);
	auto found = s.errorIndex.find(key);
	ErrorRecord* record;
	if (found == s.errorIndex.end()) {
		s.errorIndex.emplace(key, s.errors.size());
		s.errors.emplace_back();
		record = &s.errors.back();
		record->key = key;
		record->script = script.name;
		record->publicName = publicName;
		record->where = where.empty() ? std::string() : where + (function.empty() ? std::string() : ", in " + function);
		record->code = code;
		record->firstAt = now;
	} else {
		record = &s.errors[found->second];
	}
	++record->count;
	++s.errorTotal;
	++s.second.errors;
	record->lastAt = now;

	bool full = !record->shown || s.settings.errorRepeat == 0;
	std::vector<std::string> lines;
	if (full) {
		lines.push_back("[Tracer] Run time error " + std::to_string(code) + " in " + script.name + ": " + errorText(code));
		for (const std::string& detail : describe(script, failure)) {
			lines.push_back("[Tracer]   " + detail);
		}
		if (failure.located) {
			std::string at = (failure.exact ? "at " : "near ") + where;
			if (!function.empty()) {
				at += ", in " + function;
			}
			lines.push_back("[Tracer]   " + at);
			if (s.settings.source) {
				std::string text = sourceLine(script, place);
				if (!text.empty()) {
					long line = script.debug->lineAt(ucell(place));
					lines.push_back("[Tracer]   " + std::to_string(line) + " | " + text);
				}
			}
			std::vector<std::string> frames = entering ? entryBacktrace(script, failure.cip, failure.frm, failure.stk, failure.depth + 1)
				: backtrace(script, place, failure.frm, failure.depth + 1);
			for (const std::string& frame : frames) {
				lines.push_back("[Tracer]   " + frame);
			}
		} else {
			lines.push_back("[Tracer]   in public " + publicName);
		}
		if (!script.debug && !script.hinted) {
			script.hinted = true;
			lines.push_back("[Tracer]   " + script.name + " has no debug information, compile it with -d3 to get files, lines and names");
		}
	}

	bool suppress = false;
	callRuntimeError(script, code, suppress);
	if (suppress) {
		return;
	}
	if (full) {
		record->shown = true;
		record->report = lines;
		record->lastPrintAt = now;
		record->held = 0;
		printBlock(LogLevel::Error, lines);
		return;
	}
	++record->held;
	if (s.clock.microseconds(now - record->lastPrintAt) >= double(s.settings.errorRepeat) * 1000000.0) {
		flushRepeatedErrors(false);
	}
}

void flushRepeatedErrors(bool all) {
	State& s = state();
	uint64_t now = Clock::now();
	for (ErrorRecord& record : s.errors) {
		if (record.held == 0) {
			continue;
		}
		double seconds = s.clock.microseconds(now - record.lastPrintAt) / 1000000.0;
		if (!all && seconds < double(s.settings.errorRepeat)) {
			continue;
		}
		char buffer[64];
		std::snprintf(buffer, sizeof(buffer), "%.0f s", seconds);
		std::string text = "[Tracer] Run time error " + std::to_string(record.code) + " in " + record.script + (record.where.empty() ? std::string() : " at " + record.where)
			+ " happened " + std::to_string(record.held) + (record.held == 1 ? " more time" : " more times") + " in the last " + buffer + ", " + std::to_string(record.count) + " in all";
		print(LogLevel::Error, text);
		record.held = 0;
		record.lastPrintAt = now;
	}
}

}
