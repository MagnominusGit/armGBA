/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE.
 *
 * The access and HALT timing follow SameBoy (Copyright (c) Lior Halphon), under the Expat (MIT) license:
 * see LICENSES/SameBoy.txt. */
#include "gb.h"

// The Sharp SM83, one instruction at a time. Every memory access takes an M-cycle and comes at its
// start, as SameBoy (MIT) times them. Opcodes and flags as Pan Docs
// and the gbdev opcode tables give them.

enum {
	FLAG_Z = 0x80,
	FLAG_N = 0x40,
	FLAG_H = 0x20,
	FLAG_C = 0x10,
};

static inline void tick(struct GB* gb) {
	gb->now += gb->mcycle;
}

static inline uint8_t readNow(struct GB* gb, uint16_t address) {
	const uint8_t* page = gb->readMap[address >> 12];
	return page ? page[address & 0xFFF] : gbRead(gb, address);
}

static inline uint8_t read8(struct GB* gb, uint16_t address) {
	uint8_t value = readNow(gb, address);
	tick(gb);
	return value;
}

static inline void write8(struct GB* gb, uint16_t address, uint8_t value) {
	gbWrite(gb, address, value);
	tick(gb);
}

static inline uint8_t fetch(struct GB* gb) {
	struct GBCpu* cpu = &gb->cpu;
	uint8_t value = read8(gb, cpu->pc);
	if (cpu->haltBug) {
		cpu->haltBug = false;
	} else {
		++cpu->pc;
	}
	return value;
}

static inline uint16_t fetch16(struct GB* gb) {
	uint8_t lo = fetch(gb);
	return lo | fetch(gb) << 8;
}

static inline uint16_t pair(uint8_t hi, uint8_t lo) {
	return hi << 8 | lo;
}

static inline uint16_t getHL(const struct GBCpu* cpu) {
	return pair(cpu->h, cpu->l);
}

static inline void setHL(struct GBCpu* cpu, uint16_t value) {
	cpu->h = value >> 8;
	cpu->l = value;
}

static inline __attribute__((always_inline)) uint8_t getR(struct GB* gb, int r) {
	struct GBCpu* cpu = &gb->cpu;
	switch (r) {
	case 0:
		return cpu->b;
	case 1:
		return cpu->c;
	case 2:
		return cpu->d;
	case 3:
		return cpu->e;
	case 4:
		return cpu->h;
	case 5:
		return cpu->l;
	case 6:
		return read8(gb, getHL(cpu));
	default:
		return cpu->a;
	}
}

static inline __attribute__((always_inline)) void setR(struct GB* gb, int r, uint8_t value) {
	struct GBCpu* cpu = &gb->cpu;
	switch (r) {
	case 0:
		cpu->b = value;
		break;
	case 1:
		cpu->c = value;
		break;
	case 2:
		cpu->d = value;
		break;
	case 3:
		cpu->e = value;
		break;
	case 4:
		cpu->h = value;
		break;
	case 5:
		cpu->l = value;
		break;
	case 6:
		write8(gb, getHL(cpu), value);
		break;
	default:
		cpu->a = value;
		break;
	}
}

// BC, DE, HL, SP by the two bits most 16-bit instructions use.
static uint16_t getRR(const struct GBCpu* cpu, int rr) {
	switch (rr) {
	case 0:
		return pair(cpu->b, cpu->c);
	case 1:
		return pair(cpu->d, cpu->e);
	case 2:
		return getHL(cpu);
	default:
		return cpu->sp;
	}
}

static void setRR(struct GBCpu* cpu, int rr, uint16_t value) {
	switch (rr) {
	case 0:
		cpu->b = value >> 8;
		cpu->c = value;
		break;
	case 1:
		cpu->d = value >> 8;
		cpu->e = value;
		break;
	case 2:
		setHL(cpu, value);
		break;
	default:
		cpu->sp = value;
		break;
	}
}

static void push(struct GB* gb, uint16_t value) {
	struct GBCpu* cpu = &gb->cpu;
	write8(gb, --cpu->sp, value >> 8);
	write8(gb, --cpu->sp, value);
}

static uint16_t pop(struct GB* gb) {
	struct GBCpu* cpu = &gb->cpu;
	uint8_t lo = read8(gb, cpu->sp++);
	return lo | read8(gb, cpu->sp++) << 8;
}

