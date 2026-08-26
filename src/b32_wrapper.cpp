// Wrapper around Bestspeech/keynote gold to support speaking to a memory buffer instead of waveout. Various constants/parameters/etc are taken from @rommix0's bst.h.
// This is released into the public domain.

#include <math.h> // expf for the v2 tone correction shelf
#include <stdio.h> // _snprintf
#include <windows.h>
#include "b32_wrapper.h"
#include "MinHook.h"
#define riffheader_impl
#include "riffheader.h"
#include "sonic.h"

// Settings for BST parameters
#define BST_RATE_SETTING 257
#define BST_GAIN_SETTING  258
#define BST_BIT_DEPTH_SETTING 4097

// Voice data collected from BST.h made by @rommix0.
// This is split into pairs of 3 strings. The voice name, it's prefix text, and it's second prefix text? @rommix0 why are these separated?
const char* bst_voice_data[] = {
	"Fred", "~v0]~e3]~h0]~u0]~f80]", "~r0]",
	"Sara", "~v2]~e3]~h-20]~u0]~f175]", "~r0]",
	"Hary", "~v3]~e3]~h10]~u0]~f65]", "~r5]",
	"Wendy", "~v2]~e1]~h50]~u0]~f150]", "~r-5]",
	"Dexter", "~v6]~e6]~h0]~u-25]~f90]", "~r7]",
	"Alien", "~v4]~e6]~h-50]~u-20]~f115]", "~r-20]",
	"Kit", "~v5]~e3]~h40]~u0]~f230]", "~r-10]",
	"Bruno", "~v3]~e3]~h50]~u0]~f60]", "~r8]",
	"Ghost", "~v3]~e2]~h50]~u0]~f60]", "~r8]",
	"Peeper", "~v2]~e2]~h0]~u5]~f80]", "~r0]",
	"Dracula", "~v3]~e3]~h45]~u-5]~f47]", "~r10]",
	"Granny", "~v4]~e3]~h-60]~u0]~f350]", "~r20]",
	"Martha", "~v6]~e4]~h100]~u-5]~f300]", "~r-10]",
	"Tim", "~v3]~e4]~h-10]~u0]~f60]", "~r-10]",
	nullptr, nullptr, nullptr
};
int bst_voice_count = 0; // Will be initialized upon first call to bst_voices or bst_init.
const char** bst_voices_buf = nullptr; // Contains pointers to voice names (populated in bst_voices call).

// The bst_state structure and engine function typedefs live in b32_state.h so that
// b32_v2.cpp can share them. Below are the waveout definitions used by the hooks.
#include "b32_state.h"
typedef MMRESULT  (WINAPI *waveOutOpenFunc)(LPHWAVEOUT, UINT, LPCWAVEFORMATEX, DWORD_PTR, DWORD_PTR, DWORD);
waveOutOpenFunc waveOutOpenProc;
typedef MMRESULT  (WINAPI *waveOutHeaderFunc)(HWAVEOUT, WAVEHDR*, UINT);
waveOutHeaderFunc waveOutPrepareHeaderProc;
waveOutHeaderFunc waveOutWriteProc;
waveOutHeaderFunc waveOutUnprepareHeaderProc;
typedef MMRESULT  (WINAPI *waveOutResetFunc)(HWAVEOUT);
waveOutResetFunc waveOutResetProc;
waveOutResetFunc waveOutCloseProc;
typedef void (WINAPI *sleepFunc)(DWORD);
sleepFunc sleepProc;

bool winmm_hooked = false;
thread_local bst_state* winmm_hooked_state = nullptr; // Insure that we can passthrough any hooked waveout calls that come through on a thread or context other than the one we are working on while providing global access to the speech state within hooks.
HINSTANCE g_hinstance = nullptr;

