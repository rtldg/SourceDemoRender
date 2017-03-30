#include "PrecompiledHeader.hpp"
#include "Interface\Application\Application.hpp"

#include "dbg.h"

#include "HLAE\HLAE.hpp"
#include "HLAE\Sampler.hpp"

extern "C"
{
	#include "libavutil\avutil.h"
	#include "libavcodec\avcodec.h"
	#include "libavformat\avformat.h"
	#include "libswscale\swscale.h"
}

#include <ppltasks.h>
#include "readerwriterqueue.h"

namespace
{
	namespace LAV
	{
		enum class ExceptionType
		{
			AllocSWSContext,
			AllocCodecContext,
			AllocAVFrame,
			AllocAVStream,
			EncoderNotFound,
		};

		const char* ExceptionTypeToString(ExceptionType code)
		{
			auto index = static_cast<int>(code);

			static const char* names[] =
			{
				"Could not allocate pixel format conversion context",
				"Could not allocate codec context",
				"Could not allocate video frame",
				"Could not allocate video stream",
				"Encoder not found"
			};

			auto retstr = names[index];

			return retstr;
		}

		struct ExceptionNullPtr
		{
			ExceptionType Code;
			const char* Description;
		};

		struct Exception
		{
			int Code;
		};

		inline void ThrowIfNull(void* ptr, ExceptionType code)
		{
			if (!ptr)
			{
				auto desc = ExceptionTypeToString(code);

				Warning("SDR LAV: %s\n", desc);

				ExceptionNullPtr info;
				info.Code = code;
				info.Description = desc;

				throw info;
			}
		}

		inline void ThrowIfFailed(int code)
		{
			if (code < 0)
			{
				Warning("SDR LAV: %d\n", code);
				
				Exception info;
				info.Code = code;

				throw info;
			}
		}

		struct ScopedSWSContext
		{
			~ScopedSWSContext()
			{
				sws_freeContext(Context);
			}

			void Assign
			(
				int width,
				int height,
				AVPixelFormat sourceformat,
				AVPixelFormat destformat
			)
			{
				Context = sws_getContext
				(
					width,
					height,
					sourceformat,
					width,
					height,
					destformat,
					0,
					nullptr,
					nullptr,
					nullptr
				);

				ThrowIfNull(Context, LAV::ExceptionType::AllocSWSContext);
			}

			SwsContext* Get()
			{
				return Context;
			}

			SwsContext* Context = nullptr;
		};

		struct ScopedAVFContext
		{
			~ScopedAVFContext()
			{
				if (Context)
				{
					if (!(Context->oformat->flags & AVFMT_NOFILE))
					{
						avio_close(Context->pb);
					}

					avformat_free_context(Context);
				}
			}

			void Assign(const char* filename)
			{
				ThrowIfFailed
				(
					avformat_alloc_output_context2(&Context, nullptr, nullptr, filename)
				);
			}

			AVFormatContext* Get() const
			{
				return Context;
			}

			AVFormatContext* Context = nullptr;
		};

		struct ScopedCodecContext
		{
			~ScopedCodecContext()
			{
				if (Context)
				{
					avcodec_free_context(&Context);
				}
			}

			void Assign(AVCodec* codec)
			{
				Context = avcodec_alloc_context3(codec);

				ThrowIfNull(Context, LAV::ExceptionType::AllocCodecContext);
			}

			AVCodecContext* Get() const
			{
				return Context;
			}

			AVCodecContext* Context = nullptr;
		};

		struct ScopedAVCFrame
		{
			~ScopedAVCFrame()
			{
				if (Frame)
				{
					av_frame_free(&Frame);
				}
			}

			void Assign(AVPixelFormat format, int width, int height)
			{
				Frame = av_frame_alloc();

				ThrowIfNull(Frame, LAV::ExceptionType::AllocAVFrame);

				Frame->format = format;
				Frame->width = width;
				Frame->height = height;

				av_frame_get_buffer(Frame, 32);

				Frame->pts = 0;
			}

			AVFrame* Get() const
			{
				return Frame;
			}

			AVFrame* Frame = nullptr;
		};

		struct ScopedAVDictionary
		{
			~ScopedAVDictionary()
			{
				av_dict_free(&Options);
			}

			AVDictionary** Get()
			{
				return &Options;
			}

			void Set(const char* key, const char* value, int flags = 0)
			{
				ThrowIfFailed
				(
					av_dict_set(Get(), key, value, flags)
				);
			}
			
			void ParseString(const char* string)
			{
				ThrowIfFailed
				(
					av_dict_parse_string(Get(), string, "=", " ", 0)
				);
			}

