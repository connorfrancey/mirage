// Sound from mods (see audio.h): XAudio2, one source voice a sound, made lazily and destroyed once it ends.
#include "runtime/audio.h"

#include <Windows.h>
#include <xaudio2.h>

#include <spdlog/spdlog.h>

#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

namespace mirage::audio
{
	namespace
	{
		struct Wave
		{
			WAVEFORMATEX format{};
			std::vector<std::uint8_t> samples;
		};
		struct Voice
		{
			std::string owner;
			IXAudio2SourceVoice* voice = nullptr;
			std::shared_ptr<const Wave> wave;		// Its samples, kept while it plays
		};

		std::mutex g_lock;
		IXAudio2* g_engine = nullptr;
		IXAudio2MasteringVoice* g_master = nullptr;
		bool g_failed = false;						// The device couldn't be opened: not tried again
		std::map<std::filesystem::path, std::shared_ptr<const Wave>> g_waves;
		std::map<int, Voice> g_voices;
		int g_next = 1;

		bool Open(std::string* error)
		{
			if (g_engine) return true;
			if (g_failed) {
				if (error) *error = "no audio device";
				return false;
			}
			CoInitializeEx(nullptr, COINIT_MULTITHREADED);		// Already set up on this thread is fine
			HRESULT hr = XAudio2Create(&g_engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
			if (SUCCEEDED(hr)) hr = g_engine->CreateMasteringVoice(&g_master);
			if (FAILED(hr)) {
				if (g_engine) g_engine->Release();
				g_engine = nullptr;
				g_failed = true;
				spdlog::warn("Audio: XAudio2 didn't start (0x{:08X}): mods play no sound", static_cast<unsigned>(hr));
				if (error) *error = "no audio device";
				return false;
			}
			return true;
		}

		// RIFF/WAVE with a PCM fmt chunk (8 or 16 bit) and a data chunk
		std::shared_ptr<const Wave> Read(const std::filesystem::path& file, std::string* error)
		{
			auto fail = [&](const std::string& why) {
				if (error) *error = file.filename().string() + ": " + why;
				return nullptr;
			};
			std::ifstream in(file, std::ios::binary);
			if (!in) return fail("can't be read");
			const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) || std::memcmp(bytes.data() + 8, "WAVE", 4)) return fail("not a WAV file");
			auto wave = std::make_shared<Wave>();
			bool fmt = false, data = false;
			for (std::size_t at = 12; at + 8 <= bytes.size();) {
				std::uint32_t size = 0;
				std::memcpy(&size, bytes.data() + at + 4, 4);
				const std::uint8_t* body = bytes.data() + at + 8;
				if (size > bytes.size() - at - 8) size = static_cast<std::uint32_t>(bytes.size() - at - 8);	// A short last chunk
				if (!std::memcmp(bytes.data() + at, "fmt ", 4) && size >= 16) {
					std::memcpy(&wave->format, body, 16);
					wave->format.cbSize = 0;
					fmt = true;
				} else if (!std::memcmp(bytes.data() + at, "data", 4)) {
					wave->samples.assign(body, body + size);
					data = true;
				}
				at += 8 + size + (size & 1);
			}
			const WAVEFORMATEX& f = wave->format;
			if (!fmt || !data) return fail("no fmt or data chunk");
			if (f.wFormatTag != WAVE_FORMAT_PCM || (f.wBitsPerSample != 8 && f.wBitsPerSample != 16) || f.nChannels < 1 || f.nChannels > 2 ||
				f.nSamplesPerSec < 1000 || f.nSamplesPerSec > 192000)
				return fail("not 8 or 16 bit PCM, mono or stereo");
			if (f.nBlockAlign != f.nChannels * f.wBitsPerSample / 8) return fail("a bad block size");
			if (wave->samples.empty()) return fail("no samples");
			return wave;
		}

		// Voices that finished: destroyed (never from XAudio2's own callback thread: no callbacks are used)
		void Sweep()
		{
			for (auto it = g_voices.begin(); it != g_voices.end();) {
				XAUDIO2_VOICE_STATE st{};
				it->second.voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
				if (st.BuffersQueued == 0) {
					it->second.voice->DestroyVoice();
					it = g_voices.erase(it);
				} else {
					++it;
				}
			}
		}
	}

	int Play(const std::string& owner, const std::filesystem::path& file, float volume, bool loop, std::string* error)
	{
		std::lock_guard lock(g_lock);
		if (!Open(error)) return 0;
		Sweep();
		std::shared_ptr<const Wave>& wave = g_waves[file];
		if (!wave) wave = Read(file, error);
		if (!wave) {
			g_waves.erase(file);
			return 0;
		}
		IXAudio2SourceVoice* voice = nullptr;
		if (FAILED(g_engine->CreateSourceVoice(&voice, &wave->format))) {
			if (error) *error = "the audio device didn't take another sound";
			return 0;
		}
		XAUDIO2_BUFFER buffer{};
		buffer.AudioBytes = static_cast<UINT32>(wave->samples.size());
		buffer.pAudioData = wave->samples.data();
		buffer.Flags = XAUDIO2_END_OF_STREAM;
		buffer.LoopCount = loop ? XAUDIO2_LOOP_INFINITE : 0;
		voice->SetVolume(volume < 0.f ? 0.f : volume);
		if (FAILED(voice->SubmitSourceBuffer(&buffer)) || FAILED(voice->Start())) {
			voice->DestroyVoice();
			if (error) *error = "the sound didn't start";
			return 0;
		}
		const int handle = g_next++;
		g_voices[handle] = {owner, voice, wave};
		return handle;
	}

	void Stop(const std::string& owner, int handle)
	{
		std::lock_guard lock(g_lock);
		const auto it = g_voices.find(handle);
		if (it == g_voices.end() || it->second.owner != owner) return;
		it->second.voice->DestroyVoice();		// Stops it at once
		g_voices.erase(it);
	}

	bool Playing(const std::string& owner, int handle)
	{
		std::lock_guard lock(g_lock);
		Sweep();
		const auto it = g_voices.find(handle);
		return it != g_voices.end() && it->second.owner == owner;
	}

	void StopAll(const std::string& owner)
	{
		std::lock_guard lock(g_lock);
		for (auto it = g_voices.begin(); it != g_voices.end();) {
			if (it->second.owner == owner) {
				it->second.voice->DestroyVoice();
				it = g_voices.erase(it);
			} else {
				++it;
			}
		}
		// Its files may change before it loads again (a hot reload): read them anew
		g_waves.clear();
	}

	int Count()
	{
		std::lock_guard lock(g_lock);
		Sweep();
		return static_cast<int>(g_voices.size());
	}
}
