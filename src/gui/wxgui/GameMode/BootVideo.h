#pragma once

// Video playback for the Game Mode boot video (boot/boot.mp4 or boot/boot.webm in the Cemu folder).
// Decoded with FFmpeg on a worker thread; the sound goes to an AudioSink (Cemu's TV output).
// Builds without FFmpeg (CEMU_BOOT_VIDEO undefined) get no player and report it as unsupported.

#include "GameModeBackend.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace GameMode
{
	// Where the sound goes: interleaved 16-bit stereo at SampleRate(), in blocks of BlockFrames().
	class AudioSink
	{
	public:
		virtual ~AudioSink() = default;
		virtual int SampleRate() const = 0;
		virtual int BlockFrames() const = 0;
		virtual bool WantsBlock() = 0; // the output has room for another block
		virtual void FeedBlock(const int16_t* stereo) = 0;
		virtual void Start() = 0;
	};

	// False when Cemu was built without FFmpeg.
	bool IsVideoPlaybackSupported();

	// Opens the file and starts decoding. Returns null (and sets error) when it cannot be played.
	// audio may be null for a silent video.
	std::unique_ptr<VideoPlayer> CreateVideoPlayer(const std::filesystem::path& file, std::unique_ptr<AudioSink> audio, std::string& error);
}