// If bstRelBuf is not called from a windows message loop the same number of times that waveOutWrite is called, TtsWav will never return! We acomplish this with a hidden message window. We could theoretically just minhook PeekMessage or create a WH_GETMESSAGE hook with SetWindowsHookExA, but the way we're doing it is the most standard and correct, for whatever that's worth.
#define WM_REL_BUF (WM_USER + 1)
LRESULT CALLBACK on_rel_buf(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
	if (message == WM_REL_BUF) winmm_hooked_state->bstRelBuf(winmm_hooked_state->tts);
	else if (message == WM_DESTROY) PostQuitMessage(0);
	else return DefWindowProc(hwnd, message, wParam, lParam);
	return 0;
}
HWND create_message_window() {
	WNDCLASSW wc = {};
	if (!GetClassInfoW(g_hinstance, L"b32tts_wrapper_class", &wc)) {
		wc.lpfnWndProc = on_rel_buf;
		wc.hInstance	 = g_hinstance;
		wc.lpszClassName = L"b32tts_wrapper_class";
		if (!RegisterClassW(&wc)) return nullptr;
	}
	return CreateWindowExW(0, L"b32tts_wrapper_class", L"b32tts_wrapper_window", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_hinstance, nullptr);
}
b32w_export BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
	if (reason == DLL_PROCESS_ATTACH) g_hinstance = module;
	else if (reason == DLL_PROCESS_DETACH && winmm_hooked) {
		MH_DisableHook(MH_ALL_HOOKS);
		MH_Uninitialize();
	}
	return TRUE;
}

