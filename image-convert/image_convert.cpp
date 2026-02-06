/**
 * uniconv Native Plugin: image-convert
 *
 * Image format conversion using libvips.
 * Converts between image formats (HEIC, JPEG, PNG, WebP, etc.)
 * with optional resize and quality control.
 *
 * Supported options:
 *   --quality/-q   Output quality (1-100)
 *   --width/-w     Output width (aspect ratio preserved)
 *   --height/-h    Output height (aspect ratio preserved)
 *
 * Build:
 *   mkdir build && cd build
 *   cmake ..
 *   cmake --build .
 */

#include <uniconv/plugin_api.h>

#include <vips/vips8>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <sys/stat.h>

// Plugin info
static const char *targets[] = {"jpg", "jpeg", "png", "webp", "gif", "bmp", "tiff", "heic", "heif", "pdf", nullptr};
static const char *input_formats[] = {"heic", "heif", "jpg", "jpeg", "png", "webp", "gif", "bmp", "tiff", "pdf", nullptr};

// Data type information
static UniconvDataType input_types[] = {UNICONV_DATA_IMAGE, UNICONV_DATA_FILE, (UniconvDataType)0};
static UniconvDataType output_types[] = {UNICONV_DATA_IMAGE, (UniconvDataType)0};

static UniconvPluginInfo plugin_info = {
    .name = "image-convert",
    .scope = "image-convert",
    .version = "1.0.0",
    .description = "Image format conversion using libvips",
    .targets = targets,
    .input_formats = input_formats,
    .input_types = input_types,
    .output_types = output_types};

namespace
{

    // Default quality values
    constexpr int kDefaultJpegQuality = 85;
    constexpr int kDefaultWebpQuality = 80;
    constexpr int kDefaultPngQuality = 90;
    constexpr int kDefaultHeifQuality = 50;
    constexpr int kDefaultTiffQuality = 85;
    constexpr int kDefaultGenericQuality = 85;

    bool file_exists(const std::string &path)
    {
        struct stat st;
        return stat(path.c_str(), &st) == 0;
    }

    size_t get_file_size(const std::string &path)
    {
        struct stat st;
        if (stat(path.c_str(), &st) != 0)
            return 0;
        return static_cast<size_t>(st.st_size);
    }

    std::string to_lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c)
                       { return std::tolower(c); });
        return s;
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

    /**
     * Get the appropriate quality default for a format
     */
    int get_quality_for_format(const std::string &format, int quality)
    {
        if (quality > 0)
            return std::clamp(quality, 1, 100);

        std::string lower = to_lower(format);
        if (lower == "jpg" || lower == "jpeg")
            return kDefaultJpegQuality;
        if (lower == "webp")
            return kDefaultWebpQuality;
        if (lower == "png")
            return kDefaultPngQuality;
        if (lower == "heic" || lower == "heif")
            return kDefaultHeifQuality;
        if (lower == "tiff")
            return kDefaultTiffQuality;
        return kDefaultGenericQuality;
    }

    /**
     * Determine output path from request
     */
    std::string determine_output_path(const std::string &source, const char *output,
                                      const std::string &target)
    {
        if (output && output[0] != '\0')
        {
            return output;
        }

        // Default: same directory, same name, new extension
        std::string result = source;
        size_t dot = result.rfind('.');
        size_t slash = result.find_last_of("/\\");

        if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        {
            result = result.substr(0, dot);
        }
        return result + "." + target;
    }

    /**
     * Create parent directories for a path (simple implementation)
     */
    void ensure_parent_dir(const std::string &path)
    {
        size_t pos = path.find_last_of("/\\");
        if (pos == std::string::npos || pos == 0)
            return;

        std::string dir = path.substr(0, pos);
        // Use mkdir -p equivalent via system; we only need this for edge cases
        struct stat st;
        if (stat(dir.c_str(), &st) != 0)
        {
            std::string cmd = "mkdir -p '" + dir + "'";
            system(cmd.c_str());
        }
    }

} // anonymous namespace

