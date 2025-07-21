#ifndef AVUTIL_ESMPP_TRANSFER_H
#define AVUTIL_ESMPP_TRANSFER_H
#include "frame.h"

#ifdef __cplusplus
extern "C" {
#endif

int esmpp_hwframe_transfer(AVFrame *dst_frame, const AVFrame *src_frame);
int esmpp_hwframe_transfer_bysize(AVFrame *dst_frame, const AVFrame *src_frame, size_t size);

#ifdef __cplusplus
}
#endif

#endif