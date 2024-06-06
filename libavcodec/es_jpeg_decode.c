/*
 * Copyright (C) 2019  VeriSilicon
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "es_jpeg_decode.h"
#include "jpegdecapi.h"
#include "libavutil/hwcontext_es.h"
#include "esdec_wrapper.h"

#ifdef MODEL_SIMULATION
#include <deccfg.h>
extern u32 g_hw_build_id;
extern u32 g_hw_id;
extern u32 g_hw_ver;
#endif

static uint32_t find_dec_pic_wait_consume_index(VSVDECContext *dec_ctx,
                                                uint8_t *data)
{
    uint32_t i;

    pthread_mutex_lock(&dec_ctx->consume_mutex);
    for (i = 0; i < MAX_WAIT_FOR_CONSUME_BUFFERS; i++) {
        if (dec_ctx->wait_for_consume_list[i].pic
            == (struct DecPicturePpu *)data)
            break;
    }

    assert(i < dec_ctx->wait_consume_num);
    pthread_mutex_unlock(&dec_ctx->consume_mutex);
    return i;
}



static uint32_t find_dec_pic_wait_consume_empty_index(VSVDECContext *dec_ctx)
{
    uint32_t i;

    pthread_mutex_lock(&dec_ctx->consume_mutex);
    for (i = 0; i < MAX_WAIT_FOR_CONSUME_BUFFERS; i++) {
        if (dec_ctx->wait_for_consume_list[i].wait_for_consume == 0)
        break;
    }

    assert(i < MAX_WAIT_FOR_CONSUME_BUFFERS);
    pthread_mutex_unlock(&dec_ctx->consume_mutex);

    return i;
}

uint32_t ff_es_jpeg_dec_del_pic_wait_consume_list(VSVDECContext *dec_ctx,
                                              uint8_t *data)
{
    uint32_t id;
    id = find_dec_pic_wait_consume_index(dec_ctx,data);

    pthread_mutex_lock(&dec_ctx->consume_mutex);
    if(id < MAX_WAIT_FOR_CONSUME_BUFFERS) {
        dec_ctx->wait_for_consume_list[id].pic = NULL;
        dec_ctx->wait_for_consume_list[id].wait_for_consume = 0;
        if(dec_ctx->wait_consume_num > 0)
            dec_ctx->wait_consume_num--;
        av_log(NULL, AV_LOG_DEBUG,
               "ff_es_jpeg_dec_del_pic_wait_consume_list pic(@ %p) @ %d \n",
               data, id);
    }
    pthread_mutex_unlock(&dec_ctx->consume_mutex);
    return id;
}

static uint32_t add_dec_pic_wait_consume_list(VSVDECContext *dec_ctx, void *data)
{
    uint32_t id;

    id = find_dec_pic_wait_consume_empty_index(dec_ctx);
    av_log(NULL, AV_LOG_DEBUG, "add_dec_pic_wait_consume_list pic(@ %p) @ %d \n",
           data, id);

    pthread_mutex_lock(&dec_ctx->consume_mutex);
    dec_ctx->wait_for_consume_list[id].pic = (struct DecPicturePpu *)data;
    dec_ctx->wait_for_consume_list[id].wait_for_consume = 1;
    if(dec_ctx->wait_consume_num < MAX_WAIT_FOR_CONSUME_BUFFERS)
        dec_ctx->wait_consume_num++;
    assert(id < MAX_WAIT_FOR_CONSUME_BUFFERS);
    pthread_mutex_unlock(&dec_ctx->consume_mutex);
    return id;
}

static void report_dec_pic_info( VSVDECContext *dec_ctx,  struct DecPicture *picture)
{
    char info_string[2048];
    static const char* pic_types[] = {"        IDR", "Non-IDR (P)", "Non-IDR (B)"};

    av_log(dec_ctx, AV_LOG_DEBUG, "PIC %2d/%2d, type %s, ",
                              dec_ctx->pic_display_number,
                              picture->picture_info.pic_id,
                              picture->picture_info.pic_coding_type);
    if (picture->picture_info.cycles_per_mb) {
        av_log(dec_ctx, AV_LOG_DEBUG,
                              " %4d cycles / mb,",
                              picture->picture_info.cycles_per_mb);
    }

    av_log(dec_ctx, AV_LOG_DEBUG,
            " %d x %d, Crop: (%d, %d), %d x %d %s",
            picture->sequence_info.pic_width,
            picture->sequence_info.pic_height,
            picture->sequence_info.crop_params.crop_left_offset,
            picture->sequence_info.crop_params.crop_top_offset,
            picture->sequence_info.crop_params.crop_out_width,
            picture->sequence_info.crop_params.crop_out_height,
            picture->picture_info.is_corrupted ? "CORRUPT" : "");

    av_log(dec_ctx, AV_LOG_DEBUG, "%s\n", info_string);
}

uint32_t ff_es_jpeg_dec_find_empty_index(VSVDECContext *dec_ctx)
{
    uint32_t i;
    for (i = 0; i < MAX_BUFFERS; i++) {
        if (dec_ctx->output_memory_list.output_memory[i].virtual_address == 0)
            break;
    }

    assert(i < MAX_BUFFERS);
    return i;
}

void ff_es_jpeg_dec_release_ext_buffers(VSVDECContext *dec_ctx)
{
    int dma_fd;

    if(!dec_ctx) {
        av_log(dec_ctx, AV_LOG_ERROR, "dec_ctx is nullptr\n");
    }

    pthread_mutex_lock(&dec_ctx->ext_buffer_control);
    for(int i = 0; i < dec_ctx->output_memory_list.size; i++) {
        ESJpegOutputMemory *memory = &dec_ctx->output_memory_list.output_memory[i];

        dma_fd = ESDecGetDmaBufFd(&memory->mem);
        av_log(dec_ctx,
            AV_LOG_DEBUG,
            "Freeing buffer virtual_address:%p, dma_fd:%d, memory size: %d\n",
            (void *)memory->virtual_address,
            dma_fd,
            memory->mem.size);

        for (int index = 0; index < ES_VID_DEC_MAX_OUT_COUNT; index++) {
            if (memory->fd[index] >= 0 && memory->fd[index] != dma_fd) {
                av_log(dec_ctx, AV_LOG_DEBUG, "close  pp_fd[%d]: %d\n", index, memory->fd[index]);
                close(memory->fd[index]);
                memory->fd[index] = -1;
            }
        }

        if (dec_ctx->pp_enabled)
            DWLFreeLinear(dec_ctx->dwl_inst, &memory->mem);
        else
            DWLFreeRefFrm(dec_ctx->dwl_inst, &memory->mem);

        DWLmemset(&memory->mem, 0, sizeof(memory->mem));

        memory->virtual_address = NULL;
    }
    pthread_mutex_unlock(&dec_ctx->ext_buffer_control);
}

void ff_es_pri_picture_info_free(void *opaque, uint8_t *data)
{
    av_free(data);
}
void ff_es_data_free(void *opaque, uint8_t *data)
{
    return;
}


void jpeg_dec_set_default_dec_config(AVCodecContext *avctx)
{
    VSVDECContext *dec_ctx = avctx->priv_data;

    dec_ctx->avctx = avctx;
    //ff_es_jpeg_dec_init_log_header(avctx);
    dec_ctx->align = ff_es_jpeg_get_align(dec_ctx->out_stride);
    dec_ctx->service_merge_disable = DEC_X170_SERVICE_MERGE_DISABLE;
    dec_ctx->output_picture_endian = DEC_X170_OUTPUT_PICTURE_ENDIAN;
    dec_ctx->clock_gating = DEC_X170_INTERNAL_CLOCK_GATING;
    dec_ctx->latency_comp = DEC_X170_LATENCY_COMPENSATION;
    dec_ctx->bus_burst_length = DEC_X170_BUS_BURST_LENGTH;
    dec_ctx->asic_service_priority = DEC_X170_ASIC_SERVICE_PRIORITY;
    dec_ctx->data_discard = DEC_X170_DATA_DISCARD_ENABLE;
    dec_ctx->tiled_output = DEC_REF_FRM_RASTER_SCAN;
    dec_ctx->dpb_mode = DEC_DPB_FRAME;
    dec_ctx->dwl_init.client_type = DWL_CLIENT_TYPE_JPEG_DEC;

    pthread_mutex_init(&dec_ctx->ext_buffer_control, NULL);

    // memset(dec_ctx->ext_buffers, 0, sizeof(dec_ctx->ext_buffers));
    dec_ctx->buffer_release_flag = 1;
    dec_ctx->cycle_count = 0;
    dec_ctx->enable_mc = 0;
    dec_ctx->min_buffer_num = 0;
    dec_ctx->last_pic_flag = 0;
    dec_ctx->hdrs_rdy = 0;
    dec_ctx->stream_mem_index = 0;
    dec_ctx->pic_display_number = 0;
    dec_ctx->got_package_number = 0;
    dec_ctx->pic_decode_number = 0;
    dec_ctx->prev_width = 0;
    dec_ctx->prev_height = 0;
    dec_ctx->closed = 0;
    dec_ctx->extra_buffer_num = 0;
    dec_ctx->dump_pkt_count = 0;
    dec_ctx->dump_frame_count[0] = 0;
    dec_ctx->dump_frame_count[1] = 0;
    dec_ctx->frame_dump_handle[0] = NULL;
    dec_ctx->frame_dump_handle[1] = NULL;
    dec_ctx->pkt_dump_handle = NULL;
    dec_ctx->thum_exist = 0;
    dec_ctx->thum_out = 0;
    dec_ctx->task_existed = 0;

    dec_ctx->vsv_dec_config.align = dec_ctx->align;
    dec_ctx->vsv_dec_config.dec_image_type = JPEGDEC_IMAGE;
    memset(dec_ctx->vsv_dec_config.ppu_cfg, 0, sizeof(dec_ctx->vsv_dec_config.ppu_cfg));
    memset(dec_ctx->vsv_dec_config.delogo_params, 0, sizeof(dec_ctx->vsv_dec_config.delogo_params));

    // init output buffer list
    dec_ctx->output_memory_list.size = 0;
    memset(dec_ctx->output_memory_list.output_memory, 0, sizeof(dec_ctx->output_memory_list.output_memory));

#ifdef MODEL_SIMULATION
    g_hw_build_id = 0x1FB1;
    g_hw_ver = 19001;
    g_hw_id = 1000;
#endif

    dec_ctx->frame = av_frame_alloc();
    if (!dec_ctx->frame) {
        av_log(avctx, AV_LOG_ERROR, "av_frame_alloc failed\n");
    }
}

enum DecRet jpegdec_consumed(void* inst, struct DecPicturePpu *pic) {
    JpegDecOutput jpic;
    u32 i;

    DWLmemset(&jpic, 0, sizeof(JpegDecOutput));

    for (i = 0; i < DEC_MAX_OUT_COUNT;i++) {
        jpic.pictures[i].output_picture_y = pic->pictures[i].luma;
    }

    return JpegDecPictureConsumed(inst, &jpic);
}

int ff_esdec_get_next_picture(AVCodecContext *avctx, AVFrame *frame) {
    VSVDECContext *dec_ctx = avctx->priv_data;
    enum DecRet ret;
    int i;
    ret = jpeg_next_picture(avctx, dec_ctx->dec_inst, &dec_ctx->pic);
    av_log(avctx, AV_LOG_DEBUG, "JpegNextPicture return: %d\n", ret);

    if (ret == DEC_PIC_RDY) {
        dec_ctx->pic_display_number++;
        for (i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
            if(dec_ctx->pic.pictures[i].luma.virtual_address != NULL) {
                av_log(avctx, AV_LOG_DEBUG, "Dec pic rdy, %d -> %d x %d -luma.virtual_address=%p"
                                            "-luma.bus_address=%ld -luma.size=%d stride=%d"
                                            "-sequence_info.pic_height=%d -sequence_info.pic_width=%d \n",
                                            i,
                                            dec_ctx->pic.pictures[i].pic_width,
                                            dec_ctx->pic.pictures[i].pic_height,
                                            dec_ctx->pic.pictures[i].luma.virtual_address,
                                            dec_ctx->pic.pictures[i].luma.bus_address,
                                            dec_ctx->pic.pictures[i].luma.size,
                                            dec_ctx->pic.pictures[i].sequence_info.pic_stride,
                                            dec_ctx->pic.pictures[i].sequence_info.pic_height,
                                            dec_ctx->pic.pictures[i].sequence_info.pic_width);
            }
        }

        // create frame file
        for ( int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++)
            if(!dec_ctx->frame_dump_handle[i])
                ff_es_jpeg_init_frame_dump_handle(dec_ctx);
        ff_es_jpeg_frame_dump(dec_ctx);

        if (dec_ctx->thumb_mode == Decode_Pic_Thumb && dec_ctx->thum_exist && dec_ctx->thum_out)
            ff_es_jpeg_dec_output_thum(avctx, frame, &dec_ctx->pic);
        else
            ff_es_jpeg_dec_output_frame(avctx, frame, &dec_ctx->pic);

        av_log(avctx, AV_LOG_DEBUG,
                "%d got frame :size=%dx%d,data[0]=%p,data[1]=%p,buf[0]=%p\n",
                dec_ctx->pic_display_number,
                frame->width,
                frame->height,
                frame->data[0],
                frame->data[1],
                frame->buf[0]);

        jpegdec_consumed(dec_ctx->dec_inst, &dec_ctx->pic);

    } else if (ret == DEC_END_OF_STREAM) {
        av_log(avctx, AV_LOG_DEBUG, "End of stream!\n");
        dec_ctx->last_pic_flag = 1;
        return AVERROR_EOF;
    }
    return 0;
}

enum ESJpegCode ff_es_jpeg_decoder(AVCodecContext *avctx,
                                   struct DecInputParameters *jpeg_in,
                                   AVFrame *frame) {
    VSVDECContext *dec_ctx = avctx->priv_data;
    int ret = 0;

    do {
        ret = jpeg_decode(avctx,(void* )dec_ctx->dec_inst, jpeg_in);

        ff_es_jpeg_dec_print_return(avctx, ret);

        switch (ret) {
        case DEC_PIC_RDY:
            ff_esdec_get_next_picture( avctx, frame);
            break;
        case DEC_SCAN_PROCESSED:
            av_log(avctx, AV_LOG_DEBUG, "\t-JPEG: DEC_SCAN_PROCESSED\n");
            ret = jpeg_next_picture(avctx,dec_ctx->dec_inst, &dec_ctx->pic);
            av_log(avctx, AV_LOG_DEBUG, "jpegNextPicture return: %d\n", ret);
            if (ret == DEC_PIC_RDY) {
                dec_ctx->pic_display_number++;
                if (dec_ctx->thumb_mode == Decode_Pic_Thumb && dec_ctx->thum_exist && dec_ctx->thum_out)
                    ff_es_jpeg_dec_output_thum(avctx, frame, &dec_ctx->pic);
                else
                    ff_es_jpeg_dec_output_frame(avctx, frame, &dec_ctx->pic);
                av_log(avctx, AV_LOG_DEBUG,
                        "%d got frame :data[0]=%p,data[1]=%p buf[0]=%p,buf[1]=%p \n",
                        dec_ctx->pic_display_number, frame->data[0], frame->data[1],
                        frame->buf[0], frame->buf[1]);

                jpegdec_consumed(dec_ctx->dec_inst, &dec_ctx->pic);
            } else if (ret == DEC_END_OF_STREAM) {
                av_log(avctx, AV_LOG_DEBUG, "End of stream!\n");
                dec_ctx->last_pic_flag = 1;
                break;
            }
            break;
        case DEC_SLICE_RDY:
            av_log(avctx, AV_LOG_DEBUG, "\t-JPEG: DEC_SLICE_RDY\n");
            break;
        case DEC_NO_DECODING_BUFFER:
            usleep(10000);
        case DEC_WAITING_FOR_BUFFER:
            dec_ctx->extra_buffer_num = 0;
            return ES_MORE_BUFFER;
        case DEC_STRM_PROCESSED:
            av_log(avctx, AV_LOG_DEBUG, "\t-JPEG: DEC_STRM_PROCESSED\n");
            break;
        case DEC_STRM_ERROR:
            ff_esdec_get_next_picture( avctx, frame);
            return ES_ERROR;
        default:
            av_log(avctx, AV_LOG_ERROR, "jpegdecode return: %d\n", ret);
            return ES_ERROR;
        }
    } while (ret != DEC_PIC_RDY);

    return ES_OK;
}

int jpeg_init(VSVDECContext *dec_ctx)
{
    struct DecConfig config = dec_ctx->vsv_dec_config;
    struct JpegDecConfig dec_cfg;
    enum DecRet jpeg_ret;

    dec_ctx->dwl_inst = (void *)DWLInit(&dec_ctx->dwl_init);
    if (dec_ctx->dwl_inst == NULL) {
        av_log(dec_ctx, AV_LOG_ERROR, "DWL Init failed\n");
        return -1;
    } else {
        av_log(dec_ctx, AV_LOG_DEBUG, "DWL Init success\n");
    }

    if ((dec_ctx->decode_mode & DEC_LOW_LATENCY) != 0) {
        dec_ctx->low_latency = 1;
    }

    dec_cfg.decoder_mode = DEC_NORMAL;
    if (dec_ctx->low_latency)
        dec_cfg.decoder_mode = DEC_LOW_LATENCY;

    dec_cfg.align = config.align;
    if (dec_ctx->enable_mc)
        dec_cfg.mcinit_cfg.mc_enable = 1;
    else
        dec_cfg.mcinit_cfg.mc_enable = 0;
    dec_cfg.mcinit_cfg.stream_consumed_callback = NULL;
    memcpy(dec_cfg.ppu_config, config.ppu_cfg, sizeof(config.ppu_cfg));
    memcpy(dec_cfg.delogo_params, config.delogo_params, sizeof(config.delogo_params));

    jpeg_ret = JpegDecInit(&dec_ctx->dec_inst, dec_ctx->dwl_inst, &dec_cfg);

    return jpeg_ret;
}

enum DecRet jpeg_dec_get_info(VSVDECContext *dec_ctx)
{
    void *inst = (void* )dec_ctx->dec_inst;
    struct DecSequenceInfo *info = &dec_ctx->sequence_info;
    JpegDecInput jpeg_input;
    enum DecRet rv;
    JpegDecImageInfo image_info;

    jpeg_input.stream_buffer = info->jpeg_input_info.stream_buffer;
    jpeg_input.stream_length = info->jpeg_input_info.strm_len;
    jpeg_input.buffer_size = info->jpeg_input_info.buffer_size;
    jpeg_input.stream = info->jpeg_input_info.stream;

    DWLmemset(&image_info, 0, sizeof(image_info));

    rv = JpegDecGetImageInfo(inst, &jpeg_input, &image_info);

    if(rv != DEC_OK) {
        return rv;
    }

    info->scaled_width = image_info.display_width;
    info->scaled_height = image_info.display_height;
    info->pic_width = image_info.output_width;
    info->pic_height = image_info.output_height;
    info->scaled_width_thumb = image_info.display_width_thumb;
    info->scaled_height_thumb = image_info.display_height_thumb;
    info->pic_width_thumb = image_info.output_width_thumb;
    info->pic_height_thumb = image_info.output_height_thumb;
    info->output_format = image_info.output_format;
    info->output_format_thumb = image_info.output_format_thumb;
    info->coding_mode = image_info.coding_mode;
    info->coding_mode_thumb = image_info.coding_mode_thumb;
    info->thumbnail_type = image_info.thumbnail_type;
    info->img_max_dec_width = image_info.img_max_dec_width;
    info->img_max_dec_height = image_info.img_max_dec_height;
    /* update the alignment setting in "image_info" data structure and output picture width */
    info->pic_width = NEXT_MULTIPLE(info->pic_width, ALIGN(dec_ctx->vsv_dec_config.align));
    info->pic_width_thumb = NEXT_MULTIPLE(info->pic_width_thumb, ALIGN(dec_ctx->vsv_dec_config.align));

    return rv;
}

