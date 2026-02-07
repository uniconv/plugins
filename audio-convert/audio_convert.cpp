/**
 * uniconv Native Plugin: audio-convert
 *
 * Audio format conversion and audio extraction from video files.
 * Supports: mp3, wav, flac, aac, ogg, opus, m4a, wma, aiff, alac, ac3.
 *
 * Supported options:
 *   --bitrate      Output bitrate (e.g., "128k", "320k")
 *   --sample-rate  Output sample rate in Hz
 *   --channels     Number of output channels (1=mono, 2=stereo)
 *   --volume       Volume adjustment factor (1.0 = original)
 *   --start        Start time in seconds
 *   --duration     Duration in seconds
 *
 * Build:
 *   mkdir build && cd build
 *   cmake ..
 *   cmake --build .
 */

#include <uniconv/plugin_api.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

// Plugin info
static const char *targets[] = {
    "mp3", "wav", "flac", "aac", "ogg", "opus",
    "m4a", "wma", "aiff", "alac", "ac3", nullptr};

static const char *input_formats[] = {
    "mp3", "wav", "flac", "aac", "ogg", "opus",
    "m4a", "wma", "aiff", "alac", "ac3",
    "amr", "ape",
    "mp4", "mov", "avi", "webm", "mkv", "m4v",
    nullptr};

// Data type information
static UniconvDataType input_types[] = {
    UNICONV_DATA_AUDIO, UNICONV_DATA_VIDEO, UNICONV_DATA_FILE, (UniconvDataType)0};
static UniconvDataType output_types[] = {
    UNICONV_DATA_AUDIO, UNICONV_DATA_FILE, (UniconvDataType)0};

static UniconvPluginInfo plugin_info = {
    .name = "audio-convert",
    .scope = "audio-convert",
    .version = "1.0.0",
    .description = "Audio format conversion and audio extraction from video",
    .targets = targets,
    .input_formats = input_formats,
    .input_types = input_types,
    .output_types = output_types};

namespace
{

    // ---------------------------------------------------------------
    // Audio codec mapping table
    // ---------------------------------------------------------------

    struct AudioCodecInfo
    {
        const char *extension;
        AVCodecID codec_id;
        const char *format_name;
        bool lossless;
    };

    static const AudioCodecInfo codec_table[] = {
        {"mp3", AV_CODEC_ID_MP3, "mp3", false},
        {"wav", AV_CODEC_ID_PCM_S16LE, "wav", true},
        {"flac", AV_CODEC_ID_FLAC, "flac", true},
        {"aac", AV_CODEC_ID_AAC, "adts", false},
        {"ogg", AV_CODEC_ID_VORBIS, "ogg", false},
        {"opus", AV_CODEC_ID_OPUS, "ogg", false},
        {"m4a", AV_CODEC_ID_AAC, "ipod", false},
        {"wma", AV_CODEC_ID_WMAV2, "asf", false},
        {"aiff", AV_CODEC_ID_PCM_S16BE, "aiff", true},
        {"alac", AV_CODEC_ID_ALAC, "ipod", true},
        {"ac3", AV_CODEC_ID_AC3, "ac3", false},
        {nullptr, AV_CODEC_ID_NONE, nullptr, false}};

    const AudioCodecInfo *find_codec_info(const std::string &ext)
    {
        for (int i = 0; codec_table[i].extension; i++)
        {
            if (ext == codec_table[i].extension)
                return &codec_table[i];
        }
        return nullptr;
    }

    // ---------------------------------------------------------------
    // RAII wrappers for FFmpeg resources
    // ---------------------------------------------------------------

    struct AVFormatContextDeleter
    {
        void operator()(AVFormatContext *ctx) const
        {
            if (ctx)
                avformat_close_input(&ctx);
        }
    };

    struct AVFormatContextOutputDeleter
    {
        void operator()(AVFormatContext *ctx) const
        {
            if (ctx)
            {
                if (ctx->pb)
                    avio_closep(&ctx->pb);
                avformat_free_context(ctx);
            }
        }
    };

    struct AVCodecContextDeleter
    {
        void operator()(AVCodecContext *ctx) const
        {
            if (ctx)
                avcodec_free_context(&ctx);
        }
    };

