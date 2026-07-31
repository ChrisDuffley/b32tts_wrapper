import array
import json
import os
import struct
import subprocess
from collections import OrderedDict
from synthDriverHandler import SynthDriver, synthIndexReached, synthDoneSpeaking, VoiceInfo
from speech.commands import IndexCommand, PitchCommand, CharacterModeCommand, LangChangeCommand
import ctypes
from ctypes import c_char_p, c_void_p, c_long, c_float, c_wchar_p, byref, POINTER, CFUNCTYPE
import nvwave
import config
import winUser
from autoSettingsUtils.driverSetting import DriverSetting, BooleanDriverSetting, NumericDriverSetting
from autoSettingsUtils.utils import StringParameterInfo
from . import _bst_numbers
import re
import time
import queue
import threading
from logHandler import log

minRate = 200
maxRate = -90
minPitch = 43
maxPitch = 413 # carefully chozen so that the range is within all custom voices while the default value of default voice is pitch 10 in the settings ring.
minInflection = -150
maxInflection = 150
minVolume = -68
maxVolume = 12

# Thanks Rommix for the custom voices.
voices = {
	"fred": {"headsize": "1", "excitation": "3", "inflection": 0, "unvoicedVolume": 0, "pitch": 80},
	"sara": {"headsize": "2", "excitation": "3", "inflection": -20, "unvoicedVolume": 0, "pitch": 175},
	"hary": {"headsize": "3", "excitation": "3", "inflection": 10, "unvoicedVolume": 0, "pitch": 65},
	"wendy": {"headsize": "2", "excitation": "1", "inflection": 50, "unvoicedVolume": 0, "pitch": 150},
	"dexter": {"headsize": "6", "excitation": "6", "inflection": 0, "unvoicedVolume": -25, "pitch": 90},
	"alien": {"headsize": "4", "excitation": "6", "inflection": -50, "unvoicedVolume": -20, "pitch": 115},
	"kit": {"headsize": "5", "excitation": "3", "inflection": 40, "unvoicedVolume": 0, "pitch": 230},
	"bruno": {"headsize": "3", "excitation": "3", "inflection": 50, "unvoicedVolume": 0, "pitch": 60},
	"ghost": {"headsize": "3", "excitation": "2", "inflection": 50, "unvoicedVolume": 0, "pitch": 60},
	"peeper": {"headsize": "2", "excitation": "2", "inflection": 0, "unvoicedVolume": 5, "pitch": 80},
	"dracula": {"headsize": "3", "excitation": "3", "inflection": 45, "unvoicedVolume": -5, "pitch": 47},
	"granny": {"headsize": "4", "excitation": "3", "inflection": -60, "unvoicedVolume": 0, "pitch": 350},
	"martha": {"headsize": "6", "excitation": "4", "inflection": 100, "unvoicedVolume": -5, "pitch": 300},
	"tim": {"headsize": "3", "excitation": "4", "inflection": -10, "unvoicedVolume": 0, "pitch": 60}
}

# Available engine languages. "classic" is the original 1994 b32_tts.dll this addon has
# always used; the rest are the 2006 "v2" language dlls (from the Lingvosoft era, preserved
# by Rommix) which take utf-8 text and vary in output sample rate. Each entry is:
# language id -> (display label, dll filename, NVDA language code, text encoding, command mode).
# Only languages whose dll actually exists in this directory are offered in the dialog.
#
# The command mode records what each build's text frontend does with inline tilde
# commands, established empirically (byte comparison plus whisper transcription of the
# outputs):
# * "classic": the 1994 engine, all commands work including ~v headsize and ~h inflection.
# * "tilde": commands work except ~v/~h which are silently ignored (most v2 dlls).
# * "none": commands must not be sent at all. Polish reads them aloud as text; Japanese
#   vocalizes a short artifact per command while applying no effect; Greek strips them
#   along with all other non-Greek text. For these, rate is applied through the sonic
#   time stretcher and volume through the audio player instead.
languages = OrderedDict([
	("classic", ("Classic English (1994)", "b32_tts.dll", "en", "windows-1252", "classic")),
	("eng", ("English", "dll_eng.dll", "en", "utf-8", "tilde")),
	# Arabic (dll_ara.dll) is intentionally absent: that dll's synthesis core is a stub
	# which emits the same short buffer of digital silence (peak amplitude 0) no matter
	# what text it's given, in either Latin or Arabic script.
	("dut", ("Dutch", "dll_dut.dll", "nl", "utf-8", "tilde")),
	("fre", ("French", "dll_fre.dll", "fr", "utf-8", "tilde")),
	("ger", ("German", "dll_ger.dll", "de", "utf-8", "tilde")),
	("gre", ("Greek", "dll_gre.dll", "el", "utf-8", "none")),
	("heb", ("Hebrew", "dll_heb.dll", "he", "utf-8", "tilde")),
	("ita", ("Italian", "dll_ita.dll", "it", "utf-8", "tilde")),
	("jpn", ("Japanese", "dll_jpn.dll", "ja", "utf-8", "none")),
	("pol", ("Polish", "dll_pol.dll", "pl", "utf-8", "none")),
	("por", ("Portuguese", "dll_por.dll", "pt", "utf-8", "tilde")),
	("rus", ("Russian", "dll_rus.dll", "ru", "utf-8", "tilde")),
	("spa", ("Spanish", "dll_spa.dll", "es", "utf-8", "tilde")),
])

