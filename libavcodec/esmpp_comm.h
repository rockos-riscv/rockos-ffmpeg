#ifndef AVCODEC_ESMPP_H
#define AVCODEC_ESMPP_H

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <linux/limits.h>

#include "libavutil/time.h"
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

#ifndef SUCCESS
#define SUCCESS (0)
#endif

#ifndef FAILURE
#define FAILURE (-1)
#endif

#ifndef ERR_TIMEOUT
#define ERR_TIMEOUT (-2)
#endif

typedef struct {
    int width;
    int height;
    int pic_stride;    /**< picture stride for luma */
    int pic_stride_ch; /**< picture stride for chroma */
    char *ppu_channel;
    const char *prefix_name;
    const char *suffix_name;
    const char *fmt;
} DumpParas;

typedef struct {
    int64_t stop_dump_time;
    FILE *fp;
} DumpHandle;

void esmpp_set_log_level(void);

const char *esmpp_get_fmt_char(enum AVPixelFormat pix_fmt);

DumpHandle *esmpp_codec_dump_file_open(const char *dump_path, int duration, DumpParas *paras);

int esmpp_codec_compara_timeb(int64_t end_time);

int esmpp_codec_dump_file_close(DumpHandle **dump_handle);

int esmpp_codec_dump_bytes_to_file(void *data, int size, DumpHandle *dump_handle);

void esmpp_get_picsize(enum AVPixelFormat pix_fmt,
                       uint32_t width,
                       uint32_t height,
                       uint32_t h_alignment,
                       uint32_t v_alignment,
                       uint32_t *para_luma_size,
                       uint32_t *para_chroma_size,
                       uint32_t *para_picture_size);

uint64_t esmpp_get_picbufinfo(enum AVPixelFormat pix_fmt,
                            uint32_t width,
                            uint32_t height,
                            uint32_t align,
                            uint32_t alignHeight,
                            uint32_t *pStride,
                            uint32_t *pOffset,
                            uint32_t *pPlane);

#endif