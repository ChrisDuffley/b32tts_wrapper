// Internal shared state between b32_wrapper.cpp (classic 1994 engine + waveout capture)
// and b32_v2.cpp (2006 language dll support). Not part of the public API.

#ifndef B32_STATE_H
#define B32_STATE_H

#include <windows.h>
#include "b32_wrapper.h"
#include "sonic.h"

// Classic (b32_tts.dll) function typedefs, definitions collected by Rommix.
typedef int  (__cdecl *bstCreateFunc)(long*&);
typedef int  (__cdecl *TtsWavFunc)(long*, void*, const char*);
typedef void (__cdecl *bstRelBufFunc)(long*);
typedef void (__cdecl *bstCloseFunc)(long*);
typedef void (__cdecl *bstDestroyFunc)();
typedef void (__cdecl *bstSetParamsFunc)(long*, int, int);
typedef void (__cdecl *bstGetParamsFunc)(long*, int, int*);

// V2 (2006 Lingvosoft-era dll_xx.dll) function typedefs. These stripped down builds
// export only three functions and take wide character text.
typedef int (__cdecl *v2InitFunc)();
typedef int (__cdecl *v2DeInitFunc)();
typedef int (__cdecl *v2SayFunc)(const wchar_t*);

// This structure contains all state information required to use bestspeech, including the dll module handle, required bst function pointers and the bestspeak handle itself.
struct bst_state {
	HMODULE dll;
	long* tts;
	bstCreateFunc bstCreate;
	TtsWavFunc TtsWav;
	bstRelBufFunc bstRelBuf;
	bstCloseFunc bstClose;
	bstDestroyFunc bstDestroy;
	bstSetParamsFunc bstSetParams;
	bstGetParamsFunc bstGetParams;
	bool is_v2;
	v2InitFunc v2_init;
	v2DeInitFunc v2_deinit;
	v2SayFunc v2_say;
	// Per-dll text limits, all in expansion-weighted characters (see the scan in
	// bst_v2_speak). The builds differ: measured ceilings range from Hebrew's 41-char
	// phrase budget to Dutch's 151+. Set by bst_v2_setup from the dll's filename.
	int v2_chunk_limit;  // Longest text passed to one Say_TTS call.
	int v2_phrase_limit; // Longest run without an honored phrase break (overflow is dropped or truncated silently).
	int v2_token_limit;  // Longest run without any whitespace (Russian truncates these far earlier than its phrase limit).
	int sample_rate; // The true output rate: what waveOutOpen declares for classic (11025), overridden to 10800 for v2 dlls (most misdeclare 10000; Russian's honest 10800 declaration and formant alignment against classic both identify the family's true rate).
	float pending_rate_multiplier;
	float bass_lp; // One pole lowpass state for the v2 tone correction shelf.
	float bass_a;  // Its coefficient, derived from the utterance's sample rate; 0 disables the shelf (classic).
	bool v2_trim_lead;  // Strip near-silent leading edge of the current chunk's audio (chunk is not the utterance's first).
	bool v2_trim_trail; // Strip near-silent trailing edge (more chunks follow), tightening the join.
	bst_async_callback async_callback;
	void* async_callback_user;
	bool async_stop_speaking;
	char* audio;
	long audio_size;
	long audio_capacity;
	HWND message_window;
	sonicStream sonic_stream;
};

// Implemented in b32_v2.cpp.
bool bst_v2_setup(bst_state* s);   // Detects v2 exports on s->dll, initializes the engine. Returns false if s->dll is not a v2 dll.
void bst_v2_speak(bst_state* s, const char* utf8_text); // Converts utf-8 text to wide characters and synthesizes it.
void bst_v2_close(bst_state* s);   // Shuts the v2 engine down.

#endif
