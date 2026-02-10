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

#include <cairo.h>
#include <cairo-pdf.h>
#include <poppler.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>
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

    bool is_pdf_input(const std::string &path)
    {
        size_t dot = path.rfind('.');
        if (dot == std::string::npos)
            return false;
        std::string ext = to_lower(path.substr(dot + 1));
        return ext == "pdf";
    }

    vips::VImage load_pdf_as_vimage(const std::string &path)
    {
        // Build file:// URI from path
        std::string uri = "file://" + path;

        GError *error = nullptr;
        PopplerDocument *doc = poppler_document_new_from_file(uri.c_str(), nullptr, &error);
        if (!doc)
        {
            std::string msg = "Failed to open PDF: ";
            if (error)
            {
                msg += error->message;
                g_error_free(error);
            }
            throw std::runtime_error(msg);
        }

        if (poppler_document_get_n_pages(doc) == 0)
        {
            g_object_unref(doc);
            throw std::runtime_error("PDF has no pages");
        }

        PopplerPage *page = poppler_document_get_page(doc, 0);
        if (!page)
        {
            g_object_unref(doc);
            throw std::runtime_error("Failed to get first page of PDF");
        }

        double page_w, page_h;
        poppler_page_get_size(page, &page_w, &page_h);

        // Render at 300 DPI (PDF points are 72 DPI)
        constexpr double kDPI = 300.0;
        constexpr double kScale = kDPI / 72.0;
        int pixel_w = static_cast<int>(page_w * kScale);
        int pixel_h = static_cast<int>(page_h * kScale);

        cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixel_w, pixel_h);
        cairo_t *cr = cairo_create(surface);

        // White background
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_paint(cr);

        // Scale and render
        cairo_scale(cr, kScale, kScale);
        poppler_page_render(page, cr);
        cairo_destroy(cr);

        g_object_unref(page);
        g_object_unref(doc);

        cairo_surface_flush(surface);
        unsigned char *cairo_data = cairo_image_surface_get_data(surface);
        int stride = cairo_image_surface_get_stride(surface);

        // Convert BGRA (cairo) -> RGBA
        std::vector<unsigned char> rgba(static_cast<size_t>(pixel_w) * pixel_h * 4);
        for (int y = 0; y < pixel_h; y++)
        {
            const unsigned char *src_row = cairo_data + y * stride;
            unsigned char *dst_row = rgba.data() + y * pixel_w * 4;
            for (int x = 0; x < pixel_w; x++)
            {
                // Cairo ARGB32 is stored as native-endian uint32: on little-endian = BGRA bytes
                unsigned char b = src_row[x * 4 + 0];
                unsigned char g = src_row[x * 4 + 1];
                unsigned char r = src_row[x * 4 + 2];
                unsigned char a = src_row[x * 4 + 3];
                // Un-premultiply alpha
                if (a > 0 && a < 255)
                {
                    r = static_cast<unsigned char>(std::min(255, r * 255 / a));
                    g = static_cast<unsigned char>(std::min(255, g * 255 / a));
                    b = static_cast<unsigned char>(std::min(255, b * 255 / a));
                }
                dst_row[x * 4 + 0] = r;
                dst_row[x * 4 + 1] = g;
                dst_row[x * 4 + 2] = b;
                dst_row[x * 4 + 3] = a;
            }
        }

        cairo_surface_destroy(surface);

        vips::VImage image = vips::VImage::new_from_memory(
            rgba.data(), rgba.size(), pixel_w, pixel_h, 4, VIPS_FORMAT_UCHAR);
        // copy_memory() so vips owns the pixel data (rgba vector will be freed)
        return image.copy_memory();
    }

    void save_vimage_as_pdf(vips::VImage image, const std::string &output_path)
    {
        // Flatten alpha if present
        if (image.bands() == 4)
        {
            image = image.flatten();
        }

        int w = image.width();
        int h = image.height();
        int bands = image.bands(); // should be 3 (RGB) after flatten

        // Create PDF surface (1 pixel = 1 point = 1/72 inch)
        cairo_surface_t *pdf_surface = cairo_pdf_surface_create(output_path.c_str(), w, h);
        if (cairo_surface_status(pdf_surface) != CAIRO_STATUS_SUCCESS)
        {
            cairo_surface_destroy(pdf_surface);
            throw std::runtime_error("Failed to create PDF surface: " +
                                     std::string(cairo_status_to_string(cairo_surface_status(pdf_surface))));
        }

        cairo_t *cr = cairo_create(pdf_surface);

        // Create an image surface from vips data
        // Cairo expects BGRA (ARGB32 in native endian on little-endian)
        cairo_surface_t *img_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        unsigned char *cairo_data = cairo_image_surface_get_data(img_surface);
        int stride = cairo_image_surface_get_stride(img_surface);

        // Access vips pixel data
        const unsigned char *vips_data = static_cast<const unsigned char *>(image.data());
        int vips_stride = w * bands;

        for (int y = 0; y < h; y++)
        {
            const unsigned char *src_row = vips_data + y * vips_stride;
            unsigned char *dst_row = cairo_data + y * stride;
            for (int x = 0; x < w; x++)
            {
                unsigned char r = src_row[x * bands + 0];
                unsigned char g = src_row[x * bands + 1];
                unsigned char b = src_row[x * bands + 2];
                // Cairo ARGB32 on little-endian: BGRA byte order
                dst_row[x * 4 + 0] = b;
                dst_row[x * 4 + 1] = g;
                dst_row[x * 4 + 2] = r;
                dst_row[x * 4 + 3] = 255; // fully opaque
            }
        }

        cairo_surface_mark_dirty(img_surface);

        cairo_set_source_surface(cr, img_surface, 0, 0);
        cairo_paint(cr);

        cairo_destroy(cr);
        cairo_surface_destroy(img_surface);
        cairo_surface_finish(pdf_surface);

        cairo_status_t status = cairo_surface_status(pdf_surface);
        cairo_surface_destroy(pdf_surface);

        if (status != CAIRO_STATUS_SUCCESS)
        {
            throw std::runtime_error("Failed to write PDF: " +
                                     std::string(cairo_status_to_string(status)));
        }
    }

    void save_vimage_as_bmp(vips::VImage image, const std::string &output_path)
    {
        // Flatten alpha onto white background if present
        if (image.bands() == 4)
        {
            image = image.flatten();
        }

        int w = image.width();
        int h = image.height();
        int bands = image.bands(); // should be 3
        const unsigned char *data = static_cast<const unsigned char *>(image.data());

        // BMP row size: 3 bytes/pixel, padded to 4-byte boundary
        int row_size = (w * 3 + 3) & ~3;
        int pixel_data_size = row_size * h;
        int file_size = 14 + 40 + pixel_data_size; // header + DIB header + pixels

        FILE *fp = fopen(output_path.c_str(), "wb");
        if (!fp)
        {
            throw std::runtime_error("Failed to open output file: " + output_path);
        }

        auto write_u16 = [&](uint16_t v)
        { fwrite(&v, 2, 1, fp); };
        auto write_u32 = [&](uint32_t v)
        { fwrite(&v, 4, 1, fp); };

        // BMP file header (14 bytes)
        fwrite("BM", 1, 2, fp);
        write_u32(static_cast<uint32_t>(file_size));
        write_u16(0); // reserved1
        write_u16(0); // reserved2
        write_u32(14 + 40); // pixel data offset

        // BITMAPINFOHEADER (40 bytes)
        write_u32(40); // header size
        write_u32(static_cast<uint32_t>(w));
        write_u32(static_cast<uint32_t>(h));
        write_u16(1);  // planes
        write_u16(24); // bits per pixel
        write_u32(0);  // compression (none)
        write_u32(static_cast<uint32_t>(pixel_data_size));
        write_u32(2835); // X pixels/meter (~72 DPI)
        write_u32(2835); // Y pixels/meter
        write_u32(0);    // colors used
        write_u32(0);    // important colors

        // Pixel data: bottom-to-top, RGB -> BGR
        std::vector<unsigned char> row_buf(static_cast<size_t>(row_size), 0);
        for (int y = h - 1; y >= 0; y--)
        {
            const unsigned char *src_row = data + y * w * bands;
            for (int x = 0; x < w; x++)
            {
                row_buf[x * 3 + 0] = src_row[x * bands + 2]; // B
                row_buf[x * 3 + 1] = src_row[x * bands + 1]; // G
                row_buf[x * 3 + 2] = src_row[x * bands + 0]; // R
            }
            fwrite(row_buf.data(), 1, static_cast<size_t>(row_size), fp);
        }

        fclose(fp);
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
            vips::VImage image;
            if (is_pdf_input(source_path))
                image = load_pdf_as_vimage(source_path);
            else
                image = vips::VImage::new_from_file(source_path.c_str());

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
                save_vimage_as_bmp(image, output_path);
            }
            else if (target == "pdf")
            {
                save_vimage_as_pdf(image, output_path);
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