enum DecRet jpeg_dec_set_info(const void *inst, struct DecConfig config)
{
    struct JpegDecConfig dec_cfg;
    dec_cfg.dec_image_type = config.dec_image_type;
    dec_cfg.align = config.align;

    DWLmemcpy(dec_cfg.ppu_config, config.ppu_cfg, sizeof(config.ppu_cfg));
    DWLmemcpy(dec_cfg.delogo_params, config.delogo_params, sizeof(config.delogo_params));

    return JpegDecSetInfo(inst, &dec_cfg);
}

enum DecRet jpeg_get_buffer_info(void *inst, struct DecBufferInfo *buf_info)
{
    struct DecBufferInfo hbuf;
    enum DecRet rv;
    rv = JpegDecGetBufferInfo(inst, &hbuf);

    buf_info->next_buf_size = hbuf.next_buf_size;
    buf_info->buf_num = hbuf.buf_num;
    buf_info->buf_to_free = hbuf.buf_to_free;

    return rv;
}

enum DecRet jpeg_decode(AVCodecContext *avctx,void* inst, struct DecInputParameters* jpeg_in)
{
    enum DecRet rv;
    JpegDecInput jpeg_input;
    JpegDecOutput jpeg_out;
    DWLmemset(&jpeg_input, 0, sizeof(jpeg_input));
    DWLmemset(&jpeg_out, 0, sizeof(jpeg_out));

    jpeg_input.stream_buffer =jpeg_in->stream_buffer;
    jpeg_input.stream_length = jpeg_in->strm_len;
    jpeg_input.buffer_size = jpeg_in->buffer_size;
    jpeg_input.dec_image_type = jpeg_in->dec_image_type;
    jpeg_input.slice_mb_set = jpeg_in->slice_mb_set;
    jpeg_input.ri_count = jpeg_in->ri_count;
    jpeg_input.ri_array = jpeg_in->ri_array;
    jpeg_input.picture_buffer_y = jpeg_in->picture_buffer_y;
    jpeg_input.picture_buffer_cb_cr = jpeg_in->picture_buffer_cb_cr;
    jpeg_input.picture_buffer_cr = jpeg_in->picture_buffer_cr;
    jpeg_input.p_user_data = jpeg_in->p_user_data;
    jpeg_input.stream = jpeg_in->stream;

    rv = JpegDecDecode(inst, &jpeg_input, &jpeg_out);

    return rv;
}

enum DecRet jpeg_next_picture(AVCodecContext *avctx,const void *inst, struct DecPicturePpu *pic)
{
    enum DecRet rv;
    JpegDecOutput jpic;
    JpegDecImageInfo info;
    u32 stride, stride_ch, i;
    VSVDECContext *dec_ctx  = avctx->priv_data;

    memset(&jpic, 0, sizeof(JpegDecOutput));
    memset(pic, 0, sizeof(struct DecPicturePpu));
    rv = JpegDecNextPicture(inst, &jpic, &info);

    if (rv != DEC_PIC_RDY)
        return rv;
    for (i = 0; i < DEC_MAX_OUT_COUNT; i++)
    {
        stride = jpic.pictures[i].pic_stride;
        stride_ch = jpic.pictures[i].pic_stride_ch;

        pic->pictures[i].picture_info.cycles_per_mb = jpic.cycles_per_mb;
        pic->pictures[i].luma = jpic.pictures[i].output_picture_y;
        pic->pictures[i].chroma = jpic.pictures[i].output_picture_cb_cr;
        pic->pictures[i].chroma_cr = jpic.pictures[i].output_picture_cr;
        pic->pictures[i].sequence_info.pic_width = jpic.pictures[i].output_width;
        pic->pictures[i].sequence_info.pic_height = jpic.pictures[i].output_height;
        pic->pictures[i].sequence_info.scaled_width = jpic.pictures[i].display_width;
        pic->pictures[i].sequence_info.scaled_height = jpic.pictures[i].display_height;
        pic->pictures[i].sequence_info.pic_width_thumb = jpic.pictures[i].output_width_thumb;
        pic->pictures[i].sequence_info.pic_height_thumb = jpic.pictures[i].output_height_thumb;
        pic->pictures[i].sequence_info.scaled_width_thumb = jpic.pictures[i].display_width_thumb;
        pic->pictures[i].sequence_info.scaled_height_thumb = jpic.pictures[i].display_height_thumb;
        pic->pictures[i].sequence_info.bit_depth_luma = jpic.bit_depth;
        pic->pictures[i].sequence_info.bit_depth_chroma = jpic.bit_depth;

        if(dec_ctx->thum_out == 0){
            pic->pictures[i].pic_width = jpic.pictures[i].display_width;
            pic->pictures[i].pic_height = jpic.pictures[i].display_height;
        }else{
            pic->pictures[i].pic_width = jpic.pictures[i].display_width_thumb;
            pic->pictures[i].pic_height = jpic.pictures[i].display_height_thumb;
        }

        if (IS_PIC_TILE(jpic.pictures[i].output_format)) {
            pic->pictures[i].luma.size = stride * (NEXT_MULTIPLE(pic->pictures[i].pic_height, 4) / 4);
            pic->pictures[i].chroma.size = stride_ch * (NEXT_MULTIPLE(pic->pictures[i].pic_height / 2, 4) / 4);
        } else if (IS_PIC_PLANAR(jpic.pictures[i].output_format)) {
            pic->pictures[i].luma.size = stride * pic->pictures[i].pic_height;
            pic->pictures[i].chroma.size = stride_ch * pic->pictures[i].pic_height;
        } else if (jpic.pictures[i].output_format == DEC_OUT_FRM_RFC) {
            pic->pictures[i].luma.size = stride * pic->pictures[i].pic_height / 4;
            pic->pictures[i].chroma.size = stride_ch * pic->pictures[i].pic_height / 8;
        } else {
            pic->pictures[i].luma.size = stride * pic->pictures[i].pic_height;
            if (!IS_PIC_RGB(jpic.pictures[i].output_format))
                pic->pictures[i].chroma.size = stride_ch * pic->pictures[i].pic_height / 2;
        }

        pic->pictures[i].pic_stride = jpic.pictures[i].pic_stride;
        pic->pictures[i].pic_stride_ch = jpic.pictures[i].pic_stride_ch;
        pic->pictures[i].picture_info.format = jpic.pictures[i].output_format;
#ifdef SUPPORT_DEC400
        pic->pictures[i].dec400_luma_table = jpic.pictures[i].dec400_luma_table;
        pic->pictures[i].dec400_chroma_table = jpic.pictures[i].dec400_chroma_table;

#endif
    }
    pic->pictures[0].sequence_info.output_format = info.output_format;
    pic->pictures[0].sequence_info.output_format_thumb = info.output_format_thumb;
    pic->pictures[0].sequence_info.coding_mode = info.coding_mode;
    pic->pictures[0].sequence_info.coding_mode_thumb = info.coding_mode_thumb;
    pic->pictures[0].sequence_info.thumbnail_type = info.thumbnail_type;

    return rv;
}