static bool condition(const struct GBCpu* cpu, int cc) {
	switch (cc) {
	case 0:
		return !(cpu->f & FLAG_Z);
	case 1:
		return cpu->f & FLAG_Z;
	case 2:
		return !(cpu->f & FLAG_C);
	default:
		return cpu->f & FLAG_C;
	}
}

// The eight ALU operations on A, by the opcode's middle bits.
static inline __attribute__((always_inline)) void alu(struct GBCpu* cpu, int op, uint8_t value) {
	uint8_t a = cpu->a;
	unsigned carry = cpu->f & FLAG_C ? 1 : 0;
	unsigned result;
	switch (op) {
	case 0: // ADD
	case 1: // ADC
		if (op == 0) {
			carry = 0;
		}
		result = a + value + carry;
		cpu->f = ((result & 0xFF) ? 0 : FLAG_Z) | (((a & 0xF) + (value & 0xF) + carry) > 0xF ? FLAG_H : 0) |
		         (result > 0xFF ? FLAG_C : 0);
		cpu->a = result;
		break;
	case 2: // SUB
	case 3: // SBC
	case 7: // CP
		if (op != 3) {
			carry = 0;
		}
		result = a - value - carry;
		cpu->f = ((result & 0xFF) ? 0 : FLAG_Z) | FLAG_N | ((int) (a & 0xF) - (int) (value & 0xF) - (int) carry < 0 ? FLAG_H : 0) |
		         (result > 0xFF ? FLAG_C : 0);
		if (op != 7) {
			cpu->a = result;
		}
		break;
	case 4: // AND
		cpu->a = a & value;
		cpu->f = (cpu->a ? 0 : FLAG_Z) | FLAG_H;
		break;
	case 5: // XOR
		cpu->a = a ^ value;
		cpu->f = cpu->a ? 0 : FLAG_Z;
		break;
	default: // OR
		cpu->a = a | value;
		cpu->f = cpu->a ? 0 : FLAG_Z;
		break;
	}
}

// The CB-prefixed rotates, shifts, swap and bit operations.
static void prefixed(struct GB* gb) {
	struct GBCpu* cpu = &gb->cpu;
	uint8_t op = fetch(gb);
	int r = op & 7;
	int bit = op >> 3 & 7;
	uint8_t value = getR(gb, r);
	uint8_t carryIn = cpu->f & FLAG_C ? 1 : 0;
	uint8_t result;
	switch (op >> 6) {
	case 0: {
		bool carryOut;
		switch (bit) {
		case 0: // RLC
			carryOut = value >> 7;
			result = value << 1 | value >> 7;
			break;
		case 1: // RRC
			carryOut = value & 1;
			result = value >> 1 | value << 7;
			break;
		case 2: // RL
			carryOut = value >> 7;
			result = value << 1 | carryIn;
			break;
		case 3: // RR
			carryOut = value & 1;
			result = value >> 1 | carryIn << 7;
			break;
		case 4: // SLA
			carryOut = value >> 7;
			result = value << 1;
			break;
		case 5: // SRA
			carryOut = value & 1;
			result = (value >> 1) | (value & 0x80);
			break;
		case 6: // SWAP
			carryOut = false;
			result = value << 4 | value >> 4;
			break;
		default: // SRL
			carryOut = value & 1;
			result = value >> 1;
			break;
		}
		cpu->f = (result ? 0 : FLAG_Z) | (carryOut ? FLAG_C : 0);
		setR(gb, r, result);
		break;
	}
	case 1: // BIT
		cpu->f = (cpu->f & FLAG_C) | FLAG_H | (value & (1 << bit) ? 0 : FLAG_Z);
		break;
	case 2: // RES
		setR(gb, r, value & ~(1 << bit));
		break;
	default: // SET
		setR(gb, r, value | 1 << bit);
		break;
	}
}

static void daa(struct GBCpu* cpu) {
	unsigned a = cpu->a;
	uint8_t f = cpu->f;
	if (f & FLAG_N) {
		if (f & FLAG_H) {
			a -= 6;
		}
		if (f & FLAG_C) {
			a -= 0x60;
		}
	} else {
		if ((f & FLAG_H) || (a & 0xF) > 9) {
			a += 6;
		}
		if ((f & FLAG_C) || a > 0x9F) {
			a += 0x60;
		}
	}
	cpu->f = (f & (FLAG_N | FLAG_C)) | (!(f & FLAG_N) && a > 0xFF ? FLAG_C : 0);
	cpu->a = a;
	if (!cpu->a) {
		cpu->f |= FLAG_Z;
	}
}