			AVDictionary* Options = nullptr;
		};
	}
}

namespace LAV
{
	namespace
	{
		namespace Variables
		{
			ConVar SuppressLog
			(
				"sdr_movie_suppresslog", "0", FCVAR_NEVER_AS_STRING,
				"Disable logging output from LAV",
				true, 0, true, 1
			);
		}
	}

	void LogFunction
	(
		void* avcl,
		int level,
		const char* fmt,
		va_list vl
	)
	{
		if (!Variables::SuppressLog.GetBool())
		{
			MsgV(fmt, vl);
		}
	}
}

namespace
{
	struct SDRVideoWriter
	{
		void OpenFileForWrite(const char* path)
		{
			FormatContext.Assign(path);

			if (!(FormatContext.Context->oformat->flags & AVFMT_NOFILE))
			{
				LAV::ThrowIfFailed
				(
					avio_open(&FormatContext.Context->pb, path, AVIO_FLAG_WRITE)
				);
			}
		}

		void SetEncoder(const char* name)
		{
			Encoder = avcodec_find_encoder_by_name(name);

			LAV::ThrowIfNull(Encoder, LAV::ExceptionType::EncoderNotFound);

			CodecContext.Assign(Encoder);
		}

		AVStream* AddStream()
		{
			auto retptr = avformat_new_stream(FormatContext.Get(), Encoder);

			LAV::ThrowIfNull(retptr, LAV::ExceptionType::AllocAVStream);

			return retptr;
		}

		void SetCodecParametersToStream()
		{
			LAV::ThrowIfFailed
			(
				avcodec_parameters_from_context
				(
					VideoStream->codecpar,
					CodecContext.Get()
				)
			);
		}

		void OpenEncoder(AVDictionary** options)
		{
			SetCodecParametersToStream();

			LAV::ThrowIfFailed
			(
				avcodec_open2(CodecContext.Get(), Encoder, options)
			);
		}

		void WriteHeader()
		{
			LAV::ThrowIfFailed
			(
				avformat_write_header(FormatContext.Get(), nullptr)
			);
		}

		void WriteTrailer()
		{
			LAV::ThrowIfFailed
			(
				av_write_trailer(FormatContext.Get())
			);
		}

		void SetRGB24Input(uint8_t* buffer, int width, int height)
		{
			uint8_t* sourceplanes[] =
			{
				buffer
			};

			/*
				3 for 3 bytes per pixel
			*/
			int sourcestrides[] =
			{
				width * 3
			};

			if (CodecContext.Context->pix_fmt == AV_PIX_FMT_RGB24)
			{
				MainFrame.Frame->data[0] = sourceplanes[0];
				MainFrame.Frame->linesize[0] = sourcestrides[0];
			}

			else
			{
				sws_scale
				(
					FormatConverter.Get(),
					sourceplanes,
					sourcestrides,
					0,
					height,
					MainFrame.Frame->data,
					MainFrame.Frame->linesize
				);
			}
		}

		void SendRawFrame()
		{
			MainFrame.Frame->pts = CurrentPresentation++;

			auto ret = avcodec_send_frame
			(
				CodecContext.Get(),
				MainFrame.Get()
			);

			ReceivePacketFrame();
		}

		void SendFlushFrame()
		{
			auto ret = avcodec_send_frame
			(
				CodecContext.Get(),
				nullptr
			);

			ReceivePacketFrame();
		}

		void Finish()
		{
			SendFlushFrame();
			WriteTrailer();
		}

		void ReceivePacketFrame()
		{
			int status = 0;

			AVPacket packet;
			av_init_packet(&packet);

			while (status == 0)
			{
				status = avcodec_receive_packet
				(
					CodecContext.Get(),
					&packet
				);

				if (status != 0)
				{
					return;
				}

				WriteEncodedPacket(packet);
			}
		}

		void WriteEncodedPacket(AVPacket& packet)
		{
			packet.stream_index = VideoStream->index;
			packet.duration = 1;

			av_packet_rescale_ts
			(
				&packet,
				CodecContext.Context->time_base,
				VideoStream->time_base
			);

			LAV::ThrowIfFailed
			(
				av_interleaved_write_frame
				(
					FormatContext.Get(),
					&packet
				)
			);
		}

		LAV::ScopedAVFContext FormatContext;
		
		AVCodec* Encoder = nullptr;
		LAV::ScopedCodecContext CodecContext;

		/*
			Conversion from RGB24 to whatever specified
		*/
		LAV::ScopedSWSContext FormatConverter;

