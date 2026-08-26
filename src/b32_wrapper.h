#ifndef B32_WRAPPER_H
#define B32_WRAPPER_H

#ifndef b32w_export
#ifdef __cplusplus
#define b32w_export extern "C" __declspec(dllexport)
#else
#define b32w_export extern __declspec(dllexport)
#endif
#endif

struct bst_state;
typedef bool (*bst_async_callback)(char* data, long size, void* user);

b32w_export const char** bst_voices(int* count = nullptr);
// The module_path can point either at the classic 1994 engine (b32_tts.dll) or at one of the 2006 "v2" language dlls (dll_eng.dll, dll_rus.dll etc); the flavor is detected from the dll's exports. For v2 dlls, text passed to the speak functions is interpreted as utf-8 (that's how the non Latin languages work); for the classic engine it is passed to the synthesizer verbatim as before (windows-1252).
b32w_export bst_state* bst_init(const char* module_path = "b32_tts.dll");
b32w_export bst_state* bst_init_w(const wchar_t* module_path = L"b32_tts.dll");
b32w_export void bst_free(bst_state* s);
b32w_export char* bst_speak(bst_state* s, long* size, const char* text, int voice = 0, int rate = 0, float rate_multiplier = 1.0f, int gain = 0, bool pcm_header = true); // Make sure to free return values with bst_speech_free.
b32w_export void bst_speak_async(bst_state* s, bst_async_callback callback, void* user, const char* text, int voice = 0, int rate = 0, float rate_multiplier = 1.0f, int gain = 0);
b32w_export void bst_speech_free(char* data);
b32w_export int bst_get_sample_rate(bst_state* s); // The engine's output sample rate in hz. Classic is always 11025; v2 dlls vary per language (usually 10000, 10800 for Russian) and the value is only exact after the first utterance has been synthesized.
b32w_export bool bst_is_v2(bst_state* s); // True if the loaded dll is a 2006 language dll rather than the classic engine.

#endif
