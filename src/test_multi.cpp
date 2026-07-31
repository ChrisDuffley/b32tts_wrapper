// Checks that several engine instances backed by different language dlls can be alive
// at once and spoken through alternately, and that freeing one leaves the others intact.
// The NVDA addon's automatic language switching depends on this: it keeps an engine per
// language it has spoken, because reloading a dll mid sentence would be audible as a gap.
// Run from the repository root so the bin/ paths below resolve.
// This is released into the public domain.
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "b32_wrapper.h"

static void report(const char* tag, bst_state* s, const char* text) {
	long size = 0;
	char* buf = bst_speak(s, &size, text, -1, 0, 1.0f, 0, false);
	short* pcm = (short*)buf;
	long n = size / 2;
	int peak = 0;
	for (long i = 0; i < n; i++) {
		int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
		if (v > peak) peak = v;
	}
	printf("%-10s samples=%-8ld peak=%-6d rate=%d\n", tag, n, peak, bst_get_sample_rate(s));
	bst_speech_free(buf);
}

int main(int argc, const char** argv) {
	bst_state* classic = bst_init("bin/b32_tts.dll");
	bst_state* eng = bst_init("bin/dll_eng.dll");
	bst_state* fre = bst_init("bin/dll_fre.dll");
	bst_state* rus = bst_init("bin/dll_rus.dll");
	printf("init: classic=%p eng=%p fre=%p rus=%p\n", classic, eng, fre, rus);
	if (!classic || !eng || !fre || !rus) return 1;
	for (int round = 0; round < 3; round++) {
		printf("-- round %d --\n", round);
		report("classic", classic, "Hello there, this is the classic engine speaking.");
		report("eng", eng, "Hello there, this is the English language dll speaking.");
		report("fre", fre, "Bonjour, ceci est la voix francaise.");
		report("rus", rus, "\xd0\x97\xd0\xb4\xd1\x80\xd0\xb0\xd0\xb2\xd1\x81\xd1\x82\xd0\xb2\xd1\x83\xd0\xb9\xd1\x82\xd0\xb5");
	}
	// Free one and keep speaking through the rest; the addon evicts engines this way.
	bst_free(fre);
	printf("-- after freeing fre --\n");
	report("classic", classic, "Still fine after a neighbour was unloaded.");
	report("eng", eng, "Still fine after a neighbour was unloaded.");
	report("rus", rus, "\xd0\x97\xd0\xb4\xd1\x80\xd0\xb0\xd0\xb2\xd1\x81\xd1\x82\xd0\xb2\xd1\x83\xd0\xb9\xd1\x82\xd0\xb5");
	bst_free(classic);
	bst_free(eng);
	bst_free(rus);
	printf("done\n");
	return 0;
}