		/*
			Incremented and written to for every sent frame
		*/
		int64_t CurrentPresentation = 0;
		AVStream* VideoStream = nullptr;
		LAV::ScopedAVCFrame MainFrame;
	};

	struct MovieData : public SDR::Sampler::IFramePrinter
	{
		struct FutureSampleData
		{
			double Time;
			std::vector<uint8_t> Data;
		};

		using FrameQueueType = moodycamel::ReaderWriterQueue<FutureSampleData>;

		enum
		{
			/*
				Order: Red Green Blue
			*/
			BytesPerPixel = 3
		};

		/*
			Not a threadsafe function as we only operate on
			a single AVFrame
		*/
		virtual void Print(uint8_t* data) override
		{
			/*
				The reason we override the default VID_ProcessMovieFrame is
				because that one creates a local CUtlBuffer for every frame and then destroys it.
				Better that we store them ourselves and iterate over the buffered frames to
				write them out in another thread.
			*/

			Video->SetRGB24Input(data, Width, Height);
			Video->SendRawFrame();
		}

		uint32_t GetRGB24ImageSize() const
		{
			return (Width * Height) * BytesPerPixel;
		}

		bool IsStarted = false;

		uint32_t Width;
		uint32_t Height;

		double SamplingTime = 0.0;

		std::unique_ptr<SDRVideoWriter> Video;

		std::unique_ptr<SDR::Sampler::EasyByteSampler> Sampler;

		int32_t BufferedFrames = 0;
		std::unique_ptr<FrameQueueType> FramesToSampleBuffer;
		std::thread FrameHandlerThread;
	};

	MovieData CurrentMovie;
	std::atomic_bool ShouldStopFrameThread = false;

	void SDR_MovieShutdown()
	{
		if (!CurrentMovie.IsStarted)
		{
			return;
		}

		if (!ShouldStopFrameThread)
		{
			ShouldStopFrameThread = true;
			CurrentMovie.FrameHandlerThread.join();
		}

		CurrentMovie = MovieData();
	}

	/*
		In case endmovie never gets called,
		this handles the plugin_unload
	*/
	SDR::PluginShutdownFunctionAdder A1(SDR_MovieShutdown);
}

namespace
{
	namespace Variables
	{
		ConVar FrameBufferSize
		(
			"sdr_frame_buffersize", "256", FCVAR_NEVER_AS_STRING,
			"How many frames that are allowed to be buffered up for encoding. "
			"This value can be lowered or increased depending your available RAM. "
			"Keep in mind the sizes of an uncompressed RGB24 frame: \n"
			"1280x720    : 2.7 MB\n"
			"1920x1080   : 5.9 MB\n"
			"Calculation : (((x * y) * 3) / 1024) / 1024"
			"\n"
			"Multiply the frame size with the buffer size to one that fits you.\n"
			"*** Using too high of a buffer size might eventually crash the application "
			"if there no longer is any available memory ***\n"
			"\n"
			"The frame buffer queue will only build up and fall behind when the encoding "
			"is taking too long, consider not using too low of a profile.",
			true, 8, true, 384
		);

		ConVar FrameRate
		(
			"sdr_render_framerate", "60", FCVAR_NEVER_AS_STRING,
			"Movie output framerate",
			true, 30, true, 1000
		);

		ConVar Exposure
		(
			"sdr_render_exposure", "0.5", FCVAR_NEVER_AS_STRING,
			"Frame exposure fraction",
			true, 0, true, 1
		);

		ConVar SamplesPerSecond
		(
			"sdr_render_samplespersecond", "600", FCVAR_NEVER_AS_STRING,
			"Game framerate in samples",
			true, 0, false, 0
		);

		ConVar FrameStrength
		(
			"sdr_render_framestrength", "1.0", FCVAR_NEVER_AS_STRING,
			"Controls clearing of the sampling buffer upon framing. "
			"The lower the value the more cross-frame motion blur",
			true, 0, true, 1
		);

		ConVar SampleMethod
		(
			"sdr_render_samplemethod", "1", FCVAR_NEVER_AS_STRING,
			"Selects the integral approximation method: "
			"0: 1 point, rectangle method, 1: 2 point, trapezoidal rule",
			true, 0, true, 1
		);

		ConVar OutputDirectory
		(
			"sdr_outputdir", "", 0,
			"Where to save the output frames. UTF8 names are not supported in Source"
		);

		ConVar FlashWindow
		(
			"sdr_endmovieflash", "0", FCVAR_NEVER_AS_STRING,
			"Flash window when endmovie is called",
			true, 0, true, 1
		);

