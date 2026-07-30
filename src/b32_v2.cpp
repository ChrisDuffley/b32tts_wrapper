// Support for the "v2" language dlls (dll_eng.dll, dll_rus.dll etc), stripped down 2006
// builds of BeSTspeech that shipped with Lingvosoft Talking Dictionary products. Thanks to
// @rommix0 for preserving these and for the original Init_TTS/Say_TTS/DeInit_TTS definitions.
//
// These dlls differ from the classic 1994 b32_tts.dll in a few important ways:
// * They export only Init_TTS, DeInit_TTS and Say_TTS. There is no bstSetParams, so rate
//   and gain must be applied with inline tilde commands (which the text frontend still
//   parses) or by post-processing the captured audio.
// * Say_TTS takes wide character text, which is what makes the non English languages work.
// * Each utterance is synthesized into a single buffer, played via one waveOutWrite, after
//   which the dll blindly sleeps out the audio's real time duration before returning. The
//   wrapper's Sleep hook (see b32_wrapper.cpp) skips that nap so synthesis is instant.
// * Output is 16 bit mono like the classic engine. Most dlls declare 10000hz formats,
//   which renders them audibly deep; formant alignment against the classic engine and
//   the Russian dll's honest 10800 declaration both point to 10800hz as the family's
//   true rate, so the wrapper overrides all v2 output to 10800 (see waveOutOpenHook).
// This is released into the public domain.

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "b32_state.h"

// Set the B32_DEBUG environment variable to trace chunking decisions on stderr.
static bool bst_v2_debug() {
	static int cached = -1;
	if (cached < 0) cached = getenv("B32_DEBUG")? 1 : 0;
	return cached == 1;
}

bool bst_v2_setup(bst_state* s) {
	s->v2_init = (v2InitFunc)GetProcAddress(s->dll, "Init_TTS");
	s->v2_deinit = (v2DeInitFunc)GetProcAddress(s->dll, "DeInit_TTS");
	s->v2_say = (v2SayFunc)GetProcAddress(s->dll, "Say_TTS");
	if (!s->v2_init || !s->v2_say) return false;
	s->is_v2 = true;
	s->sample_rate = 10000;
	s->v2_init();
	return true;
}

// The v2 dlls have a fixed internal text buffer and crash outright on long input:
// measured limits are 252 characters for the European builds (a 256 byte buffer) and
// ~580 for Japanese. Text is therefore split into chunks below the smallest limit at
// sentence or word boundaries. Inline tilde commands reset on every Say_TTS call
// (verified: identical audio before and after a commanded utterance), so the leading
// command run is parsed off and re-applied to every chunk.
//
// They also have a fixed 128 byte PHRASE buffer: the frontend accumulates text between
// punctuation marks to shape prosody, and a punctuation-free stretch that overflows it
// is dropped in its entirety without a sound (measured: 122 chars speaks, 133 is
// silent). Mastodon handles and other long unpunctuated runs hit this constantly, and
// a dot only counts as punctuation when whitespace follows it, so "user.example.com"
// does not reset the counter. Chunk cuts are therefore also forced so that no chunk
// contains a punctuation-free run longer than V2_PHRASE_LIMIT; the engine flushes its
// phrase buffer at end of input, so a mid-sentence chunk end acts as a phrase break.
#define V2_CHUNK_LIMIT 240
#define V2_PHRASE_LIMIT 100

static bool bst_v2_is_break(wchar_t c) {
	return c == L'.' || c == L'!' || c == L'?' || c == L'\n' || c == 0x3002 || c == 0xFF01 || c == 0xFF1F;
}

static bool bst_v2_is_space(wchar_t c) {
	return c == L' ' || c == L'\t' || c == L'\n';
}

// A phrase break the engine actually honors: sentence punctuation or comma, but only
// when followed by whitespace or end of text.
static bool bst_v2_phrase_break_at(const wchar_t* text, int len, int i) {
	wchar_t c = text[i];
	bool punct = bst_v2_is_break(c) || c == L',' || c == 0x3001 || c == 0xFF0C;
	if (!punct) return false;
	return i + 1 >= len || bst_v2_is_space(text[i + 1]);
}