# Settings remembered separately for each language, snapshotted whenever the user
# switches language (and on NVDA exit) and restored when they switch back. Stored as
# a hand-editable json file in the NVDA user configuration directory so the profiles
# survive addon updates.
profileParams = ("voice", "rate", "rateBoost", "pitch", "inflection", "volume", "unvoicedVolume", "headsize", "excitation")
# Defaults applied the first time a language is selected, before the user has stored
# anything for it. The classic engine clips badly above 85 percent volume, while the
# v2 dlls are much quieter (Russian peaks below 20 percent of full scale) and want 100.
languageDefaults = {"classic": {"volume": 85}}
v2LanguageDefaults = {"volume": 100}

def _profilePath():
	try:
		import globalVars
		return os.path.join(globalVars.appArgs.configPath, "bestspeechLanguageProfiles.json")
	except Exception:
		return os.path.join(os.path.dirname(__file__), "bestspeechLanguageProfiles.json")

bst_async_callback = CFUNCTYPE(c_long, c_void_p, c_long, c_void_p)

def _patchVoicePanelLanguageRefresh():
	# NVDA's voice settings panel only refreshes its other controls when the *voice*
	# setting changes, so after a language switch the sliders would keep showing the
	# previous language's values (and our per-language supported settings wouldn't be
	# rebuilt). Patch StringDriverSettingChanger, for this driver's language setting
	# only, to trigger the same refresh a voice change does. Technique borrowed from
	# Tomi's TGSpeechBox addon, compatible with NVDA 2024.1 through 2026.1.
	try:
		import gui.settingsDialogs as sd
		changerCls = getattr(sd, "StringDriverSettingChanger", None)
		if changerCls is None or getattr(changerCls, "_bestspeechLanguageRefreshPatched", False):
			return
		origCall = changerCls.__call__
		def patchedCall(self, evt):
			origCall(self, evt)
			try:
				if getattr(getattr(self, "setting", None), "id", None) != "bstlanguage":
					return
				if getattr(getattr(self, "driver", None), "name", None) != "bestspeech":
					return
				updateFn = getattr(getattr(self, "container", None), "updateDriverSettings", None)
				if callable(updateFn):
					updateFn(changedSetting="bstlanguage")
			except Exception:
				log.debug("bestspeech: could not refresh voice panel after language change", exc_info=True)
		changerCls.__call__ = patchedCall
		changerCls._bestspeechLanguageRefreshPatched = True
	except Exception:
		log.debug("bestspeech: failed to patch voice panel language refresh", exc_info=True)

# The BGThread from espeak
class BgThread(threading.Thread):
	def __init__(self):
		super().__init__(name=f"{self.__class__.__module__}.{self.__class__.__qualname__}")
		self.daemon = True

	def run(self):
		while True:
			func, args, kwargs = bgQueue.get()
			if not func:
				break
			try:
				func(*args, **kwargs)
			except:  # noqa: E722
				log.error("Error running function from queue", exc_info=True)
			bgQueue.task_done()


def _execWhenDone(func, *args, mustBeAsync=False, **kwargs):
	global bgQueue
	if mustBeAsync or bgQueue.unfinished_tasks != 0:
		# Either this operation must be asynchronous or There is still an operation in progress.
		# Therefore, run this asynchronously in the background thread.
		bgQueue.put((func, args, kwargs))
	else:
		func(*args, **kwargs)

