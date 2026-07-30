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
// * Output is 16 bit mono like the classic engine, but the sample rate varies per language
//   dll (typically 10000hz, 10800hz for Russian), so consumers should query
//   bst_get_sample_rate rather than assuming 11025.
// This is released into the public domain.

#include <windows.h>
#include "b32_state.h"

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

void bst_v2_speak(bst_state* s, const char* utf8_text) {
	int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_text, -1, nullptr, 0);
	if (wlen <= 0) return;
	wchar_t* wtext = (wchar_t*)malloc(wlen * sizeof(wchar_t));
	if (!wtext) return;
	MultiByteToWideChar(CP_UTF8, 0, utf8_text, -1, wtext, wlen);
	s->v2_say(wtext);
	free(wtext);
}

void bst_v2_close(bst_state* s) {
	if (s->v2_deinit) s->v2_deinit();
}
