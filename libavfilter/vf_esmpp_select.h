#ifndef AVCODEC_VF_ESMPP_ADDROI_H
#define AVCODEC_VF_ESMPP_ADDROI_H

#include "libavutil/avstring.h"
#include "libavutil/common.h"
#include "libavutil/hwcontext.h"
#include "libavcodec/defs.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
#include "libavutil/imgutils.h"
#include "mpp_buffer.h"
#include "mpp_type.h"

enum MppSelectTargetState {
    ESMPP_SELECT_TARGET_LEAVE,
    ESMPP_SELECT_TARGET_ENTER,
};

typedef struct MppSelectContext {
    const AVClass *class;

    AVBufferRef *out_hw_device_ref;
    AVBufferRef *out_hw_frm_ref;
    MppBufferGroupPtr buf_grp;
    int die_idx;
    int device_idx;
    char *json_file;
    FILE *json_fp;
    int roi_frame;
    int target_detect;
    int target_detect_thres;
    int target_enter_cnt;
    int target_exit_cnt;
    int roi_skip_interval;
    enum MppSelectTargetState target_state;

    enum AVPixelFormat out_fmt;
    enum AVPixelFormat in_fmt;

    int frame_cnt;
    int frame_roi_cnt;

    ES_BOOL select_inited;

} MppSelectContext;

#endif