int ff_es_jpeg_allocate_input_buffer(VSVDECContext *dec_ctx) {
    uint32_t n_cores;
    int ret = 0;

#ifdef MODEL_SIMULATION
    n_cores = DWLReadAsicCoreCount();
#else
    n_cores = DWLReadAsicCoreCount(dec_ctx->dwl_init.client_type);
#endif
    /* number of stream buffers to allocate */
    dec_ctx->allocated_buffers = n_cores + 1;

    if (dec_ctx->allocated_buffers < NUM_OF_STREAM_BUFFERS) {
        dec_ctx->allocated_buffers = NUM_OF_STREAM_BUFFERS;
    } else if (dec_ctx->allocated_buffers > MAX_STRM_BUFFERS) {
        dec_ctx->allocated_buffers = MAX_STRM_BUFFERS;
    }

    for (int i = 0; i < dec_ctx->allocated_buffers; i++) {
        dec_ctx->stream_mem[i].mem_type = DWL_MEM_TYPE_DMA_HOST_TO_DEVICE | DWL_MEM_TYPE_CPU ;
        if (DWLMallocLinear(dec_ctx->dwl_inst, JPEG_INPUT_BUFFER_SIZE, dec_ctx->stream_mem + i) != DWL_OK) {
            av_log(dec_ctx, AV_LOG_ERROR, "Unable to allocate stream buffer memory!\n");
            ret = -1;
            break;
        } else {
            av_log(dec_ctx, AV_LOG_DEBUG,
                    "Alloc memory for %d stream ,addr = 0x%p, size is 0x%x OK\n",
                    i,
                    dec_ctx->stream_mem[i].virtual_address,
                    dec_ctx->stream_mem[i].size);
        }
    }

    return ret;
}

void ff_es_jpeg_dwl_output_mempry_free(void *opaque, uint8_t *data) {
    ESJpegOutputMemory *memory = (ESJpegOutputMemory *)data;
    if (!opaque || !memory) {
        av_log(NULL, AV_LOG_ERROR, "error !!! opaque or mem is null mem: %p\n", memory);
        return;
    }
    av_log(NULL, AV_LOG_DEBUG, "dwl output memory free size: %d, virtual_address: %p\n", memory->mem.size, memory->mem.virtual_address);

    for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if (memory->fd[i] >= 0) {
            av_log(NULL, AV_LOG_DEBUG, "output buffer close  pp_fd[%d]: %d\n", i, memory->fd[i]);
            close(memory->fd[i]);
            memory->fd[i] = -1;
        }
    }

    DWLFreeLinear(opaque, &memory->mem);
}

int ff_es_jpeg_output_buffer_fd_split(void *dwl_inst,
                                      void *dec_inst,
                                      int dev_mem_fd,
                                      ESJpegOutputMemory *memory,
                                      struct DecConfig *dec_config) {
    int ret = SUCCESS;
    int dma_fd;
    int pp_count = 0;
    struct ESDecoderWrapper esjdec;

    if (ESDecIsSimulation()) {
        return SUCCESS;
    }

    for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        memory->fd[i] = -1;
    }

    if (!dwl_inst || !dec_inst || !memory || !dec_config) {
        av_log(NULL, AV_LOG_ERROR,
                  "error !!! dwl_inst: %p, dec_inst: %p, memory: %p, dec_config: %p\n",
                  dwl_inst,
                  dec_inst,
                  memory,
                  dec_config);

        return FAILURE;
    }

    esjdec.codec = DEC_JPEG;
    esjdec.inst = dec_inst;

    dma_fd = ESDecGetDmaBufFd(&memory->mem);
    if (dma_fd < 0) {
       av_log(NULL, AV_LOG_ERROR, "dma fd is error dma_fd: %d\n", dma_fd);
        return FAILURE;
    }

    for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if (dec_config->ppu_cfg[i].enabled) {
            memory->fd[i] = dma_fd;
            pp_count++;
        }
    }
    av_log(NULL, AV_LOG_DEBUG, "pp_count: %d\n", pp_count);

    if (pp_count == ES_VID_DEC_MAX_OUT_COUNT) {
        ret = ESDecGetDmaFdSplit(dwl_inst, &esjdec, dev_mem_fd, dma_fd, memory->fd, ES_VID_DEC_MAX_OUT_COUNT);
    }

    return ret;
}

int ff_es_jpeg_allocate_output_buffer(VSVDECContext *dec_ctx,
                                      struct DecBufferInfo *buf_info) {
    struct DecConfig *config = &dec_ctx->vsv_dec_config;
    struct DWLLinearMem *mem;
    int output_buf_num = 0;
    int ret = 0;

    av_log(dec_ctx, AV_LOG_INFO, "allocate output buffer\n");

    if(buf_info->next_buf_size && !dec_ctx->extra_buffer_num) {//&& !add_external_buffer
        /* Only add minimum required buffers at first. */
        dec_ctx->buffer_size = buf_info->next_buf_size;

        if (buf_info->buf_num < JPEG_OUTPUT_BUFFER_NUM) {
            output_buf_num = JPEG_OUTPUT_BUFFER_NUM;
        } else {
            output_buf_num = buf_info->buf_num;
        }

        for(int i = 0; i < output_buf_num; i++) {
            uint32_t id;
            ESJpegOutputMemory *memory;

            memory = &dec_ctx->output_memory_list.output_memory[i];
            mem = &memory->mem;
#ifdef ENABLE_FPGA_VERIFICATION
            mem.mem_type = DWL_MEM_TYPE_DMA_DEVICE_TO_HOST |
                        DWL_MEM_TYPE_DPB;
#else
            mem->mem_type = DWL_MEM_TYPE_DPB;
#endif
            if (dec_ctx->pp_enabled)
                ret = DWLMallocLinear(dec_ctx->dwl_inst, buf_info->next_buf_size, mem);
            else
                ret = DWLMallocRefFrm(dec_ctx->dwl_inst, buf_info->next_buf_size, mem);
            if (ret || mem->virtual_address == NULL) {
                dec_ctx->output_memory_list.size = i;
                av_log(dec_ctx, AV_LOG_ERROR, "index: %d virtual_address is null\n", i);
                return -1;
            }

            ff_es_jpeg_output_buffer_fd_split(dec_ctx->dwl_inst,
                                              dec_ctx->dec_inst,
                                              dec_ctx->dev_mem_fd,
                                              memory,
                                              config);

            memset(mem->virtual_address, 0, mem->size);

            ret = JpegDecAddBuffer(dec_ctx->dec_inst, mem);

            av_log(dec_ctx, AV_LOG_DEBUG, "DecAddBuffer ret %d\n", ret);
            if(ret != DEC_OK) {
                int dma_fd = ESDecGetDmaBufFd(&memory->mem);
                for (int index = 0; index < ES_VID_DEC_MAX_OUT_COUNT; index++) {
                    if (memory->fd[index] >= 0 && memory->fd[index] != dma_fd) {
                        av_log(dec_ctx,
                               AV_LOG_ERROR,
                               "DecAddBuffer error, close  pp_fd[%d]: %d\n",
                               index, memory->fd[index]);
                        close(memory->fd[index]);
                        memory->fd[index] = -1;
                    }
                }
                if (dec_ctx->pp_enabled)
                    DWLFreeLinear(dec_ctx->dwl_inst, mem);
                else
                    DWLFreeRefFrm(dec_ctx->dwl_inst, mem);
            } else {
                id = ff_es_jpeg_dec_find_empty_index(dec_ctx);
                dec_ctx->output_memory_list.output_memory[i].virtual_address = mem->virtual_address;
                dec_ctx->buffer_consumed[id] = 1;
                if (id >= dec_ctx->output_memory_list.size)
                    dec_ctx->output_memory_list.size++;
            }
        }
        dec_ctx->extra_buffer_num = 1;
        for(int i = 0; i < dec_ctx->output_memory_list.size; i++) {
            av_log(dec_ctx, AV_LOG_DEBUG, "output_memory[%d].fd[0]: %lx\n",
                                        i, dec_ctx->output_memory_list.output_memory[i].fd[0]);
            av_log(dec_ctx, AV_LOG_DEBUG, "output_memory[%d].fd[0]: %lx\n",
                                        i, dec_ctx->output_memory_list.output_memory[i].fd[1]);
        }
    }

    return 0;
}

static void ff_es_jpeg_fill_planes(OutPutInfo *pri_picture, struct DecPicture *pic)
{
    if (!pri_picture || !pic) {
        av_log(NULL, AV_LOG_ERROR, "info  or picture is null out: %p\n", &pic);
        return;
    }

    pri_picture->key_frame = (pic->picture_info.pic_coding_type == DEC_PIC_TYPE_I);
    pri_picture->width = pic->pic_width;
    pri_picture->height = pic->pic_height;
    pri_picture->bus_address = pic->luma.bus_address;
    pri_picture->virtual_address = pic->luma.virtual_address;
    pri_picture->format = ff_codec_decfmt_to_pixfmt(pic->picture_info.format);
    switch(pri_picture->format) {
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_NV21:
        case AV_PIX_FMT_P010LE:{
            pri_picture->n_planes = 2;
            pri_picture->stride[0] = pic->pic_stride;
            pri_picture->stride[1] = pic->pic_stride_ch;
            pri_picture->offset[0] = 0;
            pri_picture->offset[1] = pic->pic_stride * pic->pic_height;
            pri_picture->size = pri_picture->offset[1] * 3 / 2;
            av_log(NULL, AV_LOG_DEBUG, "format: %d width: %d, height: %d, stride: %d, size: %zu\n",
                                        pri_picture->format, pri_picture->width, pri_picture->height,
                                        pri_picture->stride[0], pri_picture->size);
            break;
        }
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUV420P10LE:{
            pri_picture->n_planes = 3;
            pri_picture->offset[0] = 0;
            pri_picture->stride[0] = pic->pic_stride;
            pri_picture->offset[1] = pic->pic_stride * pic->pic_height;
            pri_picture->stride[1] = pic->pic_stride_ch;
            pri_picture->offset[2] = pri_picture->offset[1] +
                                     pic->pic_stride_ch * pic->pic_height / 2;
            pri_picture->stride[2] = pri_picture->stride[1];
            pri_picture->size = pri_picture->offset[1] + pic->pic_stride_ch * pic->pic_height;
            av_log(NULL, AV_LOG_DEBUG, "format: %d width: %d, height: %d, stride: %d, size: %zu\n",
                                        pri_picture->format, pri_picture->width,
                                        pri_picture->height, pri_picture->stride[0],
                                        pri_picture->size);
            break;
        }
        case AV_PIX_FMT_GRAY8:
        case AV_PIX_FMT_RGB24:
        case AV_PIX_FMT_BGR24:
        case AV_PIX_FMT_ARGB:
        case AV_PIX_FMT_ABGR:
        case AV_PIX_FMT_0RGB:
        case AV_PIX_FMT_0BGR:
            pri_picture->n_planes = 1;
            pri_picture->stride[0] = pic->pic_stride;
            pri_picture->offset[0] = 0;
            pri_picture->size = pic->pic_stride * pic->pic_height;
            av_log(NULL, AV_LOG_INFO, "format: %d width: %d, height: %d, stride: %d, size: %zu\n",
                                       pri_picture->format, pri_picture->width, pri_picture->height,
                                       pri_picture->stride[0], pri_picture->size);
            break;

        default:{
        }
    }
}

static ESJpegOutputMemory *ff_es_jpeg_find_memory_by_picture(VSVDECContext *dec_ctx, struct DecPicturePpu *pic) {
    ESJpegOutputMemory *memory = NULL;
    uint32_t *virtual_address = NULL;
    if (!dec_ctx || !pic) {
        av_log(NULL, AV_LOG_ERROR, "ctx or out_buffers or picture is null\n");
        return NULL;
    }

    for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if (pic->pictures[i].luma.virtual_address != NULL) {
            virtual_address = pic->pictures[i].luma.virtual_address;
            av_log(NULL, AV_LOG_DEBUG, "index: %d, virtual_address: %p\n", i, pic->pictures[i].luma.virtual_address);
            break;
        }
    }

    if (!virtual_address) {
        av_log(NULL, AV_LOG_ERROR, "find virtual_address failed from picture\n");
        return NULL;
    }

    for (int i = 0; i < dec_ctx->output_memory_list.size; i++) {
        memory = &dec_ctx->output_memory_list.output_memory[i];
        if (memory->virtual_address == virtual_address) {
            av_log(NULL, AV_LOG_DEBUG, "find output buffer i: %d, virtual_address: %p\n", i, virtual_address);
            break;
        }
    }

    if (!memory) {
        av_log(NULL, AV_LOG_ERROR, "find buffer failed virtual_address: %p\n", virtual_address);
    }

    return memory;
}

static void ff_es_jpeg_fill_out_frame(VSVDECContext *dec_ctx, AVFrame *frame, struct DecPicturePpu *pic)
{
    int index = -1;
    DecPicturePri *pri_pic = NULL;
    OutPutInfo *info = NULL;
    uint64_t dma_fd;
    ESJpegOutputMemory *memory;

    if (!frame || !pic) {
        av_log(NULL, AV_LOG_ERROR, "frame or pic is null out: %p", frame);
        return;
    }

    pri_pic = av_mallocz(sizeof(*pri_pic));
    if (!pri_pic){
        av_log(NULL, AV_LOG_ERROR, "pri_pic av mallocz failed");
        return;
    }

    memory = ff_es_jpeg_find_memory_by_picture(dec_ctx, pic);

    for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        pri_pic->pictures[i].enabled = pic->pictures[i].pp_enabled;
        if (pic->pictures[i].pp_enabled) {
            report_dec_pic_info(dec_ctx, &pic->pictures[i]);
            pri_pic->pic_count++;
            pri_pic->pictures[i].fd = memory->fd[i];
            ff_es_jpeg_fill_planes(&pri_pic->pictures[i], &pic->pictures[i]);
            if (i == dec_ctx->pp_out) {
                pri_pic->default_index = i;
                pri_pic->default_pic = &pri_pic->pictures[i];
            } else if (index == -1) {
                index = i;
            }
        }
    }

    if (pri_pic->pic_count > 0){
        if (!pri_pic->default_pic) {
            pri_pic->default_index = index;
            pri_pic->default_pic = &pri_pic->pictures[index];
            av_log(NULL, AV_LOG_WARNING, "DEFAULT_INDEX: %d, real pic_index: %d", dec_ctx->pp_out, index);
        }
        info = pri_pic->default_pic;
        frame->format = info->format;
        frame->width = info->width;
        frame->height = info->height;
        frame->key_frame = info->key_frame;
        for(int i = 0; i < info->n_planes; i++)
        {
            frame->data[i] = (uint8_t *)info->virtual_address + info->offset[i];
            frame->linesize[i] = info->stride[i];
        }
        if (ESDecIsSimulation()){
            dma_fd = (uint64_t)info->virtual_address;
        }
        else{
            dma_fd = (uint64_t)info->fd;
        }
        av_log(NULL, AV_LOG_INFO, "sned dma_fd: %lx\n", dma_fd);
        ff_es_codec_add_fd_to_side_data(frame, dma_fd);

        frame->buf[0] = av_buffer_create((uint8_t *)pri_pic,
                                sizeof(DecPicturePri),
                                dec_ctx->data_free,
                                dec_ctx, AV_BUFFER_FLAG_READONLY);
    } else {
        av_log(NULL, AV_LOG_ERROR, "error !!! no picture");
    }
}

