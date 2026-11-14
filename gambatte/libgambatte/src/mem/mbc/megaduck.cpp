//
//   Copyright (c) 2026 Magnominus
//
//   This program is free software; you can redistribute it and/or modify
//   it under the terms of the GNU General Public License version 2 as
//   published by the Free Software Foundation.
//
//   This program is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//   GNU General Public License version 2 for more details.
//

#include "gbint.h"
#include "mbc.h"

// armGBA: the Mega Duck's cartridges (as SameDuck has them). Two ways to change banks, a game using
// one or the other: a write to 0001 picks the bank at 4000-7FFF, and a write to B000 picks a 32 KiB
// bank for the whole of 0000-7FFF. There's no cartridge RAM.

namespace gambatte {

MegaDuck::MegaDuck(MemPtrs &memptrs)
: memptrs_(memptrs)
, rombank0_(0)
, rombank_(1)
{
	setBanks();
}

bool MegaDuck::disabledRam() const {
	return true;
}

void MegaDuck::romWrite(unsigned const p, unsigned const data, gb_ulong const /*cc*/) {
	if (p == 0x0001) {
		rombank_ = data;
		setBanks();
	}
}

unsigned MegaDuck::read(unsigned, gb_ulong) const {
	return 0xFF;
}

void MegaDuck::write(unsigned const p, unsigned const data, gb_ulong) {
	if (p == 0xB000) {
		rombank0_ = data * 2;
		rombank_ = data * 2 + 1;
		setBanks();
	}
}

void MegaDuck::saveState(SaveState::Mem &ss) const {
	ss.rombank = rombank_;
	ss.rambank = rombank0_;
	ss.enableRam = false;
}

void MegaDuck::loadState(SaveState::Mem const &ss) {
	rombank_ = ss.rombank;
	rombank0_ = ss.rambank;
	setBanks();
}

bool MegaDuck::isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const {
	return addr < rombank_size() ? bank == rombank0_ : true;
}

void MegaDuck::SyncState(NewState *ns, bool isReader) {
	NSS(rombank0_);
	NSS(rombank_);
}

void MegaDuck::setBanks() const {
	unsigned const mask = rombanks(memptrs_) - 1;
	// A000-BFFF goes to read() and write(), as the B000 register is there
	memptrs_.setRambank(MemPtrs::read_en | MemPtrs::write_en | MemPtrs::rtc_en, 0);
	memptrs_.setRombank0(rombank0_ & mask);
	memptrs_.setRombank(rombank_ & mask);
}

}
