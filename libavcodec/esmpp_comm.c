#include "esmpp_comm.h"
#include "avcodec.h"

#include "mpp_log.h"

void esmpp_set_log_level(void) {
    int level = av_log_get_level();

    switch (level) {
        case AV_LOG_PANIC:
        case AV_LOG_FATAL:
            mpp_set_log_level(MPP_LOG_FATAL);
            break;
        case AV_LOG_ERROR:
            mpp_set_log_level(MPP_LOG_ERROR);
            break;
        case AV_LOG_WARNING:
            mpp_set_log_level(MPP_LOG_WARN);
            break;
        case AV_LOG_INFO:
            mpp_set_log_level(MPP_LOG_INFO);
            break;
        case AV_LOG_DEBUG:
            mpp_set_log_level(MPP_LOG_DEBUG);
            break;
        case AV_LOG_TRACE:
        case AV_LOG_VERBOSE:
            mpp_set_log_level(MPP_LOG_VERBOSE);
            break;
        case AV_LOG_QUIET:
            mpp_set_log_level(MPP_LOG_SILENT);
            break;
        default:
            mpp_set_log_level(MPP_LOG_INFO);
            break;
    }
}

const char *esmpp_get_fmt_char(enum AVPixelFormat pix_fmt) {
    switch (pix_fmt) {
        case AV_PIX_FMT_YUV420P:
            return "yuv420p";
        case AV_PIX_FMT_NV12:
            return "nv12";
        case AV_PIX_FMT_NV21:
            return "nv21";
        case AV_PIX_FMT_UYVY422:
            return "uyvy422";
        case AV_PIX_FMT_YUYV422:
            return "yuyv422";
        case AV_PIX_FMT_YUV420P10LE:
            return "I010";
        case AV_PIX_FMT_P010LE:
            return "p010";
        default:
            return "unknown";
    }
}

#define STRIDE(variable, alignment) ((variable + alignment - 1) & (~(alignment - 1)))
void esmpp_get_picsize(enum AVPixelFormat pix_fmt,
                       uint32_t width,
                       uint32_t height,
                       uint32_t h_alignment,
                       uint32_t v_alignment,
                       uint32_t *para_luma_size,
                       uint32_t *para_chroma_size,
                       uint32_t *para_picture_size) {
    uint32_t luma_stride = 0, chroma_stride = 0;
    uint32_t luma_size = 0, chroma_size = 0, picture_size = 0;
    uint32_t v_stride = STRIDE(height, v_alignment);

    switch (pix_fmt) {
        case AV_PIX_FMT_YUV420P:
            luma_stride = STRIDE(width, h_alignment);
            chroma_stride = STRIDE(width, h_alignment) / 2;
            luma_size = luma_stride * v_stride;
            chroma_size = chroma_stride * v_stride / 2 * 2;
            break;
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_NV21:
            luma_stride = STRIDE(width, h_alignment);
            chroma_stride = STRIDE(width, h_alignment);
            luma_size = luma_stride * v_stride;
            chroma_size = chroma_stride * v_stride / 2;
            break;
        case AV_PIX_FMT_UYVY422:
        case AV_PIX_FMT_YUYV422:
            luma_stride = STRIDE(width * 2, h_alignment);
            chroma_stride = 0;
            luma_size = luma_stride * v_stride;
            chroma_size = 0;
            break;
        case AV_PIX_FMT_YUV420P10LE:
            luma_stride = STRIDE(width * 2, h_alignment);
            chroma_stride = STRIDE(width / 2 * 2, h_alignment);
            luma_size = luma_stride * v_stride;
            chroma_size = chroma_stride * v_stride / 2 * 2;
            break;
        case AV_PIX_FMT_P010LE:
            luma_stride = STRIDE(width * 2, h_alignment);
            chroma_stride = STRIDE(width * 2, h_alignment);
            luma_size = luma_stride * v_stride;
            chroma_size = chroma_stride * v_stride / 2;
            break;

        default:
            printf("not support this format\n");
            chroma_size = luma_size = 0;
            break;
    }

    picture_size = luma_size + chroma_size;
    if (para_luma_size != NULL) *para_luma_size = luma_size;
    if (para_chroma_size != NULL) *para_chroma_size = chroma_size;
    if (para_picture_size != NULL) *para_picture_size = picture_size;
}

