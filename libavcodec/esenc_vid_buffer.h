#ifndef AVCODEC_ESENC_VIDEO_BUFFER_H
#define AVCODEC_ESENC_VIDEO_BUFFER_H

#include "esenc_vid.h"

typedef struct {
    int64_t dma_fd;     // dma buffer fd
    unsigned long vpa;  // dma buffer virtual addr
    AVFrame *frame;     // refence of frame
} MemInfo;

typedef struct {
    int64_t dts;
} DtsInfo;

void ff_get_output_buffer(ESEncVidInternalContext *tb, VCEncIn *pEncIn);

void ff_get_input_picture_buffer(ESEncVidInternalContext *tb);

int32_t ff_release_input_picture_buffer(ESEncVidInternalContext *in_ctx, ptr_t in_bus_addr);

void ff_get_input_roi_qp_map_buffer(ESEncVidInternalContext *tb);

void ff_fill_roi_qp_map_buffer(ESEncVidInternalContext *tb,
                               ESEncVidContext *options,
                               VCEncIn *pEncIn,
                               VCEncInst encoder);

int32_t ff_release_input_roi_qp_map_buffer(ESEncVidInternalContext *in_ctx, ptr_t in_bus_addr);

MemInfo *ff_alloc_and_fill_mem_info(int64_t fd, unsigned long vpa, AVFrame *frame);

int ff_push_mem_info_into_queue(ESEncVidInternalContext *in_ctx, MemInfo *mem_info);

int ff_get_mem_info_queue_size(ESEncVidInternalContext *in_ctx);

MemInfo *ff_get_mem_info_by_vpa(ESEncVidInternalContext *in_ctx, unsigned long dst_va);

int ff_remove_mem_info_from_queue(ESEncVidInternalContext *in_ctx, MemInfo *mem_info);

int ff_clean_mem_info_queue(ESEncVidInternalContext *in_ctx);

DtsInfo *ff_alloc_and_fill_dts_info(int64_t dts);

int ff_push_dts_info_into_queue(ESEncVidInternalContext *in_ctx, DtsInfo *dts_info);

int ff_remove_dts_info_from_queue(ESEncVidInternalContext *in_ctx, DtsInfo *dts_info);

int64_t ff_get_and_del_min_dts_from_queue(ESEncVidInternalContext *in_ctx);

int ff_clean_dts_queue(ESEncVidInternalContext *in_ctx);

#endif