// SP plus a signed byte, for ADD SP,e and LD HL,SP+e: flags from the low byte's unsigned addition.
static uint16_t spOffset(struct GBCpu* cpu, uint8_t e) {
	uint16_t sp = cpu->sp;
	cpu->f = ((sp & 0xF) + (e & 0xF) > 0xF ? FLAG_H : 0) | ((sp & 0xFF) + e > 0xFF ? FLAG_C : 0);
	return sp + (int8_t) e;
}

// Runs the interrupt with the lowest flag set and enabled. Pushing PC's high byte can write IE, and
// the vector is chosen after it: if that leaves nothing pending, PC becomes 0.
static void dispatch(struct GB* gb) {
	struct GBCpu* cpu = &gb->cpu;
	cpu->ime = false;
	tick(gb);
	tick(gb);
	write8(gb, --cpu->sp, cpu->pc >> 8);
	uint8_t pending = gb->ie & gb->ifl & 0x1F;
	write8(gb, --cpu->sp, cpu->pc);
	if (!pending) {
		cpu->pc = 0;
		tick(gb);
		return;
	}
	int n = __builtin_ctz(pending);
	gb->ifl &= ~(1 << n);
	cpu->pc = 0x40 + n * 8;
	tick(gb);
}

// Ends HALT or STOP when the time comes: through events up to the next one.
static void idle(struct GB* gb, uint32_t until) {
	uint32_t target = gbSince(gb->nextEvent, until) < 0 ? gb->nextEvent : until;
	int32_t ahead = gbSince(target, gb->now);
	if (ahead > 0) {
		gb->now += (ahead + gb->mcycle - 1) / gb->mcycle * gb->mcycle;
	}
}

// An M-cycle of HALT. The original Game Boy looks for an interrupt 2 dots into each M-cycle but the
// first, the Color at the start of each, as SameBoy (MIT) has it. Returns whether the CPU woke, at
// the end of the M-cycle that saw the interrupt; otherwise moves on to the M-cycle that will.
static bool wake(struct GB* gb, uint32_t until) {
	struct GBCpu* cpu = &gb->cpu;
	unsigned later = gb->model == GB_MODEL_CGB ? 0 : 2;
	unsigned offset = cpu->justHalted ? 0 : later;
	cpu->justHalted = false;
	uint32_t start = gb->now;
	gb->now = start + offset;
	if (gbSince(gb->now, gb->nextEvent) >= 0) {
		gbRunEvents(gb);
	}
	// A speed switch done: the CPU goes on at once.
	if (!cpu->halted) {
		gb->now = start;
		return true;
	}
	if (gb->ie & gb->ifl & 0x1F) {
		cpu->halted = false;
		if (cpu->switching == 2) {
			cpu->switching = 0;
		}
		gb->now = start + gb->mcycle;
		return true;
	}
	uint32_t target = gbSince(gb->nextEvent, until) < 0 ? gb->nextEvent : until;
	int32_t ahead = gbSince(target, start + later);
	uint32_t cycles = ahead > 0 ? (ahead + gb->mcycle - 1) / gb->mcycle : 1;
	gb->now = start + (cycles ? cycles : 1) * gb->mcycle;
	return false;
}