static uint32_t esmpp_get_bpp(enum AVPixelFormat pixelFormat) {
    uint8_t bpp[3] = {0, 0, 0};

    switch (pixelFormat) {
        case AV_PIX_FMT_YUV420P:
            bpp[0] = 8;
            bpp[1] = bpp[2] = 2;
            break;
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_NV21:
            bpp[0] = 8;
            bpp[1] = 4;
            break;

        case AV_PIX_FMT_UYVY422:
        case AV_PIX_FMT_YUYV422:
            bpp[0] = 16;
            break;
        case AV_PIX_FMT_P010LE:
            bpp[0] = 16;
            bpp[1] = bpp[2] = 4;
            break;
        case AV_PIX_FMT_YUV420P10LE:
            bpp[0] = 16;
            bpp[1] = 8;
            break;
        default:
            return 0;
    }

    return (uint32_t)bpp[0] + bpp[1] + bpp[2];
}

#define ES_ALIGN_UP(x, a) ((((x) + ((a) - 1)) / (a)) * (a))
uint64_t esmpp_get_picbufinfo(enum AVPixelFormat pix_fmt,
                              uint32_t width,
                              uint32_t height,
                              uint32_t align,
                              uint32_t alignHeight,
                              uint32_t *pStride,
                              uint32_t *pOffset,
                              uint32_t *pPlane) {
    uint32_t bpp, plane, stride;
    uint32_t uStride, vStride, uOffset, vOffset, alignWidth, strideAlign;

    bpp = esmpp_get_bpp(pix_fmt);
    if (!bpp) return 0;
    alignWidth = (align > 0) ? ES_ALIGN_UP(width, align) : width;
    alignHeight = (alignHeight > 0) ? ES_ALIGN_UP(height, alignHeight) : height;
    strideAlign = (align < 2) ? 2 : ES_ALIGN_UP(align, 2);
    stride = ES_ALIGN_UP(alignWidth, strideAlign);

    switch (pix_fmt) {
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_NV21:
            /*  WxH Y plane followed by (W)x(H/2) interleaved U/V plane. */
            stride = alignWidth;
            stride = ES_ALIGN_UP(stride, strideAlign);
            uStride = vStride = stride;
            uOffset = vOffset = stride * alignHeight;
            plane = 2;
            break;
        case AV_PIX_FMT_YUV420P:
            /*  WxH Y plane followed by (W/2)x(H/2) U and V planes. */
            uStride = vStride = (stride / 2);
            stride = ES_ALIGN_UP(stride, strideAlign);
            uStride = ES_ALIGN_UP(uStride, strideAlign / 2);
            vStride = ES_ALIGN_UP(vStride, strideAlign / 2);
            uOffset = stride * alignHeight;
            vOffset = uOffset + vStride * alignHeight / 2;
            plane = 3;
            break;
        case AV_PIX_FMT_P010LE:
            /*  WxH Y plane followed by (W/2)x(H/2) U and V planes. */
            stride = alignWidth * 2;
            uStride = vStride = (stride / 2);
            stride = ES_ALIGN_UP(stride, strideAlign);
            uStride = ES_ALIGN_UP(uStride, strideAlign / 2);
            vStride = ES_ALIGN_UP(vStride, strideAlign / 2);
            uOffset = stride * alignHeight;
            vOffset = uOffset + uStride * alignHeight / 2;
            plane = 3;
            break;
        case AV_PIX_FMT_YUV420P10LE:
            /*  WxH Y plane followed by (W)x(H/2) interleaved U/V plane. */
            stride = alignWidth * 2;
            stride = ES_ALIGN_UP(stride, strideAlign);
            uStride = vStride = stride;
            uOffset = vOffset = stride * alignHeight;
            plane = 2;
            break;
        default:
            stride = (alignWidth * bpp) / 8;
            uStride = vStride = 0;
            uOffset = vOffset = 0;
            plane = 1;
            break;
    }

    if (pStride) {
        pStride[0] = stride;
        if (plane > 1) pStride[1] = uStride;
        if (plane > 2) pStride[2] = vStride;
    }
    if (pOffset) {
        pOffset[0] = 0;
        if (plane > 1) pOffset[1] = uOffset;
        if (plane > 2) pOffset[2] = vOffset;
    }
    if (pPlane) {
        *pPlane = plane;
    }

    return (uint64_t)alignWidth * alignHeight * bpp / 8;
}

