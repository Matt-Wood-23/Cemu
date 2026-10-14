#include "BootVideo.h"

#ifndef CEMU_BOOT_VIDEO

namespace GameMode
{
	bool IsVideoPlaybackSupported()
	{
		return false;
	}

	std::unique_ptr<VideoPlayer> CreateVideoPlayer(const std::filesystem::path&, std::unique_ptr<AudioSink>, std::string& error)
	{
		error = "this build of Cemu has no video support (built without FFmpeg)";
		return nullptr;
	}

	std::unique_ptr<VideoPlayer> CreateVideoPlayer(const uint8_t*, size_t, std::unique_ptr<AudioSink>, std::string& error)
	{
		error = "this build of Cemu has no video support (built without FFmpeg)";
		return nullptr;
	}

	// builds without FFmpeg do not carry the video
	const uint8_t* BuiltInBootVideoData()
	{
		return nullptr;
	}

	size_t BuiltInBootVideoSize()
	{
		return 0;
	}
}

#else

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace GameMode
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// longer videos are cut off here; the boot video is meant to be a few seconds
		constexpr double kMaxSeconds = 60.0;
		constexpr size_t kMaxQueuedFrames = 6;

		std::string AvError(int code)
		{
			char text[AV_ERROR_MAX_STRING_SIZE]{};
			av_strerror(code, text, sizeof(text));
			return text;
		}

		struct Frame
		{
			double pts = 0; // seconds from the start
			int width = 0, height = 0;
			std::vector<uint8_t> rgb;
		};

		// A video in memory (the one built into Cemu), read through a custom AVIOContext
		struct MemoryInput
		{
			const uint8_t* data = nullptr;
			size_t size = 0;
			size_t pos = 0;

			static int Read(void* opaque, uint8_t* buf, int bufSize)
			{
				auto* in = static_cast<MemoryInput*>(opaque);
				const size_t n = std::min<size_t>((size_t)bufSize, in->size - in->pos);
				if (n == 0)
					return AVERROR_EOF;
				std::memcpy(buf, in->data + in->pos, n);
				in->pos += n;
				return (int)n;
			}

			static int64_t Seek(void* opaque, int64_t offset, int whence)
			{
				auto* in = static_cast<MemoryInput*>(opaque);
				int64_t base;
				switch (whence & ~AVSEEK_FORCE)
				{
				case AVSEEK_SIZE: return (int64_t)in->size;
				case SEEK_SET: base = 0; break;
				case SEEK_CUR: base = (int64_t)in->pos; break;
				case SEEK_END: base = (int64_t)in->size; break;
				default: return -1;
				}
				const int64_t target = base + offset;
				if (target < 0 || target > (int64_t)in->size)
					return -1;
				in->pos = (size_t)target;
				return target;
			}
		};

		// Everything FFmpeg, owned by the decode thread
		struct Decoder
		{
			MemoryInput memory;
			AVIOContext* io = nullptr; // only for a video in memory
			AVFormatContext* format = nullptr;
			AVCodecContext* video = nullptr;
			AVCodecContext* audio = nullptr;
			SwsContext* scaler = nullptr;
			SwrContext* resampler = nullptr;
			AVPacket* packet = nullptr;
			AVFrame* frame = nullptr;
			int videoStream = -1, audioStream = -1;
			double videoStart = -1;

			~Decoder()
			{
				av_frame_free(&frame);
				av_packet_free(&packet);
				swr_free(&resampler);
				sws_freeContext(scaler);
				avcodec_free_context(&audio);
				avcodec_free_context(&video);
				avformat_close_input(&format);
				if (io)
				{
					av_freep(&io->buffer);
					avio_context_free(&io);
				}
			}
		};

		AVCodecContext* OpenDecoder(AVStream* stream, std::string& error)
		{
			const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
			if (!codec)
			{
				error = std::string("no decoder for ") + avcodec_get_name(stream->codecpar->codec_id);
				return nullptr;
			}
			AVCodecContext* context = avcodec_alloc_context3(codec);
			if (!context)
				return nullptr;
			int result = avcodec_parameters_to_context(context, stream->codecpar);
			context->thread_count = 0; // as many as useful
			context->pkt_timebase = stream->time_base;
			if (result >= 0)
				result = avcodec_open2(context, codec, nullptr);
			if (result < 0)
			{
				error = "cannot open the " + std::string(codec->name) + " decoder: " + AvError(result);
				avcodec_free_context(&context);
			}
			return context;
		}

		class FFmpegVideoPlayer : public VideoPlayer
		{
		public:
			FFmpegVideoPlayer(std::unique_ptr<Decoder> decoder, std::unique_ptr<AudioSink> audio)
				: m_audio(std::move(audio)), m_audioRate(m_audio ? m_audio->SampleRate() : 0)
			{
				m_decodeThread = std::thread([this, d = std::move(decoder)]() mutable { DecodeLoop(*d); });
				if (m_audio)
					m_audioThread = std::thread([this]() { AudioLoop(); });
			}

			~FFmpegVideoPlayer() override
			{
				{
					std::scoped_lock lock(m_mutex);
					m_stop = true;
				}
				m_cv.notify_all();
				if (m_decodeThread.joinable())
					m_decodeThread.join();
				if (m_audioThread.joinable())
					m_audioThread.join();
			}

			void SetOutputSize(int width, int height) override
			{
				m_outWidth = std::max(1, width);
				m_outHeight = std::max(1, height);
			}

			bool TakeFrame(wxImage& image) override
			{
				std::unique_lock lock(m_mutex);
				if (!m_started)
				{
					// wait for the first frame so a slow start does not skip into the video
					if (m_frames.empty())
						return false;
					m_started = true;
					m_startTime = Clock::now();
					m_cv.notify_all();
				}
				const double now = Elapsed();
				Frame shown;
				bool have = false;
				while (!m_frames.empty() && (m_frames.front().pts <= now || !m_anyShown))
				{
					shown = std::move(m_frames.front());
					m_frames.pop_front();
					have = true;
					m_anyShown = true;
				}
				if (!have)
					return false;
				m_cv.notify_all(); // room for the decoder
				lock.unlock();
				image.Create(shown.width, shown.height, false);
				std::copy(shown.rgb.begin(), shown.rgb.end(), image.GetData());
				return true;
			}

			bool IsFinished() override
			{
				std::scoped_lock lock(m_mutex);
				if (m_failed)
					return true;
				if (!m_started)
					return m_decodeDone && m_frames.empty(); // nothing decodable
				const double now = Elapsed();
				if (now >= kMaxSeconds)
					return true;
				if (!m_decodeDone || !m_frames.empty())
					return false;
				double end = m_lastVideoEnd;
				if (m_audio && m_audioRate > 0)
					end = std::max(end, (double)m_pcmTotalFrames / m_audioRate);
				return now >= end;
			}

		private:
			double Elapsed() const
			{
				return std::chrono::duration<double>(Clock::now() - m_startTime).count();
			}

			// ---- decode thread ----

			void DecodeLoop(Decoder& d)
			{
				d.packet = av_packet_alloc();
				d.frame = av_frame_alloc();
				bool eof = false;
				while (!m_stop && d.packet && d.frame)
				{
					if (!eof)
					{
						const int result = av_read_frame(d.format, d.packet);
						if (result < 0)
						{
							eof = true;
							// flush both decoders
							if (d.video)
								avcodec_send_packet(d.video, nullptr);
							if (d.audio)
								avcodec_send_packet(d.audio, nullptr);
						}
						else
						{
							if (d.packet->stream_index == d.videoStream)
								avcodec_send_packet(d.video, d.packet);
							else if (d.audio && d.packet->stream_index == d.audioStream)
								avcodec_send_packet(d.audio, d.packet);
							av_packet_unref(d.packet);
						}
					}
					bool gotVideo = ReceiveVideo(d);
					bool gotAudio = ReceiveAudio(d, eof);
					if (eof && !gotVideo && !gotAudio)
						break;
				}
				std::scoped_lock lock(m_mutex);
				m_decodeDone = true;
				m_cv.notify_all();
			}

			bool ReceiveVideo(Decoder& d)
			{
				bool any = false;
				while (!m_stop && avcodec_receive_frame(d.video, d.frame) >= 0)
				{
					any = true;
					AVStream* stream = d.format->streams[d.videoStream];
					const int64_t ts = d.frame->best_effort_timestamp != AV_NOPTS_VALUE ? d.frame->best_effort_timestamp : d.frame->pts;
					double pts = ts != AV_NOPTS_VALUE ? ts * av_q2d(stream->time_base) : 0.0;
					if (d.videoStart < 0)
						d.videoStart = pts;
					pts -= d.videoStart;
					if (pts > kMaxSeconds)
					{
						av_frame_unref(d.frame);
						continue;
					}

					// fit inside the output area, keeping the picture's shape
					const int srcW = d.frame->width, srcH = d.frame->height;
					double aspect = (double)srcW / std::max(1, srcH);
					if (d.frame->sample_aspect_ratio.num > 0 && d.frame->sample_aspect_ratio.den > 0)
						aspect *= av_q2d(d.frame->sample_aspect_ratio);
					const int boxW = m_outWidth, boxH = m_outHeight;
					int dstW = boxW, dstH = (int)std::lround(boxW / aspect);
					if (dstH > boxH)
					{
						dstH = boxH;
						dstW = (int)std::lround(boxH * aspect);
					}
					dstW = std::max(2, dstW);
					dstH = std::max(2, dstH);

					d.scaler = sws_getCachedContext(d.scaler, srcW, srcH, (AVPixelFormat)d.frame->format, dstW, dstH, AV_PIX_FMT_RGB24,
						SWS_BILINEAR, nullptr, nullptr, nullptr);
					Frame out;
					out.pts = pts;
					out.width = dstW;
					out.height = dstH;
					if (d.scaler)
					{
						// rows padded for the scaler, then packed into the frame
						const int stride = FFALIGN(dstW * 3, 32);
						std::vector<uint8_t> padded((size_t)stride * dstH + 64);
						uint8_t* dst[4] = {padded.data(), nullptr, nullptr, nullptr};
						int dstStride[4] = {stride, 0, 0, 0};
						sws_scale(d.scaler, d.frame->data, d.frame->linesize, 0, srcH, dst, dstStride);
						out.rgb.resize((size_t)dstW * dstH * 3);
						for (int y = 0; y < dstH; y++)
							std::copy_n(padded.data() + (size_t)y * stride, dstW * 3, out.rgb.data() + (size_t)y * dstW * 3);
					}
					double duration = 1.0 / 30;
#if LIBAVUTIL_VERSION_MAJOR >= 58
					if (d.frame->duration > 0)
						duration = d.frame->duration * av_q2d(stream->time_base);
					else
#endif
					if (stream->avg_frame_rate.num > 0)
						duration = 1.0 / av_q2d(stream->avg_frame_rate);
					av_frame_unref(d.frame);
					if (out.rgb.empty())
						continue;

					std::unique_lock lock(m_mutex);
					m_cv.wait(lock, [this]() { return m_stop || m_frames.size() < kMaxQueuedFrames; });
					if (m_stop)
						return any;
					m_lastVideoEnd = std::max(m_lastVideoEnd, pts + duration);
					m_frames.push_back(std::move(out));
				}
				return any;
			}

			bool ReceiveAudio(Decoder& d, bool eof)
			{
				if (!d.audio || !m_audio)
					return false;
				bool any = false;
				std::vector<int16_t> converted;
				while (!m_stop && avcodec_receive_frame(d.audio, d.frame) >= 0)
				{
					any = true;
					if (!d.resampler)
					{
						AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
						if (swr_alloc_set_opts2(&d.resampler, &stereo, AV_SAMPLE_FMT_S16, m_audioRate,
								&d.audio->ch_layout, d.audio->sample_fmt, d.audio->sample_rate, 0, nullptr) < 0 ||
							swr_init(d.resampler) < 0)
						{
							swr_free(&d.resampler);
							av_frame_unref(d.frame);
							continue; // no sound, the picture still plays
						}
					}
					const int capacity = swr_get_out_samples(d.resampler, d.frame->nb_samples);
					converted.resize((size_t)std::max(0, capacity) * 2);
					uint8_t* out = (uint8_t*)converted.data();
					const int frames = swr_convert(d.resampler, &out, capacity, (const uint8_t**)d.frame->extended_data, d.frame->nb_samples);
					av_frame_unref(d.frame);
					if (frames > 0)
						AppendPcm(converted.data(), frames);
				}
				if (eof && d.resampler)
				{
					// what the resampler still holds
					const int capacity = swr_get_out_samples(d.resampler, 0);
					if (capacity > 0)
					{
						converted.resize((size_t)capacity * 2);
						uint8_t* out = (uint8_t*)converted.data();
						const int frames = swr_convert(d.resampler, &out, capacity, nullptr, 0);
						if (frames > 0)
							AppendPcm(converted.data(), frames);
					}
				}
				return any;
			}

			void AppendPcm(const int16_t* samples, int frames)
			{
				std::scoped_lock lock(m_mutex);
				if ((double)m_pcmTotalFrames / std::max(1, m_audioRate) > kMaxSeconds)
					return;
				m_pcm.insert(m_pcm.end(), samples, samples + (size_t)frames * 2);
				m_pcmTotalFrames += frames;
				m_cv.notify_all();
			}

			// ---- audio thread: feeds the output as it asks, from the moment the first frame shows ----

			void AudioLoop()
			{
				{
					std::unique_lock lock(m_mutex);
					m_cv.wait(lock, [this]() { return m_stop || m_started || (m_decodeDone && m_frames.empty()); });
					if (m_stop || !m_started)
						return;
				}
				const int blockFrames = m_audio->BlockFrames();
				std::vector<int16_t> block((size_t)blockFrames * 2);
				bool startedOutput = false;
				int silentBlocks = 0;
				while (!m_stop)
				{
					if (!m_audio->WantsBlock())
					{
						std::this_thread::sleep_for(std::chrono::milliseconds(2));
						continue;
					}
					size_t available, needed = block.size();
					bool done;
					{
						std::scoped_lock lock(m_mutex);
						available = m_pcm.size() - m_pcmRead;
						done = m_decodeDone;
						if (available >= needed || done)
						{
							const size_t take = std::min(available, needed);
							std::copy_n(m_pcm.begin() + m_pcmRead, take, block.begin());
							std::fill(block.begin() + take, block.end(), (int16_t)0);
							m_pcmRead += take;
						}
					}
					if (available < needed && !done)
					{
						std::this_thread::sleep_for(std::chrono::milliseconds(2));
						continue;
					}
					m_audio->FeedBlock(block.data());
					if (!startedOutput)
					{
						m_audio->Start();
						startedOutput = true;
					}
					if (available < needed && ++silentBlocks > 4)
						break; // everything played, a little silence after it
				}
			}

			std::unique_ptr<AudioSink> m_audio;
			int m_audioRate = 0;
			std::thread m_decodeThread, m_audioThread;
			std::mutex m_mutex;
			std::condition_variable m_cv;
			bool m_stop = false;
			bool m_failed = false;
			bool m_decodeDone = false;
			bool m_started = false;
			bool m_anyShown = false;
			Clock::time_point m_startTime{};
			std::deque<Frame> m_frames;
			double m_lastVideoEnd = 0;
			std::vector<int16_t> m_pcm;
			size_t m_pcmRead = 0;
			int64_t m_pcmTotalFrames = 0;
			std::atomic<int> m_outWidth{1920}, m_outHeight{1080};
		};
	}

	bool IsVideoPlaybackSupported()
	{
		return true;
	}

	namespace
	{
		// The rest of opening, once d->format is open: streams and decoders.
		std::unique_ptr<VideoPlayer> FinishOpening(std::unique_ptr<Decoder> d, const std::string& name, std::unique_ptr<AudioSink> audio, std::string& error)
		{
			int result = avformat_find_stream_info(d->format, nullptr);
			if (result < 0)
			{
				error = "cannot read " + name + ": " + AvError(result);
				return nullptr;
			}
			d->videoStream = av_find_best_stream(d->format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
			if (d->videoStream < 0)
			{
				error = name + " has no video";
				return nullptr;
			}
			d->video = OpenDecoder(d->format->streams[d->videoStream], error);
			if (!d->video)
				return nullptr;
			if (audio)
			{
				d->audioStream = av_find_best_stream(d->format, AVMEDIA_TYPE_AUDIO, -1, d->videoStream, nullptr, 0);
				if (d->audioStream >= 0)
				{
					std::string audioError;
					d->audio = OpenDecoder(d->format->streams[d->audioStream], audioError);
					if (!d->audio)
						error = audioError; // plays without sound
				}
			}
			// the decoder only reads the streams it uses
			for (unsigned i = 0; i < d->format->nb_streams; i++)
			{
				if ((int)i != d->videoStream && !(d->audio && (int)i == d->audioStream))
					d->format->streams[i]->discard = AVDISCARD_ALL;
			}
			if (!d->audio)
				audio.reset();
			return std::make_unique<FFmpegVideoPlayer>(std::move(d), std::move(audio));
		}
	}

	std::unique_ptr<VideoPlayer> CreateVideoPlayer(const std::filesystem::path& file, std::unique_ptr<AudioSink> audio, std::string& error)
	{
		av_log_set_level(AV_LOG_ERROR); // FFmpeg would otherwise chatter on the console
		auto d = std::make_unique<Decoder>();
		const std::u8string u8 = file.u8string(); // FFmpeg takes UTF-8 paths on every platform
		const std::string path(u8.begin(), u8.end());
		int result = avformat_open_input(&d->format, path.c_str(), nullptr, nullptr);
		if (result < 0)
		{
			error = "cannot open " + path + ": " + AvError(result);
			return nullptr;
		}
		return FinishOpening(std::move(d), path, std::move(audio), error);
	}

	std::unique_ptr<VideoPlayer> CreateVideoPlayer(const uint8_t* data, size_t size, std::unique_ptr<AudioSink> audio, std::string& error)
	{
		av_log_set_level(AV_LOG_ERROR);
		const std::string name = "the built-in boot video";
		if (!data || size == 0)
		{
			error = name + " is missing from this build";
			return nullptr;
		}
		auto d = std::make_unique<Decoder>();
		d->memory = MemoryInput{data, size, 0};
		constexpr int kBufferSize = 64 * 1024;
		auto* buffer = static_cast<uint8_t*>(av_malloc(kBufferSize));
		if (buffer)
			d->io = avio_alloc_context(buffer, kBufferSize, 0, &d->memory, &MemoryInput::Read, nullptr, &MemoryInput::Seek);
		if (!d->io)
		{
			av_free(buffer);
			error = "out of memory";
			return nullptr;
		}
		d->format = avformat_alloc_context();
		if (!d->format)
		{
			error = "out of memory";
			return nullptr;
		}
		d->format->pb = d->io;
		d->format->flags |= AVFMT_FLAG_CUSTOM_IO;
		int result = avformat_open_input(&d->format, nullptr, nullptr, nullptr); // frees format on failure
		if (result < 0)
		{
			error = "cannot open " + name + ": " + AvError(result);
			return nullptr;
		}
		return FinishOpening(std::move(d), name, std::move(audio), error);
	}
}

#endif