static void step(struct GB* gb) {
	struct GBCpu* cpu = &gb->cpu;
	uint8_t op = fetch(gb);
	switch (op) {
	case 0x00: // NOP
		break;
	case 0x01:
	case 0x11:
	case 0x21:
	case 0x31: // LD rr,nn
		setRR(cpu, op >> 4, fetch16(gb));
		break;
	case 0x02: // LD (BC),A
		write8(gb, pair(cpu->b, cpu->c), cpu->a);
		break;
	case 0x12: // LD (DE),A
		write8(gb, pair(cpu->d, cpu->e), cpu->a);
		break;
	case 0x22: { // LD (HL+),A
		uint16_t hl = getHL(cpu);
		write8(gb, hl, cpu->a);
		setHL(cpu, hl + 1);
		break;
	}
	case 0x32: { // LD (HL-),A
		uint16_t hl = getHL(cpu);
		write8(gb, hl, cpu->a);
		setHL(cpu, hl - 1);
		break;
	}
	case 0x0A: // LD A,(BC)
		cpu->a = read8(gb, pair(cpu->b, cpu->c));
		break;
	case 0x1A: // LD A,(DE)
		cpu->a = read8(gb, pair(cpu->d, cpu->e));
		break;
	case 0x2A: { // LD A,(HL+)
		uint16_t hl = getHL(cpu);
		cpu->a = read8(gb, hl);
		setHL(cpu, hl + 1);
		break;
	}
	case 0x3A: { // LD A,(HL-)
		uint16_t hl = getHL(cpu);
		cpu->a = read8(gb, hl);
		setHL(cpu, hl - 1);
		break;
	}
	case 0x03:
	case 0x13:
	case 0x23:
	case 0x33: // INC rr
		setRR(cpu, op >> 4, getRR(cpu, op >> 4) + 1);
		tick(gb);
		break;
	case 0x0B:
	case 0x1B:
	case 0x2B:
	case 0x3B: // DEC rr
		setRR(cpu, op >> 4, getRR(cpu, op >> 4) - 1);
		tick(gb);
		break;
	case 0x04:
	case 0x0C:
	case 0x14:
	case 0x1C:
	case 0x24:
	case 0x2C:
	case 0x34:
	case 0x3C: { // INC r
		int r = op >> 3 & 7;
		uint8_t value = getR(gb, r) + 1;
		cpu->f = (cpu->f & FLAG_C) | (value ? 0 : FLAG_Z) | ((value & 0xF) == 0 ? FLAG_H : 0);
		setR(gb, r, value);
		break;
	}
	case 0x05:
	case 0x0D:
	case 0x15:
	case 0x1D:
	case 0x25:
	case 0x2D:
	case 0x35:
	case 0x3D: { // DEC r
		int r = op >> 3 & 7;
		uint8_t value = getR(gb, r) - 1;
		cpu->f = (cpu->f & FLAG_C) | FLAG_N | (value ? 0 : FLAG_Z) | ((value & 0xF) == 0xF ? FLAG_H : 0);
		setR(gb, r, value);
		break;
	}
	case 0x06:
	case 0x0E:
	case 0x16:
	case 0x1E:
	case 0x26:
	case 0x2E:
	case 0x36:
	case 0x3E: // LD r,n
		setR(gb, op >> 3 & 7, fetch(gb));
		break;
	case 0x07: // RLCA
		cpu->f = cpu->a >> 7 ? FLAG_C : 0;
		cpu->a = cpu->a << 1 | cpu->a >> 7;
		break;
	case 0x0F: // RRCA
		cpu->f = cpu->a & 1 ? FLAG_C : 0;
		cpu->a = cpu->a >> 1 | cpu->a << 7;
		break;
	case 0x17: { // RLA
		uint8_t carry = cpu->f & FLAG_C ? 1 : 0;
		cpu->f = cpu->a >> 7 ? FLAG_C : 0;
		cpu->a = cpu->a << 1 | carry;
		break;
	}
	case 0x1F: { // RRA
		uint8_t carry = cpu->f & FLAG_C ? 0x80 : 0;
		cpu->f = cpu->a & 1 ? FLAG_C : 0;
		cpu->a = cpu->a >> 1 | carry;
		break;
	}
	case 0x08: { // LD (nn),SP
		uint16_t address = fetch16(gb);
		write8(gb, address, cpu->sp);
		write8(gb, address + 1, cpu->sp >> 8);
		break;
	}
	case 0x09:
	case 0x19:
	case 0x29:
	case 0x39: { // ADD HL,rr
		uint16_t hl = getHL(cpu);
		uint16_t value = getRR(cpu, op >> 4);
		unsigned sum = hl + value;
		cpu->f = (cpu->f & FLAG_Z) | ((hl & 0xFFF) + (value & 0xFFF) > 0xFFF ? FLAG_H : 0) | (sum > 0xFFFF ? FLAG_C : 0);
		setHL(cpu, sum);
		tick(gb);
		break;
	}
	case 0x10: { // STOP
		// DIV resets (held an M-cycle longer with IME off), and the second byte is skipped only when
		// no interrupt is pending (SameBoy's stop()).
		bool pending = gb->ie & gb->ifl & 0x1F;
		if (cpu->ime) {
			gbTimerWrite(gb, 0x04, 0);
		}
		if (!pending) {
			fetch(gb);
		}
		if (!cpu->ime) {
			gbTimerWrite(gb, 0x04, 0);
		}
		gbStop(gb);
		break;
	}
	case 0x18: { // JR e
		int8_t e = fetch(gb);
		cpu->pc += e;
		tick(gb);
		break;
	}
	case 0x20:
	case 0x28:
	case 0x30:
	case 0x38: { // JR cc,e
		int8_t e = fetch(gb);
		if (condition(cpu, op >> 3 & 3)) {
			cpu->pc += e;
			tick(gb);
		}
		break;
	}
	case 0x27:
		daa(cpu);
		break;
	case 0x2F: // CPL
		cpu->a = ~cpu->a;
		cpu->f |= FLAG_N | FLAG_H;
		break;
	case 0x37: // SCF
		cpu->f = (cpu->f & FLAG_Z) | FLAG_C;
		break;
	case 0x3F: // CCF
		cpu->f = (cpu->f & FLAG_Z) | ((cpu->f & FLAG_C) ^ FLAG_C);
		break;
	case 0x76: { // HALT
		gb->attention = true;
		uint8_t pending = gb->ie & gb->ifl & 0x1F;
		if (!cpu->ime && pending) {
			cpu->haltBug = true;
		} else {
			cpu->halted = true;
			cpu->justHalted = true;
		}
		break;
	}
	case 0xC0:
	case 0xC8:
	case 0xD0:
	case 0xD8: // RET cc
		tick(gb);
		if (condition(cpu, op >> 3 & 3)) {
			cpu->pc = pop(gb);
			tick(gb);
		}
		break;
	case 0xC9: // RET
		cpu->pc = pop(gb);
		tick(gb);
		break;
	case 0xD9: // RETI
		cpu->pc = pop(gb);
		tick(gb);
		cpu->ime = true;
		gb->attention = true;
		break;
	case 0xC1:
	case 0xD1:
	case 0xE1: // POP rr
		setRR(cpu, op >> 4 & 3, pop(gb));
		break;
	case 0xF1: { // POP AF
		uint16_t value = pop(gb);
		cpu->a = value >> 8;
		cpu->f = value & 0xF0;
		break;
	}
	case 0xC5:
	case 0xD5:
	case 0xE5: // PUSH rr
		tick(gb);
		push(gb, getRR(cpu, op >> 4 & 3));
		break;
	case 0xF5: // PUSH AF
		tick(gb);
		push(gb, pair(cpu->a, cpu->f));
		break;
	case 0xC2:
	case 0xCA:
	case 0xD2:
	case 0xDA: { // JP cc,nn
		uint16_t address = fetch16(gb);
		if (condition(cpu, op >> 3 & 3)) {
			cpu->pc = address;
			tick(gb);
		}
		break;
	}
	case 0xC3: // JP nn
		cpu->pc = fetch16(gb);
		tick(gb);
		break;
	case 0xE9: // JP HL
		cpu->pc = getHL(cpu);
		break;
	case 0xC4:
	case 0xCC:
	case 0xD4:
	case 0xDC: { // CALL cc,nn
		uint16_t address = fetch16(gb);
		if (condition(cpu, op >> 3 & 3)) {
			tick(gb);
			push(gb, cpu->pc);
			cpu->pc = address;
		}
		break;
	}
	case 0xCD: { // CALL nn
		uint16_t address = fetch16(gb);
		tick(gb);
		push(gb, cpu->pc);
		cpu->pc = address;
		break;
	}
	case 0xC7:
	case 0xCF:
	case 0xD7:
	case 0xDF:
	case 0xE7:
	case 0xEF:
	case 0xF7:
	case 0xFF: // RST
		tick(gb);
		push(gb, cpu->pc);
		cpu->pc = op & 0x38;
		break;
	case 0xC6:
	case 0xCE:
	case 0xD6:
	case 0xDE:
	case 0xE6:
	case 0xEE:
	case 0xF6:
	case 0xFE: // ALU A,n
		alu(cpu, op >> 3 & 7, fetch(gb));
		break;
	case 0xCB:
		prefixed(gb);
		break;
	case 0xE0: // LDH (n),A
		write8(gb, 0xFF00 | fetch(gb), cpu->a);
		break;
	case 0xF0: // LDH A,(n)
		cpu->a = read8(gb, 0xFF00 | fetch(gb));
		break;
	case 0xE2: // LD (C),A
		write8(gb, 0xFF00 | cpu->c, cpu->a);
		break;
	case 0xF2: // LD A,(C)
		cpu->a = read8(gb, 0xFF00 | cpu->c);
		break;
	case 0xEA: // LD (nn),A
		write8(gb, fetch16(gb), cpu->a);
		break;
	case 0xFA: // LD A,(nn)
		cpu->a = read8(gb, fetch16(gb));
		break;
	case 0xE8: { // ADD SP,e
		uint8_t e = fetch(gb);
		cpu->sp = spOffset(cpu, e);
		tick(gb);
		tick(gb);
		break;
	}
	case 0xF8: { // LD HL,SP+e
		uint8_t e = fetch(gb);
		setHL(cpu, spOffset(cpu, e));
		tick(gb);
		break;
	}
	case 0xF9: // LD SP,HL
		cpu->sp = getHL(cpu);
		tick(gb);
		break;
	case 0xF3: // DI
		cpu->ime = false;
		cpu->imePending = false;
		break;
	case 0xFB: // EI
		cpu->imePending = true;
		gb->attention = true;
		break;
	case 0xD3:
	case 0xDB:
	case 0xDD:
	case 0xE3:
	case 0xE4:
	case 0xEB:
	case 0xEC:
	case 0xED:
	case 0xF4:
	case 0xFC:
	case 0xFD: // The unused opcodes lock the CPU up.
		cpu->locked = true;
		gb->attention = true;
		break;
	// LD r,r' and ALU A,r: a case each, so each knows its registers.
#define LD_R_R(op) \
	case op: \
		setR(gb, (op) >> 3 & 7, getR(gb, (op) & 7)); \
		break;
#define ALU_R(op) \
	case op: \
		alu(cpu, (op) >> 3 & 7, getR(gb, (op) & 7)); \
		break;
#define EIGHT(m, op) m(op) m(op + 1) m(op + 2) m(op + 3) m(op + 4) m(op + 5) m(op + 6) m(op + 7)
	EIGHT(LD_R_R, 0x40)
	EIGHT(LD_R_R, 0x48)
	EIGHT(LD_R_R, 0x50)
	EIGHT(LD_R_R, 0x58)
	EIGHT(LD_R_R, 0x60)
	EIGHT(LD_R_R, 0x68)
	LD_R_R(0x70) LD_R_R(0x71) LD_R_R(0x72) LD_R_R(0x73) LD_R_R(0x74) LD_R_R(0x75) LD_R_R(0x77)
	EIGHT(LD_R_R, 0x78)
	EIGHT(ALU_R, 0x80)
	EIGHT(ALU_R, 0x88)
	EIGHT(ALU_R, 0x90)
	EIGHT(ALU_R, 0x98)
	EIGHT(ALU_R, 0xA0)
	EIGHT(ALU_R, 0xA8)
	EIGHT(ALU_R, 0xB0)
	EIGHT(ALU_R, 0xB8)
#undef EIGHT
#undef ALU_R
#undef LD_R_R
	}
}

// Runs instructions until the time reaches until or V-blank starts, stopping for events as they come
// due.
void gbCpuRun(struct GB* gb, uint32_t until) {
	struct GBCpu* cpu = &gb->cpu;
	while (gbSince(until, gb->now) > 0 && !gb->frameEnded) {
		gb->attention = false;
		if (gbSince(gb->now, gb->nextEvent) >= 0) {
			gbRunEvents(gb);
		}
		if (cpu->stopped || cpu->locked) {
			idle(gb, until);
			continue;
		}
		if (cpu->halted && !wake(gb, until)) {
			continue;
		}
		uint8_t pending = gb->ie & gb->ifl & 0x1F;
		if (cpu->ime && pending) {
			dispatch(gb);
			continue;
		}
		if (cpu->imePending) {
			cpu->imePending = false;
			cpu->ime = true;
			gb->attention = true;
		}
		step(gb);
		// Then on without the checks above while nothing needs them and no event is due.
		while (!gb->attention && gbSince(until, gb->now) > 0 && gbSince(gb->nextEvent, gb->now) > 0) {
			step(gb);
		}
	}
}