class _Engine:
	"""One initialized engine, driving a single language dll.

	Automatic language switching needs a different engine part way through an utterance,
	so an engine is kept alive for every language that gets spoken rather than being torn
	down and reloaded on each switch: a reload costs a dll load plus the warmup synthesis,
	which would be audible as a gap in the middle of a sentence. Engines run in process
	through b32_wrapper.dll where that is possible and out of process through the 32 bit
	b32_helper.exe otherwise (e.g. a 32 bit dll under 64 bit NVDA 2026+). Everything here
	runs on the background speech thread; see the note in SynthDriver.__init__ about why
	the classic engine must never be touched from NVDA's main thread.
	"""

	def __init__(self, basePath, langId, wrapperDll):
		label, dllName, nvdaLang, encoding, cmdMode = languages[langId]
		self.langId = langId
		self.encoding = encoding
		self.cmdMode = cmdMode
		self.sampleRate = None
		self.handle = None
		self.helper = None
		self._basePath = basePath
		self._dllPath = os.path.join(basePath, dllName)
		self._dll = wrapperDll
		# The helper's stdin is written from both NVDA's main thread (cancel) and the
		# background speech thread (speak). Without a lock those writes can interleave
		# mid-command, corrupting the protocol framing, which surfaces as truncated
		# utterances and speech that queues instead of cancelling.
		self._lock = threading.Lock()

	def open(self):
		"""Starts the engine, in process if b32_wrapper.dll is usable and through the
		helper otherwise. Returns True if it is ready to speak."""
		if self._dll is not None and self._openInProcess():
			return True
		self.sampleRate = self._startHelper()
		return self.sampleRate is not None

	def _openInProcess(self):
		try:
			self.handle = self._dll.bst_init_w(self._dllPath)
			if not self.handle:
				raise OSError(f"bst_init failed for {self._dllPath}")
			# Warmup utterance so the wrapper learns the engine's true output sample rate
			# (the v2 language dlls don't all share one; e.g. Russian is 10800 hz).
			size = c_long(0)
			buf = self._dll.bst_speak(self.handle, byref(size), b"a", -1, 0, c_float(1.0), 0, False)
			if buf: self._dll.bst_speech_free(buf)
			self.sampleRate = self._dll.bst_get_sample_rate(c_void_p(self.handle))
			return True
		except (OSError, AttributeError):
			log.debug("bestspeech: %s could not be started in process" % self._dllPath, exc_info=True)
			# The engine may already be up and only the warmup have failed; don't leave
			# it loaded, since open() falls through to the helper from here.
			if self.handle:
				self._dll.bst_free(c_void_p(self.handle))
			self.handle = None
			return False

	def _startHelper(self):
		# Returns the engine's output sample rate as reported by the helper's startup
		# handshake, or None if the helper could not be started.
		helperPath = os.path.join(self._basePath, 'b32_helper.exe')
		try:
			self.helper = subprocess.Popen(
				[helperPath, self._dllPath],
				stdin=subprocess.PIPE,
				stdout=subprocess.PIPE,
				stderr=subprocess.DEVNULL,
				creationflags=subprocess.CREATE_NO_WINDOW
			)
		except OSError:
			log.error("bestspeech: could not launch the helper for %s" % self._dllPath, exc_info=True)
			self.helper = None
			return None
		hdr = self.readExact(8)
		if hdr is None:
			log.error("bestspeech helper died during startup for %s" % self._dllPath)
			return None
		magic, sampleRate = struct.unpack('<II', hdr)
		if magic != 0xFFFFFFFE:
			log.error("bestspeech helper sent unexpected handshake %#x" % magic)
			return None
		return sampleRate

	def restartHelper(self):
		"""Brings the helper back after it died unexpectedly. The sample rate is that of
		the same dll as before, so the player it feeds stays valid."""
		self.helper = None
		return self._startHelper() is not None

	def sendSpeak(self, txt, rateMultiplier):
		# SPEAK command: [uint32 text_len][float32 rate_mult][text bytes]. Sent as a
		# single write under the lock so a concurrent cancel from the main thread can
		# never splice its bytes into the middle of this command.
		try:
			with self._lock:
				self.helper.stdin.write(struct.pack('<If', len(txt), rateMultiplier) + txt)
				self.helper.stdin.flush()
			return True
		except OSError:
			log.debug("BSTDBG speakBg stdin write failed")
			return False

	def readExact(self, n):
		buf = b""
		while len(buf) < n:
			try:
				chunk = self.helper.stdout.read(n - len(buf))
			except OSError:
				return None
			if not chunk:
				return None
			buf += chunk
		return buf

	def cancel(self):
		if self.helper is None:
			return
		# CANCEL command: text_len == 0.
		try:
			with self._lock:
				self.helper.stdin.write(struct.pack('<I', 0))
				self.helper.stdin.flush()
		except OSError:
			pass

	def kill(self):
		if self.helper is not None:
			try:
				self.helper.kill()
			except Exception:
				pass

	def close(self):
		if self.helper is not None:
			# QUIT command, then wait for a clean exit.
			try:
				with self._lock:
					self.helper.stdin.write(struct.pack('<I', 0xFFFFFFFF))
					self.helper.stdin.flush()
			except OSError:
				pass
			try:
				self.helper.wait(timeout=2)
			except subprocess.TimeoutExpired:
				pass
			if self.helper.poll() is None:
				self.helper.kill()
			self.helper = None
		elif self.handle:
			self._dll.bst_free(c_void_p(self.handle))
			self.handle = None

