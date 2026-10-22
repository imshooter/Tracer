#pragma once

#include <amx/amx.h>

namespace tracer {

/**
 * @brief      The instructions of the abstract machine, numbered as in the file format.
 */
enum Opcode : cell {
	OpNone = 0,
	OpLoadPri = 1,
	OpLoadAlt = 2,
	OpLoadSPri = 3,
	OpLoadSAlt = 4,
	OpLrefPri = 5,
	OpLrefAlt = 6,
	OpLrefSPri = 7,
	OpLrefSAlt = 8,
	OpLoadI = 9,
	OpLodbI = 10,
	OpConstPri = 11,
	OpConstAlt = 12,
	OpAddrPri = 13,
	OpAddrAlt = 14,
	OpStorPri = 15,
	OpStorAlt = 16,
	OpStorSPri = 17,
	OpStorSAlt = 18,
	OpSrefPri = 19,
	OpSrefAlt = 20,
	OpSrefSPri = 21,
	OpSrefSAlt = 22,
	OpStorI = 23,
	OpStrbI = 24,
	OpLidx = 25,
	OpLidxB = 26,
	OpIdxaddr = 27,
	OpIdxaddrB = 28,
	OpLctrl = 31,
	OpSctrl = 32,
	OpPushPri = 36,
	OpPushAlt = 37,
	OpPushR = 38,
	OpPushC = 39,
	OpPush = 40,
	OpPushS = 41,
	OpPopPri = 42,
	OpPopAlt = 43,
	OpStack = 44,
	OpHeap = 45,
	OpProc = 46,
	OpRet = 47,
	OpRetn = 48,
	OpCall = 49,
	OpSdiv = 73,
	OpSdivAlt = 74,
	OpUdiv = 76,
	OpUdivAlt = 77,
	OpAddC = 87,
	OpIncI = 111,
	OpDecI = 116,
	OpMovs = 117,
	OpCmps = 118,
	OpFill = 119,
	OpHalt = 120,
	OpBounds = 121,
	OpSysreqPri = 122,
	OpSysreqC = 123,
	OpFile = 124,
	OpLine = 125,
	OpSymbol = 126,
	OpSrange = 127,
	OpJumpPri = 128,
	OpCasetbl = 130,
	OpPushAdr = 133,
	OpSysreqN = 135,
	OpSymtag = 136,
	OpBreak = 137,
	OpPush2C = 138,
	OpPush2 = 139,
	OpPush2S = 140,
	OpPush2Adr = 141,
	OpPush3C = 142,
	OpPush3 = 143,
	OpPush3S = 144,
	OpPush3Adr = 145,
	OpPush4C = 146,
	OpPush4 = 147,
	OpPush4S = 148,
	OpPush4Adr = 149,
	OpPush5C = 150,
	OpPush5 = 151,
	OpPush5S = 152,
	OpPush5Adr = 153,
	OpConstS = 157,
	OpCount = 158
};

/**
 * @brief      The number of cells an instruction takes, its opcode included.
 *
 * @param      code   Code section of the script.
 * @param      cip    Address of the instruction.
 * @param      size   Size of the code section in bytes.
 *
 * @return     - the number of cells
 *             - `0` the instruction is not valid
 */
inline cell instructionCells(const unsigned char* code, cell cip, cell size) {
	cell opcode = *reinterpret_cast<const cell*>(code + cip);
	switch (opcode) {
	case 138: case 139: case 140: case 141:
	case 154: case 155: case 156: case 157:
	case OpSysreqN: case OpLine: case OpSrange:
		return 3;
	case 142: case 143: case 144: case 145:
		return 4;
	case 146: case 147: case 148: case 149:
		return 5;
	case 150: case 151: case 152: case 153:
		return 6;
	case OpLoadI: case OpStorI: case OpLidx: case OpIdxaddr: case 33: case 34: case 35:
	case OpPushPri: case OpPushAlt: case OpPopPri: case OpPopAlt: case OpProc: case OpRet: case OpRetn: case 50:
	case 65: case 66: case 67: case 72: case OpSdiv: case OpSdivAlt: case 75: case OpUdiv: case OpUdivAlt:
	case 78: case 79: case 80: case 81: case 82: case 83: case 84: case 85: case 86: case 89: case 90:
	case 93: case 94: case 95: case 96: case 97: case 98: case 99: case 100: case 101: case 102: case 103: case 104:
	case 107: case 108: case OpIncI: case 112: case 113: case OpDecI: case OpSysreqPri: case OpJumpPri: case 131: case 132:
	case 134: case OpBreak:
		return 1;
	case OpFile:
	case OpSymbol:
		if (cip + cell(sizeof(cell)) * 2 > size) {
			return 0;
		}
		return 2 + *reinterpret_cast<const cell*>(code + cip + sizeof(cell)) / cell(sizeof(cell));
	case OpCasetbl:
		if (cip + cell(sizeof(cell)) * 2 > size) {
			return 0;
		}
		return 2 + 2 * *reinterpret_cast<const cell*>(code + cip + sizeof(cell)) + 1;
	default:
		if (opcode <= OpNone || opcode >= OpCount) {
			return 0;
		}
		return 2;
	}
}

}