		namespace Audio
		{
			ConVar Enable
			(
				"sdr_audio_enable", "0", FCVAR_NEVER_AS_STRING,
				"Process audio as well",
				true, 0, true, 1
			);
		}

		namespace Video
		{
			ConVar PixelFormat
			(
				"sdr_movie_encoder_pxformat", "i420", 0,
				"X264 pixel format Does nothing in png sequence. "
				"Values: I420, I444, NV12. See https://wiki.videolan.org/YUV/"
			);

			ConVar CRF
			(
				"sdr_movie_encoder_crf", "10", 0,
				"Constant rate factor value. Values: 0 (best) - 51 (worst). "
				"See https://trac.ffmpeg.org/wiki/Encode/H.264"
			);

			ConVar Preset
			{
				"sdr_movie_encoder_preset", "medium", 0,
				"X264 encoder preset. See https://trac.ffmpeg.org/wiki/Encode/H.264\n"
				"Important note: Optimally, do not use a too low of a profile as the streaming "
				"needs to be somewhat realtime.",
				[](IConVar* var, const char* oldstr, float oldfloat)
				{
					auto newstr = Preset.GetString();

					auto slowpresets =
					{
						"slow",
						"slower",
						"veryslow",
						"placebo"
					};

					for (auto preset : slowpresets)
					{
						if (_strcmpi(newstr, preset) == 0)
						{
							Warning
							(
								"SDR: Slow encoder preset chosen, "
								"this might not work very well for realtime\n"
							);

							return;
						}
					}
				}
			};

			ConVar Tune
			(
				"sdr_movie_encoder_tune", "", 0,
				"X264 encoder tune. See https://trac.ffmpeg.org/wiki/Encode/H.264"
			);
		}
	}

	void FrameThreadHandler()
	{
		auto& interfaces = SDR::GetEngineInterfaces();
		auto& movie = CurrentMovie;
		auto& nonreadyframes = movie.FramesToSampleBuffer;

		auto sampleframerate = 1.0 / static_cast<double>(Variables::SamplesPerSecond.GetInt());

		MovieData::FutureSampleData sample;
		sample.Data.reserve(movie.GetRGB24ImageSize());

		while (!ShouldStopFrameThread)
		{
			while (nonreadyframes->try_dequeue(sample))
			{
				movie.BufferedFrames--;

				auto time = sample.Time;

				if (movie.Sampler->CanSkipConstant(time, sampleframerate))
				{
					movie.Sampler->Sample(nullptr, time);
				}

				else
				{
					auto data = sample.Data.data();
					movie.Sampler->Sample(data, time);
				}
			}
		}
	}

	#if 0
	namespace Module_BaseTemplateMask
	{
		auto Pattern = SDR::MemoryPattern("");
		auto Mask = "";

		void __cdecl Override();

		using ThisFunction = decltype(Override)*;

		SDR::HookModuleMask<ThisFunction> ThisHook
		{
			"", "", Override, Pattern, Mask
		};

		void __cdecl Override()
		{

		}
	}

	namespace Module_BaseTemplateStatic
	{
		void __cdecl Override();

		using ThisFunction = decltype(Override)*;

		SDR::HookModuleStaticAddress<ThisFunction> ThisHook
		{
			"", "", Override, 0x00000000
		};

		void __cdecl Override()
		{

		}
	}
	#endif

	namespace Module_StartMovie
	{
		/*
			0x100BCAC0 static IDA address May 22 2016
		*/
		auto Pattern = SDR::MemoryPattern
		(
			"\x55\x8B\xEC\x81\xEC\x00\x00\x00\x00\xA1\x00\x00"
			"\x00\x00\xD9\x45\x18\x56\x57\xF3\x0F\x10\x40\x00"
		);

		auto Mask = "xxxxx????x????xxxxxxxxx?";

		void __cdecl Override
		(
			const char* filename, int flags, int width, int height, float framerate, int jpegquality, int unk
		);

		using ThisFunction = decltype(Override)*;

		SDR::HookModuleMask<ThisFunction> ThisHook
		{
			"engine.dll", "CL_StartMovie", Override, Pattern, Mask
		};

