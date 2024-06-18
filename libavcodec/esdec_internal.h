#ifndef AVCODEC_ESDEC_INTERNAL_H__
#define AVCODEC_ESDEC_INTERNAL_H__
#include "esdec_common.h"
#include "esdecbuffer.h"

void esdec_dwl_memory_free(void *opaque, uint8_t *data);
void esdec_picture_consume(void *opaque, uint8_t *data);
void esdec_dwl_release(void *opaque, uint8_t *data);
void esdec_stream_buffer_consumed(void *stream, void *p_user_data);
int esdec_end_stream(ESVDecInst dec_inst);

int es_decode_realloc_input_memory(ESInputPort *port, int size, InputBuffer *buffer);
ESInputPort *esdec_allocate_input_port(ESDecCodec codec,
                                       struct AVBufferRef *dwl_ref,
                                       void *dwl_init,
                                       int32_t input_buf_num);
ESOutputPort *esdec_allocate_output_port(
    ESDecCodec codec, ESVDecInst dec_inst, int dev_mem_fd, struct AVBufferRef *dwl_ref, int pp_count);
int esdec_enlarge_input_port(ESDecCodec codec, ESInputPort *port, struct AVBufferRef *dwl_ref, int32_t buf_num);
int esdec_enlarge_output_port(
    ESDecCodec codec, ESOutputPort *port, void *dec_inst, int dev_mem_fd, struct AVBufferRef *dwl_ref, int buf_num);

int esdec_wait_picture_consumed_until_timeout(ESDecCodec codec,
                                              ESVDecInst dec_inst,
                                              ESOutputPort *port,
                                              int timeout_ms);
int esdec_wait_all_pictures_consumed_unitl_timeout(ESDecCodec codec,
                                                   ESVDecInst dec_inst,
                                                   ESOutputPort *port,
                                                   int timeout_ms);
ESOutputMemory *esdec_find_memory_by_picture(ESOutputPort *port, struct DecPicturePpu *pic);
int esdec_allocate_all_output_memorys(
    ESDecCodec codec, ESOutputPort *port, ESVDecInst dec_inst, int dev_mem_fd, void *dwl_inst);
void esdec_reset_output_memorys(ESOutputPort *port);
int esdec_consumed_one_output_buffer(ESDecCodec codec, ESOutputPort *port, ESVDecInst dec_inst, OutputBuffer *buffer);
int esdec_wait_release_picture_add_buffer(ESDecCodec codec, ESVDecInst dec_inst, ESOutputPort *port, int timeout_ms);
int esdec_output_port_change(
    ESDecCodec codec, ESOutputPort *port, ESVDecInst dec_inst, int dev_mem_fd, int pp_count, int new_hdr);
int esdec_add_all_output_memorys(ESDecCodec codec, ESOutputPort *port, ESVDecInst dec_inst);
int esdec_add_all_output_memorys_until_timeout(ESDecCodec codec, ESOutputPort *port, ESVDecInst dec_inst, int timeout);
#endif