int ff_es_jpeg_dec_output_frame(AVCodecContext *avctx, AVFrame *out,
                            struct DecPicturePpu *decoded_pic)
{
    int ret = -1;
    int i;
    VSVDECContext *dec_ctx  = avctx->priv_data;
    AVVSVFramesContext *frame_hwctx;
    AVHWFramesContext *hwframe_ctx;
    struct DecPicturePpu *pic = av_mallocz(sizeof(*pic));
    DecPicturePri *pri_pic = av_mallocz(sizeof(*pri_pic));

    if(!pic)
        return AVERROR(ENOMEM);

    memset(pri_pic, 0, sizeof(DecPicturePri));
    memcpy(pic, decoded_pic, sizeof(struct DecPicturePpu));

    av_log(avctx, AV_LOG_DEBUG, "dec output pic @: %p\n",pic);

    dec_ctx->cycle_count += pic->pictures[0].picture_info.cycles_per_mb;

    ret = ff_decode_frame_props(avctx, out);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "ff_decode_frame_props failed\n");
        return ret;
    }

    out->opaque_ref = av_buffer_allocz(sizeof(VSVFramePriv));

    if (out->opaque_ref == NULL)
        goto err_exit;
    out->opaque = out->opaque_ref->data;

    for (i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        PpUnitConfig * pp = &dec_ctx->vsv_dec_config.ppu_cfg[i];
        if (pp->enabled == 1) {
            pic->pictures[i].pp_enabled = 1;
            pri_pic->pic_count++;
            av_log(avctx, AV_LOG_DEBUG,
                   "pic.pictures[%d].pp_enabled = %d\n",
                   i,pic->pictures[i].pp_enabled);
        } else {
            pic->pictures[i].pp_enabled = 0;
        }
    }

    for (i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        av_log(avctx, AV_LOG_DEBUG,
               "pic.pictures[%d].pp_enabled = %d,bit_depth_luma=%d ,pic.pictures[%d].luma.virtual_address=%p\n",
               i,pic->pictures[i].pp_enabled,pic->pictures[i].sequence_info.bit_depth_luma,i,pic->pictures[i].luma.virtual_address);
    }

    ff_es_jpeg_fill_out_frame(dec_ctx, out, pic);

    if (out->buf[0] == NULL) {
        goto err_exit;
    }

    add_dec_pic_wait_consume_list(dec_ctx, pic);

    return 0;
err_exit:

    return -1;
}

int ff_es_jpeg_dec_output_thum(AVCodecContext *avctx, AVFrame *out,
                            struct DecPicturePpu *decoded_pic)
{
    int ret = -1;
    int i;
    VSVDECContext *dec_ctx  = avctx->priv_data;
    AVVSVFramesContext *frame_hwctx;
    AVHWFramesContext *hwframe_ctx;
    struct DecPicturePpu *pic = av_mallocz(sizeof(*pic));
    DecPicturePri *pri_pic;
    ESJpegOutputMemory *memory;

    if (!pic)
        return AVERROR(ENOMEM);

    if (!out || !out->data) {
        av_log(avctx, AV_LOG_ERROR, "output frame is invaild");
        goto err_exit;
    }
    pri_pic = (DecPicturePri *)out->buf[0]->data;

    memcpy(pic, decoded_pic, sizeof(struct DecPicturePpu));

    av_log(avctx, AV_LOG_DEBUG, "dec output pic @: %p\n",pic);
    report_dec_pic_info(avctx, pic);

    dec_ctx->cycle_count += pic->pictures[0].picture_info.cycles_per_mb;

    PpUnitConfig * pp = &dec_ctx->vsv_dec_config.ppu_cfg[dec_ctx->pp_out];
    if (pp->enabled == 1) {
        pic->pictures[dec_ctx->pp_out].pp_enabled = 1;
        pri_pic->pic_count++;
        av_log(avctx, AV_LOG_DEBUG,
                "thumbnail default output pic.pictures[%d].pp_enabled = %d\n",
                dec_ctx->pp_out,
                pic->pictures[dec_ctx->pp_out].pp_enabled);
    } else {
        pic->pictures[dec_ctx->pp_out].pp_enabled = 0;
        av_log(avctx, AV_LOG_ERROR,
                "thumbnail default output pictures[%d].pp_enabled = %d\n",
                dec_ctx->pp_out,
                pic->pictures[dec_ctx->pp_out].pp_enabled);
        goto err_exit;
    }

    memory = ff_es_jpeg_find_memory_by_picture(dec_ctx, pic);

    for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if (i == dec_ctx->pp_out) {
            if (pic->pictures[i].pp_enabled) {
                pri_pic->pic_count++;
                pri_pic->pictures[2].enabled = 1;
                pri_pic->pictures[2].fd = memory->fd[i];
                ff_es_jpeg_fill_planes(&pri_pic->pictures[2], &pic->pictures[dec_ctx->pp_out]);
            }
            break;
        }
    }

    add_dec_pic_wait_consume_list(dec_ctx, pic);

    return 0;
err_exit:

    return -1;
}

int ff_es_jpeg_dec_send_avpkt_to_decode_buffer(AVCodecContext *avctx, AVPacket *avpkt,
                                           struct DWLLinearMem stream_buffer)
{
    //VSVDECContext *dec_ctx  = avctx->priv_data;
    int ret = 0;

    if (avpkt->data && avpkt->size)
        memcpy((uint8_t*)stream_buffer.virtual_address, avpkt->data, avpkt->size);
    //stream_buffer.bus_address
//#ifdef  NEW_MEM_ALLOC
    //if( stream_buffer.bus_address_rc != 0) {
        //ret = dwl_edma_rc2ep_nolink(dec_ctx->dwl_inst, stream_buffer.bus_address_rc,
                                    //stream_buffer.bus_address, stream_buffer.size);
    //}
//#endif

    return ret;
}

void ff_es_jpeg_dec_init_dec_input_paras(AVPacket *avpkt,
                                         VSVDECContext *dec_ctx,
                                         struct DecInputParameters *jpeg_in) {
    DWLmemset(jpeg_in, 0, sizeof(*jpeg_in));
    jpeg_in->stream_buffer.virtual_address = (u32 *) dec_ctx->stream_mem[dec_ctx->stream_mem_index].virtual_address;
    jpeg_in->stream_buffer.bus_address =  (addr_t)dec_ctx->stream_mem[dec_ctx->stream_mem_index].bus_address;
    jpeg_in->stream_buffer.logical_size = dec_ctx->stream_mem[dec_ctx->stream_mem_index].logical_size;
    jpeg_in->stream = (u8 *) dec_ctx->stream_mem[dec_ctx->stream_mem_index].virtual_address;
    jpeg_in->strm_len = avpkt->size;
    if(dec_ctx->low_latency) {
        jpeg_in->strm_len = dec_ctx->sw_hw_bound;
    }
    jpeg_in->buffer_size = 0;
    jpeg_in->dec_image_type = JPEGDEC_IMAGE;
}

void ff_es_jpeg_set_ppu_output_format(VSVDECContext *dec_ctx, struct DecConfig *config){
    int i;

    for(i = 0;  i< ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if(config->ppu_cfg[i].enabled == 1 ) {

            enum DecPictureFormat dstpicfmt = ff_codec_pixfmt_to_decfmt(dec_ctx->output_format[i]);

            av_log(NULL,AV_LOG_DEBUG,"[%s:%d]  dec_ctx->output_format[%d]; %d  dstpicfmt: %d\n",
                                       __func__,__LINE__, i,
                                       dec_ctx->output_format[i],dstpicfmt);

            switch (dstpicfmt) {
                case DEC_OUT_FRM_NV21SP:
                    config->ppu_cfg[i].cr_first = 1;
                    break;

                case DEC_OUT_FRM_YUV420SP:
                    // todo
                    break;

                case DEC_OUT_FRM_YUV420P:
                    config->ppu_cfg[i].planar = 1;
                    break;

                case DEC_OUT_FRM_YUV400:
                    config->ppu_cfg[i].monochrome = 1;
                    break;

                case DEC_OUT_FRM_RGB888:
                case DEC_OUT_FRM_BGR888:
                case DEC_OUT_FRM_XRGB888:
                case DEC_OUT_FRM_XBGR888:
                    config->ppu_cfg[i].rgb = 1;
                    config->ppu_cfg[i].rgb_format = dstpicfmt;
                    break;

                case DEC_OUT_FRM_ARGB888:
                case DEC_OUT_FRM_ABGR888:
                    config->ppu_cfg[i].rgb = 1;
                    config->ppu_cfg[i].rgb_stan = BT709;
                    config->ppu_cfg[i].rgb_alpha = 255;
                    config->ppu_cfg[i].rgb_format = dstpicfmt;
                    break;

                default:
                    av_log(NULL,AV_LOG_ERROR,"[%s:%d] set ppu[%d] output_format failed,"
                                             "avctx->output_format: %d dstpicfmt: %d\n",
                                              __func__, __LINE__, i,
                                              dec_ctx->output_format[i], dstpicfmt);
                    break;
            }
        }
    }
}

int ff_es_jpeg_parse_ppset(VSVDECContext *dec_ctx, CropInfo *crop_info, ScaleInfo *scale_info) {
    int i;

    for (i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        av_log(dec_ctx, AV_LOG_DEBUG, "dec_ctx->pp_setting[%d]: %s", i, dec_ctx->pp_setting[i]);

        if (dec_ctx->pp_setting[i] || dec_ctx->crop_set[i] || (i == 1 && dec_ctx->scale_set)) {
            int ret = 0;

            if (dec_ctx->pp_setting[i]) {
                ret = ff_codec_get_crop(dec_ctx->pp_setting[i], crop_info + i);
            } else if (dec_ctx->crop_set[i]) {
                ret = es_codec_get_crop(dec_ctx->crop_set[i], crop_info + i);
            }
            if (ret < 0) {
                av_log(dec_ctx, AV_LOG_ERROR, "picture_%d crop config error, please check!", i);
            }

            if( i == 0)
                continue;

            ret = 0;
            if (dec_ctx->pp_setting[i]) {
                ret = ff_dec_get_scale(dec_ctx->pp_setting[i], scale_info + i, i);
            } else if (dec_ctx->scale_set) {
                ret = es_codec_get_scale(dec_ctx->scale_set, scale_info + i);
            }
            if (ret < 0) {
                av_log(dec_ctx, AV_LOG_ERROR, "picture_%d scale config error, please check!", i);
            }
        }
    }

    return 0;
}

int ff_es_jpeg_set_ppu_crop_and_scale(VSVDECContext *dec_ctx, struct DecConfig *config)
{
    CropInfo crop_paras[ES_VID_DEC_MAX_OUT_COUNT];
    ScaleInfo scale_paras[ES_VID_DEC_MAX_OUT_COUNT];
    int i;
    int pp_enabled = 0;

    memset(crop_paras, 0, sizeof(CropInfo) * ES_VID_DEC_MAX_OUT_COUNT);
    memset(scale_paras, 0, sizeof(ScaleInfo) * ES_VID_DEC_MAX_OUT_COUNT);

    int ret = ff_es_jpeg_parse_ppset(dec_ctx, &crop_paras, &scale_paras);
    if (ret < 0)
        return -1;

    //set crop paras
    for (i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        // if config->ppu_cfg[i].enabled==0, crop is not assigned
        if (!config->ppu_cfg[i].enabled||
            (crop_paras[i].crop_xoffset == 0 && crop_paras[i].crop_yoffset == 0
            && crop_paras[i].crop_width == 0 && crop_paras[i].crop_height == 0)) {
            continue;
        }

        pp_enabled = 1;
        config->ppu_cfg[i].crop.enabled = 1;
        config->ppu_cfg[i].crop.set_by_user = 1;
        config->ppu_cfg[i].crop.x = crop_paras[i].crop_xoffset;
        config->ppu_cfg[i].crop.y = crop_paras[i].crop_yoffset;
        config->ppu_cfg[i].crop.width = crop_paras[i].crop_width;
        config->ppu_cfg[i].crop.height = crop_paras[i].crop_height;

        av_log(NULL,AV_LOG_DEBUG,"[%s:%d]  ppu_cfg[%d].crop.x: %d  "
                                           "ppu_cfg[%d].crop.y: %d  "
                                           "ppu_cfg[%d].crop.width: %d  "
                                           "ppu_cfg[%d].crop.height: %d\n",
                                           __func__,__LINE__,
                                           i,config->ppu_cfg[i].crop.x,
                                           i,config->ppu_cfg[i].crop.y,
                                           i,config->ppu_cfg[i].crop.width,
                                           i,config->ppu_cfg[i].crop.height);
    }

    //set scale paras,pp0 not support scale
    for (i = 1; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        // if config->ppu_cfg[i].enabled==0, scale is not assigned
        if (!config->ppu_cfg[i].enabled||
            scale_paras[i].scale_width == 0 && scale_paras[i].scale_height == 0) {
            continue;
        }
        pp_enabled = 1;

        if (!(scale_paras[i].scale_width == 0 && scale_paras[i].scale_height == 0)) {
            config->ppu_cfg[i].scale.enabled = 1;
            config->ppu_cfg[i].scale.width = scale_paras[i].scale_width;
            config->ppu_cfg[i].scale.height = scale_paras[i].scale_height;
            av_log(NULL,AV_LOG_DEBUG,"[%s:%d]  ppu_cfg[%d].scale.width: %d   "
                                           "ppu_cfg[%d].scale.height: %d",
                                           __func__, __LINE__,
                                           i, config->ppu_cfg[i].scale.width,
                                           i, config->ppu_cfg[i].scale.height);
        }
    }

    dec_ctx->pp_enabled = pp_enabled;

    return 0;
}