		/*
			The 7th parameter (unk) was been added in Source 2013, it's not there in Source 2007
		*/
		void __cdecl Override
		(
			const char* filename, int flags, int width, int height, float framerate, int jpegquality, int unk
		)
		{
			auto sdrpath = Variables::OutputDirectory.GetString();

			auto res = SHCreateDirectoryExA(nullptr, sdrpath, nullptr);

			switch (res)
			{
				case ERROR_SUCCESS:
				case ERROR_ALREADY_EXISTS:
				case ERROR_FILE_EXISTS:
				{
					break;
				}

				case ERROR_BAD_PATHNAME:
				case ERROR_PATH_NOT_FOUND:
				case ERROR_FILENAME_EXCED_RANGE:
				{
					Warning("SDR: Movie output path is invalid\n");
					return;
				}

				case ERROR_CANCELLED:
				{
					Warning("SDR: Extra directories were created but are hidden, aborting\n");
					return;
				}

				default:
				{
					Warning("SDR: Some unknown error happened when starting movie, related to sdr_outputdir\n");
					return;
				}
			}

			auto& movie = CurrentMovie;
				
			movie.Width = width;
			movie.Height = height;

			{
				try
				{
					movie.Video = std::make_unique<SDRVideoWriter>();

					auto vidwriter = movie.Video.get();

					AVRational timebase;
					timebase.num = 1;
					timebase.den = Variables::FrameRate.GetInt();

					bool isx264;

					{
						auto targetpath = Variables::OutputDirectory.GetString();
						auto finalname = CUtlString::PathJoin(targetpath, filename);

						CUtlString filenamestr = filename;
						auto extension = filenamestr.GetExtension();

						/*
							If the user entered png but no digit format
							we have to add it for them
						*/
						if (extension == "png")
						{
							if (!filenamestr.MatchesPattern("*%*d.png"))
							{
								filenamestr = filenamestr.StripExtension();
								filenamestr += "%05d.png";

								finalname = CUtlString::PathJoin(targetpath, filenamestr);
							}
						}

						else if (extension == "")
						{
							finalname += ".avi";
						}

						vidwriter->OpenFileForWrite(finalname);

						auto formatcontext = vidwriter->FormatContext.Get();
						auto oformat = formatcontext->oformat;

						if (strcmp(oformat->name, "image2") == 0)
						{
							isx264 = false;
							vidwriter->SetEncoder("png");
						}

						else
						{
							isx264 = true;
							vidwriter->SetEncoder("libx264");
						}
					}

					auto pxformat = AV_PIX_FMT_NONE;

					vidwriter->VideoStream = vidwriter->AddStream();
					vidwriter->VideoStream->time_base = timebase;

					auto codeccontext = vidwriter->CodecContext.Get();
					codeccontext->codec_type = AVMEDIA_TYPE_VIDEO;
					codeccontext->width = width;
					codeccontext->height = height;
					codeccontext->time_base = timebase;

					if (isx264)
					{
						auto pxformatstr = Variables::Video::PixelFormat.GetString();

						auto pxformatnames =
						{
							"i420",
							"i444",
							"nv12",
						};

						auto pxformattypes =
						{
							AV_PIX_FMT_YUV420P,
							AV_PIX_FMT_YUV444P,
							AV_PIX_FMT_NV12,
						};

						int pxformatindex = 0;

						for (auto name : pxformatnames)
						{
							if (_strcmpi(pxformatstr, name) == 0)
							{
								pxformat = *(pxformattypes.begin() + pxformatindex);
								break;
							}

							++pxformatindex;
						}

						if (pxformat == AV_PIX_FMT_NONE)
						{
							Msg("SDR: No pixel format selected, using I420\n");
							pxformat = AV_PIX_FMT_YUV420P;
						}

						codeccontext->codec_id = AV_CODEC_ID_H264;

						vidwriter->FormatConverter.Assign
						(
							width,
							height,
							AV_PIX_FMT_RGB24,
							pxformat
						);
					}

					else
					{
						pxformat = AV_PIX_FMT_RGB24;
						codeccontext->codec_id = AV_CODEC_ID_PNG;
					}

					codeccontext->pix_fmt = pxformat;
		
					/*
						Not setting this will leave different colors across
						multiple programs
					*/
					codeccontext->colorspace = AVCOL_SPC_BT470BG;
					codeccontext->color_range = AVCOL_RANGE_MPEG;

					{
						if (isx264)
						{
							auto preset = Variables::Video::Preset.GetString();
							auto tune = Variables::Video::Tune.GetString();
							auto crf = Variables::Video::CRF.GetString();

							LAV::ScopedAVDictionary options;
							options.Set("preset", preset);

							if (strlen(tune) > 0)
							{
								options.Set("tune", tune);
							}

							options.Set("crf", crf);


							vidwriter->OpenEncoder(options.Get());
						}

						else
						{
							vidwriter->OpenEncoder(nullptr);
						}
					}

					vidwriter->MainFrame.Assign
					(
						pxformat,
						width,
						height
					);

					vidwriter->WriteHeader();
				}
					
				catch (const LAV::Exception& error)
				{
					return;
				}

				catch (const LAV::ExceptionNullPtr& error)
				{
					return;
				}
			}

			ThisHook.GetOriginal()(filename, flags, width, height, framerate, jpegquality, unk);

			/*
				The original function sets host_framerate to 30 so we override it
			*/
			auto hostframerate = g_pCVar->FindVar("host_framerate");
			hostframerate->SetValue(Variables::SamplesPerSecond.GetInt());

			auto framepitch = HLAE::CalcPitch(width, MovieData::BytesPerPixel, 1);
			auto movieframeratems = 1.0 / static_cast<double>(Variables::FrameRate.GetInt());
			auto moviexposure = Variables::Exposure.GetFloat();
			auto movieframestrength = Variables::FrameStrength.GetFloat();
				
			using SampleMethod = SDR::Sampler::EasySamplerSettings::Method;
			SampleMethod moviemethod;
				
			switch (Variables::SampleMethod.GetInt())
			{
				case 0:
				{
					moviemethod = SampleMethod::ESM_Rectangle;
					break;
				}

				case 1:
				{
					moviemethod = SampleMethod::ESM_Trapezoid;
					break;
				}
			}

			SDR::Sampler::EasySamplerSettings settings
			(
				MovieData::BytesPerPixel * width, height,
				moviemethod,
				movieframeratems,
				0.0,
				moviexposure,
				movieframestrength
			);

			using SDR::Sampler::EasyByteSampler;
			auto size = movie.GetRGB24ImageSize();

			movie.IsStarted = true;

			movie.Sampler = std::make_unique<EasyByteSampler>(settings, framepitch, &movie);

			movie.FramesToSampleBuffer = std::make_unique<MovieData::FrameQueueType>(128);

			ShouldStopFrameThread = false;
			movie.FrameHandlerThread = std::thread(FrameThreadHandler);
		}
	}