class SynthDriver(SynthDriver):
	name = 'bestspeech'
	description = 'Bestspeech'
	_allSupportedSettings = (
		# Note: the id must be a single lowercase word; NVDA's settings dialog derives the
		# available-values attribute via id.capitalize(), which would mangle camelCase.
		DriverSetting("bstlanguage", "&Language", availableInSettingsRing=True),
		SynthDriver.VoiceSetting(),
		SynthDriver.RateSetting(),
		SynthDriver.RateBoostSetting(),
		SynthDriver.PitchSetting(),
		SynthDriver.InflectionSetting(),
		SynthDriver.VolumeSetting(),
		NumericDriverSetting("unvoicedVolume", "&Unvoiced Volume", defaultVal=0, availableInSettingsRing=True),
		DriverSetting("headsize", "&Headsize", defaultVal="1", availableInSettingsRing=True),
		DriverSetting("excitation", "&Excitation", defaultVal="3", availableInSettingsRing=True),
		BooleanDriverSetting("numberProcessing", "&Number Processing", defaultVal=False),
		BooleanDriverSetting("abbreviations", "&Abbreviations", defaultVal=True),
		BooleanDriverSetting("phrasePrediction", "&Phrase Prediction", defaultVal=True)
	)
	# Settings that do nothing for a given command mode are hidden from the dialog and
	# the settings ring. For "none" languages, rate still works (via sonic), volume
	# still works (via the player), and rate boost and number processing are engine
	# independent; everything command-driven is dead.
	_deadSettingsByMode = {
		"classic": (),
		"tilde": ("headsize", "inflection"),
		"none": ("voice", "pitch", "inflection", "headsize", "excitation", "unvoicedVolume", "abbreviations", "phrasePrediction"),
	}

	def _currentCmdMode(self):
		return languages[getattr(self, "_bstLanguage", "classic")][4]

	def _get_supportedSettings(self):
		dead = self._deadSettingsByMode[self._currentCmdMode()]
		if not dead:
			return self._allSupportedSettings
		return tuple(s for s in self._allSupportedSettings if s.id not in dead)
	supportedNotifications = {synthIndexReached, synthDoneSpeaking}
	supportedCommands = {PitchCommand, CharacterModeCommand, IndexCommand, LangChangeCommand}

	@classmethod
	def check(cls):
		return True

	def __init__(self):
		super().__init__()
		_patchVoicePanelLanguageRefresh()
		self._basePath = os.path.dirname(__file__)
		self.dll = None
		self._wrapperLoadAttempted = False
		# Engines that have been used, most recently used last, and the languages whose
		# engine failed to start (so we don't try to load a broken dll on every utterance).
		self._engines = OrderedDict()
		self._badLanguages = set()
		# One player per engine sample rate; the classic engine is 11025 hz and the v2
		# dlls 10800, and automatic language switching can hit both in one utterance.
		self._players = {}
		# Only offer languages whose engine dll is actually present next to this driver.
		self._availableBstLanguages = OrderedDict()
		for langId, (label, dllName, nvdaLang, encoding, cmdMode) in languages.items():
			if os.path.isfile(os.path.join(self._basePath, dllName)):
				self._availableBstLanguages[langId] = StringParameterInfo(langId, label)
		if not self._availableBstLanguages:
			# Nothing found; keep the classic entry so init proceeds (and fails loudly) the same way older addon versions did.
			self._availableBstLanguages["classic"] = StringParameterInfo("classic", languages["classic"][0])
		self._bstLanguage = "classic" if "classic" in self._availableBstLanguages else next(iter(self._availableBstLanguages))
		# Suppressed until loadSettings completes, so the profile of the previously used
		# language can't be clobbered with construction defaults during startup restore.
		self._suppressProfileSnapshot = True
		self._languageProfiles = {}
		try:
			with open(_profilePath(), "r", encoding="utf-8") as f:
				self._languageProfiles = json.load(f)
		except FileNotFoundError:
			pass
		except Exception:
			log.error("Failed to load bestspeech language profiles", exc_info=True)
		global bgQueue
		bgQueue = queue.Queue()
		self.bgThread = BgThread()
		self.bgThread.start()
		# The engine must be initialized on the background speech thread, never here on
		# NVDA's main thread: in-process, the classic engine creates its buffer-release
		# message window on whichever thread first synthesizes (the init warmup), and if
		# that isn't the thread all later synthesis runs on, release messages get
		# dispatched cross-thread by NVDA's own message loop, corrupting engine state
		# and crashing NVDA (observed on 32-bit NVDA 2025.3). Speech requests queue up
		# behind this task, so ordering is preserved.
		_execWhenDone(self._initEngine, mustBeAsync=True)
		self.rate = 90
		self.volume = self._paramToPercent(0, minVolume, maxVolume)
		self.voice = "fred" # This will automatically set all other parameters like pitch, inflection, excitation and more.
		self.numberProcessing = False
		self.abbreviations = True
		self._phrasePrediction = True
		self.table = str.maketrans("\u2019", "'")
		self.canceled = False

	# How many engines stay loaded at once. Automatic language switching only juggles a
	# couple of languages in practice, and each one held open costs a loaded dll or a
	# helper process, so the least recently used engine beyond this is dropped.
	_maxLoadedEngines = 4

	def _initEngine(self):
		# Loads b32_wrapper.dll for in process synthesis, then warms up the engine and
		# player for the currently selected language so the first utterance isn't
		# delayed by them.
		if not self._wrapperLoadAttempted:
			self._wrapperLoadAttempted = True
			wrapper_path = os.path.join(self._basePath, 'b32_wrapper.dll')
			try:
				self.dll = ctypes.cdll[wrapper_path]
				self.dll.bst_init_w.argtypes = (ctypes.c_wchar_p,)
				self.dll.bst_init_w.restype = c_void_p
				self.dll.bst_free.argtypes = (c_void_p,)
				self.dll.bst_speak_async.restype = c_void_p
				self.dll.bst_speak.argtypes = (c_void_p, POINTER(c_long), c_char_p, c_long, c_long, c_float, c_long, ctypes.c_bool)
				self.dll.bst_speak.restype = c_void_p
				self.dll.bst_speech_free.argtypes = (c_void_p,)
				self.dll.bst_get_sample_rate.argtypes = (c_void_p,)
			except (OSError, AttributeError):
				# b32_wrapper.dll could not be loaded in-process (e.g. 32-bit DLL in
				# 64-bit NVDA 2026+, or DLL simply absent). Every engine falls back to
				# the out-of-process 32-bit helper.
				self.dll = None
		engine = self._getEngine(self._bstLanguage)
		if engine is not None:
			self._getPlayer(engine.sampleRate)

	def _getEngine(self, langId):
		# Returns a ready engine for langId, starting one if this language hasn't been
		# spoken yet, or None if it can't be started. Background speech thread only.
		engine = self._engines.get(langId)
		if engine is not None:
			self._engines.move_to_end(langId)
			return engine
		if langId in self._badLanguages:
			return None
		engine = _Engine(self._basePath, langId, self.dll)
		if not engine.open():
			engine.close()
			self._badLanguages.add(langId)
			log.error("bestspeech: could not start the %s engine" % langId)
			return None
		self._engines[langId] = engine
		# Evict down to the cap, never the selected language or the one just started.
		while len(self._engines) > self._maxLoadedEngines:
			for victim in list(self._engines):
				if victim != langId and victim != self._bstLanguage:
					self._engines.pop(victim).close()
					break
			else:
				break
		return engine

	def _getPlayer(self, sampleRate):
		rate = sampleRate or 11025
		player = self._players.get(rate)
		if player is None:
			try:
				currentSoundcardOutput = config.conf['speech']['outputDevice']
			except:
				currentSoundcardOutput = config.conf["audio"]["outputDevice"]
			player = nvwave.WavePlayer(1, rate, 16, outputDevice=currentSoundcardOutput)
			self._players[rate] = player
		return player

	def _volumeParam(self, langId):
		# The v2 dlls are far quieter than the classic engine (Russian peaks below 20
		# percent of full scale), which is why their default volume is 100 against the
		# classic engine's 85: twelve db apart in this driver's parameter space. When
		# automatic language switching drops a stretch of text on the other engine
		# family, apply that offset so the sentence doesn't change loudness half way.
		volume = self._volume
		if (langId == "classic") != (self._bstLanguage == "classic"):
			volume += -12 if langId == "classic" else 12
		return max(minVolume, min(maxVolume, volume))

	def _volumeGainFactor(self, langId):
		# For "none" command mode languages the ~g gain command can't be used, so the
		# volume setting is applied as software gain on the pcm instead, following the
		# same db curve the engine's gain command uses (the volume parameter spans -68
		# to +12 db). This keeps their loudness in line with the other languages, where
		# e.g. 100 percent volume means a +12 db engine boost.
		return 10.0 ** (self._volumeParam(langId) / 20.0)

	def _scaleChunk(self, chunk, factor):
		if abs(factor - 1.0) < 0.01:
			return chunk
		arr = array.array('h')
		arr.frombytes(chunk)
		for i in range(len(arr)):
			v = int(arr[i] * factor)
			arr[i] = -32768 if v < -32768 else (32767 if v > 32767 else v)
		return arr.tobytes()

	def loadSettings(self, onlyChanged = False):
		# We can probably remove this in a bit, we override this to make sure people's excitation setting doesn't break across addon versions.
		super().loadSettings(onlyChanged)
		if self.excitation == "0": self.excitation = "3"
		self._suppressProfileSnapshot = False

	def _snapshotProfile(self):
		# Remember the current parameters under the current language.
		profile = {}
		for p in profileParams:
			try:
				profile[p] = getattr(self, p)
			except Exception:
				pass
		self._languageProfiles[self._bstLanguage] = profile
		try:
			with open(_profilePath(), "w", encoding="utf-8") as f:
				json.dump(self._languageProfiles, f, indent="\t")
		except Exception:
			log.error("Failed to save bestspeech language profiles", exc_info=True)

	def _applyProfile(self, langId):
		profile = self._languageProfiles.get(langId)
		if profile is None:
			# First time this language is used; apply just its defaults and keep everything else as is.
			profile = languageDefaults.get(langId, v2LanguageDefaults)
		# Voice first, since setting it resets pitch, inflection and friends.
		if "voice" in profile:
			try:
				self.voice = profile["voice"]
			except Exception:
				pass
		for p in profileParams:
			if p == "voice" or p not in profile:
				continue
			try:
				setattr(self, p, profile[p])
			except Exception:
				pass

	def _set_rate(self, vl):
		self._rate = self._percentToParam(vl,minRate,maxRate)

	def _get_rate(self):
		return self._paramToPercent(self._rate, minRate, maxRate)

	def _set_rateBoost(self, enable):
		self._rateBoost = enable

	def _get_rateBoost(self):
		return self._rateBoost

	def _set_pitch(self, vl):
		self._pitch = self._percentToParam(vl,minPitch,maxPitch)

	def _get_pitch(self):
		return self._paramToPercent(self._pitch, minPitch, maxPitch)

	def _set_volume(self, vl):
		self._volume = self._percentToParam(vl,minVolume,maxVolume)

	def _get_volume(self):
		return self._paramToPercent(self._volume, minVolume, maxVolume)

	def _set_unvoicedVolume(self, vl):
		self._unvoicedVolume = self._percentToParam(vl,minVolume,maxVolume)

	def _get_unvoicedVolume(self):
		return self._paramToPercent(self._unvoicedVolume, minVolume, maxVolume)

	def _set_inflection(self, vl):
		self._inflection = self._percentToParam(vl,minInflection,maxInflection)

	def _get_inflection(self):
		return self._paramToPercent(self._inflection, minInflection, maxInflection)

	def _set_headsize(self, vl):
		n = int(vl)
		self._headsize = vl if n > -1 and n < 7 else 1

	def _get_headsize(self):
		return self._headsize

	def _get_availableHeadsizes(self):
		return { str(i): StringParameterInfo(str(i), str(i)) for i in range(1, 7)}

	def _set_excitation(self, vl):
		n = int(vl)
		self._excitation = vl if n > -1 and n < 8 else 1

	def _get_excitation(self):
		return self._excitation

	def _get_availableExcitations(self):
		return { str(i): StringParameterInfo(str(i), str(i)) for i in range(1,8)}

	def _set_numberProcessing(self, val):
		self._numberProcessing = bool(val)

	def _get_numberProcessing(self):
		return self._numberProcessing

	def _set_abbreviations(self, val):
		self._abbreviations = bool(val)

	def _get_abbreviations(self):
		return self._abbreviations

	def _set_phrasePrediction(self, val):
		self._phrasePrediction = bool(val)

	def _get_phrasePrediction(self):
		return self._phrasePrediction

	def _set_bstlanguage(self, vl):
		if vl not in self._availableBstLanguages or vl == self._bstLanguage:
			return
		self.cancel()
		if not self._suppressProfileSnapshot:
			self._snapshotProfile()
		self._bstLanguage = vl
		self._applyProfile(vl)
		# The synth settings ring builds its list of settings once per synth load, so
		# without a poke it keeps offering settings the new language just hid (e.g.
		# headsize on a v2 language). The dialog reads supportedSettings fresh and is
		# refreshed by our StringDriverSettingChanger patch; the ring needs this.
		try:
			import globalVars
			ring = getattr(globalVars, "settingsRing", None)
			if ring is not None:
				ring.updateSupportedSettings(self)
		except Exception:
			log.debug("bestspeech: could not refresh synth settings ring", exc_info=True)
		# Warming up the new language's engine runs on the background thread, where all
		# other engine access happens, so we can never load a dll under an in-progress
		# utterance. The engine we were using stays loaded and is reused if the user (or
		# automatic language switching) comes back to it.
		_execWhenDone(self._initEngine, mustBeAsync=True)

	def _get_bstlanguage(self):
		return self._bstLanguage

	def _get_availableBstlanguages(self):
		return self._availableBstLanguages

	def _get_language(self):
		return languages[self._bstLanguage][2]

	def _get_availableLanguages(self):
		# NVDA asks the synthesizer which languages it can speak to decide whether a
		# stretch of text needs the "not supported" report. Without this the base
		# implementation collects the language of each VoiceInfo, which this driver
		# leaves unset (voices are speaker characters, not languages), so every
		# language came back unsupported and nothing ever switched.
		return {languages[langId][2] for langId in self._availableBstLanguages}

	def _languageForNvdaLang(self, lang):
		# Maps the language code NVDA reports for a stretch of text onto one of the
		# engine languages we have a dll for. There is one dll per language, so dialects
		# are matched on their base code (en_GB and en_US are both just English), and
		# anything we can't speak stays on the selected language.
		if not lang:
			return self._bstLanguage
		base = lang.replace("-", "_").split("_")[0].lower()
		# Stay put when the selected language already speaks it, so an English stretch in
		# a French document doesn't jump off whichever English engine the user chose.
		if languages[self._bstLanguage][2] == base:
			return self._bstLanguage
		for langId in self._availableBstLanguages:
			if languages[langId][2] == base:
				return langId
		return self._bstLanguage

	def _set_voice(self, vl):
		if not vl in voices: return
		self._voice = vl
		# set voice parameters
		for p in voices[vl]:
			if not hasattr(self, p): continue
			try:
				minimum = globals()[f"min{p.title()}"] if not "Volume" in p else globals()["minVolume"]
				maximum = globals()[f"max{p.title()}"] if not "Volume" in p else globals()["maxVolume"]
				setattr(self, p, self._paramToPercent(voices[vl][p], minimum, maximum))
			except KeyError:
				setattr(self, p, voices[vl][p])

	def _get_voice(self):
		return self._voice

	def _getAvailableVoices(self):
		return {v: VoiceInfo(v, v) for v in voices}

	# Thousands separator used by number processing, per language. The classic/English
	# frontends read comma groups naturally; European frontends treat the comma as a
	# decimal mark, so they get the dot (or space) their locale groups with instead.
	_numberSeparators = {"ger": ".", "dut": ".", "ita": ".", "spa": ".", "por": ".", "gre": ".", "fre": " ", "pol": " ", "rus": " "}

	# Spoken word for the decimal point, per language. The engines treat a dot between
	# digits as sentence punctuation ("7.1" becomes "seven. <pause> one"), so decimals
	# are rewritten as words before any other processing. Also covers version strings
	# and IP addresses, which read as "2026 point 1 point 4" style.
	_decimalWords = {
		"classic": "point", "eng": "point", "spa": "punto", "fre": "virgule",
		"ger": "Komma", "ita": "virgola", "por": "vírgula", "dut": "komma",
		"pol": "przecinek", "rus": "запятая",
		"gre": "κόμμα", "heb": "נקודה",
		"jpn": "てん",
	}

	def _normalizeDecimals(self, text, langId):
		word = self._decimalWords.get(langId, "point")
		return re.sub(r"(?<=\d)\.(?=\d)", f" {word} ", text)

	def _formatNumbers(self, text, langId):
		sep = self._numberSeparators.get(langId, ",")
		def replace_num(m):
			grouped = format(int(m.group(0)), ",")
			return grouped if sep == "," else grouped.replace(",", sep)
		return re.sub(r"\b\d{5,}\b", replace_num, text)

	def _pitchValue(self, multiplier=1.0):
		# The deeper character of the v2 voices turned out to be spectral balance, not
		# fundamental pitch (they carry ~8 percentage points more sub-500hz energy than
		# classic); that is corrected with a low shelf inside the wrapper, so pitch
		# values pass through unscaled here.
		return int(self._pitch * multiplier)

	def speak(self, speechSequence):
		# Automatic language switching hands us LangChangeCommands part way through the
		# sequence. Split it into runs of a single engine language; each run is built and
		# synthesized on its own engine, in order, by the background thread.
		runs = []
		langId = self._bstLanguage
		items = []
		for item in speechSequence:
			if isinstance(item, LangChangeCommand):
				newLangId = self._languageForNvdaLang(item.lang)
				if newLangId != langId:
					if items:
						runs.append((langId, items))
						items = []
					langId = newLangId
				continue
			items.append(item)
		if items or not runs:
			runs.append((langId, items))
		batch = []
		charMode = False
		for runLang, runItems in runs:
			text, idx, charMode = self._buildRun(runLang, runItems, charMode)
			batch.append((runLang, text, idx))
		_execWhenDone(self._speakBg, batch, mustBeAsync=True)

	def _buildRun(self, langId, items, charMode=False):
		# Turns one single-language run of a speech sequence into the text to hand that
		# language's engine, plus the indexes it contains. Character mode is carried in
		# and out because a language change can land in the middle of spelled text.
		# A run with nothing to say (a lone index, or a language change NVDA reports but
		# never puts text in) comes back with text None, so it can't cost an engine load.
		cmdMode = languages[langId][4]
		useCommands = cmdMode != "none"
		lst = ["~n10,0]" if self._abbreviations else "~n10,1]", "~~1,0]" if self._phrasePrediction else "~~1,1]"] if useCommands else []
		if charMode and useCommands: lst.append("~n1,1]")
		idx = []
		spoken = False
		char_mode_on = charMode
		pitch_modified = False
		for item in items:
			if isinstance(item, str):
				spoken = spoken or bool(item.strip())
				lst.append(item)
				if char_mode_on:
					if useCommands: lst.append("~n1,0]")
					char_mode_on = False
				if pitch_modified:
					if useCommands: lst.append(f"~f{self._pitchValue()}]")
					pitch_modified = False
			elif isinstance(item, IndexCommand):
				idx.append(item.index)
			elif isinstance(item,CharacterModeCommand):
				char_mode_on = bool(item.state)
				if useCommands: lst.append("~n1,1]" if char_mode_on else "~n1,0]")
			elif isinstance(item,PitchCommand):
				try: multiplier = item.multiplier
				except ZeroDevisionError: multiplier = 1
				if useCommands: lst.append(f"~f{self._pitchValue(multiplier)}]")
		text = " ".join(lst)
		# Collapse whitespace runs: layout padding counts toward the v2 dlls' 128 byte
		# phrase buffer, needlessly forcing mid-sentence chunk cuts (and their pauses).
		text = re.sub(r"\s{2,}", " ", text)
		text = self._normalizeDecimals(text, langId)
		if cmdMode != "classic":
			# Two v2 frontend quirks. Its normalizer expands apostrophe-s into "is",
			# so possessives read as "Ivan is"; dropping just the apostrophe gives the
			# identical-sounding plain form (Ivans, its, lets) while other contractions
			# stay untouched. And dashes are vocalized as a stray "oo"; a comma is what
			# an em dash means in prose anyway, and NVDA still announces the symbol
			# name at higher punctuation levels before text ever reaches us.
			text = re.sub(r"(?<=\w)['’]s\b", "s", text)
			text = re.sub(r"\s*[–—―]+\s*", ", ", text)
		if cmdMode == "none":
			# These languages' frontends can't read digits (Japanese and Greek drop them
			# entirely, Polish only spells them); convert numbers to words in Python.
			# With number processing off, digits are still made audible, just read out
			# individually rather than as full numbers.
			text = _bst_numbers.localizeNumbers(text, langId, self._numberProcessing)
		elif self._numberProcessing:
			text = self._formatNumbers(text, langId)
		volume = self._volumeParam(langId)
		if cmdMode == "classic":
			text = f"~r{self._rate}]~e{self._excitation}]~v{self.headsize}]~f{self._pitch}]~g{volume}]~u{self._unvoicedVolume}]~h{self._inflection}]{text} ~|"
		elif cmdMode == "tilde":
			# These dlls ignore ~v (headsize) and ~h (inflection); don't send them at all,
			# so values lingering in nvda.ini from classic sessions can never leak in here.
			text = f"~r{self._rate}]~e{self._excitation}]~f{self._pitchValue()}]~g{volume}]~u{self._unvoicedVolume}]{text} ~|"
		# "none" mode: plain text only. Rate is applied via sonic in the speak path and
		# volume via the player; anything else would be read aloud or vocalized as junk.
		return (text if spoken else None), idx, char_mode_on

	# The engine's measured ~r response (speed factor relative to ~r0), identical within
	# a few percent on the classic and v2 builds. The documented "percentage of normal
	# speed" only holds for slow rates; at the fast end the engine compresses hard
	# (~r-90 is 2.7x, nowhere near the 10x a naive 100/(100+r) mapping suggests), so
	# "none" command mode languages must follow this curve or they run absurdly fast.
	_rateCurve = ((200, 0.371), (100, 0.539), (0, 1.0), (-45, 1.660), (-61, 2.113), (-90, 2.711))

	def _rateMultiplier(self, langId):
		# Rate boost quadruples speed via sonic on every engine. For "none" command mode
		# languages the engine's own ~r rate command can't be used either, so the whole
		# rate setting is realized through sonic, following the engine's own curve. A
		# multiplier of ~1 is returned as exactly 1.0 so sonic stays fully bypassed at
		# the neutral rate (bare, unprocessed engine output).
		mult = 4.0 if self._rateBoost else 1.0
		if languages[langId][4] == "none":
			r = self._rate
			pts = self._rateCurve
			if r >= pts[0][0]:
				speed = pts[0][1]
			elif r <= pts[-1][0]:
				speed = pts[-1][1]
			else:
				for (r1, s1), (r2, s2) in zip(pts, pts[1:]):
					if r2 <= r <= r1:
						speed = s1 + (s2 - s1) * (r1 - r) / (r1 - r2)
						break
			if abs(speed - 1.0) < 0.02:
				speed = 1.0
			mult *= speed
		return max(0.3, min(8.0, mult))

	def _speakBg(self, batch):
		# Speaks the runs of one utterance back to back. Runs can land on engines with
		# different sample rates (the classic engine is 11025 hz, the v2 dlls 10800), so
		# each rate has its own player and the previous one is drained before the next
		# starts, or the two halves of the sentence would play over each other.
		self.speaking = True
		# As a dirty hack to make indent nav beeps mostly work, indicate that we've reached the first index immedietly.
		if sum(len(idx) for langId, text, idx in batch) > 1:
			for langId, text, idx in batch:
				if idx:
					synthIndexReached.notify(synth=self, index=idx.pop(0))
					break
		player = None
		pendingIdx = []
		for langId, text, idx in batch:
			if not self.speaking: return
			pendingIdx.extend(idx)
			if text is None: continue
			engine = self._getEngine(langId)
			if engine is None: continue
			# Starting an engine can take long enough for a cancel to land during it.
			if not self.speaking: return
			nextPlayer = self._getPlayer(engine.sampleRate)
			if player is not None and nextPlayer is not player:
				player.idle()
			player = nextPlayer
			if engine.helper is not None:
				self._speakRun_helper(engine, text, player)
			else:
				self._speakRun_dll(engine, text, player)
			if not self.speaking: return
			if pendingIdx:
				player.feed(b"", 0, onDone=lambda idx=pendingIdx: self._notifyIndexes(idx))
				pendingIdx = []
		if player is None:
			# Nothing could be synthesized, but NVDA still has to hear that we're done.
			self.done(pendingIdx)
			return
		player.feed(b"", 0, onDone=lambda idx=pendingIdx: self.done(idx))
		player.idle()
		log.debug("BSTDBG speakBg idle done")

	def _speakRun_dll(self, engine, text, player):
		gain = self._volumeGainFactor(engine.langId) if engine.cmdMode == "none" else None
		@bst_async_callback
		def on_audio(data, size, user):
			if not self.speaking: return False
			if gain is not None:
				chunk = self._scaleChunk(ctypes.string_at(data, size), gain)
				player.feed(chunk, len(chunk))
			else:
				player.feed(data, size)
			return True
		txt = text.translate(self.table).encode(engine.encoding, 'replace')
		self.dll.bst_speak_async(engine.handle, on_audio, None, txt, -1, 0, c_float(self._rateMultiplier(engine.langId)), 0)

	def _speakRun_helper(self, engine, text, player):
		# Restart helper if it died unexpectedly.
		if engine.helper.poll() is not None and not engine.restartHelper():
			return
		txt = text.translate(self.table).encode(engine.encoding, 'replace')
		rate_mult = self._rateMultiplier(engine.langId)
		gain = self._volumeGainFactor(engine.langId) if engine.cmdMode == "none" else None
		log.debug(f"BSTDBG speakBg start lang={engine.langId} len={len(txt)} mult={rate_mult}")
		if not engine.sendSpeak(txt, rate_mult):
			return
		# Read audio chunks until end-of-utterance sentinel (chunk_len == 0).
		fed = discarded = 0
		while True:
			hdr = engine.readExact(4)
			if hdr is None:
				log.debug("BSTDBG speakBg helper eof mid-utterance")
				break
			chunk_len = struct.unpack('<I', hdr)[0]
			if chunk_len == 0:
				break
			chunk = engine.readExact(chunk_len)
			if chunk is None:
				log.debug("BSTDBG speakBg helper eof mid-chunk")
				break
			if self.speaking:
				if gain is not None:
					chunk = self._scaleChunk(chunk, gain)
				player.feed(chunk, len(chunk))
				fed += len(chunk)
			else:
				discarded += len(chunk)
		log.debug(f"BSTDBG speakBg sentinel fed={fed} discarded={discarded} speaking={self.speaking}")

	def _notifyIndexes(self, idx):
		for i in idx:
			synthIndexReached.notify(synth=self, index=i)

	def done(self, idx):
		log.debug(f"BSTDBG done idx={idx}")
		self._notifyIndexes(idx)
		synthDoneSpeaking.notify(synth=self)

	def _stopEngines(self):
		for engine in list(self._engines.values()):
			engine.close()
		self._engines.clear()

	def terminate(self):
		# Remember this language's parameters for next session before shutting down.
		if not self._suppressProfileSnapshot:
			self._snapshotProfile()
		self.cancel()
		bgQueue.put((None, None, None))
		# Kill the helpers before joining the background thread: if an engine ever hangs
		# mid-utterance, the background thread is blocked reading that helper's stdout,
		# and joining it first would deadlock NVDA (observed as a lockup when switching
		# synthesizers). Killing the helper gives that read EOF.
		for engine in list(self._engines.values()):
			engine.kill()
		self.bgThread.join()
		self._stopEngines()
		for player in list(self._players.values()):
			try:
				player.close()
			except Exception:
				pass
		self._players.clear()

	def cancel(self):
		log.debug("BSTDBG cancel")
		self.speaking = False
		while True:
			try:
				item = bgQueue.get_nowait()
			except queue.Empty:
				break
		for player in list(self._players.values()):
			player.stop()
		# Any loaded engine could be the one mid-utterance; the cancel is cheap and a
		# no-op for the idle ones.
		for engine in list(self._engines.values()):
			engine.cancel()

	def pause(self, switch):
		for player in list(self._players.values()):
			player.pause(switch)