int ff_es_jpeg_get_align(uint32_t stride) {
    switch (stride)
    {
        case 1:
            return DEC_ALIGN_1B;
        case 8:
            return DEC_ALIGN_8B;
        case 16:
            return DEC_ALIGN_16B;
        case 32:
            return DEC_ALIGN_32B;
        case 64:
            return DEC_ALIGN_64B;
        case 128:
            return DEC_ALIGN_128B;
        case 256:
            return DEC_ALIGN_256B;
        case 512:
            return DEC_ALIGN_512B;
        case 1024:
            return DEC_ALIGN_1024B;
        case 2048:
            return DEC_ALIGN_2048B;

        default:
            av_log(NULL,AV_LOG_ERROR,"invaild stride: %d\n", stride);
    }
    return DEC_ALIGN_64B;
}

int ff_es_jpeg_init_frame_dump_handle(VSVDECContext *dec_ctx) {
    int ret = 0;

    for ( int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if(dec_ctx->cfg_pp_enabled[i] == 1 && dec_ctx->frame_dump[i] && !dec_ctx->frame_dump_handle[i]) {
            DumpParas paras;
            paras.width = dec_ctx->pic.pictures[i].pic_width;
            paras.height = dec_ctx->pic.pictures[i].pic_height;
            paras.pic_stride = dec_ctx->pic.pictures[i].pic_stride;
            paras.pic_stride_ch = dec_ctx->pic.pictures[i].pic_stride_ch;
            paras.prefix_name = "jdec";

            if (i == 0)
                paras.ppu_channel = "pp0";
            else
                paras.ppu_channel = "pp01";

            if (IS_PIC_RGB_FMT(dec_ctx->pic.pictures[i].picture_info.format))
                paras.suffix_name = "rgb";
            else
                paras.suffix_name = "yuv";

            paras.fmt = ff_codec_decfmt_to_char(dec_ctx->pic.pictures[i].picture_info.format);

            dec_ctx->frame_dump_handle[i] = ff_codec_dump_file_open(dec_ctx->dump_path, dec_ctx->frame_dump_time[i], &paras);

        }
    }

    return ret;
}

int ff_es_jpeg_init_pkt_dump_handle(VSVDECContext *dec_ctx) {
    int ret = 0;
    struct DecSequenceInfo *info = &dec_ctx->sequence_info;
    DumpParas paras;

    if(dec_ctx->packet_dump && !dec_ctx->pkt_dump_handle) {
        paras.width = info->pic_width;
        paras.height = info->pic_height;
        paras.pic_stride = 0;
        paras.pic_stride_ch = 0;
        paras.prefix_name = "jdec";
        paras.suffix_name = "jpeg";
        paras.fmt = NULL;
        dec_ctx->pkt_dump_handle = ff_codec_dump_file_open(dec_ctx->dump_path, dec_ctx->packet_dump_time, &paras);
    }

    return ret;
}

int ff_es_jpeg_frame_dump(VSVDECContext *dec_ctx) {
    int ret = 0;
    for ( int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if(dec_ctx->cfg_pp_enabled[i] == 1 && dec_ctx->frame_dump[i]) {
            if (dec_ctx->frame_dump_handle[i]) {
                ret = ff_codec_dump_data_to_file_by_decpicture(&dec_ctx->pic.pictures[i], dec_ctx->frame_dump_handle[i]);
                if (ret == ERR_TIMEOUT) {
                    av_log(NULL, AV_LOG_INFO, "frame dump timeout\n");
                    ff_codec_dump_file_close(&dec_ctx->frame_dump_handle[i]);
                    dec_ctx->frame_dump[i] = 0;
                    return 0;
                } else if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR, "write file error\n");
                    return -1;
                }
            } else {
                av_log(NULL, AV_LOG_ERROR,"fp is not inited\n");
                return -1;
            }
        }
    }

    return 0;
}

int ff_es_jpeg_pkt_dump(VSVDECContext *dec_ctx) {
    int ret = 0;
    if(dec_ctx->packet_dump) {
        if (dec_ctx->pkt_dump_handle) {
            ret = ff_codec_dump_bytes_to_file(dec_ctx->avpkt.data, dec_ctx->avpkt.size,  dec_ctx->pkt_dump_handle);
            if (ret == ERR_TIMEOUT) {
                av_log(NULL, AV_LOG_INFO, "pkt dump timeout\n");
                ff_codec_dump_file_close(&dec_ctx->pkt_dump_handle);
                dec_ctx->packet_dump = 0;
                return 0;
            } else if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "write frame into file failed\n");
                return -1;
            }
        } else {
            av_log(NULL, AV_LOG_ERROR, "fp is not inited\n");
            return -1;
        }
    }

    return 0;
}

void ff_es_jpeg_dec_update_dec_input_paras(VSVDECContext *dec_ctx,
                                            struct DecInputParameters *jpeg_in)
{
    if(dec_ctx->sequence_info.thumbnail_type == JPEGDEC_THUMBNAIL_JPEG) {
        if (dec_ctx->thumb_mode == Only_Decode_Thumb) {
            jpeg_in->dec_image_type =JPEGDEC_THUMBNAIL;
            dec_ctx->thum_out = 1;
        } else {
            jpeg_in->dec_image_type = JPEGDEC_IMAGE;
        }
        dec_ctx->thum_exist = 1;
        av_log(NULL, AV_LOG_DEBUG, "thumbnail exits\n");
    } else if((dec_ctx->sequence_info.thumbnail_type == JPEGDEC_NO_THUMBNAIL)
                ||(dec_ctx->sequence_info.thumbnail_type == JPEGDEC_THUMBNAIL_NOT_SUPPORTED_FORMAT)) {
        jpeg_in->dec_image_type = JPEGDEC_IMAGE;
    }
}

int ff_es_jpeg_dec_modify_thum_config_by_sequence_info(AVCodecContext *avctx)
{
    VSVDECContext *dec_ctx  = avctx->priv_data;
    struct DecSequenceInfo *image_info = &dec_ctx->sequence_info;
    struct DecConfig *config = &dec_ctx->vsv_dec_config;
    int i;

    u32 display_width = (image_info->scaled_width + 1) & ~0x1;
    u32 display_height = (image_info->scaled_height + 1) & ~0x1;
    u32 display_width_thumb = (image_info->scaled_width_thumb + 1) & ~0x1;
    u32 display_height_thumb = (image_info->scaled_height_thumb + 1) & ~0x1;
    u32 crop_width_thumb = 0;
    u32 crop_height_thumb = 0;

    if (!config->ppu_cfg[0].crop.set_by_user) {
      config->ppu_cfg[0].crop.width = dec_ctx->thum_out ? display_width_thumb: display_width;
      config->ppu_cfg[0].crop.height = dec_ctx->thum_out ? display_height_thumb: display_height;
      config->ppu_cfg[0].crop.enabled = 1;
    }
    u32 crop_w = config->ppu_cfg[0].crop.width;
    u32 crop_h = config->ppu_cfg[0].crop.height;

    crop_width_thumb = NEXT_MULTIPLE(crop_w - 1, 2);
    crop_height_thumb = NEXT_MULTIPLE(crop_h - 1, 2);
    image_info->pic_width = NEXT_MULTIPLE(crop_width_thumb, ALIGN(config->align));
    image_info->pic_width_thumb = NEXT_MULTIPLE(crop_width_thumb, ALIGN(config->align));
    image_info->pic_height = crop_height_thumb;
    image_info->pic_height_thumb = crop_height_thumb;

    if (dec_ctx->thum_out == 1) {
        for (i = 0; i < DEC_MAX_PPU_COUNT; i++) {
            if (!config->ppu_cfg[i].enabled)
                    continue;
            if (config->ppu_cfg[i].scale.enabled == 1) {
                config->ppu_cfg[i].scale.scale_by_ratio = 0;
                config->ppu_cfg[i].scale.width = image_info->scaled_width_thumb;
                config->ppu_cfg[i].scale.height = image_info->scaled_height_thumb;
            }
            if (config->ppu_cfg[i].crop.enabled == 1) {
                config->ppu_cfg[i].crop.enabled = 0;
                config->ppu_cfg[i].crop.set_by_user = 0;
            }
        }
    }
    return 0;
}

bool check_scale_value( uint32_t v) {
    if (v == -1 || v == -2 || v == -4 || v == -8) {
        return TRUE;
    }
    return FALSE;
}