	namespace Module_CL_EndMovie
	{
		/*
			0x100BAE40 static IDA address May 22 2016
		*/
		auto Pattern = SDR::MemoryPattern
		(
			"\x80\x3D\x00\x00\x00\x00\x00\x0F\x84\x00\x00\x00\x00"
			"\xD9\x05\x00\x00\x00\x00\x51\xB9\x00\x00\x00\x00"
		);

		auto Mask = "xx?????xx????xx????xx????";

		void __cdecl Override();

		using ThisFunction = decltype(Override)*;

		SDR::HookModuleMask<ThisFunction> ThisHook
		{
			"engine.dll", "CL_EndMovie", Override, Pattern, Mask
		};

		void __cdecl Override()
		{
			ThisHook.GetOriginal()();

			auto hostframerate = g_pCVar->FindVar("host_framerate");
			hostframerate->SetValue(0);

			Msg("SDR: Ending movie, if there are buffered frames this might take a moment\n");

			auto task = concurrency::create_task([]()
			{
				/*
					Let the worker thread complete the sampling and
					giving the frames to the encoder
				*/
				if (!ShouldStopFrameThread)
				{
					ShouldStopFrameThread = true;
					CurrentMovie.FrameHandlerThread.join();
				}

				/*
					Let the encoder finish all the delayed frames
				*/
				CurrentMovie.Video->Finish();
			});

			task.then([]()
			{
				SDR_MovieShutdown();

				if (Variables::FlashWindow.GetBool())
				{
					auto& interfaces = SDR::GetEngineInterfaces();
					interfaces.EngineClient->FlashWindow();
				}

				Msg("SDR: Movie is now complete\n");
			});
		}
	}

	namespace Module_CVideoMode_WriteMovieFrame
	{
		/*
			0x102011B0 static IDA address June 3 2016
		*/
		auto Pattern = SDR::MemoryPattern
		(
			"\x55\x8B\xEC\x51\x80\x3D\x00\x00\x00\x00\x00\x53"
			"\x8B\x5D\x08\x57\x8B\xF9\x8B\x83\x00\x00\x00\x00"
		);

		auto Mask = "xxxxxx?????xxxxxxxxx????";

