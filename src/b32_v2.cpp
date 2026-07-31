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

// Text limits vary per build; all were measured by length ladders against each dll
// (durations flatten at truncation, drop to zero at a whole-phrase drop, and the
// process dies past the hard buffer end around 256). Values here sit safely under the
// worst measured ceiling for each dll; everything is in the same expansion-weighted
// characters the chunker's scan counts.
// * phrase: most builds DROP a phrase-break-free stretch that overflows their phrase
//   buffer. Plain-text ceilings: por 113-117, ita 119-123, spa 125-129, fre/gre/pol
//   131-139, ger 141-149, dut 151-169, eng >=140 (content dependent; normalization
//   can shorten the effective room, hence the margins).
// * token: Russian resets its counter at spaces but truncates a single whitespace-free
//   token past ~48 chars (and silently skips long Latin tokens entirely, cut or not).
//   Hebrew's counter ignores spaces altogether - only punctuation resets it - and its
//   budget is a tiny ~41 chars.
// * chunk: Russian garbles the whole utterance past ~235 chars when commas are dense
//   (other builds hold to ~250); Hebrew produces partial output past ~90 regardless of
//   punctuation; Japanese truncates at ~9 SECONDS of synthesized audio, which no
//   punctuation resets, so its calls must stay near 40 chars (~8s at neutral rate).
static void bst_v2_limits(bst_state* s) {
	s->v2_chunk_limit = 240;
	s->v2_phrase_limit = 112;
	s->v2_token_limit = 112;
	wchar_t path[MAX_PATH];
	if (!GetModuleFileNameW(s->dll, path, MAX_PATH)) return;
	wchar_t* base = wcsrchr(path, L'\\');
	base = base? base + 1 : path;
	_wcslwr(base);
	if (wcsncmp(base, L"dll_", 4) != 0) return;
	const wchar_t* lang = base + 4;
	if (!wcsncmp(lang, L"por", 3)) s->v2_phrase_limit = s->v2_token_limit = 94;
	else if (!wcsncmp(lang, L"ita", 3)) s->v2_phrase_limit = s->v2_token_limit = 100;
	else if (!wcsncmp(lang, L"spa", 3)) s->v2_phrase_limit = s->v2_token_limit = 104;
	else if (!wcsncmp(lang, L"fre", 3) || !wcsncmp(lang, L"gre", 3) || !wcsncmp(lang, L"pol", 3)) s->v2_phrase_limit = s->v2_token_limit = 108;
	else if (!wcsncmp(lang, L"rus", 3)) { s->v2_chunk_limit = 224; s->v2_token_limit = 40; }
	else if (!wcsncmp(lang, L"heb", 3)) { s->v2_chunk_limit = 88; s->v2_phrase_limit = 34; s->v2_token_limit = 34; }
	else if (!wcsncmp(lang, L"jpn", 3)) s->v2_chunk_limit = 40;
}

