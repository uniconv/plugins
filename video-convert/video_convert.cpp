/**
 * uniconv Native Plugin: video-convert
 *
 * Bidirectional video format conversion supporting mp4, mov, avi, webm, mkv, m4v, gif.
 *
 * Supported options:
 *   --fps       Output frames per second (0 = keep original; default 10 for GIF)
 *   --width     Output width (height auto-scaled)
 *   --height    Output height (width auto-scaled)
 *   --quality   Output quality (1-100, higher is better)
 *   --start     Start time in seconds
 *   --duration  Duration in seconds
 *   --loop      Number of loops for GIF (0 = infinite)
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
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

// Plugin info
static const char *targets[] = {"mp4", "mov", "avi", "webm", "mkv", "m4v", "gif", nullptr};
static const char *input_formats[] = {"mp4", "mov", "avi", "webm", "mkv", "m4v", "gif", nullptr};

// Data type information
static UniconvDataType input_types[] = {UNICONV_DATA_VIDEO, UNICONV_DATA_FILE, (UniconvDataType)0};
static UniconvDataType output_types[] = {UNICONV_DATA_VIDEO, UNICONV_DATA_IMAGE, (UniconvDataType)0};

static UniconvPluginInfo plugin_info = {
    .name = "video-convert",
    .scope = "video-convert",
    .version = "1.1.2",
    .description = "Bidirectional video format conversion (mp4, mov, avi, webm, mkv, m4v, gif)",
    .targets = targets,
    .input_formats = input_formats,
    .input_types = input_types,
    .output_types = output_types};

namespace
{

    // RAII wrappers for FFmpeg resources
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

    struct SwsContextDeleter
    {
        void operator()(SwsContext *ctx) const
        {
            if (ctx)
                sws_freeContext(ctx);
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
    using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;
    using FilterGraphPtr = std::unique_ptr<AVFilterGraph, AVFilterGraphDeleter>;

    /**
     * Convert string to lowercase
     */
    std::string to_lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c)
                       { return std::tolower(c); });
        return s;
    }

    /**
     * Check if a file exists
     */
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

    /**
     * Get file size
     */
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

    /**
     * Get option value as string
     */
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

    /**
     * Get option as integer with default
     */
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

    /**
     * Get option as float with default
     */
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

    /**
     * Get FFmpeg format name for a target extension
     */
    const char *get_format_name(const std::string &target)
    {
        if (target == "mkv")
            return "matroska";
        if (target == "m4v")
            return "mp4";
        return target.c_str();
    }

    /**
     * Find a suitable video encoder for the given target format
     */
    const AVCodec *find_video_encoder(const std::string &target)
    {
        const AVCodec *codec = nullptr;

        if (target == "mp4" || target == "mov" || target == "mkv" || target == "m4v")
        {
            codec = avcodec_find_encoder_by_name("libx264");
            if (!codec)
                codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        }
        else if (target == "webm")
        {
            codec = avcodec_find_encoder_by_name("libvpx");
            if (!codec)
                codec = avcodec_find_encoder(AV_CODEC_ID_VP8);
        }
        else if (target == "avi")
        {
            codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
        }

        return codec;
    }

    /**
     * Set quality parameters on the encoder context based on format and quality value (1-100)
     */
    void set_video_quality(AVCodecContext *enc_ctx, const std::string &target, int quality)
    {
        if (quality < 1)
            quality = 1;
        if (quality > 100)
            quality = 100;

        if (enc_ctx->codec_id == AV_CODEC_ID_H264)
        {
            // CRF: 0 (lossless) to 51 (worst). Map quality 100->0, 1->51
            int crf = 51 - (quality * 51) / 100;
            av_opt_set_int(enc_ctx->priv_data, "crf", crf, 0);
            av_opt_set(enc_ctx->priv_data, "preset", "medium", 0);
        }
        else if (enc_ctx->codec_id == AV_CODEC_ID_VP8 || enc_ctx->codec_id == AV_CODEC_ID_VP9)
        {
            // CRF: 4 (best) to 63 (worst). Map quality 100->4, 1->63
            int crf = 63 - (quality * 59) / 100;
            enc_ctx->bit_rate = 0;
            av_opt_set_int(enc_ctx->priv_data, "crf", crf, 0);
        }
        else if (enc_ctx->codec_id == AV_CODEC_ID_MPEG4)
        {
            // qscale: 1 (best) to 31 (worst). Map quality 100->1, 1->31
            int qscale = 31 - (quality * 30) / 100;
            enc_ctx->flags |= AV_CODEC_FLAG_QSCALE;
            enc_ctx->global_quality = FF_QP2LAMBDA * qscale;
        }
    }

    /**
     * Bidirectional video format converter using libav
     */
    class VideoConverter
    {
    public:
        struct Options
        {
            std::string target;
            int fps = 0;        // 0 = keep original
            int width = -1;     // -1 = keep original
            int height = -1;    // -1 = keep original
            int quality = 75;   // 1-100
            float start = 0.0f;
            float duration = 0.0f; // 0 = full duration
            int loop = 0;          // 0 = infinite (GIF only)
        };

        std::string convert(const std::string &input_path,
                            const std::string &output_path,
                            const Options &opts)
        {
            if (opts.target == "gif")
            {
                return convert_to_gif(input_path, output_path, opts);
            }
            return convert_to_video(input_path, output_path, opts);
        }

    private:
        std::string av_err_str(int errnum, const char *prefix)
        {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(errnum, errbuf, sizeof(errbuf));
            return std::string(prefix) + ": " + errbuf;
        }

        /**
         * Calculate output dimensions given input dimensions and requested width/height.
         * Preserves aspect ratio and ensures even dimensions.
         */
        void calc_dimensions(int in_w, int in_h, int req_w, int req_h, int &out_w, int &out_h)
        {
            if (req_w > 0 && req_h > 0)
            {
                out_w = req_w;
                out_h = req_h;
            }
            else if (req_w > 0)
            {
                out_w = req_w;
                out_h = (in_h * req_w) / in_w;
            }
            else if (req_h > 0)
            {
                out_h = req_h;
                out_w = (in_w * req_h) / in_h;
            }
            else
            {
                out_w = in_w;
                out_h = in_h;
            }
            // Ensure even dimensions
            out_w = (out_w / 2) * 2;
            out_h = (out_h / 2) * 2;
        }

        /**
         * Convert to GIF — palette-based path for high-quality animated GIF output.
         * Mostly preserved from original implementation.
         */
        std::string convert_to_gif(const std::string &input_path,
                                   const std::string &output_path,
                                   const Options &opts)
        {
            int ret;
            int fps = opts.fps > 0 ? opts.fps : 10;

            // Open input
            AVFormatContext *fmt_ctx_raw = nullptr;
            ret = avformat_open_input(&fmt_ctx_raw, input_path.c_str(), nullptr, nullptr);
            if (ret < 0)
            {
                return av_err_str(ret, "Failed to open input file");
            }
            FormatContextPtr fmt_ctx(fmt_ctx_raw);

            ret = avformat_find_stream_info(fmt_ctx.get(), nullptr);
            if (ret < 0)
            {
                return av_err_str(ret, "Failed to find stream info");
            }

            // Find video stream
            int video_stream_idx = -1;
            for (unsigned i = 0; i < fmt_ctx->nb_streams; i++)
            {
                if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                {
                    video_stream_idx = i;
                    break;
                }
            }
            if (video_stream_idx < 0)
            {
                return "No video stream found";
            }

            AVStream *video_stream = fmt_ctx->streams[video_stream_idx];
            AVCodecParameters *codecpar = video_stream->codecpar;

            // Find decoder
            const AVCodec *decoder = avcodec_find_decoder(codecpar->codec_id);
            if (!decoder)
            {
                return "Decoder not found for input video";
            }

            // Create decoder context
            CodecContextPtr dec_ctx(avcodec_alloc_context3(decoder));
            if (!dec_ctx)
            {
                return "Failed to allocate decoder context";
            }

            ret = avcodec_parameters_to_context(dec_ctx.get(), codecpar);
            if (ret < 0)
            {
                return av_err_str(ret, "Failed to copy codec parameters");
            }

            ret = avcodec_open2(dec_ctx.get(), decoder, nullptr);
            if (ret < 0)
            {
                return av_err_str(ret, "Failed to open decoder");
            }

            // Calculate output dimensions
            int out_width, out_height;
            calc_dimensions(dec_ctx->width, dec_ctx->height,
                            opts.width, opts.height, out_width, out_height);

            bool needs_scale = (out_width != dec_ctx->width || out_height != dec_ctx->height);

            // Setup filter graph for palette generation
            FilterGraphPtr filter_graph(avfilter_graph_alloc());
            if (!filter_graph)
            {
                return "Failed to allocate filter graph";
            }

            AVFilterContext *buffersrc_ctx = nullptr;
            AVFilterContext *buffersink_ctx = nullptr;

            // Build filter description
            std::ostringstream filter_descr;
            filter_descr << "fps=" << fps;
            if (needs_scale)
            {
                filter_descr << ",scale=" << out_width << ":" << out_height << ":flags=lanczos";
            }
            filter_descr << ",split[s0][s1];";
            filter_descr << "[s0]palettegen=max_colors=256:stats_mode=diff[p];";
            filter_descr << "[s1][p]paletteuse=dither=floyd_steinberg";

            // Create buffer source
            const AVFilter *buffersrc = avfilter_get_by_name("buffer");
            const AVFilter *buffersink = avfilter_get_by_name("buffersink");

            char args[512];
            snprintf(args, sizeof(args),
                     "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                     dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
                     video_stream->time_base.num, video_stream->time_base.den,
                     dec_ctx->sample_aspect_ratio.num,
                     dec_ctx->sample_aspect_ratio.den > 0 ? dec_ctx->sample_aspect_ratio.den : 1);

            ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                                               args, nullptr, filter_graph.get());
            if (ret < 0)
            {
                return av_err_str(ret, "Failed to create buffer source");
            }

            ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                                               nullptr, nullptr, filter_graph.get());
            if (ret < 0)
            {
                return av_err_str(ret, "Failed to create buffer sink");
            }

            // Parse and configure filter graph
            AVFilterInOut *outputs = avfilter_inout_alloc();
            AVFilterInOut *inputs = avfilter_inout_alloc();

            outputs->name = av_strdup("in");
            outputs->filter_ctx = buffersrc_ctx;
            outputs->pad_idx = 0;
            outputs->next = nullptr;

            inputs->name = av_strdup("out");
            inputs->filter_ctx = buffersink_ctx;
            inputs->pad_idx = 0;
            inputs->next = nullptr;

            ret = avfilter_graph_parse_ptr(filter_graph.get(), filter_descr.str().c_str(),
                                           &inputs, &outputs, nullptr);
            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);

            if (ret < 0)
            {
                return av_err_str(ret, "Failed to parse filter graph");
            }

            ret = avfilter_graph_config(filter_graph.get(), nullptr);
            if (ret < 0)
            {
                return av_err_str(ret, "Failed to configure filter graph");
            }

            // Get output dimensions from filter
            out_width = av_buffersink_get_w(buffersink_ctx);
            out_height = av_buffersink_get_h(buffersink_ctx);

            // Open output file
            AVFormatContext *out_fmt_ctx_raw = nullptr;
            ret = avformat_alloc_output_context2(&out_fmt_ctx_raw, nullptr, "gif", output_path.c_str());
            if (ret < 0 || !out_fmt_ctx_raw)
            {
                return av_err_str(ret, "Failed to create output context");
            }
            FormatContextOutputPtr out_fmt_ctx(out_fmt_ctx_raw);

            // Find GIF encoder
            const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_GIF);
            if (!encoder)
            {
                return "GIF encoder not found";
            }

            // Create output stream
            AVStream *out_stream = avformat_new_stream(out_fmt_ctx.get(), nullptr);
            if (!out_stream)
            {
                return "Failed to create output stream";
            }

            // Create encoder context
            CodecContextPtr enc_ctx(avcodec_alloc_context3(encoder));
            if (!enc_ctx)
            {
                return "Failed to allocate encoder context";
            }

            enc_ctx->width = out_width;
            enc_ctx->height = out_height;
            enc_ctx->pix_fmt = AV_PIX_FMT_PAL8;
            enc_ctx->time_base = AVRational{1, fps};
            enc_ctx->framerate = AVRational{fps, 1};

            if (out_fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
            {
                enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }

            ret = avcodec_open2(enc_ctx.get(), encoder, nullptr);
            if (ret < 0)
            {
                return av_err_str(ret, "Failed to open encoder");
            }

            ret = avcodec_parameters_from_context(out_stream->codecpar, enc_ctx.get());
            if (ret < 0)
            {
                return av_err_str(ret, "Failed to copy encoder parameters");
            }

            out_stream->time_base = enc_ctx->time_base;

            // Set GIF loop count in metadata
            av_dict_set(&out_fmt_ctx->metadata, "loop", std::to_string(opts.loop).c_str(), 0);

            // Open output file
            ret = avio_open(&out_fmt_ctx->pb, output_path.c_str(), AVIO_FLAG_WRITE);
            if (ret < 0)
            {
                return av_err_str(ret, "Failed to open output file");
            }

            ret = avformat_write_header(out_fmt_ctx.get(), nullptr);
            if (ret < 0)
            {
                return av_err_str(ret, "Failed to write header");
            }

            // Seek to start position if specified
            if (opts.start > 0)
            {
                int64_t start_ts = static_cast<int64_t>(opts.start * AV_TIME_BASE);
                av_seek_frame(fmt_ctx.get(), -1, start_ts, AVSEEK_FLAG_BACKWARD);
            }

            // Calculate end timestamp
            int64_t end_ts = INT64_MAX;
            if (opts.duration > 0)
            {
                end_ts = static_cast<int64_t>((opts.start + opts.duration) * AV_TIME_BASE);
            }

            // Allocate frames and packet
            FramePtr frame(av_frame_alloc());
            FramePtr filt_frame(av_frame_alloc());
            PacketPtr packet(av_packet_alloc());

            if (!frame || !filt_frame || !packet)
            {
                return "Failed to allocate frame/packet";
            }

            int64_t frame_count = 0;

            // Main decode/filter/encode loop
            while (true)
            {
                ret = av_read_frame(fmt_ctx.get(), packet.get());
                if (ret < 0)
                {
                    break; // EOF or error
                }

                if (packet->stream_index != video_stream_idx)
                {
                    av_packet_unref(packet.get());
                    continue;
                }

                // Check if we've passed the duration limit
                if (packet->pts != AV_NOPTS_VALUE)
                {
                    int64_t pts_time = av_rescale_q(packet->pts, video_stream->time_base,
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
                {
                    continue; // Skip bad packets
                }

                while (ret >= 0)
                {
                    ret = avcodec_receive_frame(dec_ctx.get(), frame.get());
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    {
                        break;
                    }
                    if (ret < 0)
                    {
                        return av_err_str(ret, "Error decoding frame");
                    }

                    // Push frame to filter graph
                    ret = av_buffersrc_add_frame_flags(buffersrc_ctx, frame.get(),
                                                       AV_BUFFERSRC_FLAG_KEEP_REF);
                    av_frame_unref(frame.get());

                    if (ret < 0)
                    {
                        return av_err_str(ret, "Error feeding filter graph");
                    }

                    // Pull filtered frames
                    while (true)
                    {
                        ret = av_buffersink_get_frame(buffersink_ctx, filt_frame.get());
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                        {
                            break;
                        }
                        if (ret < 0)
                        {
                            return av_err_str(ret, "Error getting filtered frame");
                        }

                        // Set presentation timestamp
                        filt_frame->pts = frame_count++;

                        // Encode frame
                        ret = avcodec_send_frame(enc_ctx.get(), filt_frame.get());
                        av_frame_unref(filt_frame.get());

                        if (ret < 0)
                        {
                            return av_err_str(ret, "Error sending frame to encoder");
                        }

                        while (ret >= 0)
                        {
                            ret = avcodec_receive_packet(enc_ctx.get(), packet.get());
                            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                            {
                                break;
                            }
                            if (ret < 0)
                            {
                                return av_err_str(ret, "Error encoding frame");
                            }

                            av_packet_rescale_ts(packet.get(), enc_ctx->time_base,
                                                 out_stream->time_base);
                            packet->stream_index = out_stream->index;

                            ret = av_interleaved_write_frame(out_fmt_ctx.get(), packet.get());
                            if (ret < 0)
                            {
                                return av_err_str(ret, "Error writing frame");
                            }
                        }
                    }
                }
            }

            // Flush decoder
            avcodec_send_packet(dec_ctx.get(), nullptr);
            while (avcodec_receive_frame(dec_ctx.get(), frame.get()) >= 0)
            {
                av_buffersrc_add_frame_flags(buffersrc_ctx, frame.get(), 0);
                av_frame_unref(frame.get());

                while (av_buffersink_get_frame(buffersink_ctx, filt_frame.get()) >= 0)
                {
                    filt_frame->pts = frame_count++;
                    avcodec_send_frame(enc_ctx.get(), filt_frame.get());
                    av_frame_unref(filt_frame.get());

                    while (avcodec_receive_packet(enc_ctx.get(), packet.get()) >= 0)
                    {
                        av_packet_rescale_ts(packet.get(), enc_ctx->time_base, out_stream->time_base);
                        packet->stream_index = out_stream->index;
                        av_interleaved_write_frame(out_fmt_ctx.get(), packet.get());
                    }
                }
            }

            // Flush filter graph
            av_buffersrc_add_frame_flags(buffersrc_ctx, nullptr, 0);
            while (av_buffersink_get_frame(buffersink_ctx, filt_frame.get()) >= 0)
            {
                filt_frame->pts = frame_count++;
                avcodec_send_frame(enc_ctx.get(), filt_frame.get());
                av_frame_unref(filt_frame.get());

                while (avcodec_receive_packet(enc_ctx.get(), packet.get()) >= 0)
                {
                    av_packet_rescale_ts(packet.get(), enc_ctx->time_base, out_stream->time_base);
                    packet->stream_index = out_stream->index;
                    av_interleaved_write_frame(out_fmt_ctx.get(), packet.get());
                }
            }

            // Flush encoder
            avcodec_send_frame(enc_ctx.get(), nullptr);
            while (avcodec_receive_packet(enc_ctx.get(), packet.get()) >= 0)
            {
                av_packet_rescale_ts(packet.get(), enc_ctx->time_base, out_stream->time_base);
                packet->stream_index = out_stream->index;
                av_interleaved_write_frame(out_fmt_ctx.get(), packet.get());
            }

            // Write trailer
            ret = av_write_trailer(out_fmt_ctx.get());
            if (ret < 0)
            {
                return av_err_str(ret, "Error writing trailer");
            }

            return ""; // Success
        }

        /**
         * Convert to video format — supports mp4, mov, avi, webm, mkv, m4v.
         * Handles video re-encoding with optional audio stream copy.
         */
        std::string convert_to_video(const std::string &input_path,
                                     const std::string &output_path,
                                     const Options &opts)
        {
            int ret;

            // Open input
            AVFormatContext *fmt_ctx_raw = nullptr;
            ret = avformat_open_input(&fmt_ctx_raw, input_path.c_str(), nullptr, nullptr);
            if (ret < 0)
            {
                return av_err_str(ret, "Failed to open input file");
            }
            FormatContextPtr fmt_ctx(fmt_ctx_raw);

            ret = avformat_find_stream_info(fmt_ctx.get(), nullptr);
            if (ret < 0)
            {
                return av_err_str(ret, "Failed to find stream info");
            }

            // Find video stream
            int video_stream_idx = -1;
            int audio_stream_idx = -1;
            for (unsigned i = 0; i < fmt_ctx->nb_streams; i++)
            {
                if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                    video_stream_idx < 0)
                {
                    video_stream_idx = i;
                }
                else if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
                         audio_stream_idx < 0)
                {
                    audio_stream_idx = i;
                }
            }
            if (video_stream_idx < 0)
            {
                return "No video stream found";
            }

            AVStream *video_stream = fmt_ctx->streams[video_stream_idx];
            AVCodecParameters *codecpar = video_stream->codecpar;

            // Find decoder
            const AVCodec *decoder = avcodec_find_decoder(codecpar->codec_id);
            if (!decoder)
            {
                return "Decoder not found for input video";
            }

            // Create decoder context
            CodecContextPtr dec_ctx(avcodec_alloc_context3(decoder));
            if (!dec_ctx)
            {
                return "Failed to allocate decoder context";
            }

            ret = avcodec_parameters_to_context(dec_ctx.get(), codecpar);
            if (ret < 0)
            {
                return av_err_str(ret, "Failed to copy codec parameters");
            }

            ret = avcodec_open2(dec_ctx.get(), decoder, nullptr);
            if (ret < 0)
            {
                return av_err_str(ret, "Failed to open decoder");
            }

            // Calculate output dimensions
            int out_width, out_height;
            calc_dimensions(dec_ctx->width, dec_ctx->height,
                            opts.width, opts.height, out_width, out_height);

            bool needs_scale = (out_width != dec_ctx->width || out_height != dec_ctx->height);

            // Find video encoder
            const AVCodec *encoder = find_video_encoder(opts.target);
            if (!encoder)
            {
                return "No suitable encoder found for target: " + opts.target;
            }

            // Determine output pixel format
            AVPixelFormat out_pix_fmt = AV_PIX_FMT_YUV420P;
            {
                const AVPixelFormat *fmts = nullptr;
                int num_fmts = 0;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
                const void *fmt_list = nullptr;
                if (avcodec_get_supported_config(nullptr, encoder, AV_CODEC_CONFIG_PIX_FORMAT,
                                                  0, &fmt_list, &num_fmts) >= 0 && fmt_list)
                {
                    fmts = static_cast<const AVPixelFormat *>(fmt_list);
                }
#else
                fmts = encoder->pix_fmts;
                if (fmts)
                {
                    for (num_fmts = 0; fmts[num_fmts] != AV_PIX_FMT_NONE; num_fmts++) {}
                }
#endif
                if (fmts && num_fmts > 0)
                {
                    bool found = false;
                    for (int i = 0; i < num_fmts && fmts[i] != AV_PIX_FMT_NONE; i++)
                    {
                        if (fmts[i] == AV_PIX_FMT_YUV420P)
                        {
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                    {
                        out_pix_fmt = fmts[0];
                    }
                }
            }

            // Determine fps
            int fps = opts.fps;
            if (fps <= 0)
            {
                // Keep original fps
                double src_fps = av_q2d(video_stream->avg_frame_rate);
                if (src_fps <= 0)
                    src_fps = av_q2d(video_stream->r_frame_rate);
                if (src_fps <= 0)
                    src_fps = 30.0;
                fps = static_cast<int>(src_fps + 0.5);
                if (fps <= 0)
                    fps = 30;
            }

            // Build filter graph
            FilterGraphPtr filter_graph(avfilter_graph_alloc());
            if (!filter_graph)
            {
                return "Failed to allocate filter graph";
            }

            AVFilterContext *buffersrc_ctx = nullptr;
            AVFilterContext *buffersink_ctx = nullptr;

            std::ostringstream filter_descr;
            bool need_filter = false;

            if (opts.fps > 0)
            {
                filter_descr << "fps=" << fps;
                need_filter = true;
            }

            if (needs_scale)
            {
                if (need_filter)
                    filter_descr << ",";
                filter_descr << "scale=" << out_width << ":" << out_height << ":flags=lanczos";
                need_filter = true;
            }

            // Always add format filter to ensure correct pixel format
            if (need_filter)
                filter_descr << ",";
            filter_descr << "format=" << av_get_pix_fmt_name(out_pix_fmt);
            need_filter = true;

            // Create buffer source
            const AVFilter *buffersrc = avfilter_get_by_name("buffer");
            const AVFilter *buffersink = avfilter_get_by_name("buffersink");

            char src_args[512];
            snprintf(src_args, sizeof(src_args),
                     "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                     dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
                     video_stream->time_base.num, video_stream->time_base.den,
                     dec_ctx->sample_aspect_ratio.num,
                     dec_ctx->sample_aspect_ratio.den > 0 ? dec_ctx->sample_aspect_ratio.den : 1);

            ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                                               src_args, nullptr, filter_graph.get());
            if (ret < 0)
            {
                return av_err_str(ret, "Failed to create buffer source");
            }

            ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                                               nullptr, nullptr, filter_graph.get());
            if (ret < 0)
            {
                return av_err_str(ret, "Failed to create buffer sink");
            }

            // Parse and configure filter graph
            AVFilterInOut *f_outputs = avfilter_inout_alloc();
            AVFilterInOut *f_inputs = avfilter_inout_alloc();

            f_outputs->name = av_strdup("in");
            f_outputs->filter_ctx = buffersrc_ctx;
            f_outputs->pad_idx = 0;
            f_outputs->next = nullptr;

            f_inputs->name = av_strdup("out");
            f_inputs->filter_ctx = buffersink_ctx;
            f_inputs->pad_idx = 0;
            f_inputs->next = nullptr;

            ret = avfilter_graph_parse_ptr(filter_graph.get(), filter_descr.str().c_str(),
                                           &f_inputs, &f_outputs, nullptr);
            avfilter_inout_free(&f_inputs);
            avfilter_inout_free(&f_outputs);

            if (ret < 0)
            {
                return av_err_str(ret, "Failed to parse filter graph");
            }

            ret = avfilter_graph_config(filter_graph.get(), nullptr);
            if (ret < 0)
            {
                return av_err_str(ret, "Failed to configure filter graph");
            }

            // Create output context
            AVFormatContext *out_fmt_ctx_raw = nullptr;
            const char *format_name = get_format_name(opts.target);
            ret = avformat_alloc_output_context2(&out_fmt_ctx_raw, nullptr,
                                                  format_name, output_path.c_str());
            if (ret < 0 || !out_fmt_ctx_raw)
            {
                return av_err_str(ret, "Failed to create output context");
            }
            FormatContextOutputPtr out_fmt_ctx(out_fmt_ctx_raw);

            // Create video output stream
            AVStream *out_video_stream = avformat_new_stream(out_fmt_ctx.get(), nullptr);
            if (!out_video_stream)
            {
                return "Failed to create output video stream";
            }

            // Create encoder context
            CodecContextPtr enc_ctx(avcodec_alloc_context3(encoder));
            if (!enc_ctx)
            {
                return "Failed to allocate encoder context";
            }

            enc_ctx->width = out_width;
            enc_ctx->height = out_height;
            enc_ctx->pix_fmt = out_pix_fmt;
            enc_ctx->time_base = AVRational{1, fps};
            enc_ctx->framerate = AVRational{fps, 1};
            enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;

            set_video_quality(enc_ctx.get(), opts.target, opts.quality);

            if (out_fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
            {
                enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }

            ret = avcodec_open2(enc_ctx.get(), encoder, nullptr);
            if (ret < 0)
            {
                return av_err_str(ret, "Failed to open video encoder");
            }

            ret = avcodec_parameters_from_context(out_video_stream->codecpar, enc_ctx.get());
            if (ret < 0)
            {
                return av_err_str(ret, "Failed to copy video encoder parameters");
            }
            out_video_stream->time_base = enc_ctx->time_base;

            // Audio stream copy (if input has audio and output format supports it)
            AVStream *out_audio_stream = nullptr;
            bool copy_audio = false;
            if (audio_stream_idx >= 0)
            {
                AVStream *in_audio = fmt_ctx->streams[audio_stream_idx];
                // Check if the output format supports this audio codec
                if (avformat_query_codec(out_fmt_ctx->oformat,
                                         in_audio->codecpar->codec_id,
                                         FF_COMPLIANCE_NORMAL) == 1)
                {
                    out_audio_stream = avformat_new_stream(out_fmt_ctx.get(), nullptr);
                    if (out_audio_stream)
                    {
                        ret = avcodec_parameters_copy(out_audio_stream->codecpar,
                                                      in_audio->codecpar);
                        if (ret >= 0)
                        {
                            out_audio_stream->time_base = in_audio->time_base;
                            out_audio_stream->codecpar->codec_tag = 0;
                            copy_audio = true;
                        }
                    }
                }
            }

            // Open output file
            ret = avio_open(&out_fmt_ctx->pb, output_path.c_str(), AVIO_FLAG_WRITE);
            if (ret < 0)
            {
                return av_err_str(ret, "Failed to open output file");
            }

            ret = avformat_write_header(out_fmt_ctx.get(), nullptr);
            if (ret < 0)
            {
                return av_err_str(ret, "Failed to write header");
            }

            // Seek to start position if specified
            if (opts.start > 0)
            {
                int64_t start_ts = static_cast<int64_t>(opts.start * AV_TIME_BASE);
                av_seek_frame(fmt_ctx.get(), -1, start_ts, AVSEEK_FLAG_BACKWARD);
            }

            // Calculate end timestamp
            int64_t end_ts = INT64_MAX;
            if (opts.duration > 0)
            {
                end_ts = static_cast<int64_t>((opts.start + opts.duration) * AV_TIME_BASE);
            }

            // Allocate frames and packet
            FramePtr frame(av_frame_alloc());
            FramePtr filt_frame(av_frame_alloc());
            PacketPtr packet(av_packet_alloc());

            if (!frame || !filt_frame || !packet)
            {
                return "Failed to allocate frame/packet";
            }

            int64_t frame_count = 0;

            // Main decode/filter/encode loop
            while (true)
            {
                ret = av_read_frame(fmt_ctx.get(), packet.get());
                if (ret < 0)
                {
                    break; // EOF or error
                }

                // Copy audio packets directly
                if (copy_audio && packet->stream_index == audio_stream_idx)
                {
                    AVStream *in_audio = fmt_ctx->streams[audio_stream_idx];

                    // Check duration limit for audio too
                    if (packet->pts != AV_NOPTS_VALUE && opts.duration > 0)
                    {
                        int64_t pts_time = av_rescale_q(packet->pts, in_audio->time_base,
                                                        AVRational{1, AV_TIME_BASE});
                        if (pts_time > end_ts)
                        {
                            av_packet_unref(packet.get());
                            continue;
                        }
                    }

                    av_packet_rescale_ts(packet.get(), in_audio->time_base,
                                         out_audio_stream->time_base);
                    packet->stream_index = out_audio_stream->index;
                    packet->pos = -1;
                    av_interleaved_write_frame(out_fmt_ctx.get(), packet.get());
                    continue;
                }

                if (packet->stream_index != video_stream_idx)
                {
                    av_packet_unref(packet.get());
                    continue;
                }

                // Check if we've passed the duration limit
                if (packet->pts != AV_NOPTS_VALUE)
                {
                    int64_t pts_time = av_rescale_q(packet->pts, video_stream->time_base,
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
                {
                    continue; // Skip bad packets
                }

                while (ret >= 0)
                {
                    ret = avcodec_receive_frame(dec_ctx.get(), frame.get());
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    {
                        break;
                    }
                    if (ret < 0)
                    {
                        return av_err_str(ret, "Error decoding frame");
                    }

                    // Push frame to filter graph
                    ret = av_buffersrc_add_frame_flags(buffersrc_ctx, frame.get(),
                                                       AV_BUFFERSRC_FLAG_KEEP_REF);
                    av_frame_unref(frame.get());

                    if (ret < 0)
                    {
                        return av_err_str(ret, "Error feeding filter graph");
                    }

                    // Pull filtered frames
                    while (true)
                    {
                        ret = av_buffersink_get_frame(buffersink_ctx, filt_frame.get());
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                        {
                            break;
                        }
                        if (ret < 0)
                        {
                            return av_err_str(ret, "Error getting filtered frame");
                        }

                        filt_frame->pts = frame_count++;

                        // Encode frame
                        ret = avcodec_send_frame(enc_ctx.get(), filt_frame.get());
                        av_frame_unref(filt_frame.get());

                        if (ret < 0)
                        {
                            return av_err_str(ret, "Error sending frame to encoder");
                        }

                        while (ret >= 0)
                        {
                            ret = avcodec_receive_packet(enc_ctx.get(), packet.get());
                            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                            {
                                break;
                            }
                            if (ret < 0)
                            {
                                return av_err_str(ret, "Error encoding frame");
                            }

                            av_packet_rescale_ts(packet.get(), enc_ctx->time_base,
                                                 out_video_stream->time_base);
                            packet->stream_index = out_video_stream->index;

                            ret = av_interleaved_write_frame(out_fmt_ctx.get(), packet.get());
                            if (ret < 0)
                            {
                                return av_err_str(ret, "Error writing frame");
                            }
                        }
                    }
                }
            }

            // Flush decoder
            avcodec_send_packet(dec_ctx.get(), nullptr);
            while (avcodec_receive_frame(dec_ctx.get(), frame.get()) >= 0)
            {
                av_buffersrc_add_frame_flags(buffersrc_ctx, frame.get(), 0);
                av_frame_unref(frame.get());

                while (av_buffersink_get_frame(buffersink_ctx, filt_frame.get()) >= 0)
                {
                    filt_frame->pts = frame_count++;
                    avcodec_send_frame(enc_ctx.get(), filt_frame.get());
                    av_frame_unref(filt_frame.get());

                    while (avcodec_receive_packet(enc_ctx.get(), packet.get()) >= 0)
                    {
                        av_packet_rescale_ts(packet.get(), enc_ctx->time_base,
                                             out_video_stream->time_base);
                        packet->stream_index = out_video_stream->index;
                        av_interleaved_write_frame(out_fmt_ctx.get(), packet.get());
                    }
                }
            }

            // Flush filter graph
            av_buffersrc_add_frame_flags(buffersrc_ctx, nullptr, 0);
            while (av_buffersink_get_frame(buffersink_ctx, filt_frame.get()) >= 0)
            {
                filt_frame->pts = frame_count++;
                avcodec_send_frame(enc_ctx.get(), filt_frame.get());
                av_frame_unref(filt_frame.get());

                while (avcodec_receive_packet(enc_ctx.get(), packet.get()) >= 0)
                {
                    av_packet_rescale_ts(packet.get(), enc_ctx->time_base,
                                         out_video_stream->time_base);
                    packet->stream_index = out_video_stream->index;
                    av_interleaved_write_frame(out_fmt_ctx.get(), packet.get());
                }
            }

            // Flush encoder
            avcodec_send_frame(enc_ctx.get(), nullptr);
            while (avcodec_receive_packet(enc_ctx.get(), packet.get()) >= 0)
            {
                av_packet_rescale_ts(packet.get(), enc_ctx->time_base,
                                     out_video_stream->time_base);
                packet->stream_index = out_video_stream->index;
                av_interleaved_write_frame(out_fmt_ctx.get(), packet.get());
            }

            // Write trailer
            ret = av_write_trailer(out_fmt_ctx.get());
            if (ret < 0)
            {
                return av_err_str(ret, "Error writing trailer");
            }

            return ""; // Success
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
        {
            return nullptr;
        }

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
        {
            av_log_set_level(AV_LOG_INFO);
        }
        else
        {
            av_log_set_level(AV_LOG_QUIET);
        }

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
        std::string target = "gif"; // default for backward compat
        if (request->target && request->target[0] != '\0')
        {
            target = to_lower(request->target);
        }

        // Determine output path
        std::string output_path;
        if (request->output)
        {
            output_path = request->output;
        }
        else
        {
            // Use input stem + target extension
            size_t dot = source_path.rfind('.');
            if (dot != std::string::npos)
            {
                output_path = source_path.substr(0, dot) + "." + target;
            }
            else
            {
                output_path = source_path + "." + target;
            }
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
        VideoConverter::Options opts;
        opts.target = target;
        opts.fps = get_option_int(request, "fps", 0);
        opts.width = get_option_int(request, "width", -1);
        opts.height = get_option_int(request, "height", -1);
        opts.quality = get_option_int(request, "quality", 75);
        opts.start = get_option_float(request, "start", 0.0f);
        opts.duration = get_option_float(request, "duration", 0.0f);
        opts.loop = get_option_int(request, "loop", 0);

        // Clamp FPS to reasonable range (only when explicitly set)
        if (opts.fps > 0)
        {
            if (opts.fps < 1)
                opts.fps = 1;
            int max_fps = (target == "gif") ? 30 : 120;
            if (opts.fps > max_fps)
                opts.fps = max_fps;
        }

        // Clamp quality
        if (opts.quality < 1)
            opts.quality = 1;
        if (opts.quality > 100)
            opts.quality = 100;

        // Dry run
        if (request->dry_run)
        {
            result->status = UNICONV_SUCCESS;
            result->output = strdup(output_path.c_str());

            std::ostringstream extra;
            extra << "{\"dry_run\": true";
            extra << ", \"target\": \"" << target << "\"";
            extra << ", \"fps\": " << opts.fps;
            if (opts.width > 0)
                extra << ", \"width\": " << opts.width;
            if (opts.height > 0)
                extra << ", \"height\": " << opts.height;
            extra << ", \"quality\": " << opts.quality;
            if (opts.start > 0)
                extra << ", \"start\": " << opts.start;
            if (opts.duration > 0)
                extra << ", \"duration\": " << opts.duration;
            if (target == "gif")
                extra << ", \"loop\": " << opts.loop;
            extra << "}";
            result->extra_json = strdup(extra.str().c_str());

            return result;
        }

        // Convert
        VideoConverter converter;
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
        extra << ", \"fps\": " << opts.fps;
        if (opts.width > 0)
            extra << ", \"width\": " << opts.width;
        if (opts.height > 0)
            extra << ", \"height\": " << opts.height;
        extra << ", \"quality\": " << opts.quality;
        if (opts.start > 0)
            extra << ", \"start\": " << opts.start;
        if (opts.duration > 0)
            extra << ", \"duration\": " << opts.duration;
        if (target == "gif")
            extra << ", \"loop\": " << opts.loop;
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