int ff_es_jpeg_dec_modify_config_by_sequence_info(AVCodecContext *avctx)
{
    VSVDECContext *dec_ctx  = avctx->priv_data;
    enum DecRet rv_info;

    /* process pp size -1/-2/-4/-8. */
    struct DecConfig *config = &dec_ctx->vsv_dec_config;
    int i;
    uint32_t alignh = dec_ctx->sequence_info.is_interlaced ? 4 : 2;
    uint32_t alignw = 2;

    uint32_t original_width = dec_ctx->sequence_info.pic_width;
    uint32_t original_height = dec_ctx->sequence_info.pic_height;

    //crop
    for (i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if (config->ppu_cfg[i].enabled && config->ppu_cfg[i].crop.enabled) {
            if (config->ppu_cfg[i].crop.x > original_width ||
                config->ppu_cfg[i].crop.y > original_height ||
                ((config->ppu_cfg[i].crop.x + config->ppu_cfg[i].crop.width) > original_width) ||
                ((config->ppu_cfg[i].crop.x + config->ppu_cfg[i].crop.height) > original_height)) {
                av_log(avctx,AV_LOG_ERROR,"invalid crop config, original_width: %d original_height: %d\n",
                    original_width, original_height);
                return -1;
            }

            // check value
            if(config->ppu_cfg[i].crop.width < 48 || config->ppu_cfg[i].crop.height < 48) {
                av_log(avctx,AV_LOG_ERROR,"pp%d invalid crop config, crop.width: %d crop.height: %d, "
                                               "request values equal to or more than 48\n",
                                               i, config->ppu_cfg[i].crop.width, config->ppu_cfg[i].crop.height);
                return -1;
            }

            if ((config->ppu_cfg[i].crop.width % 2) || (config->ppu_cfg[i].crop.height % 2)) {
                av_log(avctx,AV_LOG_ERROR,"pp%d invalid crop config, crop.width: %d crop.height: %d, request values is even\n",
                                               i, config->ppu_cfg[i].crop.width, config->ppu_cfg[i].crop.height);
                config->ppu_cfg[i].crop.width = NEXT_MULTIPLE(config->ppu_cfg[i].crop.width, alignw);
                config->ppu_cfg[i].crop.height = NEXT_MULTIPLE(config->ppu_cfg[i].crop.height, alignh);
                //return -1;
            }
        }
    }

    // scale
    for (i = 1; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if (config->ppu_cfg[i].enabled && config->ppu_cfg[i].scale.enabled) {
            if (check_scale_value(config->ppu_cfg[i].scale.width) ||
                check_scale_value(config->ppu_cfg[i].scale.height)) {

                if (config->ppu_cfg[i].scale.width == -1
                    && config->ppu_cfg[i].scale.height == -1) {
                    rv_info = DEC_INFOPARAM_ERROR;
                    break;
                }

                if (config->ppu_cfg[i].crop.enabled) {
                    if (config->ppu_cfg[i].crop.width != original_width) {
                        original_width = config->ppu_cfg[i].crop.width;
                    }
                    if (config->ppu_cfg[i].crop.height != original_height) {
                        original_height = config->ppu_cfg[i].crop.height;
                    }
                }

                av_log(avctx,AV_LOG_DEBUG,"original_width = %d, original_height = %d\n",
                    original_width, original_height);

                if (config->ppu_cfg[i].scale.width == -1 &&
                    !check_scale_value(config->ppu_cfg[i].scale.height) &&
                    config->ppu_cfg[i].scale.height > 0) {
                    config->ppu_cfg[i].scale.width =
                        NEXT_MULTIPLE((original_width
                                    * config->ppu_cfg[i].scale.height)/original_height, alignw);
                    config->ppu_cfg[i].scale.height =
                        NEXT_MULTIPLE(config->ppu_cfg[i].scale.height, alignh);
                } else if (config->ppu_cfg[i].scale.height == -1 &&
                        !check_scale_value(config->ppu_cfg[i].scale.width) &&
                        config->ppu_cfg[i].scale.width > 0) {
                    config->ppu_cfg[i].scale.width =
                        NEXT_MULTIPLE(config->ppu_cfg[i].scale.width, alignw);
                    config->ppu_cfg[i].scale.height =
                        NEXT_MULTIPLE((original_height
                                    * config->ppu_cfg[i].scale.width)/original_width, alignh);
                } else if (check_scale_value(config->ppu_cfg[i].scale.width) &&
                        check_scale_value(config->ppu_cfg[i].scale.height)) {
                    config->ppu_cfg[i].scale.scale_by_ratio = 1;
                    config->ppu_cfg[i].scale.ratio_x = -config->ppu_cfg[i].scale.width;
                    config->ppu_cfg[i].scale.ratio_y = -config->ppu_cfg[i].scale.height;
                    config->ppu_cfg[i].scale.width = 0;
                    config->ppu_cfg[i].scale.height = 0;
                } else {
                    rv_info = DEC_INFOPARAM_ERROR;
                    av_log(avctx,AV_LOG_ERROR,"invalid scale config, scale.width: %d scale.height: %d\n",
                    config->ppu_cfg[i].scale.width, config->ppu_cfg[i].scale.height);
                    break;
                }
            } else if (config->ppu_cfg[i].scale.width > 0 && config->ppu_cfg[i].scale.width > 0) {
                if(config->ppu_cfg[i].scale.width > original_width ||
                    config->ppu_cfg[i].scale.height > original_height) {
                    av_log(avctx,AV_LOG_ERROR,"invalid scale config, scale.width: %d scale.height: %d\n",
                    config->ppu_cfg[i].scale.width, config->ppu_cfg[i].scale.height);
                    return -1;
                }
            } else if (config->ppu_cfg[i].scale.width != 0 && config->ppu_cfg[i].scale.height != 0){
                av_log(avctx,AV_LOG_ERROR,"invalid scale config, scale.width: %d scale.height: %d\n",
                    config->ppu_cfg[i].scale.width, config->ppu_cfg[i].scale.height);
                return -1;
            }
        }
    }

#if 0
    //-JpegDecImageInfo does not have a crop_Params structure, so this operation is not required
    /* Ajust user cropping params based on cropping params from sequence info. */
    if (dec_ctx->sequence_info.crop_params.crop_left_offset != 0
        || dec_ctx->sequence_info.crop_params.crop_top_offset != 0
        || dec_ctx->sequence_info.crop_params.crop_out_width != dec_ctx->sequence_info.pic_width
        || dec_ctx->sequence_info.crop_params.crop_out_height != dec_ctx->sequence_info.pic_height) {
            for (i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
                if (!config->ppu_cfg[i].enabled)
                    continue;

            if (!config->ppu_cfg[i].crop.enabled) {
                config->ppu_cfg[i].crop.x = dec_ctx->sequence_info.crop_params.crop_left_offset;
                config->ppu_cfg[i].crop.y = dec_ctx->sequence_info.crop_params.crop_top_offset;
                config->ppu_cfg[i].crop.width =
                    NEXT_MULTIPLE(dec_ctx->sequence_info.crop_params.crop_out_width, 2);
                config->ppu_cfg[i].crop.height =
                    NEXT_MULTIPLE(dec_ctx->sequence_info.crop_params.crop_out_height, 2);
            } else {
                config->ppu_cfg[i].crop.x += dec_ctx->sequence_info.crop_params.crop_left_offset;
                config->ppu_cfg[i].crop.y += dec_ctx->sequence_info.crop_params.crop_top_offset;
                if(!config->ppu_cfg[i].crop.width)
                    config->ppu_cfg[i].crop.width =
                        dec_ctx->sequence_info.crop_params.crop_out_width;
                if(!config->ppu_cfg[i].crop.height)
                    config->ppu_cfg[i].crop.height =
                        dec_ctx->sequence_info.crop_params.crop_out_height;
            }
            config->ppu_cfg[i].enabled = 1;
            config->ppu_cfg[i].crop.enabled = 1;
        }
    }
    //The validity of the ppu_cfg parameter is checked by CheckPpUnitConfig in JpegDecSetInfo
    for (i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if (config->ppu_cfg[i].crop.enabled) {
            if ((config->ppu_cfg[i].scale.width > config->ppu_cfg[i].crop.width)
                || (config->ppu_cfg[i].scale.height > config->ppu_cfg[i].crop.height)
                || (config->ppu_cfg[i].crop.width > dec_ctx->sequence_info.crop_params.crop_out_width)
                || (config->ppu_cfg[i].crop.height > dec_ctx->sequence_info.crop_params.crop_out_height)) {
                    return -1;
            }
        } else {
            if ((config->ppu_cfg[i].scale.width > dec_ctx->sequence_info.crop_params.crop_out_width)
                || (config->ppu_cfg[i].scale.height > dec_ctx->sequence_info.crop_params.crop_out_height)) {
                    return -1;
            }
        }
    }
#endif
#if 0
    {
        // for bypass mode or 10bit case use pp0 datapath for performance
        if (config->ppu_cfg[0].enabled == 2) {
            av_log(avctx,AV_LOG_DEBUG,
                   "dec_ctx->sequence_info.bit_depth_luma = %d, dec_ctx->sequence_info.bit_depth_chroma = %d\n",
                    dec_ctx->sequence_info.bit_depth_luma, dec_ctx->sequence_info.bit_depth_chroma);
            if (dec_ctx->sequence_info.bit_depth_luma > 8
                || dec_ctx->sequence_info.bit_depth_chroma > 8) {
                av_log(avctx,AV_LOG_DEBUG,"adaptive to enable pp0\n");
                config->ppu_cfg[0].enabled = 1;
            } else {
                av_log(avctx,AV_LOG_DEBUG,"adaptive to disable pp0\n");
                config->ppu_cfg[0].enabled = 0;
            }
        }
    }

    //check PP setting legal
    rv_info = DEC_OK;
    for(i=0; i<4; i++) {
        if(config->ppu_cfg[i].enabled == 1) {
            if(ppmax_output_pic_h[i] == 0 ) {
                if((config->ppu_cfg[i].scale.height > dec_ctx->sequence_info.pic_height)
                    ||(config->ppu_cfg[i].scale.width > dec_ctx->sequence_info.pic_width)) {
                    av_log(avctx,AV_LOG_DEBUG,
                           "PP[%d] Height setting is illegal: %d > MAX (%d)\n",i,
                           config->ppu_cfg[i].scale.height,ppmax_output_pic_h[i]==0
                           ? dec_ctx->sequence_info.pic_height : ppmax_output_pic_h[i]);
                    rv_info = DEC_INFOPARAM_ERROR;
                    break;
                }
            } else {
                if((config->ppu_cfg[i].scale.height > ppmax_output_pic_h[i])
                    ||(config->ppu_cfg[i].scale.width > ppmax_output_pic_h[i])) {
                    av_log(avctx,AV_LOG_DEBUG,
                           "PP[%d] Height setting is illegal: %d > MAX (%d)\n",i,
                           config->ppu_cfg[i].scale.height,ppmax_output_pic_h[i]==0
                           ? dec_ctx->sequence_info.pic_height : ppmax_output_pic_h[i]);
                    rv_info = DEC_INFOPARAM_ERROR;
                    break;
                }
            }
        }
    }
    if(rv_info == DEC_INFOPARAM_ERROR) {
        return -1;
    }

    if(dec_ctx->disable_dec400) {
        //disable all shaper for pp
        ff_es_jpeg_dec_disable_all_pp_shaper(&dec_ctx->vsv_dec_config);
    } else {
        //check SR ratio capability
        if(dec_ctx->sequence_info.pic_height > 1088)
            shaper_en_num = 3;
        else
            shaper_en_num = 2;

        for(i=0; i<4; i++) {
            sr[i] = (double)max_output_pic_h[i]/max_input_pic_h;
            sr_sum += sr[i];
        }

        for(i=0; i<4; i++) {
            if(config->ppu_cfg[i].enabled == 1) {
                //10bit stream disable shaper when shaper_enabled = 2
                if((i == 0)&&(config->ppu_cfg[i].shaper_enabled == 2)) {
                    if((dec_ctx->sequence_info.bit_depth_luma > 8)
                        ||(dec_ctx->sequence_info.bit_depth_chroma > 8)) {
                        config->ppu_cfg[i].shaper_enabled = 0;//disable when 10bit
                        continue;
                    } else {
                        config->ppu_cfg[i].shaper_enabled = 1;//enable when 8bit
                    }
                }

                if (config->ppu_cfg[i].scale.enabled == 0) {
                    sr_p[i] = 1;
                } else if (config->ppu_cfg[i].crop.enabled) {
                    sr_p[i] = (double)config->ppu_cfg[i].scale.height/config->ppu_cfg[i].crop.height;
                } else {
                    sr_p[i] = (double)config->ppu_cfg[i].scale.height/dec_ctx->sequence_info.pic_height;
                }
                sr_p_sum += sr_p[i];
            }
            av_log(avctx,AV_LOG_DEBUG,"PP%d enabled=%d,SR_sum=%f,SR_P_sum=%f\n",
                   i,config->ppu_cfg[i].enabled,sr_sum,sr_p_sum);
            if((shaper_en_num > 0)
               &&(sr_p_sum < sr_sum)
               &&(config->ppu_cfg[i].enabled == 1)) {
                config->ppu_cfg[i].shaper_enabled = 1;
                shaper_en_num --;
            } else
              config->ppu_cfg[i].shaper_enabled = 0;
        }

    }
    av_log(avctx,AV_LOG_DEBUG,"in %s : %d ppu cfg :\n",__func__,__LINE__);

    if(dec_ctx->sequence_info.is_interlaced) {
        config->ppu_cfg[0].enabled = 1;
        for(int i=0;i<4;i++) {
            if(config->ppu_cfg[i].enabled == 1) {
                config->ppu_cfg[i].tiled_e = 0;
                config->ppu_cfg[i].shaper_enabled = 0;
                config->ppu_cfg[i].align = DEC_ALIGN_1024B;
            }
        }
    }
#endif

    ff_es_jpeg_ppu_print(config);

    return 0;
}

void ff_es_jpeg_ppu_print(struct DecConfig *config){

    for(int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if (config->ppu_cfg[i].enabled) {
            av_log(NULL, AV_LOG_DEBUG, "ppu_cfg[%d].tiled_e = %d\n", i,
               config->ppu_cfg[i].tiled_e);
            av_log(NULL, AV_LOG_DEBUG, "ppu_cfg[%d].scale.enabled = %d\n",
                i, config->ppu_cfg[i].scale.enabled);
            av_log(NULL, AV_LOG_DEBUG, "ppu_cfg[%d].scale.scale_by_ratio = %d\n",
                i, config->ppu_cfg[i].scale.scale_by_ratio);
            av_log(NULL, AV_LOG_DEBUG, "ppu_cfg[%d].scale.ratio_x = %d\n",
                i, config->ppu_cfg[i].scale.ratio_x);
            av_log(NULL, AV_LOG_DEBUG, "ppu_cfg[%d].scale.ratio_y = %d\n",
                i, config->ppu_cfg[i].scale.ratio_y);
            av_log(NULL, AV_LOG_DEBUG, "ppu_cfg[%d].scale.width = %d\n",
                i, config->ppu_cfg[i].scale.width);
            av_log(NULL, AV_LOG_DEBUG, "ppu_cfg[%d].scale.height = %d\n",
                i, config->ppu_cfg[i].scale.height);
            av_log(NULL, AV_LOG_DEBUG, "ppu_cfg[%d].crop.enabled = %d\n",
                i, config->ppu_cfg[i].crop.enabled);
            av_log(NULL, AV_LOG_DEBUG, "ppu_cfg[%d].crop.x = %d\n",
                i, config->ppu_cfg[i].crop.x);
            av_log(NULL, AV_LOG_DEBUG, "ppu_cfg[%d].crop.y = %d\n",
                i, config->ppu_cfg[i].crop.y);
            av_log(NULL, AV_LOG_DEBUG, "ppu_cfg[%d].crop.width = %d\n",
                i, config->ppu_cfg[i].crop.width);
            av_log(NULL, AV_LOG_DEBUG, "ppu_cfg[%d].crop.height = %d\n",
                i, config->ppu_cfg[i].crop.height);
            av_log(NULL, AV_LOG_DEBUG, "ppu_cfg[%d].out_p010 = %d\n",
                i, config->ppu_cfg[i].out_p010);
            av_log(NULL, AV_LOG_DEBUG, "ppu_cfg[%d].out_I010 = %d\n",
                i, config->ppu_cfg[i].out_I010);
            av_log(NULL, AV_LOG_DEBUG, "ppu_cfg[%d].align = %d\n",
                i, config->ppu_cfg[i].align);
            av_log(NULL, AV_LOG_DEBUG, "ppu_cfg[%d].shaper_enabled = %d\n",
                i, config->ppu_cfg[i].shaper_enabled);
            av_log(NULL, AV_LOG_DEBUG, "ppu_cfg[%d].cr_first = %d\n",
                i, config->ppu_cfg[i].cr_first);
            av_log(NULL, AV_LOG_DEBUG, "ppu_cfg[%d].rgb = %d\n",
                i, config->ppu_cfg[i].rgb);
            av_log(NULL, AV_LOG_DEBUG, "ppu_cfg[%d].rgb_format = %d\n",
                i, config->ppu_cfg[i].rgb_format);
        }
    }
}

void ff_es_jpeg_dec_disable_all_pp_shaper(struct DecConfig *config)
{
    int i;
    if(!config)
        return;
    for(i = 0; i < 4; i++)
        config->ppu_cfg[i].shaper_enabled = 0;
}

void ff_es_jpeg_dec_init_log_header(AVCodecContext *avctx)
{
    VSVDECContext *dec_ctx = avctx->priv_data;

#ifdef FB_SYSLOG_ENABLE
    static char module_name[] = "DEC";
    if(strlen(dec_ctx->module_name))
        dec_ctx->log_header.module_name = dec_ctx->module_name;
    else
        dec_ctx->log_header.module_name = module_name;
    dec_ctx->log_header.device_id = get_deviceId(dec_ctx->dev_name);
#endif
}

