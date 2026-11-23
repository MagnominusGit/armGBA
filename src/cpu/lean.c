/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The exact CPU, table-driven: each instruction goes straight from its opcode bits to a handler that
// decodes and runs that kind of instruction, with its bus accesses in the same order as step.c's,
// so the machine ends up exactly where step.c leaves it. Kinds without a handler of their own yet
// go through step.c's decoder and executor.

#include "decode.h"
#include "gba.h"

typedef void (*LeanHandler)(struct GBA*, uint32_t opcode, uint32_t address);

// ARM opcodes by bits 27-20 and 7-4; Thumb by bits 15-6.
static LeanHandler armTable[4096];
static LeanHandler thumbTable[1024];

// Whether the flags satisfy an instruction's condition field, as step.c's passes().
static inline bool passes(uint32_t cpsr, int cond) {
	bool n = cpsr & PSR_N;
	bool z = cpsr & PSR_Z;
	bool c = cpsr & PSR_C;
	bool v = cpsr & PSR_V;
	switch (cond) {
	case 0x0:
		return z;
	case 0x1:
		return !z;
	case 0x2:
		return c;
	case 0x3:
		return !c;
	case 0x4:
		return n;
	case 0x5:
		return !n;
	case 0x6:
		return v;
	case 0x7:
		return !v;
	case 0x8:
		return c && !z;
	case 0x9:
		return !c || z;
	case 0xA:
		return n == v;
	case 0xB:
		return n != v;
	case 0xC:
		return !z && n == v;
	case 0xD:
		return z || n != v;
	case 0xE:
		return true;
	default:
		return false;
	}
}

// Any ARM instruction, by step.c's decoder and executor. Its condition has passed.
static void armGeneric(struct GBA* gba, uint32_t opcode, uint32_t address) {
	struct Inst in;
	decodeArm(&in, address, opcode);
	stepExecute(gba, &in);
}

// Any Thumb instruction, likewise; only conditional branches have a condition.
static void thumbGeneric(struct GBA* gba, uint32_t opcode, uint32_t address) {
	struct Inst in;
	decodeThumb(&in, address, (uint16_t) opcode);
	if (in.cond == COND_AL || passes(gba->cpu.cpsr, in.cond)) {
		stepExecute(gba, &in);
	}
}

// Sets N and Z from a result.
static inline void setNz(struct CPU* cpu, uint32_t d) {
	cpu->cpsr = (cpu->cpsr & ~(PSR_N | PSR_Z)) | (d & PSR_N) | (d ? 0 : PSR_Z);
}

// N, Z, C and V from an addition's or subtraction's result.
static inline void setNzcv(struct CPU* cpu, uint32_t d, bool c, bool v) {
	cpu->cpsr = (cpu->cpsr & ~(PSR_N | PSR_Z | PSR_C | PSR_V)) | (d & PSR_N) | (d ? 0 : PSR_Z) | (c ? PSR_C : 0) | (v ? PSR_V : 0);
}

static inline uint32_t add(struct CPU* cpu, uint32_t n, uint32_t m) {
	uint32_t d = n + m;
	setNzcv(cpu, d, d < n, (~(n ^ m) & (n ^ d)) >> 31);
	return d;
}

static inline uint32_t sub(struct CPU* cpu, uint32_t n, uint32_t m) {
	uint32_t d = n - m;
	setNzcv(cpu, d, n >= m, ((n ^ m) & (n ^ d)) >> 31);
	return d;
}

// Thumb LSL, LSR and ASR by a constant: MOVS of a register shifted by it.
static void thumbShiftImm(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	struct CPU* cpu = &gba->cpu;
	uint32_t v = cpu->r[(opcode >> 3) & 7];
	unsigned a = (opcode >> 6) & 0x1F;
	bool carry;
	uint32_t d;
	switch (opcode >> 11) {
	case 0:
		carry = a ? (v >> (32 - a)) & 1 : !!(cpu->cpsr & PSR_C);
		d = a ? v << a : v;
		break;
	case 1:
		carry = a ? (v >> (a - 1)) & 1 : v >> 31;
		d = a ? v >> a : 0;
		break;
	default:
		carry = a ? (v >> (a - 1)) & 1 : v >> 31;
		d = a ? (uint32_t) ((int32_t) v >> a) : (uint32_t) ((int32_t) v >> 31);
		break;
	}
	cpu->r[opcode & 7] = d;
	setNz(cpu, d);
	cpu->cpsr = (cpu->cpsr & ~PSR_C) | (carry ? PSR_C : 0);
}

