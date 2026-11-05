/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The LCD effects on the GPU, as RetroArch's armGBA presets draw them (shaders/): LCD Ghosting and
// Interlaced Rows at the console's resolution, from the picture before, then the grid, LCD Subpixels
// and the Game Boy pixel shadow at the size shown. They run in a GL context of their own, sharing
// textures with the renderer's, so SDL's own GL state is never touched: the picture goes into a
// texture there, and the last pass draws into an SDL texture that the renderer shows. Ghosting moves
// on once for each picture the core sends, however often the screen is drawn.

#include "lcd-gl.h"
#include "frontend.h"

#include <stdlib.h>
#include <string.h>

typedef unsigned GLuint;
typedef unsigned GLenum;
typedef int GLint;
typedef int GLsizei;
typedef char GLchar;
typedef float GLfloat;
typedef unsigned char GLboolean;
enum {
	GL_TRIANGLE_STRIP = 0x0005,
	GL_TEXTURE_2D = 0x0DE1,
	GL_UNSIGNED_BYTE = 0x1401,
	GL_FLOAT = 0x1406,
	GL_RGB = 0x1907,
	GL_RGBA = 0x1908,
	GL_UNPACK_ALIGNMENT = 0x0CF5,
	GL_NEAREST = 0x2600,
	GL_LINEAR = 0x2601,
	GL_TEXTURE_MAG_FILTER = 0x2800,
	GL_TEXTURE_MIN_FILTER = 0x2801,
	GL_TEXTURE_WRAP_S = 0x2802,
	GL_TEXTURE_WRAP_T = 0x2803,
	GL_CLAMP_TO_EDGE = 0x812F,
	GL_UNSIGNED_SHORT_5_6_5 = 0x8363,
	GL_TEXTURE0 = 0x84C0,
	GL_TEXTURE_BINDING_2D = 0x8069,
	GL_ARRAY_BUFFER = 0x8892,
	GL_FRAGMENT_SHADER = 0x8B30,
	GL_VERTEX_SHADER = 0x8B31,
	GL_COMPILE_STATUS = 0x8B81,
	GL_LINK_STATUS = 0x8B82,
	GL_FRAMEBUFFER = 0x8D40,
	GL_COLOR_ATTACHMENT0 = 0x8CE0,
	GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
};