bool bst_v2_setup(bst_state* s) {
	s->v2_init = (v2InitFunc)GetProcAddress(s->dll, "Init_TTS");
	s->v2_deinit = (v2DeInitFunc)GetProcAddress(s->dll, "DeInit_TTS");
	s->v2_say = (v2SayFunc)GetProcAddress(s->dll, "Say_TTS");
	if (!s->v2_init || !s->v2_say) return false;
	s->is_v2 = true;
	s->sample_rate = 10000;
	bst_v2_limits(s);
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
// They also have a fixed PHRASE buffer: the frontend accumulates text between
// punctuation marks to shape prosody, and a punctuation-free stretch that overflows it
// is dropped in its entirety without a sound (or truncated, on the builds that are
// merciful). Mastodon handles and other long unpunctuated runs hit this constantly,
// and a dot only counts as punctuation when whitespace follows it, so
// "user.example.com" does not reset the counter. Chunk cuts are therefore forced so
// that no chunk contains a run longer than the dll's limits (see bst_v2_limits); the
// engine flushes its phrase buffer at end of input, so a mid-sentence chunk end acts
// as a phrase break.

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
	int window = s->v2_chunk_limit - prefix_len;
	// Degenerate prefix; better to risk a long chunk than emit confetti. Small-chunk
	// builds (Japanese) get a lower floor so a command prefix can't defeat their limit.
	int window_floor = s->v2_chunk_limit < 80? 24 : 40;
	if (window < window_floor) window = window_floor;
	// Note: no short-text fast path here. Even an utterance under the chunk limit must
	// go through the loop so the phrase-limit scan below can split it; a 130-240 char
	// stretch without honored punctuation (a URL glued to surrounding words, say)
	// would otherwise reach the engine whole and get its first phrase silently dropped.
	wchar_t* chunk = (wchar_t*)malloc((prefix_len + window + 1) * sizeof(wchar_t));
	if (!chunk) {
		free(wtext);
		return;
	}
	int pos = 0;
	while (pos < content_len && !s->async_stop_speaking) {
		int remain = content_len - pos;
		int take = remain <= window? remain : window;
		// First pass: force a cut before any punctuation-free run can overflow the
		// engine's phrase buffer. The buffer holds NORMALIZED text, so characters are
		// weighted by their expansion: a digit becomes a number word ("9" -> "nine",
		// six digits of "973520" -> ~60 chars) and URL-ish symbols become words like
		// "slash". Two counters cover the builds' two behaviors: "run" resets only at
		// honored phrase breaks (the drop-the-whole-phrase buffer most builds have),
		// "token" also resets at whitespace (the per-word counter Russian truncates
		// on). A phrase cut lands at the last space or URL separator inside the
		// overlong run; a token by definition has none, so it's cut where it stands.
		{
			int run = 0, token = 0, run_space = -1, forced = -1;
			for (int i = 0; i < take; i++) {
				wchar_t c = content[pos + i];
				if (bst_v2_phrase_break_at(content + pos, remain, i)) {
					run = 0;
					token = 0;
					run_space = -1;
					continue;
				}
				if (bst_v2_is_space(c)) token = 0;
				if (bst_v2_is_space(c) || c == L'/' || c == L'-' || c == L'_') run_space = i;
				int w;
				if (c >= L'0' && c <= L'9') w = 10;
				else if (c == L'/' || c == L'-' || c == L'_' || c == L':' || c == L'@' || c == L'%' || c == L'#' || c == L'&' || c == L'+' || c == L'=' || c == L'.') w = 6;
				else w = 1;
				run += w;
				if (!bst_v2_is_space(c)) token += w;
				if (run >= s->v2_phrase_limit) {
					forced = run_space > 0? run_space + 1 : i;
					break;
				}
				if (token >= s->v2_token_limit) {
					forced = i;
					break;
				}
			}
			if (forced > 0 && forced < take) take = forced;
		}
		if (take < remain) {
			// Prefer to break after sentence punctuation the engine honors (dot inside
			// a URL or filename doesn't count), then after an honored comma (a pause
			// there sounds intended), else at whitespace.
			int cut = -1;
			for (int i = take - 1; i > take / 4; i--) {
				if (bst_v2_is_break(content[pos + i]) && bst_v2_phrase_break_at(content + pos, remain, i)) { cut = i + 1; break; }
			}
			if (cut < 0) {
				for (int i = take - 1; i > take / 4; i--) {
					wchar_t c = content[pos + i];
					if ((c == L',' || c == 0x3001 || c == 0xFF0C) && bst_v2_phrase_break_at(content + pos, remain, i)) { cut = i + 1; break; }
				}
			}
			if (cut < 0) {
				for (int i = take - 1; i > take / 4; i--) {
					wchar_t c = content[pos + i];
					if (c == L' ' || c == L'\t') { cut = i + 1; break; }
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
		// Tighten the joins: strip dead air from chunk edges that face another chunk.
		s->v2_trim_lead = pos > 0;
		s->v2_trim_trail = pos + take < content_len;
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