// Thumb ADD and SUB of a register or a 3-bit constant.
static void thumbAddSub3(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	struct CPU* cpu = &gba->cpu;
	uint32_t n = cpu->r[(opcode >> 3) & 7];
	uint32_t m = (opcode & 0x0400) ? (opcode >> 6) & 7 : cpu->r[(opcode >> 6) & 7];
	cpu->r[opcode & 7] = (opcode & 0x0200) ? sub(cpu, n, m) : add(cpu, n, m);
}

// Thumb MOV, CMP, ADD and SUB with an 8-bit constant.
static void thumbImm8(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	struct CPU* cpu = &gba->cpu;
	int rd = (opcode >> 8) & 7;
	uint32_t imm = opcode & 0xFF;
	switch ((opcode >> 11) & 3) {
	case 0:
		cpu->r[rd] = imm;
		setNz(cpu, imm);
		break;
	case 1:
		sub(cpu, cpu->r[rd], imm);
		break;
	case 2:
		cpu->r[rd] = add(cpu, cpu->r[rd], imm);
		break;
	default:
		cpu->r[rd] = sub(cpu, cpu->r[rd], imm);
		break;
	}
}

// Thumb's conditional branch (conditions 0-13).
static void thumbBranchCond(struct GBA* gba, uint32_t opcode, uint32_t address) {
	struct CPU* cpu = &gba->cpu;
	if (passes(cpu->cpsr, (opcode >> 8) & 0xF)) {
		cpu->r[15] = address + 4 + (int32_t) (int8_t) opcode * 2;
		stepReload(gba);
	}
}

// A Thumb load into a low register or store from one, as step.c's mem() makes it: the next fetch is
// nonsequential, and a load takes an internal cycle after its read. A signed halfword load from an
// odd address reads the byte there, sign-extended.
static inline void thumbMem(struct GBA* gba, int rd, uint32_t address, int size, bool load, bool signExtend) {
	struct CPU* cpu = &gba->cpu;
	cpu->fetchSeq = false;
	if (load) {
		uint32_t value = busRead(gba, address, size, false);
		if (signExtend) {
			value = (size == 1 || (address & 1)) ? (uint32_t) (int8_t) value : (uint32_t) (int16_t) value;
		}
		busIdle(gba);
		cpu->r[rd] = value;
	} else {
		busWrite(gba, address, cpu->r[rd], size, false);
	}
}

// LDR from the literal pool: the word-aligned PC plus a constant.
static void thumbLdrPc(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	thumbMem(gba, (opcode >> 8) & 7, (gba->cpu.r[15] & ~3u) + ((opcode & 0xFF) << 2), 4, true, false);
}

// Loads and stores with a register offset, including the signed loads.
static void thumbMemReg(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	static const struct {
		bool load;
		uint8_t size;
		bool signExtend;
	} forms[8] = {
		{ false, 4, false }, { false, 2, false }, { false, 1, false }, { true, 1, true },
		{ true, 4, false }, { true, 2, false }, { true, 1, false }, { true, 2, true },
	};
	int form = (opcode >> 9) & 7;
	struct CPU* cpu = &gba->cpu;
	thumbMem(gba, opcode & 7, cpu->r[(opcode >> 3) & 7] + cpu->r[(opcode >> 6) & 7], forms[form].size, forms[form].load, forms[form].signExtend);
}