static struct {
	GLuint (*CreateShader)(GLenum);
	void (*ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
	void (*CompileShader)(GLuint);
	void (*GetShaderiv)(GLuint, GLenum, GLint*);
	void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
	void (*DeleteShader)(GLuint);
	GLuint (*CreateProgram)(void);
	void (*AttachShader)(GLuint, GLuint);
	void (*BindAttribLocation)(GLuint, GLuint, const GLchar*);
	void (*LinkProgram)(GLuint);
	void (*GetProgramiv)(GLuint, GLenum, GLint*);
	void (*DeleteProgram)(GLuint);
	void (*UseProgram)(GLuint);
	GLint (*GetUniformLocation)(GLuint, const GLchar*);
	void (*Uniform1i)(GLint, GLint);
	void (*Uniform1f)(GLint, GLfloat);
	void (*Uniform2f)(GLint, GLfloat, GLfloat);
	void (*Uniform3f)(GLint, GLfloat, GLfloat, GLfloat);
	void (*GenTextures)(GLsizei, GLuint*);
	void (*DeleteTextures)(GLsizei, const GLuint*);
	void (*BindTexture)(GLenum, GLuint);
	void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
	void (*TexParameteri)(GLenum, GLenum, GLint);
	void (*ActiveTexture)(GLenum);
	void (*PixelStorei)(GLenum, GLint);
	void (*GenFramebuffers)(GLsizei, GLuint*);
	void (*DeleteFramebuffers)(GLsizei, const GLuint*);
	void (*BindFramebuffer)(GLenum, GLuint);
	void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
	GLenum (*CheckFramebufferStatus)(GLenum);
	void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
	void (*BindBuffer)(GLenum, GLuint);
	void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
	void (*EnableVertexAttribArray)(GLuint);
	void (*DrawArrays)(GLenum, GLint, GLsizei);
	void (*GetIntegerv)(GLenum, GLint*);
	void (*Finish)(void);
} gl;

static const char* const glNames[] = {
	"glCreateShader", "glShaderSource", "glCompileShader", "glGetShaderiv", "glGetShaderInfoLog", "glDeleteShader",
	"glCreateProgram", "glAttachShader", "glBindAttribLocation", "glLinkProgram", "glGetProgramiv", "glDeleteProgram",
	"glUseProgram", "glGetUniformLocation", "glUniform1i", "glUniform1f", "glUniform2f", "glUniform3f", "glGenTextures",
	"glDeleteTextures", "glBindTexture", "glTexImage2D", "glTexParameteri", "glActiveTexture", "glPixelStorei",
	"glGenFramebuffers", "glDeleteFramebuffers", "glBindFramebuffer", "glFramebufferTexture2D", "glCheckFramebufferStatus",
	"glViewport", "glBindBuffer", "glVertexAttribPointer", "glEnableVertexAttribArray", "glDrawArrays", "glGetIntegerv",
	"glFinish",
};

// Both passes draw one square over their target; v_at runs 0 to 1 across it, row 0 of a picture at 0.
static const char vertexSource[] =
	"attribute vec2 a_position;\n"
	"varying vec2 v_at;\n"
	"void main() {\n"
	"	gl_Position = vec4(a_position, 0.0, 1.0);\n"
	"	v_at = a_position * 0.5 + 0.5;\n"
	"}\n";

// armgba-lcd-fade.slang. The first picture, or one after the effects were off, starts from itself.
static const char fadeSource[] =
	"uniform sampler2D u_picture;\n"
	"uniform sampler2D u_before;\n"
	"uniform vec2 u_size;\n"
	"uniform float u_frame, u_ghosting, u_interlaced, u_darken, u_lighten, u_lead, u_fresh, u_bgr;\n"
	"varying vec2 v_at;\n"
	"void main() {\n"
	"	vec4 c = texture2D(u_picture, v_at);\n"
	"	vec3 now = u_bgr > 0.5 ? c.bgr : c.rgb;\n"
	"	vec3 before = texture2D(u_before, v_at).rgb;\n"
	"	bool interlaced = u_interlaced > 0.5;\n"
	"	if (u_fresh > 0.5 || (u_ghosting <= 0.0 && !interlaced)) {\n"
	"		gl_FragColor = vec4(now, 1.0);\n"
	"		return;\n"
	"	}\n"
	"	float row = floor(v_at.y * u_size.y);\n"
	"	bool weak = interlaced && mod(row + u_frame, 2.0) > 0.5;\n"
	"	if (u_ghosting <= 0.0) {\n"
	"		gl_FragColor = vec4(weak ? before : now, 1.0);\n"
	"		return;\n"
	"	}\n"
	"	float scale = !interlaced ? 1.0 : weak ? 1.0 / (2.0 - 1.0 / u_lead) : u_lead;\n"
	"	float frames = 16.743 / u_ghosting / scale;\n"
	"	float dark = 1.0 - exp(-frames / u_darken);\n"
	"	float light = 1.0 - exp(-frames / u_lighten);\n"
	"	vec3 was = floor(before * 255.0 + 0.5), want = floor(now * 255.0 + 0.5);\n"
	"	vec3 linearWas = pow(was / 255.0, vec3(2.2)), linearWant = pow(want / 255.0, vec3(2.2));\n"
	"	vec3 diff = linearWant - linearWas;\n"
	"	vec3 moved = linearWas + diff * mix(vec3(light), vec3(dark), 1.0 - step(vec3(0.0), diff));\n"
	"	vec3 next = floor(pow(moved, vec3(1.0 / 2.2)) * 255.0 + 0.5);\n"
	"	next += sign(want - was) * step(abs(next - was), vec3(0.0)) * step(0.5, abs(want - was));\n"
	"	gl_FragColor = vec4(next / 255.0, 1.0);\n"
	"}\n";

// armgba-lcd.slang, with the screen's values from lcd.h; smooth draws the picture filtered where
// there's no grid.
static const char gridSource[] =
	"uniform sampler2D u_picture;\n"
	"uniform vec2 u_size, u_shown;\n"
	"uniform float u_share, u_across, u_down, u_smooth;\n"
	"uniform vec3 u_gap, u_unlit;\n"
	"uniform float u_shadowStart, u_shadowScale;\n"
	"uniform vec2 u_shadowAt;\n"
	"uniform float u_subpixels, u_bgr;\n"
	"varying vec2 v_at;\n"
	"float tentBelow(float u) {\n"
	"	u = clamp(u, -1.0, 1.0);\n"
	"	return u < 0.0 ? (u + 1.0) * (u + 1.0) * 0.5 : 1.0 - (1.0 - u) * (1.0 - u) * 0.5;\n"
	"}\n"
	"float gapCoverage(float p, float size) {\n"
	"	float halfGap = u_share * 0.5;\n"
	"	float below = floor(p) - p;\n"
	"	float above = below + 1.0;\n"
	"	return min(tentBelow((below + halfGap) / size) - tentBelow((below - halfGap) / size) +\n"
	"	           tentBelow((above + halfGap) / size) - tentBelow((above - halfGap) / size), 1.0);\n"
	"}\n"
	"float stripeBelow(float u, float stripe) {\n"
	"	return floor(u) / 3.0 + clamp(fract(u) - stripe / 3.0, 0.0, 1.0 / 3.0);\n"
	"}\n"
	"vec3 toLinear(vec3 c) {\n"
	"	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));\n"
	"}\n"
	"vec3 fromLinear(vec3 l) {\n"
	"	return mix(l * 12.92, 1.055 * pow(l, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, l));\n"
	"}\n"
	"float level(vec3 c) {\n"
	"	return dot(toLinear(c), vec3(0.2126, 0.7152, 0.0722));\n"
	"}\n"
	"void main() {\n"
	"	vec2 at = v_at * u_size;\n"
	"	vec2 size = u_size / u_shown;\n"
	"	vec2 shown = floor((floor(at / size) + 0.5) * size + 0.001);\n"
	"	vec3 color = texture2D(u_picture, (shown + 0.5) / u_size).rgb;\n"
	"	if (u_smooth > 0.5) {\n"
	"		color = texture2D(u_picture, v_at).rgb;\n"
	"	}\n"
	"	float fade = clamp(0.1 / max(size.x, size.y), 0.0, 1.0);\n"
	"	if (u_share > 0.0) {\n"
	"		float across = gapCoverage(at.x, size.x) * u_across * fade;\n"
	"		float down = gapCoverage(at.y, size.y) * u_down * fade;\n"
	"		float covered = across + down - across * down;\n"
	"		float acrossAll = u_share * u_across * fade, downAll = u_share * u_down * fade;\n"
	"		float gapShown = max(acrossAll + downAll - acrossAll * downAll, 0.0001);\n"
	"		vec3 want = toLinear(color);\n"
	"		vec3 lit = toLinear(u_gap);\n"
	"		vec3 inside = (want - gapShown * lit) / (1.0 - gapShown);\n"
	"		lit = mix(lit, want / gapShown, step(inside, vec3(0.0)));\n"
	"		if (level(u_unlit) == 0.0) {\n"
	"			lit = mix(lit, (want - (1.0 - gapShown)) / gapShown, step(vec3(1.0), inside));\n"
	"		}\n"
	"		color = fromLinear(mix(clamp(inside, 0.0, 1.0), clamp(lit, 0.0, 1.0), covered));\n"
	"	}\n"
	"	if (u_subpixels > 0.0) {\n"
	"		float pitch = 1.0 / size.x;\n"
	"		float mixed = u_subpixels * clamp(pitch - 1.0, 0.0, 1.0);\n"
	"		float first = ceil(shown.x * pitch - 0.501);\n"
	"		float taken = max(ceil((shown.x + 1.0) * pitch - 0.501) - first, 1.0);\n"
	"		float column = floor(at.x * pitch) - first;\n"
	"		float from = column / taken, to = (column + 1.0) / taken;\n"
	"		float bands = clamp(floor(pitch + 0.001), 1.0, 16.0);\n"
	"		vec3 stripes = u_bgr > 0.5 ? vec3(2.0, 1.0, 0.0) : vec3(0.0, 1.0, 2.0);\n"
	"		vec3 share = vec3(0.0);\n"
	"		for (int i = 0; i < 16; ++i) {\n"
	"			float b = float(i);\n"
	"			if (b >= bands) {\n"
	"				break;\n"
	"			}\n"
	"			float lo = b / bands, hi = (b + 1.0) / bands;\n"
	"			float overlap = min(to, hi) - max(from, lo);\n"
	"			if (overlap > 0.0) {\n"
	"				vec3 below = vec3(stripeBelow(lo, stripes.r), stripeBelow(lo, stripes.g), stripeBelow(lo, stripes.b));\n"
	"				vec3 above = vec3(stripeBelow(hi, stripes.r), stripeBelow(hi, stripes.g), stripeBelow(hi, stripes.b));\n"
	"				share += overlap * bands * (above - below);\n"
	"			}\n"
	"		}\n"
	"		share /= to - from;\n"
	"		color = fromLinear(toLinear(color) * (1.0 - mixed + mixed * share));\n"
	"	}\n"
	"	if (u_shadowScale > 0.0) {\n"
	"		vec3 caster = texture2D(u_picture, (at - u_shadowAt) / u_size).rgb;\n"
	"		color *= 1.0 - clamp((u_shadowStart - level(caster)) * u_shadowScale, 0.0, 1.0);\n"
	"	}\n"
	"	gl_FragColor = vec4(color, 1.0);\n"
	"}\n";

enum { FADE, GRID, PROGRAMS };

static SDL_GLContext context, rendererContext;
static bool failed, es;
static GLuint programs[PROGRAMS], framebuffer;
// The picture as the core sent it, and the faded pictures, this one's and the one before.
static GLuint picture, faded[2];
static int pictureWidth, pictureHeight;
static enum retro_pixel_format pictureFormat;
static int fadedNow;
static bool fadedValid, pictureNew;
static unsigned pictures;
static uint8_t* packed;
// The SDL texture the grid pass draws into, at the size shown, and its GL name.
static SDL_Texture* screen;
static GLuint screenTexture;
static int screenWidth, screenHeight;

bool lcdGlUsable(void) {
	SDL_RendererInfo info;
	return !failed && app.renderer && SDL_GetRendererInfo(app.renderer, &info) == 0 && !strncmp(info.name, "opengl", 6);
}

void lcdGlNewPicture(void) {
	pictureNew = true;
}

static GLuint compile(GLenum kind, const char* body) {
	static const char desktop[] = "#version 110\n";
	static const char mobile[] =
		"#version 100\n#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n";
	const char* const parts[2] = { es ? mobile : desktop, body };
	GLuint const shader = gl.CreateShader(kind);
	gl.ShaderSource(shader, 2, parts, NULL);
	gl.CompileShader(shader);
	GLint ok = 0;
	gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024] = "";
		gl.GetShaderInfoLog(shader, sizeof(log), NULL, log);
		SDL_Log("LCD effects: a shader didn't compile: %s", log);
		gl.DeleteShader(shader);
		return 0;
	}
	return shader;
}