		/*
			The "thisptr" in this context is a CVideoMode_MaterialSystem in this structure:
			
			CVideoMode_MaterialSystem
				CVideoMode_Common
					IVideoMode

			WriteMovieFrame belongs to CVideoMode_Common and ReadScreenPixels overriden
			by CVideoMode_MaterialSystem.
			The global engine variable "videomode" is of type CVideoMode_MaterialSystem
			which is what called WriteMovieFrame.
			
			For more usage see: VideoMode_Create (0x10201130) and VideoMode_Destroy (0x10201190)
			Static IDA addresses June 3 2016

			The purpose of overriding this function completely is to prevent the constant image buffer
			allocation that Valve does every movie frame. We just provide one buffer that gets reused.
		*/
		void __fastcall Override
		(
			void* thisptr, void* edx, void* info
		)
		{
			/*
				0x101FFF80 static IDA address June 3 2016
			*/
			static auto readscreenpxaddr = SDR::GetAddressFromPattern
			(
				"engine.dll",
				SDR::MemoryPattern
				(
					"\x55\x8B\xEC\x83\xEC\x14\x80\x3D\x00\x00\x00\x00\x00"
					"\x0F\x85\x00\x00\x00\x00\x8B\x0D\x00\x00\x00\x00"
				),
				"xxxxxxxx?????xx????xx????"
			);

			using ReadScreenPxType = void(__fastcall*)
			(
				void*,
				void*,
				int x,
				int y,
				int w,
				int h,
				void* buffer,
				int format
			);

			static auto readscreenpxfunc = static_cast<ReadScreenPxType>(readscreenpxaddr);

			auto width = CurrentMovie.Width;
			auto height = CurrentMovie.Height;

			auto& movie = CurrentMovie;

			auto sampleframerate = 1.0 / static_cast<double>(Variables::SamplesPerSecond.GetInt());
			auto& time = movie.SamplingTime;

			MovieData::FutureSampleData newsample;
			newsample.Time = time;
			newsample.Data.resize(movie.GetRGB24ImageSize());

			/*
				3 = IMAGE_FORMAT_BGR888
				2 = IMAGE_FORMAT_RGB888
			*/
			readscreenpxfunc(thisptr, edx, 0, 0, width, height, newsample.Data.data(), 2);

			auto buffersize = Variables::FrameBufferSize.GetInt();

			/*
				Encoder is falling behind, too much input with too little output
			*/
			if (movie.BufferedFrames > buffersize)
			{
				Warning("SDR: Too many buffered frames, waiting for encoder\n");

				while (movie.BufferedFrames > 1)
				{
					std::this_thread::sleep_for(1ms);
				}

				Warning
				(
					"SDR: Encoder caught up, consider using faster encoding settings or "
					"increasing sdr_frame_buffersize.\n"
					R"(Type "help sdr_frame_buffersize" for more information.)" "\n"
				);
			}

			movie.BufferedFrames++;
			movie.FramesToSampleBuffer->enqueue(std::move(newsample));

			time += sampleframerate;
		}

		using ThisFunction = decltype(Override)*;

		SDR::HookModuleMask<ThisFunction> ThisHook
		{
			"engine.dll", "CVideoMode_WriteMovieFrame", Override, Pattern, Mask
		};
	}

	namespace Module_SNDRecordBuffer
	{
		/*
			0x1007C710 static IDA address March 21 2017
		*/
		auto Pattern = SDR::MemoryPattern
		(
			"\x55\x8B\xEC\x8B\x0D\x00\x00\x00\x00\x53\x56\x57\x85"
			"\xC9\x74\x0B\x8B\x01\x8B\x40\x38\xFF\xD0\x84\xC0\x75"
			"\x0D\x80\x3D\x00\x00\x00\x00\x00\x0F\x84\x00\x00\x00"
			"\x00"
		);
		
		auto Mask =
		(
			"xxxxx????xxxxxxxxxxxxxxxxxxxx?????xx????"
		);

		void __cdecl Override();

		using ThisFunction = decltype(Override)*;

		SDR::HookModuleMask<ThisFunction> ThisHook
		{
			"engine.dll", "SNDRecordBuffer", Override, Pattern, Mask
		};

		void __cdecl Override()
		{
			if (Variables::Audio::Enable.GetBool())
			{
				ThisHook.GetOriginal()();
			}
		}
	}

	namespace Module_WaveCreateTmpFile
	{
		/*
			0x1008EC90 static IDA address March 21 2017
		*/
		auto Pattern = SDR::MemoryPattern
		(
			"\x55\x8B\xEC\x81\xEC\x00\x00\x00\x00\x8D\x85\x00\x00"
			"\x00\x00\x57\x68\x00\x00\x00\x00\x50\xFF\x75\x08\xE8"
			"\x00\x00\x00\x00\x68\x00\x00\x00\x00\x8D\x85\x00\x00"
			"\x00\x00\x68\x00\x00\x00\x00\x50\xE8\x00\x00\x00\x00"
			"\x8B\x0D\x00\x00\x00\x00\x8D\x95\x00\x00\x00\x00\x83"
			"\xC4\x18\x83\xC1\x04\x8B\x01\x6A\x00\x68\x00\x00\x00"
			"\x00\x52\xFF\x50\x08\x8B\xF8\x85\xFF\x0F\x84\x00\x00"
			"\x00\x00"
		);
		
