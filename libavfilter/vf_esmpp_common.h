#ifndef VF_ESMPP_COMMON
#define VF_ESMPP_COMMON

#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>
#include <limits.h>
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
#include "mpp_type.h"
#include "libavutil/esmpp_utils.h"

#define SUCCESS (0)
#define FAILURE (-1)

typedef struct MppFilterContext {
    const AVClass *class;
    MppCtxPtr mctx;

    FFFrameSync fs;
    ES_BOOL fs_init;
    int nb_inputs;
    enum AVPixelFormat in_fmt;
    enum AVPixelFormat out_fmt;

    // output hwcontext
    AVBufferRef *out_hw_device_ref;
    AVBufferRef *out_hw_frm_ref;
    MppBufferGroupPtr buf_grp;

    RECT_S src_rect;
    RECT_S dst_rect;
    ROTATION_E src_rotation;
    ROTATION_E dst_rotation;
    ES_S32 src_global_alpha;
    ES_S32 dst_global_alpha;
    ES_S32 blend_mode;

    char *crop_set;
    char *clip_set;
    int32_t output_w_set;
    int32_t output_h_set;
    int32_t output_fmt_set;
    char *rotation_set;
    int32_t src_global_alpha_set;
    int32_t dst_global_alpha_set;
    int32_t blend_mode_set;
    int32_t in_eof;
    int32_t snapshot_interval;
    int32_t out_cnt;
    int32_t snapshot_cnt;

    int enable_logo;
    int logo_x_dir;
    int logo_y_dir;
    int logo_x;
    int logo_y;
    int rand_logo_duration;
    int rand_logo_frames;
    int rand_logo_count;
    int rand_logo_sent;
    int rand_logo_pos;
    RECT_S disp_rect;

    char *nor_mm_r;       // normalization maxMinReciprocal
    char *nor_min;        // normalization minValue
    char *nor_mean;       // normalization meanValue
    char *nor_std_r;      // normalization stdReciprocal
    uint32_t nor_step_r;  // normalization stepReciprocal
    int32_t nor_mode;     // normalization mode
    TdeNnParam nor_params;
    int32_t nor_set;

#ifdef TRANSCODE_INFO_REPORT
    TdeReportInfo report_info;
    ES_S32 in_buffer_num;
    ES_S32 out_buffer_num;
    ES_S32 fail_buffer_num;
    ES_BOOL process_started;
#endif
    int die_idx;
    int device_idx;
    ES_BOOL mpp_init;

    void *handle;
} MppFilterContext;

// normalization rgb
typedef struct _NorRgb {
    uint32_t R;
    uint32_t G;
    uint32_t B;
} NorRgb;

#define MAX_ID 1024  // max id
#define BITS_PER_UNIT (sizeof(uint32_t) * CHAR_BIT)

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

int esmpp_set_mpp_frame(MppFramePtr mpp_frame,
                        MppFrameFormat fmt,
                        int width,
                        int height,
                        int *stride,
                        int *offset,
                        ROTATION_E rotation,
                        ES_S32 global_alpha);

int esmpp_stack_get_offset_stride(
    const AVFrame *in, enum AVPixelFormat fmt, int *stride, int *offset, uint32_t *pic_size);

int init_hwcontext_buf(AVFilterLink *filterlink,
                       AVBufferRef **hw_device_ref,
                       AVBufferRef **hw_frm_ref,
                       int pixelfmt,
                       int device_idx,
                       int die_idx);
void get_dev_info(AVBufferRef *hw_device_ctx, AVBufferRef *hw_frames_ctx, int *dev_id, int *die_id);

int check_rect_if_valid(RECT_S *rect, int frm_width, int frm_height, enum AVPixelFormat fmt);

uint64_t get_ticks_by_ms(void);

// snapshot
ES_BOOL esmpp_check_snapshot(int interval, int output_count, int snapshot_output);
int esmpp_snapshot_send_frame(AVFilterLink *outlink, AVFrame *outframe);

// normalization
int parse_normalization(MppFilterContext *s);
#endif