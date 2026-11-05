#version 450

// Copyright (c) 2026 Magnominus
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// armGBA's LCD Ghosting and Interlaced Rows for RetroArch, drawn as the core draws them on the CPU
// (libretro/lcd.c), at the console's resolution, from the last frame shown. Ghosting moves each
// subpixel in linear light from its last level towards the new picture by a share each frame, faster
// as it darkens than as it lightens, with the screen's time constants; 0.25 to 0.75 take that share
// of them. Levels stay 8-bit, gamma encoded, as on the CPU, and a level that would round back to
// where it was takes one step. Interlaced Rows, on the Color's and Advance's screens, drives alternate
// rows each frame: alone, the other rows keep what they showed; with ghosting, the driven rows move
// with their times shortened to Driven Rows' Share and the others lengthened, so that over two frames
// every row moves as far as without it.

layout(push_constant) uniform Push {
	vec4 SourceSize;
	uint FrameCount;
	float GHOSTING;
	float INTERLACED;
	float GHOST_DARKEN;
	float GHOST_LIGHTEN;
	float DRIVEN_SHARE;
} params;

#pragma parameter GHOSTING "LCD Ghosting (0 Off, 0.25 to 1)" 0.0 0.0 1.0 0.25
#pragma parameter INTERLACED "Interlaced Rows (Color and Advance)" 0.0 0.0 1.0 1.0
#pragma parameter GHOST_DARKEN "Ghosting: Time to Darken (ms)" 15.0 5.0 200.0 1.0
#pragma parameter GHOST_LIGHTEN "Ghosting: Time to Lighten (ms)" 30.0 5.0 200.0 1.0
#pragma parameter DRIVEN_SHARE "Interlaced Rows: Driven Rows' Share of the Time" 0.8 0.5 1.0 0.05

layout(std140, set = 0, binding = 0) uniform UBO {
	mat4 MVP;
} global;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;

void main() {
	gl_Position = global.MVP * Position;
	vTexCoord = TexCoord;
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;
layout(set = 0, binding = 3) uniform sampler2D PassFeedback0;

void main() {
	vec3 now = texture(Source, vTexCoord).rgb;
	vec3 before = texture(PassFeedback0, vTexCoord).rgb;
	bool interlaced = params.INTERLACED > 0.5;
	if (params.FrameCount < 2u || (params.GHOSTING <= 0.0 && !interlaced)) {
		FragColor = vec4(now, 1.0);
		return;
	}
	float row = floor(vTexCoord.y * params.SourceSize.y);
	bool weak = interlaced && mod(row + float(params.FrameCount), 2.0) > 0.5;
	if (params.GHOSTING <= 0.0) {
		FragColor = vec4(weak ? before : now, 1.0);
		return;
	}
	// A frame lasts 16.743 ms; Driven Rows' Share and the others' add up to two plain frames' rates.
	float lead = params.DRIVEN_SHARE;
	float scale = !interlaced ? 1.0 : weak ? 1.0 / (2.0 - 1.0 / lead) : lead;
	float frames = 16.743 / params.GHOSTING / scale;
	float dark = 1.0 - exp(-frames / params.GHOST_DARKEN);
	float light = 1.0 - exp(-frames / params.GHOST_LIGHTEN);
	vec3 was = floor(before * 255.0 + 0.5), want = floor(now * 255.0 + 0.5);
	vec3 linearWas = pow(was / 255.0, vec3(2.2)), linearWant = pow(want / 255.0, vec3(2.2));
	vec3 diff = linearWant - linearWas;
	vec3 moved = linearWas + diff * mix(vec3(light), vec3(dark), 1.0 - step(vec3(0.0), diff));
	vec3 next = floor(pow(moved, vec3(1.0 / 2.2)) * 255.0 + 0.5);
	next += sign(want - was) * step(abs(next - was), vec3(0.0)) * step(0.5, abs(want - was));
	FragColor = vec4(next / 255.0, 1.0);
}