// Actual waveout hooks, most of these are no-ops/passthroughs accept for open and write. The no-ops must still exist to insure that no unwanted function calls from bestspeech reach the WinMM API.
// The classic engine passes our state pointer through TtsWav's window argument which arrives here as the callback, giving us a precise ownership check. The v2 dlls open with CALLBACK_NULL, so for those we claim any waveOutOpen made on the thread we're currently synthesizing on (winmm_hooked_state is thread_local, so genuine waveout use on other threads still passes through).
MMRESULT WINAPI waveOutOpenHook(LPHWAVEOUT outptr, UINT device, LPCWAVEFORMATEX format, DWORD_PTR callback, DWORD_PTR instance, DWORD flags) {
	if (!winmm_hooked_state || ((bst_state*)callback != winmm_hooked_state && !winmm_hooked_state->is_v2)) return waveOutOpenProc(outptr, device, format, callback, instance, flags);
	*outptr = (HWAVEOUT)winmm_hooked_state; // Now all other hooks will receive state information in their first parameter, though we prefer to use winmm_hooked_state. This also makes sure our hook returns a semblance of what the calling function is expecting.
	// Most v2 dlls declare a 10000hz format, which renders their 11025-family synthesis
	// core audibly deep and chesty; 11025 itself overshoots into chipmunk. Two clues
	// pin the true rate at 10800: formant envelope alignment against the classic engine
	// peaks at 10700-10800hz, and the Russian dll actually declares 10800 - the one
	// build that told the truth. All v2 output is therefore played at 10800 (wav
	// headers, the helper handshake and sonic all follow).
	winmm_hooked_state->sample_rate = winmm_hooked_state->is_v2? 10800 : format->nSamplesPerSec;
	winmm_hooked_state->bass_lp = 0.0f;
	winmm_hooked_state->bass_a = 0.0f; // Set below if the tone shelf is wanted.
	// Now that the true output format is known, bring the sonic stream in line with it. The v2 language dlls don't all share one sample rate, so this can't be hardcoded.
	if (winmm_hooked_state->sonic_stream && sonicGetSampleRate(winmm_hooked_state->sonic_stream) != winmm_hooked_state->sample_rate) sonicSetSampleRate(winmm_hooked_state->sonic_stream, winmm_hooked_state->sample_rate);
	if (!winmm_hooked_state->sonic_stream && winmm_hooked_state->pending_rate_multiplier != 1.0f) winmm_hooked_state->sonic_stream = sonicCreateStream(winmm_hooked_state->sample_rate, 1);
	if (winmm_hooked_state->sonic_stream) sonicSetSpeed(winmm_hooked_state->sonic_stream, winmm_hooked_state->pending_rate_multiplier);
	if (!winmm_hooked_state->audio && !winmm_hooked_state->async_callback) {
		winmm_hooked_state->audio = (char*)malloc(winmm_hooked_state->audio_capacity);
		if (winmm_hooked_state->audio_size) make_wav_header_in_place((wav_header*)winmm_hooked_state->audio, 0, winmm_hooked_state->sample_rate, format->wBitsPerSample, format->nChannels, format->wFormatTag);
	}
	return MMSYSERR_NOERROR;
}
MMRESULT WINAPI waveOutPrepareHeaderHook(HWAVEOUT ptr, WAVEHDR* header, UINT size) {
	if (!winmm_hooked_state || (bst_state*)ptr != winmm_hooked_state) return waveOutPrepareHeaderProc(ptr, header, size);
	return MMSYSERR_NOERROR;
}
inline void waveOutput(short* data, DWORD data_len) {
	// winmm_hooked_state is expected to be valid!
	// Never deliver empty blocks. Sonic legitimately returns 0 samples while it
	// buffers input for a speed change, and a zero length block must not reach
	// consumers: the helper's stdout protocol uses a zero length chunk as its
	// end-of-utterance sentinel, so passing one through would truncate speech and
	// desync the stream.
	if (!data_len) return;
	if (winmm_hooked_state->bass_a > 0.0f) {
		// The v2 tone correction shelf; see waveOutOpenHook for the rationale. Runs
		// after sonic so filter state stays continuous over an utterance's chunks.
		float lp = winmm_hooked_state->bass_lp, a = winmm_hooked_state->bass_a;
		DWORD n = data_len / sizeof(short);
		for (DWORD i = 0; i < n; i++) {
			lp += a * ((float)data[i] - lp);
			float y = (float)data[i] - 0.369f * lp; // 0.369 = 1 - 10^(-4/20), a -4db cut
			data[i] = (short)(y < -32768.0f? -32768.0f : (y > 32767.0f? 32767.0f : y));
		}
		winmm_hooked_state->bass_lp = lp;
	}
	if (winmm_hooked_state->async_callback) {
		if (!winmm_hooked_state->async_callback((char*)data, data_len, winmm_hooked_state->async_callback_user)) {
			winmm_hooked_state->async_stop_speaking = true;
			return;
		}
	} else {
		if (winmm_hooked_state->audio_size + data_len >= winmm_hooked_state->audio_capacity) {
			// Must keep doubling until the chunk fits; v2 dlls deliver a whole utterance in one write, which can be many times the current capacity.
			while (winmm_hooked_state->audio_size + data_len >= winmm_hooked_state->audio_capacity) winmm_hooked_state->audio_capacity *= 2;
			winmm_hooked_state->audio = (char*)realloc(winmm_hooked_state->audio, winmm_hooked_state->audio_capacity);
		}
		memcpy(winmm_hooked_state->audio + winmm_hooked_state->audio_size, data, data_len);
		winmm_hooked_state->audio_size += data_len;
	}
}
MMRESULT WINAPI waveOutWriteHook(HWAVEOUT ptr, WAVEHDR* header, UINT size) {
	if (!winmm_hooked_state || (bst_state*)ptr != winmm_hooked_state) return waveOutWriteProc(ptr, header, size);
	if (winmm_hooked_state->is_v2) {
		// The v2 dlls have no bstRelBuf; they just want the header marked played.
		header->dwFlags |= WHDR_DONE;
		header->dwFlags &= ~WHDR_INQUEUE;
	} else PostMessage(winmm_hooked_state->message_window, WM_REL_BUF, 0, 0);
	if (winmm_hooked_state->async_stop_speaking) return MMSYSERR_NOERROR; // Callback returned false, drop all remaining buffers.
	short* data = (short*)header->lpData;
	DWORD data_len = header->dwBufferLength;
	if (winmm_hooked_state->is_v2 && (winmm_hooked_state->v2_trim_lead || winmm_hooked_state->v2_trim_trail)) {
		// Chunked v2 utterance: strip near-silent edges facing another chunk so the
		// engine's per-utterance dead air doesn't become a long pause at every join.
		DWORD n = data_len / sizeof(short);
		const short th = 160;
		const DWORD max_trim = 3200; // ~300ms
		if (winmm_hooked_state->v2_trim_lead) {
			DWORD k = 0;
			while (k < n && k < max_trim && data[k] > -th && data[k] < th) k++;
			data += k;
			n -= k;
		}
		if (winmm_hooked_state->v2_trim_trail && n) {
			DWORD k = 0;
			while (k < n && k < max_trim && data[n - 1 - k] > -th && data[n - 1 - k] < th) k++;
			n -= k;
		}
		data_len = n * sizeof(short);
	}
	// The read cap must be in samples, not bytes: passing data_len (bytes) as maxSamples
	// let sonic write up to twice the engine's buffer when slowing down (output larger
	// than input), corrupting the heap. Rate boost never hit this (speedup shrinks
	// output), but the no-command languages drive their whole rate setting through
	// sonic in both directions. Excess slowdown output stays buffered in the stream and
	// is drained by the flush loop in waveOutCloseHook.
	if (winmm_hooked_state->sonic_stream && sonicGetSpeed(winmm_hooked_state->sonic_stream) != 1.0f && sonicWriteShortToStream(winmm_hooked_state->sonic_stream, data, data_len / sizeof(short)))
		data_len = sonicReadShortFromStream(winmm_hooked_state->sonic_stream, data, data_len / sizeof(short)) * sizeof(short);
	waveOutput(data, data_len);
	return MMSYSERR_NOERROR;
}
MMRESULT WINAPI waveOutUnprepareHeaderHook(HWAVEOUT ptr, WAVEHDR* header, UINT size) {
	if (!winmm_hooked_state || (bst_state*)ptr != winmm_hooked_state) return waveOutUnprepareHeaderProc(ptr, header, size);
	return MMSYSERR_NOERROR;
}
MMRESULT WINAPI waveOutResetHook(HWAVEOUT ptr) {
	if (!winmm_hooked_state || (bst_state*)ptr != winmm_hooked_state) return waveOutResetProc(ptr);
	return MMSYSERR_NOERROR;
}
MMRESULT WINAPI waveOutCloseHook(HWAVEOUT ptr) {
	if (!winmm_hooked_state || (bst_state*)ptr != winmm_hooked_state) return waveOutCloseProc(ptr);
	// We need to flush sonic's buffer and feed any remaining output to either the callback or our memory output.
	if (winmm_hooked_state->sonic_stream) sonicFlushStream(winmm_hooked_state->sonic_stream);
	while (winmm_hooked_state->sonic_stream && sonicSamplesAvailable(winmm_hooked_state->sonic_stream)) {
		short data[1024];
		DWORD data_len = sonicReadShortFromStream(winmm_hooked_state->sonic_stream, data, 1024) * sizeof(short);
		if (!winmm_hooked_state->async_stop_speaking && data_len) waveOutput(data, data_len);
	}
	return MMSYSERR_NOERROR;
}
// After writing an utterance's single buffer, a v2 dll sleeps out the audio's entire real time playback duration before Say_TTS returns. Since our write hook consumes the audio instantly, that nap is pure added latency (5+ seconds for a paragraph); skip it. This only fires on the synthesis thread while a v2 utterance is in progress, so the engine's short internal waits and every other caller of Sleep in the process remain untouched.
void WINAPI sleepHook(DWORD ms) {
	if (winmm_hooked_state && winmm_hooked_state->is_v2 && ms >= 100) return;
	sleepProc(ms);
}