// Loads and stores of words, bytes and halfwords with a 5-bit offset.
static void thumbMemImm(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	static const uint8_t sizes[6] = { 4, 4, 1, 1, 2, 2 };
	int form = (opcode >> 11) - 0x0C;
	int size = sizes[form];
	thumbMem(gba, opcode & 7, gba->cpu.r[(opcode >> 3) & 7] + ((opcode >> 6) & 0x1F) * size, size, form & 1, false);
}

// Loads and stores relative to the stack pointer.
static void thumbMemSp(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	thumbMem(gba, (opcode >> 8) & 7, gba->cpu.r[13] + ((opcode & 0xFF) << 2), 4, opcode & 0x0800, false);
}

// ADD of a constant to the word-aligned PC or to the stack pointer, into a low register; and the stack
// pointer moved by a constant. None of them sets flags.
static void thumbAddPc(struct GBA* gba, uint32_t opcode, uint32_t address) {
	gba->cpu.r[(opcode >> 8) & 7] = ((address + 4) & ~3u) + ((opcode & 0xFF) << 2);
}

static void thumbAddSp(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	gba->cpu.r[(opcode >> 8) & 7] = gba->cpu.r[13] + ((opcode & 0xFF) << 2);
}

static void thumbMoveSp(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	uint32_t offset = (opcode & 0x7F) << 2;
	gba->cpu.r[13] = (opcode & 0x80) ? gba->cpu.r[13] - offset : gba->cpu.r[13] + offset;
}

// The register-to-register operations but MUL. Shifts by a register take an internal cycle, after
// which the fetch is nonsequential.
static void thumbAlu(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	struct CPU* cpu = &gba->cpu;
	int rd = opcode & 7;
	int rs = (opcode >> 3) & 7;
	int op = (opcode >> 6) & 0xF;
	bool c = cpu->cpsr & PSR_C;
	if (op == 2 || op == 3 || op == 4 || op == 7) {
		busIdle(gba);
		cpu->fetchSeq = false;
		uint32_t v = cpu->r[rd];
		unsigned s = cpu->r[rs] & 0xFF;
		bool carry = c;
		uint32_t d = v;
		if (s) {
			switch (op) {
			case 2:
				carry = s < 32 ? (v >> (32 - s)) & 1 : s == 32 ? v & 1 : 0;
				d = s < 32 ? v << s : 0;
				break;
			case 3:
				carry = s < 32 ? (v >> (s - 1)) & 1 : s == 32 ? v >> 31 : 0;
				d = s < 32 ? v >> s : 0;
				break;
			case 4:
				carry = s < 32 ? (v >> (s - 1)) & 1 : v >> 31;
				d = s < 32 ? (uint32_t) ((int32_t) v >> s) : (uint32_t) ((int32_t) v >> 31);
				break;
			default:
				if (s & 31) {
					carry = (v >> ((s & 31) - 1)) & 1;
					d = (v >> (s & 31)) | (v << (32 - (s & 31)));
				} else {
					carry = v >> 31;
				}
				break;
			}
		}
		cpu->r[rd] = d;
		setNz(cpu, d);
		cpu->cpsr = (cpu->cpsr & ~PSR_C) | (carry ? PSR_C : 0);
		return;
	}
	uint32_t n = cpu->r[rd];
	uint32_t m = cpu->r[rs];
	uint32_t d;
	switch (op) {
	case 0x0:
		d = cpu->r[rd] = n & m;
		break;
	case 0x1:
		d = cpu->r[rd] = n ^ m;
		break;
	case 0x5:
		d = cpu->r[rd] = n + m + c;
		setNzcv(cpu, d, (uint64_t) n + m + c > 0xFFFFFFFFu, (~(n ^ m) & (n ^ d)) >> 31);
		return;
	case 0x6:
		d = cpu->r[rd] = n - m - !c;
		setNzcv(cpu, d, (uint64_t) n >= (uint64_t) m + !c, ((n ^ m) & (n ^ d)) >> 31);
		return;
	case 0x8:
		d = n & m;
		break;
	case 0x9:
		// NEG: RSB from 0.
		cpu->r[rd] = sub(cpu, 0, m);
		return;
	case 0xA:
		sub(cpu, n, m);
		return;
	case 0xB:
		add(cpu, n, m);
		return;
	case 0xC:
		d = cpu->r[rd] = n | m;
		break;
	case 0xE:
		d = cpu->r[rd] = n & ~m;
		break;
	default:
		d = cpu->r[rd] = ~m;
		break;
	}
	setNz(cpu, d);
}

