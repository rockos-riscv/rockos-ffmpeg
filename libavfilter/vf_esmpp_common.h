#ifndef VF_ESMPP_COMMON
#define VF_ESMPP_COMMON

#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include "libavutil/avstring.h"
#include "libavutil/common.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_esmpp.h"
#include "libavcodec/defs.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
#include "libavutil/imgutils.h"
#include "avfilter.h"
#include "formats.h"
#include "framesync.h"
#include "internal.h"
#include "video.h"
#include "mpp_tde_api.h"
#include "mpp_buffer.h"

#define SUCCESS (0)
#define FAILURE (-1)

ES_BOOL write_buffer_to_file(const void *buffer,
                             ES_S32 size,
                             const char *path,
                             ES_U32 width,
                             ES_U32 height,
                             enum AVPixelFormat format,
                             ES_S32 index);

int frame_create_buf(
    AVFrame *frame, uint8_t *data, int size, void (*free)(void *opaque, uint8_t *data), void *opaque, int flags);

size_t get_pic_buf_info(
    enum AVPixelFormat fmt, int width, int height, int align, int align_h, int *p_stride, int *p_offset, int *p_plane);

int get_alignment_by_format(enum AVPixelFormat fmt);

void adjust_width_height_by_format(enum AVPixelFormat fmt, int *width, int *height);

int esmpp_get_frame_data_size(const enum AVPixelFormat fmt, const AVFrame *in);

int esmpp_memcpy_host2device(const enum AVPixelFormat fmt, const AVFrame *in, void *out_vir);

void esmpp_free_frame_buf(void *opaque, uint8_t *data);

void esmpp_free_drm_desc(void *opaque, uint8_t *data);

int esmpp_buffer_export_frame(AVFrame *frame, MppFramePtr mpp_frame, int nb_planes, int *offset, int *stride);

MppFrameFormat ff_fmt_to_mpp_fmt(enum AVPixelFormat ff_fmt);

#endif