void winmm_hook() {
	if (winmm_hooked) return;
	MH_Initialize();
	MH_CreateHook((LPVOID)waveOutOpen, (LPVOID)waveOutOpenHook, (LPVOID*)&waveOutOpenProc);
	MH_CreateHook((LPVOID)waveOutPrepareHeader, (LPVOID)waveOutPrepareHeaderHook, (LPVOID*)&waveOutPrepareHeaderProc);
	MH_CreateHook((LPVOID)waveOutWrite, (LPVOID)waveOutWriteHook, (LPVOID*)&waveOutWriteProc);
	MH_CreateHook((LPVOID)waveOutUnprepareHeader, (LPVOID)waveOutUnprepareHeaderHook, (LPVOID*)&waveOutUnprepareHeaderProc);
	MH_CreateHook((LPVOID)waveOutReset, (LPVOID)waveOutResetHook, (LPVOID*)&waveOutResetProc);
	MH_CreateHook((LPVOID)waveOutClose, (LPVOID)waveOutCloseHook, (LPVOID*)&waveOutCloseProc);
	MH_CreateHook((LPVOID)Sleep, (LPVOID)sleepHook, (LPVOID*)&sleepProc);
	MH_EnableHook(MH_ALL_HOOKS);
	winmm_hooked = TRUE;
}

