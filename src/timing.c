/* Copyright (c) 2013-2016 Jeffrey Pfau
 * Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba.h"

#include <limits.h>

// The same ordering rules as mGBA's mTiming: events due at the same time run in priority order.
//
// With cpu.overclock set, the CPU runs on a clock of its own, (4 + overclock) / 4 times the
// console's, as MAME's CPU overclock does. Event times stay on the console's clock and the cycle
// counter is on the CPU's. Both start together at reset, so a time on one is found on the other
// from the CPU cycles since then (globalCycles).

static int quarters(struct GBA* gba) {
	return 4 + gba->cpu.overclock;
}

// The console's time at a CPU time, and the first CPU time at or after a console time.
static uint64_t consoleAt(struct GBA* gba, uint64_t cpu) {
	return cpu * 4 / quarters(gba);
}

static uint64_t cpuAt(struct GBA* gba, uint64_t console) {
	return (console * quarters(gba) + 3) / 4;
}

// An event's time on the console's clock, as the one nearest the clock.
static uint64_t eventTime(struct GBA* gba, uint32_t when) {
	uint64_t now = consoleAt(gba, gba->timing.globalCycles);
	return now + (int32_t) (when - (uint32_t) now);
}

// CPU cycles from the clock, before the cycle counter, to an event.
static int32_t untilEvent(struct GBA* gba, struct Event* event) {
	if (!gba->cpu.overclock) {
		return event->when - gba->timing.masterCycles;
	}
	return cpuAt(gba, eventTime(gba, event->when)) - gba->timing.globalCycles;
}

// The console's cycles since reset.
uint64_t timingConsoleCycles(struct GBA* gba) {
	if (!gba->cpu.overclock) {
		return gba->timing.globalCycles + (uint32_t) gba->cpu.cycles;
	}
	return consoleAt(gba, gba->timing.globalCycles + gba->cpu.cycles);
}

// The CPU's cycles since reset at a time on the console's clock.
uint64_t timingCpuCycles(struct GBA* gba, uint64_t console) {
	return cpuAt(gba, console);
}

// The CPU's time at a console time near now, for the times compiled code compares its clock with.
uint32_t timingCpuAt(struct GBA* gba, uint32_t time) {
	if (!gba->cpu.overclock) {
		return time;
	}
	uint64_t now = timingConsoleCycles(gba);
	return cpuAt(gba, now + (int32_t) (time - (uint32_t) now));
}

// No events, and the clock at zero.
void timingClear(struct GBA* gba) {
	gba->timing.root = NULL;
	gba->timing.reroot = NULL;
	gba->timing.masterCycles = 0;
	gba->timing.globalCycles = 0;
}

// Events are a list in time order, and by priority where times are equal. when is in cycles from
// now.
void timingSchedule(struct GBA* gba, struct Event* event, int32_t when) {
	struct Timing* timing = &gba->timing;
	int32_t nextEvent;
	if (gba->cpu.overclock) {
		uint64_t at = timingConsoleCycles(gba) + when;
		event->when = at;
		nextEvent = cpuAt(gba, at) - timing->globalCycles;
	} else {
		nextEvent = when + gba->cpu.cycles;
		event->when = nextEvent + timing->masterCycles;
	}
	if (nextEvent < gba->cpu.nextEvent) {
		gba->cpu.nextEvent = nextEvent;
	}
	struct Event** previous = timing->reroot ? &timing->reroot : &timing->root;
	struct Event* next = *previous;
	while (next) {
		int32_t after = next->when - event->when;
		if (after > 0 || (after == 0 && next->priority > event->priority)) {
			break;
		}
		previous = &next->next;
		next = next->next;
	}
	event->next = next;
	*previous = event;
}

// The same, with when on the clock itself.
void timingScheduleAbsolute(struct GBA* gba, struct Event* event, int32_t when) {
	timingSchedule(gba, event, when - timingCurrentTime(gba));
}

// The same, cycles of the CPU's clock from now, late of the console's already gone: with the CPU Clock
// away from 100%, at the last of the console's cycles not after them, for what is on the bus the CPU
// runs on (DMA's start, the interrupt controller) and so must not come later than it would.
void timingScheduleCpu(struct GBA* gba, struct Event* event, int32_t cycles, int32_t late) {
	if (!gba->cpu.overclock) {
		timingSchedule(gba, event, cycles - late);
		return;
	}
	uint64_t const cpu = gba->timing.globalCycles + gba->cpu.cycles;
	timingSchedule(gba, event, (int32_t) (consoleAt(gba, cpu + cycles) - consoleAt(gba, cpu)) - late);
}

// Takes an event off the list if it is on it.
void timingDeschedule(struct GBA* gba, struct Event* event) {
	struct Timing* timing = &gba->timing;
	struct Event** previous = timing->reroot ? &timing->reroot : &timing->root;
	struct Event* next = *previous;
	while (next) {
		if (next == event) {
			*previous = next->next;
			return;
		}
		previous = &next->next;
		next = next->next;
	}
}

// Whether an event is waiting.
bool timingIsScheduled(struct GBA* gba, struct Event* event) {
	struct Event* next = gba->timing.root ? gba->timing.root : gba->timing.reroot;
	for (; next; next = next->next) {
		if (next == event) {
			return true;
		}
	}
	return false;
}

// Returns the cycles until the first event, as the CPU's cycle counter counts them.
int32_t timingNextEvent(struct GBA* gba) {
	struct Event* next = gba->timing.root;
	if (!next) {
		return INT_MAX;
	}
	return untilEvent(gba, next) - gba->cpu.cycles;
}

// Returns the console's cycles from now until a scheduled event.
int32_t timingUntil(struct GBA* gba, struct Event* event) {
	if (gba->cpu.overclock) {
		return eventTime(gba, event->when) - timingConsoleCycles(gba);
	}
	return event->when - gba->timing.masterCycles - gba->cpu.cycles;
}

// Makes the event loop return after the event running now, keeping the rest for the next call.
void timingInterrupt(struct GBA* gba) {
	if (!gba->timing.root) {
		return;
	}
	gba->timing.reroot = gba->timing.root;
	gba->timing.root = NULL;
}

// Moves the clock on by cycles and runs every event due, each told how late it is in the console's
// cycles. Returns the cycles until the next one.
int32_t timingTick(struct GBA* gba, int32_t cycles) {
	struct Timing* timing = &gba->timing;
	timing->masterCycles += cycles;
	timing->globalCycles += cycles;
	while (timing->root) {
		struct Event* next = timing->root;
		int32_t nextWhen = untilEvent(gba, next);
		if (nextWhen > 0) {
			return nextWhen;
		}
		timing->root = next->next;
		int32_t late = -nextWhen;
		if (gba->cpu.overclock) {
			late = consoleAt(gba, timing->globalCycles) - eventTime(gba, next->when);
		}
		next->callback(gba, next->context, late);
	}
	if (timing->reroot) {
		timing->root = timing->reroot;
		timing->reroot = NULL;
		gba->cpu.nextEvent = timingNextEvent(gba);
	}
	return gba->cpu.nextEvent;
}

bool timingDeferrable(struct GBA* gba, int32_t time) {
	for (struct Event* event = gba->timing.root; event; event = event->next) {
		if (untilEvent(gba, event) > time) {
			break;
		}
		if (event->endsRun || dmaStartEvent(event) || !videoDeferrable(gba, event) || !timerDeferrable(gba, event)) {
			return false;
		}
	}
	return true;
}

// After compiled code moved nextEvent up to leave early.
void timingResetNextEvent(struct GBA* gba) {
	struct Event* next = gba->timing.root;
	gba->cpu.nextEvent = next ? untilEvent(gba, next) : INT_MAX;
}

// Called when the CPU's cycle counter reaches nextEvent: the counter is folded into the clock, the
// due events run, and a halted CPU is moved straight to the next event.
void processEvents(struct GBA* gba) {
	struct CPU* cpu = &gba->cpu;
	++gba->eventRuns;
	int32_t nextEvent = cpu->nextEvent;
	while (cpu->cycles >= nextEvent) {
		cpu->nextEvent = INT_MAX;
		nextEvent = 0;
		int32_t cycles = cpu->cycles;
		cpu->cycles = 0;
		nextEvent = timingTick(gba, cycles < nextEvent ? nextEvent : cycles);
		cpu->nextEvent = nextEvent;
		// While halted, the CPU sleeps until IE & IF, whatever IME and the I flag say; waking takes
		// a cycle. DMA still runs. A sleeping CPU moves on to the next event, unless an event asked
		// for control back: the console then stops where that event ran.
		if (cpu->halted && !gba->performingDma) {
			if (gba->irq.available) {
				cpu->halted = 0;
				cpu->cycles += 1;
			} else if (gba->dmaRunnable) {
				dmaRun(gba);
			} else if (!gba->returnRequested) {
				cpu->cycles = nextEvent;
			}
		}
		if (gba->earlyExit || gba->returnRequested) {
			// Asked for control back: compiled code looks at events again at once, and leaves.
			if (gba->returnRequested) {
				cpu->nextEvent = cpu->cycles;
			}
			break;
		}
	}
	gba->earlyExit = false;
}
