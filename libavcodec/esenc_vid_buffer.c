#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tools.h"
#include "esenc_vid_buffer.h"

void ff_get_output_buffer(ESEncVidInternalContext *in_ctx, VCEncIn *p_enc_in) {
    int32_t i_buf;
    for (i_buf = 0; i_buf < in_ctx->stream_buf_num; i_buf++) {
        // find output buffer of multi-cores
        in_ctx->outbuf_mem[i_buf] =
            &(in_ctx->outbuf_mem_factory[in_ctx->picture_enc_cnt % in_ctx->parallel_core_num][i_buf]);

        p_enc_in->busOutBuf[i_buf] = in_ctx->outbuf_mem[i_buf]->busAddress;
        p_enc_in->outBufSize[i_buf] = in_ctx->outbuf_mem[i_buf]->size;
        p_enc_in->pOutBuf[i_buf] = in_ctx->outbuf_mem[i_buf]->virtualAddress;
    }
}

void ff_fill_roi_qp_map_buffer(ESEncVidInternalContext *in_ctx,
                               ESEncVidContext *options,
                               VCEncIn *p_enc_in,
                               VCEncInst encoder) {
    struct vcenc_instance *vcenc_instance = (struct vcenc_instance *)encoder;

    if (!options->skip_map_enable && !options->roi_map_delta_qp_enable) {
        // av_log(NULL, AV_LOG_ERROR, "setup roi map data fail\n");
        return;
    }

    if (in_ctx->cu_map_buf_len && in_ctx->cu_map_buf) {
        p_enc_in->roiMapDeltaSize = MIN(in_ctx->cu_map_buf_len, in_ctx->roi_map_delta_qp_mem->size);
        p_enc_in->roiMapDeltaQpAddr = in_ctx->roi_map_delta_qp_mem->busAddress;
        memcpy(in_ctx->roi_map_delta_qp_mem->virtualAddress, in_ctx->cu_map_buf, p_enc_in->roiMapDeltaSize);
        free(in_ctx->cu_map_buf);
        in_ctx->cu_map_buf = NULL;
        in_ctx->cu_map_buf_len = 0;
    } else {
        p_enc_in->roiMapDeltaSize = 0;
        p_enc_in->roiMapDeltaQpAddr = NULL;
    }
}

static int32_t ff_get_avaliable_input_buffer_index(uint32_t buf_cnt, uint8_t *buf_status) {
    int32_t i = 0;

    if (buf_status == NULL) return -1;
    if (!buf_cnt) return -1;

    for (i = 0; i < buf_cnt; i++) {
        if (!buf_status[i]) {
            buf_status[i] = 1;
            return i;
        }
    }
    return -1;
}

void ff_get_input_picture_buffer(ESEncVidInternalContext *in_ctx) {
    int32_t input_buffer_index = 0;

    // find YUV frame buffer
    input_buffer_index = ff_get_avaliable_input_buffer_index(in_ctx->buffer_cnt, in_ctx->picture_mem_status);
    if (input_buffer_index < 0) {
        av_log(NULL, AV_LOG_ERROR, "ff_get_avaliable_input_buffer_index fail\n");
        return;
    }
    av_log(NULL, AV_LOG_DEBUG, "current_buffer_index: %d, buffer_cnt: %d\n", input_buffer_index, in_ctx->buffer_cnt);

    in_ctx->picture_mem = &(in_ctx->picture_mem_factory[input_buffer_index]);
}

int32_t ff_release_input_picture_buffer(ESEncVidInternalContext *in_ctx, ptr_t in_bus_addr) {
    int32_t i = 0;

    if (in_ctx == NULL) return -1;
    // not using by encoder, clear flag
    if (in_bus_addr) {
        for (i = 0; i < in_ctx->buffer_cnt; i++) {
            if (in_ctx->picture_mem_factory[i].busAddress == in_bus_addr) {
                if (in_ctx->picture_mem_status[i] > 0) in_ctx->picture_mem_status[i] = 0;
                return 0;
            }
        }

        av_log(NULL, AV_LOG_ERROR, "ff_release_input_picture_buffer %d, %x fail\n", in_bus_addr, in_bus_addr);
        return -1;
    }

    return 0;
}