// ADD, CMP and MOV reaching r8-r15, where the PC isn't written: ADD and MOV set no flags.
static void thumbHiReg(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	struct CPU* cpu = &gba->cpu;
	int hd = (opcode & 7) | ((opcode >> 4) & 8);
	uint32_t m = cpu->r[(opcode >> 3) & 0xF];
	switch ((opcode >> 8) & 3) {
	case 0:
		cpu->r[hd] += m;
		break;
	case 1:
		sub(cpu, cpu->r[hd], m);
		break;
	default:
		cpu->r[hd] = m;
		break;
	}
}

// The unconditional branch, and BL's two halves.
static void thumbBranch(struct GBA* gba, uint32_t opcode, uint32_t address) {
	gba->cpu.r[15] = address + 4 + (((int32_t) ((uint32_t) opcode << 21)) >> 20);
	stepReload(gba);
}

static void thumbBl1(struct GBA* gba, uint32_t opcode, uint32_t address) {
	gba->cpu.r[14] = address + 4 + (((int32_t) ((uint32_t) opcode << 21)) >> 9);
}

static void thumbBl2(struct GBA* gba, uint32_t opcode, uint32_t address) {
	struct CPU* cpu = &gba->cpu;
	uint32_t target = cpu->r[14] + ((opcode & 0x7FF) << 1);
	cpu->r[14] = (address + 2) | 1;
	cpu->r[15] = target;
	stepReload(gba);
}

// ARM data processing with a constant or a register shifted by a constant, writing a register other
// than the PC; anything else goes through step.c.
static void armDataProcessing(struct GBA* gba, uint32_t opcode, uint32_t address) {
	struct CPU* cpu = &gba->cpu;
	int rd = (opcode >> 12) & 0xF;
	if (rd == 15) {
		armGeneric(gba, opcode, address);
		return;
	}
	bool const c = cpu->cpsr & PSR_C;
	bool carry;
	uint32_t m;
	if (opcode & 0x02000000) {
		unsigned rotate = (opcode >> 7) & 0x1E;
		m = rotate ? ((opcode & 0xFF) >> rotate) | ((opcode & 0xFF) << (32 - rotate)) : opcode & 0xFF;
		carry = rotate ? m >> 31 : c;
	} else {
		uint32_t v = cpu->r[opcode & 0xF];
		unsigned a = (opcode >> 7) & 0x1F;
		switch ((opcode >> 5) & 3) {
		case SHIFT_LSL:
			carry = a ? (v >> (32 - a)) & 1 : c;
			m = v << a;
			break;
		case SHIFT_LSR:
			carry = a ? (v >> (a - 1)) & 1 : v >> 31;
			m = a ? v >> a : 0;
			break;
		case SHIFT_ASR:
			carry = a ? (v >> (a - 1)) & 1 : v >> 31;
			m = (uint32_t) ((int32_t) v >> (a ? a : 31));
			break;
		default:
			carry = a ? (v >> (a - 1)) & 1 : v & 1;
			m = a ? (v >> a) | (v << (32 - a)) : ((uint32_t) c << 31) | (v >> 1);
			break;
		}
	}
	uint32_t const n = cpu->r[(opcode >> 16) & 0xF];
	bool const setFlags = opcode & 0x00100000;
	uint32_t d;
	switch ((opcode >> 21) & 0xF) {
	case ALU_AND:
		d = cpu->r[rd] = n & m;
		break;
	case ALU_EOR:
		d = cpu->r[rd] = n ^ m;
		break;
	case ALU_SUB:
		if (setFlags) {
			cpu->r[rd] = sub(cpu, n, m);
		} else {
			cpu->r[rd] = n - m;
		}
		return;
	case ALU_RSB:
		if (setFlags) {
			cpu->r[rd] = sub(cpu, m, n);
		} else {
			cpu->r[rd] = m - n;
		}
		return;
	case ALU_ADD:
		if (setFlags) {
			cpu->r[rd] = add(cpu, n, m);
		} else {
			cpu->r[rd] = n + m;
		}
		return;
	case ALU_ADC:
		d = cpu->r[rd] = n + m + c;
		if (setFlags) {
			setNzcv(cpu, d, (uint64_t) n + m + c > 0xFFFFFFFFu, (~(n ^ m) & (n ^ d)) >> 31);
		}
		return;
	case ALU_SBC:
		d = cpu->r[rd] = n - m - !c;
		if (setFlags) {
			setNzcv(cpu, d, (uint64_t) n >= (uint64_t) m + !c, ((n ^ m) & (n ^ d)) >> 31);
		}
		return;
	case ALU_RSC:
		d = cpu->r[rd] = m - n - !c;
		if (setFlags) {
			setNzcv(cpu, d, (uint64_t) m >= (uint64_t) n + !c, ((m ^ n) & (m ^ d)) >> 31);
		}
		return;
	case ALU_TST:
		d = n & m;
		break;
	case ALU_TEQ:
		d = n ^ m;
		break;
	case ALU_CMP:
		sub(cpu, n, m);
		return;
	case ALU_CMN:
		add(cpu, n, m);
		return;
	case ALU_ORR:
		d = cpu->r[rd] = n | m;
		break;
	case ALU_MOV:
		d = cpu->r[rd] = m;
		break;
	case ALU_BIC:
		d = cpu->r[rd] = n & ~m;
		break;
	default:
		d = cpu->r[rd] = ~m;
		break;
	}
	if (setFlags) {
		setNz(cpu, d);
		cpu->cpsr = (cpu->cpsr & ~PSR_C) | (carry ? PSR_C : 0);
	}
}