int ff_es_jpeg_dec_paras_check(AVCodecContext *avctx) {
    VSVDECContext *dec_ctx  = avctx->priv_data;

    if (dec_ctx->scale_set == NULL)
        av_log(avctx, AV_LOG_ERROR, "scale_set is null\n");

    enum DecPictureFormat dstpicfmt = ff_codec_pixfmt_to_decfmt(dec_ctx->output_format[0]);
    if(IS_PIC_RGB(dstpicfmt) && dec_ctx->cfg_pp_enabled[0] == 1) {
        av_log(avctx, AV_LOG_ERROR, "cannot set pp0 output format as rgb.\n",dec_ctx->pp_out);
        return -1;
    }

    if (dec_ctx->cfg_pp_enabled[0] == 1 && dec_ctx->cfg_pp_enabled[1] == 0) {
        if(dec_ctx->pp_out == 1) {
            av_log(avctx, AV_LOG_ERROR, "pp_out=1, pp1 disable.\n",dec_ctx->pp_out);
            return -1;
        }
    } else if (dec_ctx->cfg_pp_enabled[0] == 0 && dec_ctx->cfg_pp_enabled[1] == 1) {
        if(dec_ctx->pp_out == 0) {
            av_log(avctx, AV_LOG_ERROR, "pp_out=0, but pp0 disable.\n",dec_ctx->pp_out);
            return -1;
        }
    } else if (dec_ctx->cfg_pp_enabled[0] == 0 && dec_ctx->cfg_pp_enabled[1] == 0) {
        av_log(avctx, AV_LOG_WARNING, "pp0,pp1 are both disable.\n",dec_ctx->pp_out);
        return -1;
    }

    if (dec_ctx->fdump) {
        dec_ctx->frame_dump[0] = dec_ctx->cfg_pp_enabled[0];
        dec_ctx->frame_dump[1] = dec_ctx->cfg_pp_enabled[1];
    }

    return 0;
}

int ff_es_jpeg_dec_init_ppu_cfg(AVCodecContext *avctx,struct DecConfig *config)
{
    VSVDECContext *dec_ctx  = avctx->priv_data;
    int ret = 0;

    config->ppu_cfg[0].enabled = dec_ctx->cfg_pp_enabled[0];
    config->ppu_cfg[1].enabled = dec_ctx->cfg_pp_enabled[1];

    //set crop and scale
    ret = ff_es_jpeg_set_ppu_crop_and_scale(dec_ctx, config);
    if (ret < 0)
        return -1;

    //set output_format
    ff_es_jpeg_set_ppu_output_format(dec_ctx, config);

    dec_ctx->pp_enabled = 1;

    return 0;
}

void ff_es_jpeg_dec_print_image_info(AVCodecContext *avctx,struct DecSequenceInfo * image_info) {
  assert(image_info);

  /* Select if Thumbnail or full resolution image will be decoded */
  if(image_info->thumbnail_type == JPEGDEC_THUMBNAIL_JPEG) {
    /* decode thumbnail */
    av_log(avctx, AV_LOG_DEBUG, "\t-JPEG THUMBNAIL IN STREAM\n");
    av_log(avctx, AV_LOG_DEBUG, "\t-JPEG THUMBNAIL INFO\n");
    av_log(avctx, AV_LOG_DEBUG, "\t\t-JPEG thumbnail display resolution(W x H): %d x %d\n",
            image_info->scaled_width_thumb, image_info->scaled_height_thumb);
    av_log(avctx, AV_LOG_DEBUG, "\t\t-JPEG thumbnail HW decoded RESOLUTION(W x H): %d x %d\n",
            NEXT_MULTIPLE(image_info->scaled_width_thumb, 16),
            NEXT_MULTIPLE(image_info->scaled_height_thumb, 8));
    av_log(avctx, AV_LOG_DEBUG, "\t\t-JPEG thumbnail OUTPUT SIZE(Stride x H): %d x %d\n",
            image_info->pic_width_thumb, image_info->pic_height_thumb);

    /* stream type */
    switch (image_info->coding_mode_thumb) {
    case JPEG_BASELINE:
      av_log(avctx, AV_LOG_DEBUG, "\t\t-JPEG: STREAM TYPE: JPEG_BASELINE\n");
      break;
    case JPEG_PROGRESSIVE:
      av_log(avctx, AV_LOG_DEBUG, "\t\t-JPEG: STREAM TYPE: JPEG_PROGRESSIVE\n");
      break;
    case JPEG_NONINTERLEAVED:
      av_log(avctx, AV_LOG_DEBUG, "\t\t-JPEG: STREAM TYPE: JPEG_NONINTERLEAVED\n");
      break;
    }

    if(image_info->output_format_thumb) {
      switch (image_info->output_format_thumb) {
      case DEC_OUT_FRM_YUV400:
        av_log(avctx, AV_LOG_DEBUG,
                "\t\t-JPEG: THUMBNAIL OUTPUT: DEC_OUT_FRM_YUV400\n");
        break;
      case DEC_OUT_FRM_YUV420SP:
        av_log(avctx, AV_LOG_DEBUG,
                "\t\t-JPEG: THUMBNAIL OUTPUT: DEC_OUT_FRM_YUV420SP\n");
        break;
      case DEC_OUT_FRM_YUV422SP:
        av_log(avctx, AV_LOG_DEBUG,
                "\t\t-JPEG: THUMBNAIL OUTPUT: DEC_OUT_FRM_YUV422SP\n");
        break;
      case DEC_OUT_FRM_YUV440:
        av_log(avctx, AV_LOG_DEBUG,
                "\t\t-JPEG: THUMBNAIL OUTPUT: DEC_OUT_FRM_YUV440\n");
        break;
      case DEC_OUT_FRM_YUV411SP:
        av_log(avctx, AV_LOG_DEBUG,
                "\t\t-JPEG: THUMBNAIL OUTPUT: DEC_OUT_FRM_YUV411SP\n");
        break;
      case DEC_OUT_FRM_YUV444SP:
        av_log(avctx, AV_LOG_DEBUG,
                "\t\t-JPEG: THUMBNAIL OUTPUT: DEC_OUT_FRM_YUV444SP\n");
        break;
      default:
        av_log(avctx, AV_LOG_DEBUG,
                "\t\t-JPEG: THUMBNAIL OUTPUT: NOT SUPPORT\n");
        break;
      }
    }
  } else if(image_info->thumbnail_type == JPEGDEC_NO_THUMBNAIL) {
    /* decode full image */
    av_log(avctx, AV_LOG_DEBUG,
            "\t-NO THUMBNAIL IN STREAM ==> Decode full resolution image\n");
  } else if(image_info->thumbnail_type == JPEGDEC_THUMBNAIL_NOT_SUPPORTED_FORMAT) {
    /* decode full image */
    av_log(avctx, AV_LOG_DEBUG,
            "\tNot SUPPORTED THUMBNAIL IN STREAM ==> Decode full resolution image\n");
  }

  av_log(avctx, AV_LOG_DEBUG, "\t-JPEG FULL RESOLUTION INFO\n");
  av_log(avctx, AV_LOG_DEBUG, "\t\t-JPEG display resolution(W x H): %d x %d\n",
          image_info->scaled_width, image_info->scaled_height);
  av_log(avctx, AV_LOG_DEBUG, "\t\t-JPEG HW decoded RESOLUTION(W x H): %d x %d\n",
          NEXT_MULTIPLE(image_info->scaled_width, 8),
          NEXT_MULTIPLE(image_info->scaled_height, 8));
  av_log(avctx, AV_LOG_DEBUG, "\t\t-JPEG OUTPUT SIZE(Stride x H): %d x %d\n",
          image_info->pic_width, image_info->pic_height);
  if(image_info->output_format) {
    switch (image_info->output_format) {
    case DEC_OUT_FRM_YUV400:
      av_log(avctx, AV_LOG_DEBUG,
              "\t\t-JPEG: FULL RESOLUTION OUTPUT: DEC_OUT_FRM_YUV400\n");
#ifdef ASIC_TRACE_SUPPORT
      decoding_tools.sampling_4_0_0 = 1;
#endif
      break;
    case DEC_OUT_FRM_YUV420SP:
      av_log(avctx, AV_LOG_DEBUG,
              "\t\t-JPEG: FULL RESOLUTION OUTPUT: DEC_OUT_FRM_YUV420SP\n");
#ifdef ASIC_TRACE_SUPPORT
      decoding_tools.sampling_4_2_0 = 1;
#endif
      break;
    case DEC_OUT_FRM_YUV422SP:
      av_log(avctx, AV_LOG_DEBUG,
              "\t\t-JPEG: FULL RESOLUTION OUTPUT: DEC_OUT_FRM_YUV422SP\n");
#ifdef ASIC_TRACE_SUPPORT
      decoding_tools.sampling_4_2_2 = 1;
#endif
      break;
    case DEC_OUT_FRM_YUV440:
      av_log(avctx, AV_LOG_DEBUG,
              "\t\t-JPEG: FULL RESOLUTION OUTPUT: DEC_OUT_FRM_YUV440\n");
#ifdef ASIC_TRACE_SUPPORT
      decoding_tools.sampling_4_4_0 = 1;
#endif
      break;
    case DEC_OUT_FRM_YUV411SP:
      av_log(avctx, AV_LOG_DEBUG,
              "\t\t-JPEG: FULL RESOLUTION OUTPUT: DEC_OUT_FRM_YUV411SP\n");
#ifdef ASIC_TRACE_SUPPORT
      decoding_tools.sampling_4_1_1 = 1;
#endif
      break;
    case DEC_OUT_FRM_YUV444SP:
      av_log(avctx, AV_LOG_DEBUG,
              "\t\t-JPEG: FULL RESOLUTION OUTPUT: DEC_OUT_FRM_YUV444SP\n");
#ifdef ASIC_TRACE_SUPPORT
      decoding_tools.sampling_4_4_4 = 1;
#endif
      break;
    default:
      av_log(avctx, AV_LOG_DEBUG,
              "\t\t-JPEG: FULL RESOLUTION OUTPUT: NOT SUPPORT\n");
      break;
    }
  }

  /* stream type */
switch (image_info->coding_mode) {
  case JPEG_BASELINE:
    av_log(avctx, AV_LOG_DEBUG, "\t\t-JPEG: STREAM TYPE: JPEG_BASELINE\n");
    break;
  case JPEG_PROGRESSIVE:
    av_log(avctx, AV_LOG_DEBUG, "\t\t-JPEG: STREAM TYPE: JPEG_PROGRESSIVE\n");
#ifdef ASIC_TRACE_SUPPORT
    decoding_tools.progressive = 1;
#endif
    break;
  case JPEG_NONINTERLEAVED:
    av_log(avctx, AV_LOG_DEBUG, "\t\t-JPEG: STREAM TYPE: JPEG_NONINTERLEAVED\n");
    break;
  }

  if(image_info->thumbnail_type == JPEGDEC_THUMBNAIL_JPEG) {
    av_log(avctx, AV_LOG_DEBUG, "\t-JPEG ThumbnailType: JPEG\n");
#ifdef ASIC_TRACE_SUPPORT
    decoding_tools.thumbnail = 1;
#endif
  } else if(image_info->thumbnail_type == JPEGDEC_NO_THUMBNAIL)
    av_log(avctx, AV_LOG_DEBUG, "\t-JPEG ThumbnailType: NO THUMBNAIL\n");
  else if(image_info->thumbnail_type == JPEGDEC_THUMBNAIL_NOT_SUPPORTED_FORMAT)
    av_log(avctx, AV_LOG_DEBUG, "\t-JPEG ThumbnailType: NOT SUPPORTED THUMBNAIL\n");
}

static enum AVPixelFormat ff_es_jpeg_get_format(VSVDECContext *dec_ctx) {
    enum AVPixelFormat format = AV_PIX_FMT_NV12;
    if (!dec_ctx) {
        av_log(dec_ctx, AV_LOG_ERROR, "dec_ctx is null\n");
        return AV_PIX_FMT_NONE;
    }

    for (int i = 0; i < DEC_MAX_PPU_COUNT; i++) {
        if (dec_ctx->cfg_pp_enabled[i] && dec_ctx->pp_out == i) {
            format = dec_ctx->output_format[i];
            av_log(dec_ctx, AV_LOG_DEBUG, "target pp: %d, format: %d\n", i, format);
            break;
        }
    }

    return format;
}

int ff_es_jpeg_dec_init_hwctx(AVCodecContext *avctx)
{
    int ret=0;
    AVHWFramesContext *hw_frames_ctx;
    VSVDECContext *dec_ctx = avctx->priv_data;

    avctx->sw_pix_fmt = ff_es_jpeg_get_format(dec_ctx);
    enum AVPixelFormat pix_fmts[3] = {AV_PIX_FMT_ES, avctx->sw_pix_fmt, AV_PIX_FMT_NONE};
    avctx->pix_fmt = ff_get_format(avctx, pix_fmts);
    av_log(avctx, AV_LOG_INFO,
             "avctx sw_pix_fmt: %s, pix_fmt: %s\n",
             av_get_pix_fmt_name(avctx->sw_pix_fmt),
             av_get_pix_fmt_name(avctx->pix_fmt));

    if (avctx->hw_frames_ctx) {
        dec_ctx->hwframe = av_buffer_ref(avctx->hw_frames_ctx);
        if (!dec_ctx->hwframe) {
            ret = AVERROR(ENOMEM);
            goto error;
        }
    } else {
        av_log(avctx, AV_LOG_TRACE, "%s(%d) avctx->hw_device_ctx = %p\n",
                          __FUNCTION__, __LINE__, avctx->hw_device_ctx);
        if (avctx->hw_device_ctx) {
            dec_ctx->hwdevice = av_buffer_ref(avctx->hw_device_ctx);
            av_log(avctx, AV_LOG_TRACE, "%s(%d) dec_ctx->hwdevice = %p\n",
                                __FUNCTION__, __LINE__, dec_ctx->hwdevice);
            if (!dec_ctx->hwdevice) {
                ret = AVERROR(ENOMEM);
                goto error;
            }
        } else {
            ret = av_hwdevice_ctx_create(&dec_ctx->hwdevice,
                                         AV_HWDEVICE_TYPE_ES,
                                         "es",
                                         NULL,
                                         0);
            if (ret < 0)
                goto error;
        }

        dec_ctx->hwframe = av_hwframe_ctx_alloc(dec_ctx->hwdevice);
        if (!dec_ctx->hwframe) {
            av_log(avctx, AV_LOG_ERROR, "av_hwframe_ctx_alloc failed\n");
            ret = AVERROR(ENOMEM);
            goto error;
        }

        hw_frames_ctx = (AVHWFramesContext *)dec_ctx->hwframe->data;
        hw_frames_ctx->format = AV_PIX_FMT_ES;
        hw_frames_ctx->sw_format = avctx->sw_pix_fmt;
    }

    if (avctx->pix_fmt == AV_PIX_FMT_ES) {
        dec_ctx->extra_hw_frames = avctx->extra_hw_frames > 1 ? avctx->extra_hw_frames : 1;
    }
    av_log(avctx, AV_LOG_INFO,
             "dec_ctx extra_hw_frames: %d, avctx extra_hw_frames: %d\n",
             dec_ctx->extra_hw_frames,
             avctx->extra_hw_frames);
    return 0;
error:
    av_log(avctx, AV_LOG_ERROR, "av_hwframe_ctx_init failed\n");
    return ret;
}