void ff_get_input_roi_qp_map_buffer(ESEncVidInternalContext *in_ctx) {
    int32_t input_buffer_index = 0;

    if (!in_ctx->cu_map_buf_len || !in_ctx->cu_map_buf) return;
    // find YUV frame buffer
    input_buffer_index = ff_get_avaliable_input_buffer_index(in_ctx->buffer_cnt, in_ctx->roi_qp_map_mem_status);
    if (input_buffer_index < 0) {
        av_log(NULL, AV_LOG_ERROR, "roi_qp_map, ff_get_avaliable_input_buffer_index fail\n");
        return;
    }
    av_log(NULL,
           AV_LOG_DEBUG,
           "roi_qp_map, current_buffer_index: %d, buffer_cnt: %d\n",
           input_buffer_index,
           in_ctx->buffer_cnt);

    // find ROI Map buffer of multi-cores
    in_ctx->roi_map_delta_qp_mem = &(in_ctx->roi_map_delta_qp_mem_factory[input_buffer_index]);
}

int32_t ff_release_input_roi_qp_map_buffer(ESEncVidInternalContext *in_ctx, ptr_t in_bus_addr) {
    int32_t i = 0;

    if (in_ctx == NULL) return -1;
    // not using by encoder, clear flag
    if (in_bus_addr) {
        for (i = 0; i < in_ctx->buffer_cnt; i++) {
            if (in_ctx->roi_map_delta_qp_mem_factory[i].busAddress == in_bus_addr) {
                if (in_ctx->roi_qp_map_mem_status[i] > 0) in_ctx->roi_qp_map_mem_status[i] = 0;
                return 0;
            }
        }

        av_log(
            NULL, AV_LOG_ERROR, "roi_qp_map, ff_release_input_picture_buffer %d, %x fail\n", in_bus_addr, in_bus_addr);
        return -1;
    }

    return 0;
}

MemInfo *ff_alloc_and_fill_mem_info(int64_t fd, unsigned long vpa, AVFrame *frame) {
    MemInfo *mem = (MemInfo *)malloc(sizeof(MemInfo));
    if (mem) {
        mem->dma_fd = fd;
        mem->vpa = vpa;
        if (frame) {
            mem->frame = av_frame_clone(frame);
            // av_log(NULL, AV_LOG_WARNING, "share fd, clone frame, old: %p, new: %p\n", frame, mem->frame);
        } else {
            mem->frame = NULL;
        }
    }

    return mem;
}

int ff_push_mem_info_into_queue(ESEncVidInternalContext *in_ctx, MemInfo *mem_info) {
    if (!in_ctx || !mem_info) return -1;

    pthread_mutex_lock(&in_ctx->in_mem_queue_mutex);
    es_queue_push_tail(in_ctx->in_mem_queue, (void *)mem_info);
    pthread_mutex_unlock(&in_ctx->in_mem_queue_mutex);

    return 0;
}

int ff_get_mem_info_queue_size(ESEncVidInternalContext *in_ctx) {
    int ret = 0;
    if (!in_ctx) return -1;

    pthread_mutex_lock(&in_ctx->in_mem_queue_mutex);
    ret = es_queue_get_length(in_ctx->in_mem_queue);
    pthread_mutex_unlock(&in_ctx->in_mem_queue_mutex);

    return ret;
}

MemInfo *ff_get_mem_info_by_vpa(ESEncVidInternalContext *in_ctx, unsigned long dst_va) {
    if (!in_ctx) return NULL;

    pthread_mutex_lock(&in_ctx->in_mem_queue_mutex);

    ESQueue *q = in_ctx->in_mem_queue;
    List *node = NULL;
    int found = 0;
    MemInfo *mem_info = NULL;
    node = q->head;

    while (node) {
        mem_info = (MemInfo *)node->data;
        if (mem_info && (mem_info->vpa == dst_va)) {
            found = 1;
            break;
        } else {
            node = node->next;
        }
    }

    pthread_mutex_unlock(&in_ctx->in_mem_queue_mutex);
    return found ? mem_info : NULL;
}

int ff_remove_mem_info_from_queue(ESEncVidInternalContext *in_ctx, MemInfo *mem_info) {
    int ret = 0;
    if (!in_ctx || !mem_info) return -1;

    pthread_mutex_lock(&in_ctx->in_mem_queue_mutex);
    ret = es_queue_delete_data(in_ctx->in_mem_queue, (void *)mem_info);
    pthread_mutex_unlock(&in_ctx->in_mem_queue_mutex);
    if (mem_info->frame) {
        // av_log(NULL, AV_LOG_WARNING, "share fd, unref frame: %p\n", mem_info->frame);
        av_frame_free(&mem_info->frame);
        mem_info->frame = NULL;
    }
    free(mem_info);
    mem_info = NULL;

    return ret;
}