// ARM LDR, STR, LDRB and STRB, as step.c's mem() makes them: loads into the PC, and write-back to
// it, go through step.c.
static void armMem(struct GBA* gba, uint32_t opcode, uint32_t address) {
	struct CPU* cpu = &gba->cpu;
	int rd = (opcode >> 12) & 0xF;
	int rn = (opcode >> 16) & 0xF;
	bool const pre = opcode & 0x01000000;
	bool const writeback = !pre || (opcode & 0x00200000);
	bool const load = opcode & 0x00100000;
	if (rd == 15 || (rn == 15 && writeback)) {
		armGeneric(gba, opcode, address);
		return;
	}
	uint32_t offset;
	if (opcode & 0x02000000) {
		uint32_t v = cpu->r[opcode & 0xF];
		unsigned a = (opcode >> 7) & 0x1F;
		switch ((opcode >> 5) & 3) {
		case SHIFT_LSL:
			offset = v << a;
			break;
		case SHIFT_LSR:
			offset = a ? v >> a : 0;
			break;
		case SHIFT_ASR:
			offset = (uint32_t) ((int32_t) v >> (a ? a : 31));
			break;
		default:
			offset = a ? (v >> a) | (v << (32 - a)) : ((uint32_t) !!(cpu->cpsr & PSR_C) << 31) | (v >> 1);
			break;
		}
	} else {
		offset = opcode & 0xFFF;
	}
	uint32_t const base = cpu->r[rn];
	uint32_t const moved = (opcode & 0x00800000) ? base + offset : base - offset;
	uint32_t const at = pre ? moved : base;
	int const size = (opcode & 0x00400000) ? 1 : 4;
	cpu->fetchSeq = false;
	if (load) {
		uint32_t value = busRead(gba, at, size, false);
		if (writeback) {
			cpu->r[rn] = moved;
		}
		busIdle(gba);
		cpu->r[rd] = value;
		return;
	}
	busWrite(gba, at, cpu->r[rd], size, false);
	if (writeback) {
		cpu->r[rn] = moved;
	}
}