    struct AVFrameDeleter
    {
        void operator()(AVFrame *frame) const
        {
            if (frame)
                av_frame_free(&frame);
        }
    };

    struct AVPacketDeleter
    {
        void operator()(AVPacket *pkt) const
        {
            if (pkt)
                av_packet_free(&pkt);
        }
    };

    struct SwrContextDeleter
    {
        void operator()(SwrContext *ctx) const
        {
            if (ctx)
                swr_free(&ctx);
        }
    };

    struct AVFilterGraphDeleter
    {
        void operator()(AVFilterGraph *graph) const
        {
            if (graph)
                avfilter_graph_free(&graph);
        }
    };

    using FormatContextPtr = std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;
    using FormatContextOutputPtr = std::unique_ptr<AVFormatContext, AVFormatContextOutputDeleter>;
    using CodecContextPtr = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;
    using FramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;
    using PacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;
    using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;
    using FilterGraphPtr = std::unique_ptr<AVFilterGraph, AVFilterGraphDeleter>;

    // ---------------------------------------------------------------
    // Helper functions
    // ---------------------------------------------------------------

    std::string to_lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c)
                       { return std::tolower(c); });
        return s;
    }

    bool file_exists(const std::string &path)
    {
        FILE *f = fopen(path.c_str(), "rb");
        if (f)
        {
            fclose(f);
            return true;
        }
        return false;
    }

    size_t get_file_size(const std::string &path)
    {
        FILE *f = fopen(path.c_str(), "rb");
        if (!f)
            return 0;
        fseek(f, 0, SEEK_END);
        size_t size = ftell(f);
        fclose(f);
        return size;
    }

    std::string get_option(const UniconvRequest *req, const char *key)
    {
        if (req->get_plugin_option && req->options_ctx)
        {
            const char *val = req->get_plugin_option(key, req->options_ctx);
            if (val)
                return val;
        }
        if (req->get_core_option && req->options_ctx)
        {
            const char *val = req->get_core_option(key, req->options_ctx);
            if (val)
                return val;
        }
        return "";
    }

    int get_option_int(const UniconvRequest *req, const char *key, int default_val)
    {
        std::string val = get_option(req, key);
        if (val.empty())
            return default_val;
        try
        {
            return std::stoi(val);
        }
        catch (...)
        {
            return default_val;
        }
    }

    float get_option_float(const UniconvRequest *req, const char *key, float default_val)
    {
        std::string val = get_option(req, key);
        if (val.empty())
            return default_val;
        try
        {
            return std::stof(val);
        }
        catch (...)
        {
            return default_val;
        }
    }

    std::string get_option_string(const UniconvRequest *req, const char *key)
    {
        return get_option(req, key);
    }

    /**
     * Parse bitrate string like "320k" or "128k" into integer bits/sec.
     * Returns 0 if empty or unparsable.
     */
    int64_t parse_bitrate(const std::string &s)
    {
        if (s.empty())
            return 0;
        try
        {
            std::string lower = to_lower(s);
            size_t pos = 0;
            long val = std::stol(lower, &pos);
            if (pos < lower.size() && lower[pos] == 'k')
                return val * 1000;
            if (pos < lower.size() && lower[pos] == 'm')
                return val * 1000000;
            return val;
        }
        catch (...)
        {
            return 0;
        }
    }

    // ---------------------------------------------------------------
    // AudioConverter class
    // ---------------------------------------------------------------

    class AudioConverter
    {
    public:
        struct Options
        {
            std::string target;
            std::string bitrate;     // e.g. "320k"
            int sample_rate = 0;     // 0 = keep original
            int channels = 0;        // 0 = keep original
            float volume = 1.0f;     // 1.0 = no change
            float start = 0.0f;
            float duration = 0.0f;   // 0 = full duration
        };

        std::string convert(const std::string &input_path,
                            const std::string &output_path,
                            const Options &opts)
        {
            const AudioCodecInfo *codec_info = find_codec_info(opts.target);
            if (!codec_info)
            {
                return "Unsupported target format: " + opts.target;
            }

            int ret;

            // ----- Open input -----
            AVFormatContext *fmt_ctx_raw = nullptr;
            ret = avformat_open_input(&fmt_ctx_raw, input_path.c_str(), nullptr, nullptr);
            if (ret < 0)
                return av_err_str(ret, "Failed to open input file");
            FormatContextPtr fmt_ctx(fmt_ctx_raw);

            ret = avformat_find_stream_info(fmt_ctx.get(), nullptr);
            if (ret < 0)
                return av_err_str(ret, "Failed to find stream info");

            // ----- Find audio stream -----
            int audio_stream_idx = -1;
            for (unsigned i = 0; i < fmt_ctx->nb_streams; i++)
            {
                if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
                {
                    audio_stream_idx = i;
                    break;
                }
            }
            if (audio_stream_idx < 0)
                return "No audio stream found in input file";

            AVStream *audio_stream = fmt_ctx->streams[audio_stream_idx];
            AVCodecParameters *codecpar = audio_stream->codecpar;

            // ----- Open decoder -----
            const AVCodec *decoder = avcodec_find_decoder(codecpar->codec_id);
            if (!decoder)
                return "Decoder not found for input audio";

            CodecContextPtr dec_ctx(avcodec_alloc_context3(decoder));
            if (!dec_ctx)
                return "Failed to allocate decoder context";

            ret = avcodec_parameters_to_context(dec_ctx.get(), codecpar);
            if (ret < 0)
                return av_err_str(ret, "Failed to copy codec parameters");

            ret = avcodec_open2(dec_ctx.get(), decoder, nullptr);
            if (ret < 0)
                return av_err_str(ret, "Failed to open decoder");

            // ----- Find encoder -----
            const AVCodec *encoder = avcodec_find_encoder(codec_info->codec_id);
            if (!encoder)
                return "Encoder not found for target: " + opts.target;

            // ----- Determine encoder sample format -----
            AVSampleFormat enc_sample_fmt = AV_SAMPLE_FMT_NONE;
            {
                const AVSampleFormat *fmts = nullptr;
                int num_fmts = 0;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
                const void *fmt_list = nullptr;
                if (avcodec_get_supported_config(nullptr, encoder, AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                                  0, &fmt_list, &num_fmts) >= 0 && fmt_list)
                {
                    fmts = static_cast<const AVSampleFormat *>(fmt_list);
                }
#else
                fmts = encoder->sample_fmts;
                if (fmts)
                {
                    for (num_fmts = 0; fmts[num_fmts] != AV_SAMPLE_FMT_NONE; num_fmts++) {}
                }
#endif
                if (fmts && num_fmts > 0)
                {
                    // Try to match the decoder's sample format first
                    bool found = false;
                    for (int i = 0; i < num_fmts && fmts[i] != AV_SAMPLE_FMT_NONE; i++)
                    {
                        if (fmts[i] == dec_ctx->sample_fmt)
                        {
                            enc_sample_fmt = fmts[i];
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                        enc_sample_fmt = fmts[0];
                }
                else
                {
                    enc_sample_fmt = dec_ctx->sample_fmt;
                }
            }

            // ----- Determine sample rate -----
            int out_sample_rate = dec_ctx->sample_rate;
            if (opts.sample_rate > 0)
                out_sample_rate = opts.sample_rate;

            // Opus requires >= 48000 Hz
            if (codec_info->codec_id == AV_CODEC_ID_OPUS && out_sample_rate < 48000)
                out_sample_rate = 48000;

            // ----- Determine channel count -----
            int out_channels = dec_ctx->ch_layout.nb_channels;
            if (opts.channels > 0)
                out_channels = opts.channels;

            // ----- Build filter graph -----
            // abuffer → [volume →] aformat → abuffersink
            FilterGraphPtr filter_graph(avfilter_graph_alloc());
            if (!filter_graph)
                return "Failed to allocate filter graph";

            AVFilterContext *abuffersrc_ctx = nullptr;
            AVFilterContext *abuffersink_ctx = nullptr;

            const AVFilter *abuffersrc = avfilter_get_by_name("abuffer");
            const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
            if (!abuffersrc || !abuffersink)
                return "Audio filter not found (abuffer/abuffersink)";

            // Build abuffer source args
            char ch_layout_str[64];
            av_channel_layout_describe(&dec_ctx->ch_layout, ch_layout_str, sizeof(ch_layout_str));

            char src_args[512];
            snprintf(src_args, sizeof(src_args),
                     "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
                     audio_stream->time_base.num, audio_stream->time_base.den,
                     dec_ctx->sample_rate,
                     av_get_sample_fmt_name(dec_ctx->sample_fmt),
                     ch_layout_str);

            ret = avfilter_graph_create_filter(&abuffersrc_ctx, abuffersrc, "in",
                                               src_args, nullptr, filter_graph.get());
            if (ret < 0)
                return av_err_str(ret, "Failed to create abuffer source");

            ret = avfilter_graph_create_filter(&abuffersink_ctx, abuffersink, "out",
                                               nullptr, nullptr, filter_graph.get());
            if (ret < 0)
                return av_err_str(ret, "Failed to create abuffersink");

            // Build filter description
            std::ostringstream filter_descr;
            bool need_comma = false;

            if (opts.volume != 1.0f)
            {
                filter_descr << "volume=" << opts.volume;
                need_comma = true;
            }

            // Build aformat string for output conversion
            {
                if (need_comma)
                    filter_descr << ",";

                // Build the output channel layout string
                AVChannelLayout out_ch_layout = {};
                av_channel_layout_default(&out_ch_layout, out_channels);
                char out_ch_str[64];
                av_channel_layout_describe(&out_ch_layout, out_ch_str, sizeof(out_ch_str));
                av_channel_layout_uninit(&out_ch_layout);

                filter_descr << "aformat=sample_fmts="
                             << av_get_sample_fmt_name(enc_sample_fmt)
                             << ":sample_rates=" << out_sample_rate
                             << ":channel_layouts=" << out_ch_str;
            }

            // Parse and configure filter graph
            AVFilterInOut *f_outputs = avfilter_inout_alloc();
            AVFilterInOut *f_inputs = avfilter_inout_alloc();

            f_outputs->name = av_strdup("in");
            f_outputs->filter_ctx = abuffersrc_ctx;
            f_outputs->pad_idx = 0;
            f_outputs->next = nullptr;

            f_inputs->name = av_strdup("out");
            f_inputs->filter_ctx = abuffersink_ctx;
            f_inputs->pad_idx = 0;
            f_inputs->next = nullptr;

            ret = avfilter_graph_parse_ptr(filter_graph.get(), filter_descr.str().c_str(),
                                           &f_inputs, &f_outputs, nullptr);
            avfilter_inout_free(&f_inputs);
            avfilter_inout_free(&f_outputs);

            if (ret < 0)
                return av_err_str(ret, "Failed to parse filter graph");

            ret = avfilter_graph_config(filter_graph.get(), nullptr);
            if (ret < 0)
                return av_err_str(ret, "Failed to configure filter graph");

            // ----- Open encoder context -----
            CodecContextPtr enc_ctx(avcodec_alloc_context3(encoder));
            if (!enc_ctx)
                return "Failed to allocate encoder context";

            enc_ctx->sample_rate = out_sample_rate;
            enc_ctx->sample_fmt = enc_sample_fmt;
            av_channel_layout_default(&enc_ctx->ch_layout, out_channels);
            enc_ctx->time_base = AVRational{1, out_sample_rate};

            // Set bitrate for lossy formats
            if (!codec_info->lossless)
            {
                int64_t bitrate = parse_bitrate(opts.bitrate);
                if (bitrate > 0)
                    enc_ctx->bit_rate = bitrate;
            }

            // Global header flag
            AVFormatContext *out_fmt_ctx_raw = nullptr;
            ret = avformat_alloc_output_context2(&out_fmt_ctx_raw, nullptr,
                                                  codec_info->format_name,
                                                  output_path.c_str());
            if (ret < 0 || !out_fmt_ctx_raw)
                return av_err_str(ret, "Failed to create output context");
            FormatContextOutputPtr out_fmt_ctx(out_fmt_ctx_raw);

            if (out_fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
                enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

            // Vorbis requires strict_std_compliance to be set to experimental
            if (codec_info->codec_id == AV_CODEC_ID_VORBIS)
                enc_ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

            ret = avcodec_open2(enc_ctx.get(), encoder, nullptr);
            if (ret < 0)
                return av_err_str(ret, "Failed to open audio encoder");

            // ----- Create output stream -----
            AVStream *out_stream = avformat_new_stream(out_fmt_ctx.get(), nullptr);
            if (!out_stream)
                return "Failed to create output stream";

            ret = avcodec_parameters_from_context(out_stream->codecpar, enc_ctx.get());
            if (ret < 0)
                return av_err_str(ret, "Failed to copy encoder parameters");

            out_stream->time_base = enc_ctx->time_base;

            // Open output file
            ret = avio_open(&out_fmt_ctx->pb, output_path.c_str(), AVIO_FLAG_WRITE);
            if (ret < 0)
                return av_err_str(ret, "Failed to open output file");

            ret = avformat_write_header(out_fmt_ctx.get(), nullptr);
            if (ret < 0)
                return av_err_str(ret, "Failed to write header");

            // ----- Seek to start position -----
            if (opts.start > 0)
            {
                int64_t start_ts = static_cast<int64_t>(opts.start * AV_TIME_BASE);
                av_seek_frame(fmt_ctx.get(), -1, start_ts, AVSEEK_FLAG_BACKWARD);
            }

            // Calculate end timestamp
            int64_t end_ts = INT64_MAX;
            if (opts.duration > 0)
                end_ts = static_cast<int64_t>((opts.start + opts.duration) * AV_TIME_BASE);

            // ----- Decode / filter / encode loop -----
            FramePtr frame(av_frame_alloc());
            FramePtr filt_frame(av_frame_alloc());
            PacketPtr packet(av_packet_alloc());

            if (!frame || !filt_frame || !packet)
                return "Failed to allocate frame/packet";

            while (true)
            {
                ret = av_read_frame(fmt_ctx.get(), packet.get());
                if (ret < 0)
                    break; // EOF or error

                if (packet->stream_index != audio_stream_idx)
                {
                    av_packet_unref(packet.get());
                    continue;
                }

                // Check duration limit
                if (packet->pts != AV_NOPTS_VALUE)
                {
                    int64_t pts_time = av_rescale_q(packet->pts, audio_stream->time_base,
                                                    AVRational{1, AV_TIME_BASE});
                    if (pts_time > end_ts)
                    {
                        av_packet_unref(packet.get());
                        break;
                    }
                }

                ret = avcodec_send_packet(dec_ctx.get(), packet.get());
                av_packet_unref(packet.get());

                if (ret < 0)
                    continue; // Skip bad packets

                while (ret >= 0)
                {
                    ret = avcodec_receive_frame(dec_ctx.get(), frame.get());
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                        break;
                    if (ret < 0)
                        return av_err_str(ret, "Error decoding frame");

                    // Push frame to filter graph
                    ret = av_buffersrc_add_frame_flags(abuffersrc_ctx, frame.get(),
                                                       AV_BUFFERSRC_FLAG_KEEP_REF);
                    av_frame_unref(frame.get());

                    if (ret < 0)
                        return av_err_str(ret, "Error feeding filter graph");

                    // Pull filtered frames
                    while (true)
                    {
                        ret = av_buffersink_get_frame(abuffersink_ctx, filt_frame.get());
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                            break;
                        if (ret < 0)
                            return av_err_str(ret, "Error getting filtered frame");

                        // Encode frame
                        ret = avcodec_send_frame(enc_ctx.get(), filt_frame.get());
                        av_frame_unref(filt_frame.get());

                        if (ret < 0)
                            return av_err_str(ret, "Error sending frame to encoder");

                        while (ret >= 0)
                        {
                            ret = avcodec_receive_packet(enc_ctx.get(), packet.get());
                            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                                break;
                            if (ret < 0)
                                return av_err_str(ret, "Error encoding frame");

                            av_packet_rescale_ts(packet.get(), enc_ctx->time_base,
                                                 out_stream->time_base);
                            packet->stream_index = out_stream->index;

                            ret = av_interleaved_write_frame(out_fmt_ctx.get(), packet.get());
                            if (ret < 0)
                                return av_err_str(ret, "Error writing frame");
                        }
                    }
                }
            }

            // ----- Flush decoder -----
            avcodec_send_packet(dec_ctx.get(), nullptr);
            while (avcodec_receive_frame(dec_ctx.get(), frame.get()) >= 0)
            {
                av_buffersrc_add_frame_flags(abuffersrc_ctx, frame.get(), 0);
                av_frame_unref(frame.get());

                while (av_buffersink_get_frame(abuffersink_ctx, filt_frame.get()) >= 0)
                {
                    avcodec_send_frame(enc_ctx.get(), filt_frame.get());
                    av_frame_unref(filt_frame.get());

                    while (avcodec_receive_packet(enc_ctx.get(), packet.get()) >= 0)
                    {
                        av_packet_rescale_ts(packet.get(), enc_ctx->time_base,
                                             out_stream->time_base);
                        packet->stream_index = out_stream->index;
                        av_interleaved_write_frame(out_fmt_ctx.get(), packet.get());
                    }
                }
            }

            // ----- Flush filter graph -----
            av_buffersrc_add_frame_flags(abuffersrc_ctx, nullptr, 0);
            while (av_buffersink_get_frame(abuffersink_ctx, filt_frame.get()) >= 0)
            {
                avcodec_send_frame(enc_ctx.get(), filt_frame.get());
                av_frame_unref(filt_frame.get());

                while (avcodec_receive_packet(enc_ctx.get(), packet.get()) >= 0)
                {
                    av_packet_rescale_ts(packet.get(), enc_ctx->time_base,
                                         out_stream->time_base);
                    packet->stream_index = out_stream->index;
                    av_interleaved_write_frame(out_fmt_ctx.get(), packet.get());
                }
            }

            // ----- Flush encoder -----
            avcodec_send_frame(enc_ctx.get(), nullptr);
            while (avcodec_receive_packet(enc_ctx.get(), packet.get()) >= 0)
            {
                av_packet_rescale_ts(packet.get(), enc_ctx->time_base,
                                     out_stream->time_base);
                packet->stream_index = out_stream->index;
                av_interleaved_write_frame(out_fmt_ctx.get(), packet.get());
            }

            // ----- Write trailer -----
            ret = av_write_trailer(out_fmt_ctx.get());
            if (ret < 0)
                return av_err_str(ret, "Error writing trailer");

            return ""; // Success
        }

    private:
        std::string av_err_str(int errnum, const char *prefix)
        {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(errnum, errbuf, sizeof(errbuf));
            return std::string(prefix) + ": " + errbuf;
        }
    };

} // anonymous namespace

extern "C"
{

    UNICONV_EXPORT int uniconv_plugin_init(void)
    {
        avformat_network_init();
        return 0;
    }

    UNICONV_EXPORT UniconvPluginInfo *uniconv_plugin_info(void)
    {
        return &plugin_info;
    }

    UNICONV_EXPORT UniconvResult *uniconv_plugin_execute(const UniconvRequest *request)
    {
        UniconvResult *result = static_cast<UniconvResult *>(calloc(1, sizeof(UniconvResult)));
        if (!result)
            return nullptr;

        // Validate input
        if (!request || !request->source)
        {
            result->status = UNICONV_ERROR;
            result->error = strdup("Invalid request: missing source");
            return result;
        }

        // Set FFmpeg log level based on verbose core option
        std::string verbose = get_option(request, "verbose");
        if (verbose == "true")
            av_log_set_level(AV_LOG_INFO);
        else
            av_log_set_level(AV_LOG_QUIET);

        std::string source_path = request->source;

        // Check input file exists
        if (!file_exists(source_path))
        {
            result->status = UNICONV_ERROR;
            std::string msg = "Input file not found: " + source_path;
            result->error = strdup(msg.c_str());
            return result;
        }

        // Determine target format
        std::string target = "mp3"; // default
        if (request->target && request->target[0] != '\0')
            target = to_lower(request->target);

        // Validate target
        if (!find_codec_info(target))
        {
            result->status = UNICONV_ERROR;
            std::string msg = "Unsupported target format: " + target;
            result->error = strdup(msg.c_str());
            return result;
        }

        // Determine output path
        std::string output_path;
        if (request->output)
        {
            output_path = request->output;
        }
        else
        {
            size_t dot = source_path.rfind('.');
            if (dot != std::string::npos)
                output_path = source_path.substr(0, dot) + "." + target;
            else
                output_path = source_path + "." + target;
        }

        // Check if output exists (unless force)
        if (!request->force && file_exists(output_path))
        {
            result->status = UNICONV_ERROR;
            std::string msg = "Output file already exists: " + output_path +
                              " (use --force to overwrite)";
            result->error = strdup(msg.c_str());
            return result;
        }

        // Get options
        AudioConverter::Options opts;
        opts.target = target;
        opts.bitrate = get_option_string(request, "bitrate");
        opts.sample_rate = get_option_int(request, "sample-rate", 0);
        opts.channels = get_option_int(request, "channels", 0);
        opts.volume = get_option_float(request, "volume", 1.0f);
        opts.start = get_option_float(request, "start", 0.0f);
        opts.duration = get_option_float(request, "duration", 0.0f);

        // Clamp channels
        if (opts.channels < 0)
            opts.channels = 0;
        if (opts.channels > 8)
            opts.channels = 8;

        // Clamp volume
        if (opts.volume < 0.0f)
            opts.volume = 0.0f;
        if (opts.volume > 10.0f)
            opts.volume = 10.0f;

        // Dry run
        if (request->dry_run)
        {
            result->status = UNICONV_SUCCESS;
            result->output = strdup(output_path.c_str());

            std::ostringstream extra;
            extra << "{\"dry_run\": true";
            extra << ", \"target\": \"" << target << "\"";
            if (!opts.bitrate.empty())
                extra << ", \"bitrate\": \"" << opts.bitrate << "\"";
            if (opts.sample_rate > 0)
                extra << ", \"sample_rate\": " << opts.sample_rate;
            if (opts.channels > 0)
                extra << ", \"channels\": " << opts.channels;
            if (opts.volume != 1.0f)
                extra << ", \"volume\": " << opts.volume;
            if (opts.start > 0)
                extra << ", \"start\": " << opts.start;
            if (opts.duration > 0)
                extra << ", \"duration\": " << opts.duration;
            extra << "}";
            result->extra_json = strdup(extra.str().c_str());

            return result;
        }

        // Convert
        AudioConverter converter;
        std::string error = converter.convert(source_path, output_path, opts);

        if (!error.empty())
        {
            result->status = UNICONV_ERROR;
            result->error = strdup(error.c_str());
            return result;
        }

        // Verify output was created
        if (!file_exists(output_path))
        {
            result->status = UNICONV_ERROR;
            result->error = strdup("Conversion completed but output file not found");
            return result;
        }

        // Success
        result->status = UNICONV_SUCCESS;
        result->output = strdup(output_path.c_str());
        result->output_size = get_file_size(output_path);

        std::ostringstream extra;
        extra << "{\"target\": \"" << target << "\"";
        if (!opts.bitrate.empty())
            extra << ", \"bitrate\": \"" << opts.bitrate << "\"";
        if (opts.sample_rate > 0)
            extra << ", \"sample_rate\": " << opts.sample_rate;
        if (opts.channels > 0)
            extra << ", \"channels\": " << opts.channels;
        if (opts.volume != 1.0f)
            extra << ", \"volume\": " << opts.volume;
        if (opts.start > 0)
            extra << ", \"start\": " << opts.start;
        if (opts.duration > 0)
            extra << ", \"duration\": " << opts.duration;
        extra << "}";
        result->extra_json = strdup(extra.str().c_str());

        return result;
    }

    UNICONV_EXPORT void uniconv_plugin_free_result(UniconvResult *result)
    {
        if (result)
        {
            free(result->output);
            free(result->error);
            free(result->extra_json);
            free(result);
        }
    }

} // extern "C"