int ff_clean_mem_info_queue(ESEncVidInternalContext *in_ctx) {
    if (!in_ctx) return -1;

    pthread_mutex_lock(&in_ctx->in_mem_queue_mutex);
    ESQueue *q = in_ctx->in_mem_queue;
    List *node = NULL;
    MemInfo *mem_info = NULL;
    node = q->head;

    while (node) {
        mem_info = (MemInfo *)node->data;
        // delete node
        if (mem_info) {
            es_queue_delete_data(q, mem_info);
            if (mem_info->frame) av_frame_free(&mem_info->frame);
            free(mem_info);
        }
        node = node->next;
    }

    pthread_mutex_unlock(&in_ctx->in_mem_queue_mutex);

    return 0;
}

DtsInfo *ff_alloc_and_fill_dts_info(int64_t dts) {
    DtsInfo *dts_info = (DtsInfo *)malloc(sizeof(DtsInfo));
    if (dts_info) {
        dts_info->dts = dts;
        // av_log(NULL, AV_LOG_WARNING, "alloc dts_info: %p, dts: %ld\n", dts_info, dts_info->dts);
    }

    return dts_info;
}

int ff_remove_dts_info_from_queue(ESEncVidInternalContext *in_ctx, DtsInfo *dts_info) {
    int ret = 0;
    if (!in_ctx || !dts_info) return -1;

    pthread_mutex_lock(&in_ctx->dts_queue_mutex);
    ret = es_queue_delete_data(in_ctx->dts_queue, (void *)dts_info);
    pthread_mutex_unlock(&in_ctx->dts_queue_mutex);
    free(dts_info);
    dts_info = NULL;

    return ret;
}

int ff_push_dts_info_into_queue(ESEncVidInternalContext *in_ctx, DtsInfo *dts_info) {
    if (!in_ctx || !dts_info) return -1;

    pthread_mutex_lock(&in_ctx->dts_queue_mutex);
    es_queue_push_tail(in_ctx->dts_queue, (void *)dts_info);
    pthread_mutex_unlock(&in_ctx->dts_queue_mutex);

    return 0;
}

int64_t ff_get_and_del_min_dts_from_queue(ESEncVidInternalContext *in_ctx) {
    if (!in_ctx) return NULL;

    pthread_mutex_lock(&in_ctx->dts_queue_mutex);

    ESQueue *q = in_ctx->dts_queue;
    List *node = NULL;
    int64_t min_pts = AV_NOPTS_VALUE;
    DtsInfo *min_dts_info = NULL;
    DtsInfo *dts_info = NULL;
    node = q->head;

    while (node) {
        dts_info = (DtsInfo *)node->data;
        if (dts_info) {
            // av_log(NULL, AV_LOG_WARNING, "found dts_info: %p, dts: %ld\n", dts_info, dts_info->dts);
            if (min_pts == AV_NOPTS_VALUE) {
                min_pts = dts_info->dts;
                min_dts_info = dts_info;
            } else {
                if (min_pts > dts_info->dts) {
                    min_pts = dts_info->dts;
                    min_dts_info = dts_info;
                }
            }
        }

        node = node->next;
    }

    // delete node
    es_queue_delete_data(q, min_dts_info);
    pthread_mutex_unlock(&in_ctx->dts_queue_mutex);

    // av_log(NULL, AV_LOG_WARNING, "min_dts_info: %p, min_info->dts: %ld, min_pts: %ld\n", min_dts_info,
    // min_dts_info->dts, min_pts);
    return min_pts;
}

int ff_clean_dts_queue(ESEncVidInternalContext *in_ctx) {
    if (!in_ctx) return -1;

    pthread_mutex_lock(&in_ctx->dts_queue_mutex);

    ESQueue *q = in_ctx->dts_queue;
    List *node = NULL;
    DtsInfo *dts_info = NULL;
    node = q->head;

    while (node) {
        dts_info = (DtsInfo *)node->data;
        // delete node
        if (dts_info) {
            es_queue_delete_data(q, dts_info);
            free(dts_info);
        }
        node = node->next;
    }

    pthread_mutex_unlock(&in_ctx->dts_queue_mutex);

    return 0;
}