		auto Mask =
		(
			"xxxxx????xx????xx????xxxxx????x????xx????x????xx????"
			"xx????xx????xxxxxxxxxxx????xxxxxxxxxx????"
		);

		void __cdecl Override
		(
			const char* filename, int rate, int bits, int channels
		);

		using ThisFunction = decltype(Override)*;

		SDR::HookModuleMask<ThisFunction> ThisHook
		{
			"engine.dll", "WaveCreateTmpFile", Override, Pattern, Mask
		};

		/*
			"rate" will always be 44100
			"bits" will always be 16
			"channels" will always be 2

			According to engine\audio\private\snd_mix.cpp @ 4003
		*/
		void __cdecl Override
		(
			const char* filename, int rate, int bits, int channels
		)
		{
			if (Variables::Audio::Enable.GetBool())
			{
				ThisHook.GetOriginal()(filename, rate, bits, channels);
			}
		}
	}

	namespace Module_WaveAppendTmpFile
	{
		/*
			0x1008EBE0 static IDA address March 21 2017
		*/
		auto Pattern = SDR::MemoryPattern
		(
			"\x55\x8B\xEC\x81\xEC\x00\x00\x00\x00\x8D\x85\x00\x00"
			"\x00\x00\x57\x68\x00\x00\x00\x00\x50\xFF\x75\x08\xE8"
			"\x00\x00\x00\x00\x68\x00\x00\x00\x00\x8D\x85\x00\x00"
			"\x00\x00\x68\x00\x00\x00\x00\x50\xE8\x00\x00\x00\x00"
			"\x8B\x0D\x00\x00\x00\x00\x8D\x95\x00\x00\x00\x00\x83"
			"\xC4\x18\x83\xC1\x04\x8B\x01\x6A\x00\x68\x00\x00\x00"
			"\x00\x52\xFF\x50\x08\x8B\xF8\x85\xFF\x74\x47"
		);

		auto Mask =
		(
			"xxxxx????xx????xx????xxxxx????x????xx????x????xx????"
			"xx????xx????xxxxxxxxxxx????xxxxxxxxxx"
		);

		void __cdecl Override
		(
			const char* filename, void* buffer, int samplebits, int samplecount
		);

		using ThisFunction = decltype(Override)*;

		SDR::HookModuleMask<ThisFunction> ThisHook
		{
			"engine.dll", "WaveAppendTmpFile", Override, Pattern, Mask
		};

		/*
			"samplebits" will always be 16

			According to engine\audio\private\snd_mix.cpp @ 4074
		*/
		void __cdecl Override
		(
			const char* filename, void* buffer, int samplebits, int samplecount
		)
		{
			if (Variables::Audio::Enable.GetBool())
			{
				ThisHook.GetOriginal()(filename, buffer, samplebits, samplecount);
			}
		}
	}

	namespace Module_WaveFixupTmpFile
	{
		/*
			0x1008EE30 static IDA address March 21 2017
		*/
		auto Pattern = SDR::MemoryPattern
		(
			"\x55\x8B\xEC\x81\xEC\x00\x00\x00\x00\x8D\x85\x00\x00"
			"\x00\x00\x56\x68\x00\x00\x00\x00\x50\xFF\x75\x08\xE8"
			"\x00\x00\x00\x00\x68\x00\x00\x00\x00\x8D\x85\x00\x00"
			"\x00\x00\x68\x00\x00\x00\x00\x50\xE8\x00\x00\x00\x00"
			"\x8B\x0D\x00\x00\x00\x00\x8D\x95\x00\x00\x00\x00\x83"
			"\xC4\x18\x83\xC1\x04\x8B\x01\x6A\x00\x68\x00\x00\x00"
			"\x00"
		);
		
		auto Mask =
		(
			"xxxxx????xx????xx????xxxxx????x????xx????x????xx????x"
			"x????xx????xxxxxxxxxxx????"
		);

		void __cdecl Override(const char* filename);

		using ThisFunction = decltype(Override)*;

		SDR::HookModuleMask<ThisFunction> ThisHook
		{
			"engine.dll", "WaveFixupTmpFile", Override, Pattern, Mask
		};

		/*
			Gets called when the movie is ended
		*/
		void __cdecl Override(const char* filename)
		{
			if (Variables::Audio::Enable.GetBool())
			{
				ThisHook.GetOriginal()(filename);
			}
		}
	}
}