static GLuint link(const char* fragment) {
	GLuint const vertex = compile(GL_VERTEX_SHADER, vertexSource), pixels = compile(GL_FRAGMENT_SHADER, fragment);
	GLuint program = 0;
	if (vertex && pixels) {
		program = gl.CreateProgram();
		gl.AttachShader(program, vertex);
		gl.AttachShader(program, pixels);
		gl.BindAttribLocation(program, 0, "a_position");
		gl.LinkProgram(program);
		GLint ok = 0;
		gl.GetProgramiv(program, GL_LINK_STATUS, &ok);
		if (!ok) {
			gl.DeleteProgram(program);
			program = 0;
		}
	}
	if (vertex) {
		gl.DeleteShader(vertex);
	}
	if (pixels) {
		gl.DeleteShader(pixels);
	}
	return program;
}

static GLuint makeTexture(int w, int h) {
	GLuint t = 0;
	gl.GenTextures(1, &t);
	gl.BindTexture(GL_TEXTURE_2D, t);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	if (w) {
		gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	}
	return t;
}

// Without the GPU's effects, the core draws them again.
static bool giveUp(void) {
	failed = true;
	optionsChanged = true;
	return false;
}

// The context, sharing the renderer's, and the programs; the renderer's context is current again
// after. Once it has failed on a renderer, it isn't tried again until the renderer's made again.
static bool start(void) {
	if (context) {
		return true;
	}
	SDL_RendererInfo info;
	SDL_GetRendererInfo(app.renderer, &info);
	es = !strncmp(info.name, "opengles", 8);
	// The renderer's context is current once its commands so far have run.
	SDL_RenderFlush(app.renderer);
	rendererContext = SDL_GL_GetCurrentContext();
	if (!rendererContext) {
		return giveUp();
	}
	SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
	if (es) {
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	}
	context = SDL_GL_CreateContext(app.window);
	SDL_GL_ResetAttributes();
	if (!context) {
		SDL_Log("LCD effects: no GL context: %s", SDL_GetError());
		SDL_GL_MakeCurrent(app.window, rendererContext);
		return giveUp();
	}
	void** const functions = (void**) &gl;
	bool found = true;
	for (size_t i = 0; i < sizeof(glNames) / sizeof(*glNames); ++i) {
		functions[i] = SDL_GL_GetProcAddress(glNames[i]);
		found &= functions[i] != NULL;
	}
	if (found) {
		programs[FADE] = link(fadeSource);
		programs[GRID] = link(gridSource);
		gl.GenFramebuffers(1, &framebuffer);
	}
	if (!found || !programs[FADE] || !programs[GRID]) {
		SDL_GL_MakeCurrent(app.window, rendererContext);
		lcdGlStop();
		return giveUp();
	}
	SDL_GL_MakeCurrent(app.window, rendererContext);
	return true;
}