DumpHandle *esmpp_codec_dump_file_open(const char *dump_path, int duration, DumpParas *paras) {
    DumpHandle *dump_handle = NULL;
    char file_path[PATH_MAX];
    time_t now;
    char time_char[128];
    struct tm *tm;
    int ret;

    if (!dump_path) {
        av_log(NULL, AV_LOG_ERROR, "error !!! dump path is null\n");
        return NULL;
    }

    if (!paras) {
        av_log(NULL, AV_LOG_ERROR, "error !!! paras is null\n");
        return NULL;
    }

    ret = access(dump_path, 0);
    if (ret == -1) {
        av_log(NULL, AV_LOG_INFO, "dump_path: %s does not exist\n", dump_path);
        if (mkdir(dump_path, 0731) == -1) {
            av_log(NULL, AV_LOG_ERROR, "create dump_path: %s failed errno: %d\n", dump_path, errno);
            return NULL;
        }
    } else {
        av_log(NULL, AV_LOG_INFO, "dump_path: %s exist\n", dump_path);
    }

    if (duration <= 0) {
        av_log(NULL, AV_LOG_ERROR, "invalid dump_time: %d\n", duration);
        return NULL;
    }

    // get local time
    time(&now);
    tm = localtime(&now);
    strftime(time_char, sizeof(time_char), "%y%m%d%H%M%S", tm);

    dump_handle = (DumpHandle *)malloc(sizeof(DumpHandle));
    if (!dump_handle) {
        av_log(NULL, AV_LOG_ERROR, "dump_handle malloc failed\n");
        return NULL;
    }

    dump_handle->stop_dump_time = av_gettime_relative() + duration * 1000;

    if (!paras->pic_stride && !paras->pic_stride_ch && !paras->fmt) {
        snprintf(file_path,
                 sizeof(file_path),
                 "%s/%s_%s_%dms_%dx%d.%s",
                 dump_path,
                 paras->prefix_name,
                 time_char,
                 duration,
                 paras->width,
                 paras->height,
                 paras->suffix_name);
    } else if (!paras->pic_stride && !paras->pic_stride_ch) {
        snprintf(file_path,
                 sizeof(file_path),
                 "%s/%s_%s_%dms_%dx%d_%s.%s",
                 dump_path,
                 paras->prefix_name,
                 time_char,
                 duration,
                 paras->width,
                 paras->height,
                 paras->fmt,
                 paras->suffix_name);
    } else {
        snprintf(file_path,
                 sizeof(file_path),
                 "%s/%s_%s_%s_%dms_%dx%d_%dx%d(stride)_%s.%s",
                 dump_path,
                 paras->prefix_name,
                 paras->ppu_channel,
                 time_char,
                 duration,
                 paras->width,
                 paras->height,
                 paras->pic_stride,
                 paras->pic_stride_ch,
                 paras->fmt,
                 paras->suffix_name);
    }

    dump_handle->fp = fopen(file_path, "ab+");
    if (dump_handle->fp) {
        av_log(NULL, AV_LOG_INFO, "open %s success\n", file_path);
    } else {
        free(dump_handle);
        av_log(NULL, AV_LOG_ERROR, "open %s failed\n", file_path);
        return NULL;
    }

    return dump_handle;
}

int esmpp_codec_dump_file_close(DumpHandle **dump_handle) {
    if (!dump_handle || !(*dump_handle)) {
        return 0;
    }
    fflush((*dump_handle)->fp);
    fclose((*dump_handle)->fp);
    (*dump_handle)->fp = NULL;
    free(*dump_handle);
    *dump_handle = NULL;
    return 0;
}

// if now_time > end_time, return 1
int esmpp_codec_compara_timeb(int64_t end_time) {
    int64_t now_time;
    now_time = av_gettime_relative();

    if ((now_time - end_time) > 0) {
        return 1;
    } else {
        return 0;
    }
}

int esmpp_codec_dump_bytes_to_file(void *data, int size, DumpHandle *dump_handle) {
    int len = 0;

    if (!dump_handle || !dump_handle->fp) {
        av_log(NULL, AV_LOG_ERROR, " invalid dump_handle\n");
        return FAILURE;
    }

    if (!data || size <= 0) {
        return FAILURE;
    }

    if (esmpp_codec_compara_timeb(dump_handle->stop_dump_time) > 0) {
        av_log(NULL, AV_LOG_INFO, "data dump stop\n");
        return ERR_TIMEOUT;
    } else {
        len = fwrite(data, 1, size, dump_handle->fp);
        fflush(dump_handle->fp);
        if (len != size) {
            av_log(NULL, AV_LOG_ERROR, "write data to file error !!! len: %d, data size: %d\n", len, size);
        }
    }

    return len;
}