extern "C"
{

    UNICONV_EXPORT int uniconv_plugin_init(void)
    {
        if (VIPS_INIT("image-convert"))
        {
            return -1;
        }
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

        std::string source_path = request->source;
        std::string target = request->target ? to_lower(request->target) : "";

        // Check input file exists
        if (!file_exists(source_path))
        {
            result->status = UNICONV_ERROR;
            std::string msg = "Input file not found: " + source_path;
            result->error = strdup(msg.c_str());
            return result;
        }

        // Determine output path
        std::string output_path = determine_output_path(source_path, request->output, target);

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
        int quality_opt = get_option_int(request, "quality", 0);
        int width = get_option_int(request, "width", 0);
        int height = get_option_int(request, "height", 0);

        // Dry run
        if (request->dry_run)
        {
            result->status = UNICONV_SUCCESS;
            result->output = strdup(output_path.c_str());

            std::ostringstream extra;
            extra << "{\"dry_run\": true";
            if (quality_opt > 0)
                extra << ", \"quality\": " << quality_opt;
            if (width > 0)
                extra << ", \"width\": " << width;
            if (height > 0)
                extra << ", \"height\": " << height;
            extra << "}";
            result->extra_json = strdup(extra.str().c_str());

            return result;
        }

        try
        {
            // Load image
            vips::VImage image = vips::VImage::new_from_file(source_path.c_str());

            size_t input_size = get_file_size(source_path);

            // Record original dimensions before resize
            int orig_width = image.width();
            int orig_height = image.height();

            // Apply resize if width or height specified
            if (width > 0 || height > 0)
            {
                if (width > 0 && height > 0)
                {
                    // Resize to exact dimensions
                    image = image.thumbnail_image(width,
                                                  vips::VImage::option()
                                                      ->set("height", height)
                                                      ->set("size", VIPS_SIZE_FORCE));
                }
                else if (width > 0)
                {
                    // Resize by width, maintain aspect ratio
                    image = image.thumbnail_image(width);
                }
                else if (height > 0)
                {
                    // Resize by height, maintain aspect ratio
                    double scale = static_cast<double>(height) / image.height();
                    int new_width = static_cast<int>(image.width() * scale);
                    image = image.thumbnail_image(new_width,
                                                  vips::VImage::option()->set("height", height));
                }
            }

            // Ensure parent directory exists
            ensure_parent_dir(output_path);

            // Get quality setting
            int quality = get_quality_for_format(target, quality_opt);

            // Save based on format
            if (target == "jpg" || target == "jpeg")
            {
                image.jpegsave(output_path.c_str(),
                               vips::VImage::option()->set("Q", quality));
            }
            else if (target == "png")
            {
                // PNG: quality maps to compression (100=no compression, 0=max)
                int compression = (100 - quality) * 9 / 100;
                image.pngsave(output_path.c_str(),
                              vips::VImage::option()->set("compression", compression));
            }
            else if (target == "webp")
            {
                image.webpsave(output_path.c_str(),
                               vips::VImage::option()->set("Q", quality));
            }
            else if (target == "gif")
            {
                image.gifsave(output_path.c_str());
            }
            else if (target == "tiff")
            {
                image.tiffsave(output_path.c_str(),
                               vips::VImage::option()->set("Q", quality));
            }
            else if (target == "heic" || target == "heif")
            {
                image.heifsave(output_path.c_str(),
                               vips::VImage::option()->set("Q", quality));
            }
            else if (target == "bmp")
            {
                image.magicksave(output_path.c_str(),
                                 vips::VImage::option()->set("format", "BMP"));
            }
            else if (target == "pdf")
            {
                image.magicksave(output_path.c_str(),
                                 vips::VImage::option()->set("format", "PDF"));
            }
            else
            {
                result->status = UNICONV_ERROR;
                std::string msg = "Unsupported output format: " + target;
                result->error = strdup(msg.c_str());
                return result;
            }

            // Verify output was created
            if (!file_exists(output_path))
            {
                result->status = UNICONV_ERROR;
                result->error = strdup("Conversion completed but output file not found");
                return result;
            }

            size_t output_size = get_file_size(output_path);

            // Success
            result->status = UNICONV_SUCCESS;
            result->output = strdup(output_path.c_str());
            result->output_size = output_size;

            // Extra metadata
            std::ostringstream extra;
            extra << "{\"input_dimensions\": {\"width\": " << orig_width
                  << ", \"height\": " << orig_height << "}"
                  << ", \"output_dimensions\": {\"width\": " << image.width()
                  << ", \"height\": " << image.height() << "}"
                  << "}";
            result->extra_json = strdup(extra.str().c_str());

            return result;
        }
        catch (const vips::VError &e)
        {
            result->status = UNICONV_ERROR;
            std::string msg = "libvips error: " + std::string(e.what());
            result->error = strdup(msg.c_str());
            return result;
        }
        catch (const std::exception &e)
        {
            result->status = UNICONV_ERROR;
            std::string msg = std::string("Error: ") + e.what();
            result->error = strdup(msg.c_str());
            return result;
        }
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
