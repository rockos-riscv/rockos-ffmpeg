#ifndef AVCODEC_ES_VF_BUFFER_H__
#define AVCODEC_ES_VF_BUFFER_H__

// #include <dectypes.h>
#include "esvfqueue.h"
#include "esvfcommon.h"

typedef struct ESMemory ESMemory;

typedef struct _MemInfo
{
    int w;
    int h;
    int linesize[4]; // output frame linesize
    int offset[4]; // output frame yuv data's offset
    int datasize[4]; // size of output frame yuv data'plane
    int size; // total size of output frame
} MemInfo;


typedef struct ESBuffer {
    int flags;
    struct AVBufferRef *buffer_ref;
    struct AVBufferRef *port_ref;
    uint32_t *vir_addr;
    void *mem;
    uint32_t max_size;
    ESMemory *memory;
} ESBuffer;

struct ESMemory {
    uint64_t fd;
    void *vir_addr;
#ifdef MODEL_SIMULATION
    void *dma_buf;
#else
    es_dma_buf *dma_buf;
#endif
    uint32_t size;
    ESBuffer buffer;
    struct AVBufferRef *buffer_ref;
    struct AVBufferRef *port_ref;
};

typedef struct ESOutputPort {
    int mem_num;
    int max_mem_num;
    int mem_size;
    ESMemory **mems;
    MemInfo *mem_info;
    ESFifoQueue *frame_queue;
    struct AVBufferRef *port_ref;
} ESOutputPort;

void esvf_output_port_unref(ESOutputPort **output_port);

ESOutputPort *esvf_allocate_output_port(int mem_num,
                                        enum AVPixelFormat out_fmt,
                                        uint32_t width,
                                        uint32_t height,
                                        uint32_t stride_align);

int esvf_allocate_one_output_memorys(ESOutputPort *port);

int esvf_allocate_all_output_memorys(ESOutputPort *port);

void esvf_buffer_consume(void *opaque, uint8_t *data);

int esvf_get_buffer_unitl_timeout(ESFifoQueue *queue, ESBuffer *buffer, int timeout);

#endif