void bst_v2_speak(bst_state* s, const char* utf8_text) {
	int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_text, -1, nullptr, 0);
	if (wlen <= 0) return;
	wchar_t* wtext = (wchar_t*)malloc(wlen * sizeof(wchar_t));
	if (!wtext) return;
	MultiByteToWideChar(CP_UTF8, 0, utf8_text, -1, wtext, wlen);
	int total = wlen - 1; // sans terminator
	// Parse the leading run of ~...] commands; it becomes each chunk's prefix.
	int prefix_len = 0;
	while (prefix_len < total && wtext[prefix_len] == L'~') {
		int j = prefix_len + 1;
		while (j < total && wtext[j] != L']') j++;
		if (j >= total) break; // unterminated; treat as content
		prefix_len = j + 1;
	}
	wchar_t* content = wtext + prefix_len;
	int content_len = total - prefix_len;
	int window = V2_CHUNK_LIMIT - prefix_len;
	if (window < 40) window = 40; // degenerate prefix; better to risk a long chunk than emit confetti
	if (total <= V2_CHUNK_LIMIT) {
		s->v2_say(wtext);
		free(wtext);
		return;
	}
	wchar_t* chunk = (wchar_t*)malloc((V2_CHUNK_LIMIT + window + 1) * sizeof(wchar_t));
	if (!chunk) {
		free(wtext);
		return;
	}
	int pos = 0;
	while (pos < content_len && !s->async_stop_speaking) {
		int remain = content_len - pos;
		int take = remain <= window? remain : window;
		// First pass: force a cut before any punctuation-free run can overflow the
		// engine's phrase buffer. Runs reset at honored phrase breaks; a cut lands at
		// the last space inside the overlong run (hard mid-word only as a last resort).
		{
			int run = 0, run_space = -1, forced = -1;
			for (int i = 0; i < take; i++) {
				if (bst_v2_phrase_break_at(content + pos, remain, i)) {
					run = 0;
					run_space = -1;
					continue;
				}
				if (bst_v2_is_space(content[pos + i])) run_space = i;
				run++;
				if (run >= V2_PHRASE_LIMIT) {
					forced = run_space > 0? run_space + 1 : i;
					break;
				}
			}
			if (forced > 0 && forced < take) take = forced;
		}
		if (take < remain) {
			// Prefer to break after sentence punctuation, else at whitespace.
			int cut = -1;
			for (int i = take - 1; i > take / 4; i--) {
				if (bst_v2_is_break(content[pos + i])) { cut = i + 1; break; }
			}
			if (cut < 0) {
				for (int i = take - 1; i > take / 4; i--) {
					wchar_t c = content[pos + i];
					if (c == L' ' || c == L'\t' || c == 0x3001 || c == 0xFF0C) { cut = i + 1; break; }
				}
			}
			if (cut > 0) take = cut;
			else {
				// Hard cut: never split inside a mid-text ~...] command.
				int last_tilde = -1, last_bracket = -1;
				for (int i = 0; i < take; i++) {
					if (content[pos + i] == L'~') last_tilde = i;
					else if (content[pos + i] == L']') last_bracket = i;
				}
				if (last_tilde > last_bracket && last_tilde > 0) take = last_tilde;
			}
		}
		memcpy(chunk, wtext, prefix_len * sizeof(wchar_t));
		memcpy(chunk + prefix_len, content + pos, take * sizeof(wchar_t));
		chunk[prefix_len + take] = 0;
		if (bst_v2_debug()) fwprintf(stderr, L"[v2chunk] pos=%d take=%d stop=%d text=%.48s\n", pos, take, (int)s->async_stop_speaking, chunk + prefix_len);
		s->v2_say(chunk);
		if (bst_v2_debug()) fwprintf(stderr, L"[v2chunk] said, stop=%d\n", (int)s->async_stop_speaking);
		pos += take;
	}
	free(chunk);
	free(wtext);
}

void bst_v2_close(bst_state* s) {
	if (s->v2_deinit) s->v2_deinit();
}