// The core's picture into its texture: RGB565 as it is, XRGB8888 as bytes, blue first, which the
// fade pass turns round.
static void uploadPicture(void) {
	int const w = app.gameWidth, h = app.gameHeight;
	bool const wide = app.format == RETRO_PIXEL_FORMAT_XRGB8888;
	size_t const row = (size_t) w * (wide ? 4 : 2);
	if (!picture || w != pictureWidth || h != pictureHeight || app.format != pictureFormat) {
		if (!picture) {
			picture = makeTexture(0, 0);
		}
		gl.BindTexture(GL_TEXTURE_2D, picture);
		gl.TexImage2D(GL_TEXTURE_2D, 0, wide ? GL_RGBA : GL_RGB, w, h, 0, wide ? GL_RGBA : GL_RGB, wide ? GL_UNSIGNED_BYTE : GL_UNSIGNED_SHORT_5_6_5, NULL);
		pictureWidth = w;
		pictureHeight = h;
		pictureFormat = app.format;
		for (int i = 0; i < 2; ++i) {
			if (faded[i]) {
				gl.DeleteTextures(1, &faded[i]);
			}
			faded[i] = makeTexture(w, h);
		}
		fadedValid = false;
		free(packed);
		packed = NULL;
	}
	// GLES 2 has no row length, so a picture with spare bytes on its rows is packed first.
	const uint8_t* data = app.picture;
	if (app.picturePitch != row) {
		if (!packed) {
			packed = malloc(row * h);
		}
		if (!packed) {
			return;
		}
		for (int y = 0; y < h; ++y) {
			memcpy(packed + row * y, app.picture + app.picturePitch * y, row);
		}
		data = packed;
	}
	gl.BindTexture(GL_TEXTURE_2D, picture);
	gl.PixelStorei(GL_UNPACK_ALIGNMENT, wide ? 4 : 2);
	gl.TexImage2D(GL_TEXTURE_2D, 0, wide ? GL_RGBA : GL_RGB, w, h, 0, wide ? GL_RGBA : GL_RGB, wide ? GL_UNSIGNED_BYTE : GL_UNSIGNED_SHORT_5_6_5, data);
}

