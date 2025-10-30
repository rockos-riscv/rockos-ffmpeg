#ifndef AVCODEC_VF_ESMPP_ADDROI_H
#define AVCODEC_VF_ESMPP_ADDROI_H

#include "libavutil/avstring.h"
#include "libavutil/common.h"
#include "libavutil/hwcontext.h"
//#include "libavutil/hwcontext_esmpp.h"
#include "libavcodec/defs.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
#include "libavutil/imgutils.h"
#include "mpp_buffer.h"
#include "mpp_type.h"
#include "es_roi_def.h"

typedef struct MppAddRoiInInfo
{
    enum AVPixelFormat in_fmt;
    int die_idx;
    int device_idx;
    ES_BOOL use_hwaccel;
} MppAddRoiInInfo;

typedef struct MppAddRoiContext
{
    const AVClass *class;
    MppRoiCtxPtr mctx;
    AVBufferRef *out_hw_device_ref;
    AVBufferRef *out_hw_frm_ref;
    MppBufferGroupPtr buf_grp;
    AVRational qoffset;
    char *model_dir;
    char *dump_roi_path;
    FILE *dump_roi_fp;
    int dump_roi_enable;
    int rand_roi_num;
    int mpp_log_level;
    int roi_skip_interval;

    enum AVPixelFormat out_fmt;
    enum AVPixelFormat in_fmt;
    ROTATION_E src_rotation;
    ROTATION_E dst_rotation;
    ES_S32 src_global_alpha;
    ES_S32 dst_global_alpha;

    ES_BOOL mpp_init;
    int frame_cnt;

    int die_idx;
    int device_idx;
    int roi_number;

    MppAddRoiInInfo in_info;
    MppRoiBoxInfo *roi_info;
    int max_boxes;
    bool roi_inited;

    int draw_enable;
    uint32_t rectangle_color;
    uint32_t rectangle_thickness;
    uint32_t bg_color;
    uint32_t font_color;
    uint32_t font_width;
    uint32_t font_height;
    char *ttf_path;
    MppCtxPtr draw_mctx;
    ES_BOOL draw_init;

    char *fifo_path;
    FILE *pipe_file;
    bool use_system_timestamp;
} MppAddRoiContext;

#endif