// LDM and STM without the S bit, and Thumb's PUSH, POP, LDMIA and STMIA, as step.c's block() makes
// them: an empty list moves the base as sixteen registers would and transfers r15; the base is
// written back after the first access; a load takes an internal cycle after its reads.
static inline void blockTransfer(struct GBA* gba, int rn, uint16_t list, bool load, bool up, bool pre, bool writeback, bool thumb) {
	struct CPU* cpu = &gba->cpu;
	int count = list ? __builtin_popcount(list) : 16;
	if (!list) {
		list = 0x8000;
	}
	uint32_t base = cpu->r[rn];
	uint32_t moved = up ? base + 4 * count : base - 4 * count;
	uint32_t address = up ? base : moved;
	if (!up) {
		pre = !pre;
	}
	cpu->fetchSeq = false;
	bool seq = false;
	for (int i = 0; i < 16; ++i) {
		if (!(list & (1 << i))) {
			continue;
		}
		if (pre) {
			address += 4;
		}
		uint32_t at = (address >> 24) < REGION_SRAM ? address & ~3u : address;
		if (load) {
			uint32_t value = busRead(gba, at, 4, seq);
			if (writeback && !seq) {
				cpu->r[rn] = moved;
			}
			cpu->r[i] = value;
		} else {
			uint32_t value = cpu->r[i];
			if (i == 15) {
				value += thumb ? 2 : 4;
			}
			busWrite(gba, at, value, 4, seq);
			if (writeback && !seq) {
				cpu->r[rn] = moved;
			}
		}
		if (!pre) {
			address += 4;
		}
		seq = true;
	}
	if (load) {
		busIdle(gba);
		if (list & 0x8000) {
			stepReload(gba);
		}
	}
}

static void armBlock(struct GBA* gba, uint32_t opcode, uint32_t address) {
	if (opcode & 0x00400000) {
		armGeneric(gba, opcode, address);
		return;
	}
	blockTransfer(gba, (opcode >> 16) & 0xF, opcode & 0xFFFF, opcode & 0x00100000, opcode & 0x00800000, opcode & 0x01000000,
	              opcode & 0x00200000, false);
}

// B and BL: a signed 24-bit word offset from the PC, which is 8 ahead.
static void armBranch(struct GBA* gba, uint32_t opcode, uint32_t address) {
	struct CPU* cpu = &gba->cpu;
	if (opcode & 0x01000000) {
		cpu->r[14] = address + 4;
	}
	cpu->r[15] = address + 8 + ((int32_t) (opcode << 8) >> 6);
	stepReload(gba);
}

// The handler for an ARM opcode's bits 27-20 and 7-4, i.
static LeanHandler armHandler(int i) {
	int const bits27to20 = i >> 4;
	bool const bit4 = i & 1;
	bool const bit7 = i & 8;
	// TST, TEQ, CMP and CMN without S: MRS, MSR and BX.
	bool const statusOp = (bits27to20 & 0x19) == 0x10;
	switch (bits27to20 >> 5) {
	case 0:
		if (bit4 || statusOp) {
			return armGeneric;
		}
		return armDataProcessing;
	case 1:
		return statusOp ? armGeneric : armDataProcessing;
	case 2:
		return armMem;
	case 3:
		return bit4 ? armGeneric : armMem;
	case 4:
		return armBlock;
	case 5:
		return armBranch;
	default:
		(void) bit7;
		return armGeneric;
	}
}

static void thumbPush(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	blockTransfer(gba, 13, (opcode & 0xFF) | ((opcode & 0x100) ? 0x4000 : 0), false, false, true, true, true);
}

static void thumbPop(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	blockTransfer(gba, 13, (opcode & 0xFF) | ((opcode & 0x100) ? 0x8000 : 0), true, true, false, true, true);
}

static void thumbBlock(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	blockTransfer(gba, (opcode >> 8) & 7, opcode & 0xFF, opcode & 0x0800, true, false, true, true);
}