static void drawSquare(GLuint target, int w, int h) {
	static const GLfloat corners[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
	gl.Viewport(0, 0, w, h);
	gl.BindBuffer(GL_ARRAY_BUFFER, 0);
	gl.VertexAttribPointer(0, 2, GL_FLOAT, 0, 0, corners);
	gl.EnableVertexAttribArray(0);
	gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void uniform1(GLuint program, const char* name, float value) {
	gl.Uniform1f(gl.GetUniformLocation(program, name), value);
}

static void uniformColor(GLuint program, const char* name, uint32_t c) {
	gl.Uniform3f(gl.GetUniformLocation(program, name), (c >> 16 & 0xFF) / 255.0f, (c >> 8 & 0xFF) / 255.0f, (c & 0xFF) / 255.0f);
}

// LCD Ghosting and Interlaced Rows: the new picture against the last one shown, into the other faded
// texture. Off, the picture's own texture goes on, and the next time they're on starts afresh.
static GLuint fade(const struct LcdGlLook* look) {
	bool const on = look->ghosting || look->interlaced;
	if (!on) {
		fadedValid = false;
		return picture;
	}
	if (!pictureNew && fadedValid) {
		return faded[fadedNow];
	}
	int const next = !fadedNow;
	GLuint const p = programs[FADE];
	gl.UseProgram(p);
	gl.ActiveTexture(GL_TEXTURE0);
	gl.BindTexture(GL_TEXTURE_2D, picture);
	gl.ActiveTexture(GL_TEXTURE0 + 1);
	gl.BindTexture(GL_TEXTURE_2D, faded[fadedNow]);
	gl.Uniform1i(gl.GetUniformLocation(p, "u_picture"), 0);
	gl.Uniform1i(gl.GetUniformLocation(p, "u_before"), 1);
	gl.Uniform2f(gl.GetUniformLocation(p, "u_size"), (float) pictureWidth, (float) pictureHeight);
	uniform1(p, "u_frame", (float) (pictures & 1));
	uniform1(p, "u_ghosting", look->ghosting / 4.0f);
	uniform1(p, "u_interlaced", look->interlaced);
	uniform1(p, "u_darken", look->darken);
	uniform1(p, "u_lighten", look->lighten);
	uniform1(p, "u_lead", look->lead);
	uniform1(p, "u_fresh", !fadedValid);
	uniform1(p, "u_bgr", pictureFormat == RETRO_PIXEL_FORMAT_XRGB8888);
	drawSquare(faded[next], pictureWidth, pictureHeight);
	gl.ActiveTexture(GL_TEXTURE0);
	fadedNow = next;
	fadedValid = true;
	return faded[fadedNow];
}

// The SDL texture the grid pass draws into, made at the size shown, and its GL name, found with the
// renderer's context current.
static bool makeScreen(int w, int h) {
	if (screen && w == screenWidth && h == screenHeight) {
		return true;
	}
	if (screen) {
		SDL_DestroyTexture(screen);
	}
	screen = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, w, h);
	screenWidth = w;
	screenHeight = h;
	screenTexture = 0;
	float tw = 0, th = 0;
	if (!screen || SDL_GL_BindTexture(screen, &tw, &th) != 0) {
		return false;
	}
	GLint name = 0;
	gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &name);
	SDL_GL_UnbindTexture(screen);
	// A rectangle or padded texture can't be drawn into here.
	screenTexture = tw == 1 && th == 1 ? (GLuint) name : 0;
	return screenTexture != 0;
}