int ff_es_dec_drop_pkt(AVCodecContext *avctx, AVPacket *avpkt)
{
    VSVDECContext *dec_ctx = avctx->priv_data;

    if (!dec_ctx->pic_decode_number) {
        return 0;
    }

    if (dec_ctx->got_package_number % (dec_ctx->drop_frame_interval + 1) == 0) {
        av_log(avctx, AV_LOG_DEBUG, "drop pkt number: %d\n", dec_ctx->got_package_number);
        av_packet_unref(avpkt);
        return -1;
    } else {
        // frame->pts = dec_ctx->pic_decode_number;
        dec_ctx->pic_decode_number++;
    }
    return 0;
}

void ff_es_jpeg_dec_print_return(AVCodecContext *avctx,enum DecRet jpeg_ret) {
    static enum DecRet prev_retval = 0xFFFFFF;

    switch (jpeg_ret) {
    case DEC_PIC_RDY:
        av_log(avctx, AV_LOG_DEBUG, "JpegDecDecode API returned : DEC_PIC_RDY\n");
        break;
    case DEC_OK:
        av_log(avctx, AV_LOG_DEBUG, "JpegDecDecode API returned : DEC_OK\n");
        break;
    case DEC_ERROR:
        av_log(avctx, AV_LOG_ERROR, "JpegDecDecode API returned : DEC_ERROR\n");
        break;
    case DEC_HW_TIMEOUT:
        av_log(avctx, AV_LOG_ERROR, "JpegDecDecode API returned : JPEGDEC_HW_TIMEOUT\n");
        break;
    case DEC_UNSUPPORTED:
        av_log(avctx, AV_LOG_ERROR, "JpegDecDecode API returned : DEC_UNSUPPORTED\n");
        break;
    case DEC_PARAM_ERROR:
        av_log(avctx, AV_LOG_ERROR, "JpegDecDecode API returned : DEC_PARAM_ERROR\n");
        break;
    case DEC_MEMFAIL:
        av_log(avctx, AV_LOG_ERROR, "JpegDecDecode API returned : DEC_MEMFAIL\n");
        break;
    case DEC_INITFAIL:
        av_log(avctx, AV_LOG_ERROR, "JpegDecDecode API returned : DEC_INITFAIL\n");
        break;
    case DEC_HW_BUS_ERROR:
        av_log(avctx, AV_LOG_ERROR, "JpegDecDecode API returned : DEC_HW_BUS_ERROR\n");
        break;
    case DEC_SYSTEM_ERROR:
        av_log(avctx, AV_LOG_ERROR, "JpegDecDecode API returned : DEC_SYSTEM_ERROR\n");
        break;
    case DEC_DWL_ERROR:
        av_log(avctx, AV_LOG_ERROR, "JpegDecDecode API returned : DEC_DWL_ERROR\n");
        break;
    case DEC_INVALID_STREAM_LENGTH:
        av_log(avctx, AV_LOG_ERROR,
                "JpegDecDecode API returned : DEC_INVALID_STREAM_LENGTH\n");
        break;
    case DEC_STRM_ERROR:
        av_log(avctx, AV_LOG_ERROR, "JpegDecDecode API returned : DEC_STRM_ERROR\n");
        break;
    case DEC_INVALID_INPUT_BUFFER_SIZE:
        av_log(avctx, AV_LOG_ERROR,
                "JpegDecDecode API returned : DEC_INVALID_INPUT_BUFFER_SIZE\n");
        break;
    case DEC_INCREASE_INPUT_BUFFER:
        av_log(avctx, AV_LOG_DEBUG,
                "JpegDecDecode API returned : DEC_INCREASE_INPUT_BUFFER\n");
        break;
    case DEC_SLICE_MODE_UNSUPPORTED:
        av_log(avctx, AV_LOG_ERROR,
                "JpegDecDecode API returned : DEC_SLICE_MODE_UNSUPPORTED\n");
        break;
    case DEC_NO_DECODING_BUFFER:
        if (prev_retval == DEC_NO_DECODING_BUFFER) break;
        av_log(avctx, AV_LOG_ERROR,
                "JpegDecDecode API returned : DEC_NO_DECODING_BUFFER\n");
        break;
    case DEC_WAITING_FOR_BUFFER:
        av_log(avctx, AV_LOG_ERROR,
                "JpegDecDecode API returned : DEC_WAITING_FOR_BUFFER\n");
        break;
    case DEC_FORMAT_NOT_SUPPORTED:
        av_log(avctx, AV_LOG_ERROR,
                "JpegDecDecode API returned : DEC_FORMAT_NOT_SUPPORTED\n");
        break;
    case DEC_STRM_PROCESSED:
        av_log(avctx, AV_LOG_ERROR,
                "JpegDecDecode API returned : DEC_STRM_PROCESSED\n");
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "JpegDecDecode API returned unknown status\n");
        break;
    }
    prev_retval = jpeg_ret;
}

uint32_t ff_es_jpeg_low_latency_task_init(VSVDECContext *dec_ctx) {
    int ret = 0;
    dec_ctx->decode_end_flag = 1;
    dec_ctx->sw_hw_bound = 0;
    dec_ctx->task = NULL;
    dec_ctx->pic_decoded == 0;
    dec_ctx->send_strm_info.strm_bus_addr =
                            dec_ctx->send_strm_info.strm_bus_start_addr =
                            dec_ctx->stream_mem[dec_ctx->stream_mem_index].bus_address;
    dec_ctx->send_strm_info.strm_vir_addr =
                            dec_ctx->send_strm_info.strm_vir_start_addr =
                            (u8 *)dec_ctx->stream_mem[dec_ctx->stream_mem_index].virtual_address;
    sem_init(&dec_ctx->send_sem, 0, 0);
    sem_init(&dec_ctx->frame_sem, 0, 0);
    dec_ctx->task = ff_es_jpeg_run_task(ff_es_jpeg_send_bytestrm_task, dec_ctx);
    dec_ctx->task_existed = 1;

    return ret;
}

task_handle ff_es_jpeg_run_task(task_func func, void* param) {
  int ret;
  pthread_attr_t attr;
  struct sched_param par;
  pthread_t* thread_handle = malloc(sizeof(pthread_t));

  pthread_attr_init(&attr);

#if 0  // maybe it will be uses in haps
  ret = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  assert(ret == 0);
  ret = pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
  par.sched_priority = 60;
  ret = pthread_attr_setschedparam(&attr, &par);
#endif

  ret = pthread_create(thread_handle, &attr, func, param);
  assert(ret == 0);

  if(ret != 0)
  {
    free(thread_handle);
    thread_handle = NULL;
  }

  return thread_handle;
}

void ff_es_jpeg_wait_for_task_completion(task_handle task) {
  int ret = pthread_join(*((pthread_t*)task), NULL);
  assert(ret == 0);
  free(task);
  task = NULL;
}

void* ff_es_jpeg_send_bytestrm_task(void* param) {
    VSVDECContext *dec_ctx = (VSVDECContext *)param;
    dec_ctx->decode_end_flag = 0;

    addr_t start_bus_addr = dec_ctx->send_strm_info.strm_bus_addr;
    uint32_t packet_size = LOW_LATENCY_PACKET_SIZE;
    uint32_t send_len = 0;
    uint32_t bytes = 0;
    dec_ctx->pic_decoded = 0;

    while(!dec_ctx->decode_end_flag) {
        // av_log(NULL, AV_LOG_ERROR, "pkt.size: %d send_len: %d\n", dec_ctx->avpkt.size, send_len);
        if(dec_ctx->avpkt.size - send_len > packet_size && dec_ctx->avpkt.data) {
            bytes = packet_size;
            memcpy((uint8_t*)dec_ctx->send_strm_info.strm_vir_addr,
                    dec_ctx->avpkt.data + send_len,
                    packet_size);
            dec_ctx->send_strm_info.strm_bus_addr += packet_size;
            send_len += packet_size;
            dec_ctx->send_strm_info.last_flag = 0;
            dec_ctx->send_strm_info.strm_vir_addr += packet_size;
        } else if (dec_ctx->avpkt.data){
            bytes = dec_ctx->avpkt.size - send_len;
            memcpy((uint8_t*)dec_ctx->send_strm_info.strm_vir_addr,
                    dec_ctx->avpkt.data + send_len,
                    bytes);
            dec_ctx->send_strm_info.strm_bus_addr += bytes;
            send_len += bytes;
            dec_ctx->send_strm_info.last_flag = 1;
            dec_ctx->send_strm_info.strm_vir_addr += bytes;
        }

        if(dec_ctx->sw_hw_bound == 0) {
            dec_ctx->sw_hw_bound =
                ff_es_jpeg_find_imagedata(dec_ctx->send_strm_info.strm_vir_start_addr, send_len);
            if(dec_ctx->sw_hw_bound || dec_ctx->send_strm_info.last_flag)
            {
                sem_post(&dec_ctx->send_sem);
            }
            else
                continue;
        }
        JpegDecUpdateStrmInfoCtrl(dec_ctx->dec_inst,
                                  dec_ctx->send_strm_info.last_flag,
                                  dec_ctx->send_strm_info.strm_bus_addr);
#ifdef ENABLE_FPGA_VERIFICATION
        if (send_strm_len > 10000000)
        usleep(100);
        else
        usleep(1000);
#endif
        if(dec_ctx->pic_decoded == 1) {
            dec_ctx->pic_decoded = 0;
            dec_ctx->send_strm_info.last_flag = 0;
            send_len = 0;
            sem_wait(&dec_ctx->frame_sem);
            start_bus_addr = dec_ctx->send_strm_info.strm_bus_addr;
            dec_ctx->sw_hw_bound = 0;
        }
    }

    return NULL;
}

u32 ff_es_jpeg_find_imagedata(u8 * p_stream, u32 stream_length)
{
    u32 read_bits;
    u32 marker_byte;
    u32 current_byte;
    u32 header_length;
    /* check pointers & parameters */
    if(p_stream == NULL)
        return 0;
    /* Check the stream lenth */
    if(stream_length < 1)
        return 0;

    read_bits = 0;

    /* Read decoding parameters */
    while(read_bits  < stream_length) {
        /* Look for marker prefix byte from stream */
        marker_byte = p_stream[read_bits];
        if(marker_byte == 0xFF) {
            if(read_bits + 1 >= stream_length)
                return 0;
            current_byte = p_stream[read_bits + 1];
            /* switch to certain header decoding */
            switch (current_byte)
            {
                case 0xC0: //SOF0:
                case 0xC2: //SOF2:
                case 0xDB: //DQT:
                case 0xC4: //DHT:
                case 0xDD: //DRI:
                case 0xE1: //APP1:
                case 0xE2: //APP2:
                case 0xE3: //APP3:
                case 0xE4: //APP4:
                case 0xE5: //APP5:
                case 0xE6: //APP6:
                case 0xE7: //APP7:
                case 0xE8: //APP8:
                case 0xE9: //APP9:
                case 0xEA: //APP10:
                case 0xEB: //APP11:
                case 0xEC: //APP12:
                case 0xED: //APP13:
                case 0xEE: //APP14:
                case 0xEF: //APP15:
                case 0xDC: //DNL:
                case 0xFE: //COM:
            /*
                case 0xC1: //SOF1:
                case 0xC3: //SOF3:
                case 0xC5: //SOF5:
                case 0xC6: //SOF6:
                case 0xC7: //SOF7:
                case 0xC8: //SOF9:
                case 0xCA: //SOF10:
                case 0xCB: //SOF11:
                case 0xCD: //SOF13:
                case 0xCE: //SOF14:
                case 0xCF: //SOF15:
                case 0xCC: //DAC:
                case 0xDE: //DHP:
            */
                case 0xE0: //APP0:
                    if(read_bits + 3 >= stream_length)
                        return 0;
                    header_length = (p_stream[read_bits + 2] << 8) + p_stream[read_bits + 3];
                    if((read_bits + header_length) > stream_length)
                        return 0;
                    if(header_length != 0)
                        read_bits += (header_length + 1);
                    break;

                case 0xDA: //SOS:
                    /* SOS length */
                    if(read_bits + 3 >= stream_length)
                        return 0;
                    header_length = (p_stream[read_bits + 2] << 8) + p_stream[read_bits + 3];
                    if((read_bits + header_length) > stream_length)
                        return 0;
                    /* jump over SOS header */
                    if(header_length != 0)
                        read_bits += (header_length + 1);

                    if(read_bits >= stream_length)
                        return 0;
                    else
                    /* return a big value to control sw */
                        return DEC_X170_MAX_STREAM_VC8000D;
                    break;
                default:
                    read_bits++;
                    break;
            }
        } else {
            read_bits++;
        }
    }
    return 0;
}