inline void voices_count() {
	if (!bst_voice_count) {
		for (int i = 0; bst_voice_data[i]; i += 3) bst_voice_count++;
	}
}

// Finally the public library interface itself.
b32w_export const char** bst_voices(int* count) {
	voices_count();
	if (!bst_voices_buf) bst_voices_buf = (const char**) malloc(sizeof(const char*) * (bst_voice_count + 1));
	for (int i = 0; i < bst_voice_count; i++) bst_voices_buf[i] = bst_voice_data[i * 3];
	bst_voices_buf[bst_voice_count] = nullptr;
	if (count) *count = bst_voice_count;
	return bst_voices_buf;
}
inline bst_state* bst_init_from_hmodule(HMODULE hmod) {
	if (!hmod) return nullptr;
	voices_count();
	bst_state* s = (bst_state*)malloc(sizeof(bst_state));
	memset(s, 0, sizeof(bst_state));
	s->dll = hmod;
	s->bstCreate = (bstCreateFunc)GetProcAddress(s->dll, "bstCreate");
	s->TtsWav = (TtsWavFunc)GetProcAddress(s->dll, "TtsWav");
	s->bstRelBuf = (bstRelBufFunc)GetProcAddress(s->dll, "bstRelBuf");
	s->bstDestroy = (bstDestroyFunc)GetProcAddress(s->dll, "bstDestroy");
	s->bstClose = (bstCloseFunc)GetProcAddress(s->dll, "bstClose");
	s->bstSetParams = (bstSetParamsFunc)GetProcAddress(s->dll, "bstSetParams");
	s->bstGetParams = (bstGetParamsFunc)GetProcAddress(s->dll, "bstGetParams");
	s->message_window = nullptr; // We'll create this in the speak function encase the user calls speak on another thread from init.
	s->sonic_stream = nullptr; // We'll create this the first time a rate multiplier is applied.
	s->sample_rate = 11025;
	s->pending_rate_multiplier = 1.0f;
	if (!s->bstCreate || !s->TtsWav) {
		// Not a classic engine; try the 2006 language dll interface before giving up.
		if (bst_v2_setup(s)) return s;
		FreeLibrary(s->dll);
		free(s);
		return nullptr;
	}
	if (s->bstCreate(s->tts)) {
		FreeLibrary(s->dll);
		free(s);
		return nullptr;
	}
	s->bstSetParams(s->tts, BST_BIT_DEPTH_SETTING, 16);
	return s;
}
b32w_export bst_state* bst_init(const char* module_path) {
	return bst_init_from_hmodule(LoadLibraryA(module_path));
}
b32w_export bst_state* bst_init_w(const wchar_t* module_path) {
	return bst_init_from_hmodule(LoadLibraryW(module_path));
}
b32w_export void bst_free(bst_state* s) {
	if (!s) return;
	if (s->sonic_stream) sonicDestroyStream(s->sonic_stream);
	if (s->is_v2) bst_v2_close(s);
	else {
		s->bstClose(s->tts);
		s->bstDestroy();
	}
	FreeLibrary(s->dll);
	if (s->message_window) DestroyWindow(s->message_window);
	free(s);
}
inline void bst_speak_internal(bst_state* s, const char* text, int voice, int rate, float rate_multiplier, int gain) {
	if (!s->is_v2 && !s->message_window) s->message_window = create_message_window();
	s->pending_rate_multiplier = rate_multiplier; // For v2 the sonic stream is created in waveOutOpenHook where the dll's true sample rate is first known.
	if (!s->is_v2 && rate_multiplier != 1.0f && !s->sonic_stream) s->sonic_stream = sonicCreateStream(11025, 1);
	if (s->sonic_stream) sonicSetSpeed(s->sonic_stream, rate_multiplier);
	bool text_allocated = false;
	if (s->is_v2) {
		// No bstSetParams here; the v2 frontend still parses inline tilde commands, so express rate and gain that way. Rate is negated to preserve this API's higher-is-faster convention (the engine itself interprets lower numbers as faster).
		char params[64] = "";
		if (rate != 0) _snprintf(params, sizeof(params), "~r%d]", rate * -1);
		if (gain != 0) _snprintf(params + strlen(params), sizeof(params) - strlen(params), "~g%d]", gain);
		const char* prefix1 = voice >= 0 && voice < bst_voice_count? bst_voice_data[voice * 3 + 1] : "";
		const char* prefix2 = voice >= 0 && voice < bst_voice_count? bst_voice_data[voice * 3 + 2] : "";
		if (params[0] || prefix1[0]) {
			int text_len = strlen(params) + strlen(prefix1) + strlen(prefix2) + strlen(text) + 1;
			char* actual_text = (char*)malloc(text_len);
			_snprintf(actual_text, text_len, "%s%s%s%s", prefix1, prefix2, params, text);
			text = actual_text;
			text_allocated = true;
		}
	} else {
		if (voice >= 0 && voice < bst_voice_count) { // prepend voice prefixes
			int text_len = strlen(bst_voice_data[voice * 3 + 1]) + strlen(bst_voice_data[voice * 3 + 2]) + strlen(text) + 1;
			char* actual_text = (char*)malloc(text_len);
			_snprintf(actual_text, text_len, "%s%s%s", bst_voice_data[voice * 3 + 1], bst_voice_data[voice * 3 + 2], text);
			text = actual_text;
			text_allocated = true;
		}
		s->bstSetParams(s->tts, BST_RATE_SETTING, rate * -1); // Bestspeech interprets lower numbers as faster rates.
		s->bstSetParams(s->tts, BST_GAIN_SETTING, gain);
	}
	winmm_hook();
	winmm_hooked_state = s;
	if (s->is_v2) bst_v2_speak(s, text);
	else s->TtsWav(s->tts, s, text);
	winmm_hooked_state = nullptr;
	if (text_allocated) free((void*)text);
}
b32w_export char* bst_speak(bst_state* s, long* size, const char* text, int voice, int rate, float rate_multiplier, int gain, bool pcm_header) {
	if (!s || !text) return nullptr;
	s->audio_size = pcm_header? sizeof(wav_header) : 0;
	s->audio_capacity = 16384;
	s->async_callback = nullptr;
	s->async_stop_speaking = false;
	bst_speak_internal(s, text, voice, rate, rate_multiplier, gain);
	if (pcm_header) {
		wav_header* h = (wav_header*)s->audio; // We must correct filesize here.
		h->wav_size = s->audio_size - 8;
		h->data_bytes = s->audio_size - sizeof(wav_header);
	}
	if (size) *size = s->audio_size;
	char* data = s->audio;
	s->audio = nullptr; // Will be allocated upon next speech segment.
	return data;
}
b32w_export void bst_speak_async(bst_state* s, bst_async_callback callback, void* user, const char* text, int voice, int rate, float rate_multiplier, int gain) {
	if (!s || !text) return;
	s->audio = nullptr;
	s->async_callback = callback;
	s->async_callback_user = user;
	s->async_stop_speaking = false;
	bst_speak_internal(s, text, voice, rate, rate_multiplier, gain);
}
b32w_export void bst_speech_free(char* data) {
	free(data);
}
b32w_export int bst_get_sample_rate(bst_state* s) {
	if (!s) return 0;
	return s->sample_rate;
}
b32w_export bool bst_is_v2(bst_state* s) {
	return s && s->is_v2;
}