bool lcdGlDraw(SDL_Rect to, const struct LcdGlLook* look) {
	if (!app.picture || !app.gameWidth || to.w <= 0 || to.h <= 0 || !lcdGlUsable() || !start()) {
		return false;
	}
	if (!makeScreen(to.w, to.h)) {
		return false;
	}
	SDL_RenderFlush(app.renderer);
	SDL_GL_MakeCurrent(app.window, context);
	if (pictureNew || !picture || app.gameWidth != pictureWidth || app.gameHeight != pictureHeight || app.format != pictureFormat) {
		uploadPicture();
	}
	GLuint const source = fade(look);
	if (pictureNew) {
		++pictures;
		pictureNew = false;
	}
	GLuint const p = programs[GRID];
	gl.UseProgram(p);
	gl.ActiveTexture(GL_TEXTURE0);
	gl.BindTexture(GL_TEXTURE_2D, source);
	gl.Uniform1i(gl.GetUniformLocation(p, "u_picture"), 0);
	gl.Uniform2f(gl.GetUniformLocation(p, "u_size"), (float) pictureWidth, (float) pictureHeight);
	gl.Uniform2f(gl.GetUniformLocation(p, "u_shown"), (float) to.w, (float) to.h);
	uniform1(p, "u_share", look->gaps ? look->share : 0);
	uniform1(p, "u_across", look->across);
	uniform1(p, "u_down", look->down);
	uniform1(p, "u_smooth", look->smooth && !look->gaps && !look->subpixels && !look->shadowScale);
	uniformColor(p, "u_gap", look->gap);
	uniformColor(p, "u_unlit", look->unlit);
	uniform1(p, "u_shadowStart", look->shadowStart);
	uniform1(p, "u_shadowScale", look->shadowScale);
	gl.Uniform2f(gl.GetUniformLocation(p, "u_shadowAt"), look->shadowX, look->shadowY);
	uniform1(p, "u_subpixels", look->subpixels);
	uniform1(p, "u_bgr", look->bgr);
	// The faded picture's own row 0 is drawn first, at the texture's start, as SDL draws it at the top.
	drawSquare(screenTexture, to.w, to.h);
	gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
	gl.Finish();
	SDL_GL_MakeCurrent(app.window, rendererContext);
	SDL_RenderCopy(app.renderer, screen, NULL, &to);
	return true;
}

void lcdGlStop(void) {
	if (screen) {
		SDL_DestroyTexture(screen);
		screen = NULL;
	}
	screenTexture = 0;
	screenWidth = screenHeight = 0;
	if (context) {
		SDL_GLContext const current = SDL_GL_GetCurrentContext();
		SDL_GL_MakeCurrent(app.window, context);
		for (int i = 0; i < PROGRAMS; ++i) {
			if (programs[i]) {
				gl.DeleteProgram(programs[i]);
			}
			programs[i] = 0;
		}
		GLuint textures[3] = { picture, faded[0], faded[1] };
		for (int i = 0; i < 3; ++i) {
			if (textures[i]) {
				gl.DeleteTextures(1, &textures[i]);
			}
		}
		if (framebuffer) {
			gl.DeleteFramebuffers(1, &framebuffer);
		}
		SDL_GL_MakeCurrent(app.window, current != context ? current : NULL);
		SDL_GL_DeleteContext(context);
	}
	context = rendererContext = NULL;
	picture = faded[0] = faded[1] = framebuffer = 0;
	pictureWidth = pictureHeight = 0;
	fadedValid = false;
	free(packed);
	packed = NULL;
	// A renderer made again gets another try.
	failed = false;
}