// The handler for a Thumb opcode's bits 15-6, i.
static LeanHandler thumbHandler(int i) {
	int const format = i >> 5;
	switch (format) {
	case 0x00:
	case 0x01:
	case 0x02:
		return thumbShiftImm;
	case 0x03:
		return thumbAddSub3;
	case 0x04:
	case 0x05:
	case 0x06:
	case 0x07:
		return thumbImm8;
	case 0x08:
		if (!(i & 0x10)) {
			// Bits 9-6: MUL stays generic.
			return (i & 0xF) == 0xD ? thumbGeneric : thumbAlu;
		}
		// Bits 9-8 pick ADD, CMP, MOV or BX; bit 7 is the destination's high bit, and with bits 2-0
		// (not part of i) it might be the PC, so these are only taken when the high bit is clear.
		if (((i >> 2) & 3) != 3 && !(i & 2)) {
			return thumbHiReg;
		}
		return thumbGeneric;
	case 0x09:
		return thumbLdrPc;
	case 0x0A:
	case 0x0B:
		return thumbMemReg;
	case 0x0C:
	case 0x0D:
	case 0x0E:
	case 0x0F:
	case 0x10:
	case 0x11:
		return thumbMemImm;
	case 0x12:
	case 0x13:
		return thumbMemSp;
	case 0x14:
		return thumbAddPc;
	case 0x15:
		return thumbAddSp;
	case 0x16:
		return ((i >> 2) & 0xF) == 0 ? thumbMoveSp : ((i >> 2) & 0xE) == 4 ? thumbPush : thumbGeneric;
	case 0x17:
		return ((i >> 2) & 0xE) == 0xC ? thumbPop : thumbGeneric;
	case 0x18:
	case 0x19:
		return thumbBlock;
	case 0x1A:
	case 0x1B:
		return ((i >> 2) & 0xF) < 0xE ? thumbBranchCond : thumbGeneric;
	case 0x1C:
		return thumbBranch;
	case 0x1E:
		return thumbBl1;
	case 0x1F:
		return thumbBl2;
	default:
		return thumbGeneric;
	}
}

static void fillTables(void) {
	for (int i = 0; i < 4096; ++i) {
		armTable[i] = armHandler(i);
	}
	for (int i = 0; i < 1024; ++i) {
		thumbTable[i] = thumbHandler(i);
	}
}

// One instruction, as step.c's step() runs it: an IRQ to take, tracing or a cheat cartridge's trigger
// go through step() itself.
static inline void leanOne(struct GBA* gba) {
	struct CPU* cpu = &gba->cpu;
	if ((gba->irq.line && !cpu->irqMasked) || gba->traceFile || gba->cartTrapped) {
		stepInstruction(gba);
		return;
	}
	uint32_t const opcode = cpu->prefetch[0];
	// The I flag as it is before the instruction decides whether an IRQ is taken after it.
	cpu->irqMasked = cpu->cpsr & PSR_I;
	cpu->prefetch[0] = cpu->prefetch[1];
	if (cpu->cpsr & PSR_T) {
		cpu->r[15] += 2;
		cpu->prefetch[1] = busFetch(gba, cpu->r[15], true);
		thumbTable[(opcode >> 6) & 0x3FF](gba, opcode, cpu->r[15] - 4);
	} else {
		cpu->r[15] += 4;
		cpu->prefetch[1] = busFetch(gba, cpu->r[15], false);
		if ((opcode >> 28) == COND_AL || passes(cpu->cpsr, opcode >> 28)) {
			armTable[((opcode >> 16) & 0xFF0) | ((opcode >> 4) & 0xF)](gba, opcode, cpu->r[15] - 8);
		}
	}
}

void leanStep(struct GBA* gba) {
	if (!armTable[0]) {
		fillTables();
	}
	leanOne(gba);
}

// Instructions until the frame ends, something asks for control back or the CPU halts.
void leanRun(struct GBA* gba) {
	if (!armTable[0]) {
		fillTables();
	}
	struct CPU* cpu = &gba->cpu;
	uint32_t const frame = gba->video.frameCounter;
	while (gba->video.frameCounter == frame && !gba->returnRequested && !cpu->halted) {
		leanOne(gba);
	}
}
