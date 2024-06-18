#ifndef AVCODEC_ES_DEC_BUFFER_H__
#define AVCODEC_ES_DEC_BUFFER_H__

#include <dectypes.h>
#include "esqueue.h"
#include "es_common.h"
#include "es_codec_private.h"

#define OUTPUT_BUFFERFLAG_EOS (1)

struct AVPacket;
struct DecPicturePpu;
typedef struct ESOutputMemory ESOutputMemory;

typedef struct InputBuffer {
    int32_t size;
    int32_t max_size;
    int64_t pts;
    uint32_t *vir_addr;
    size_t bus_address;
    uint32_t logical_size;
    int64_t reordered_opaque;
} InputBuffer;

typedef struct OutputBuffer {
    int flags;
    int64_t pts;
    int64_t reordered_opaque;
    struct AVBufferRef *buffer_ref;
    struct AVBufferRef *port_ref;
    uint32_t *vir_addr;
    ESOutputMemory *memory;
} OutputBuffer;

typedef struct ReorderPkt {
    uint32_t pic_id;
    int64_t dts;
    int64_t pts;
    int64_t reordered_opaque;
} ReorderPkt;

typedef struct ESInputMemory {
    int fd;
    struct DWLLinearMem mem;
    struct AVBufferRef *buffer_ref;
    struct AVBufferRef *port_ref;
} ESInputMemory;

typedef struct ESInputPort {
    int mem_num;
    ESInputMemory **input_mems;
    ESFifoQueue *packet_queue;
    ESFifoQueue *release_queue;
    struct AVBufferRef *port_ref;
    struct AVBufferRef *dwl_ref;
} ESInputPort;

typedef enum OutputMemoryState
{
    OUTPUT_MEMORY_STATE_INITED = 0,
    OUTPUT_MEMORY_STATE_CONSUMED,
    OUTPUT_MEMORY_STATE_CONSUME_QUEUE,
    OUTPUT_MEMORY_STATE_FRAME_QUEUE,
    OUTPUT_MEMORY_STATE_FFMPEG,
    OUTPUT_MEMORY_STATE_ERROR,
} OutputMemoryState;

struct ESOutputMemory {
    int is_added;
    OutputMemoryState state;
    int fd[ES_VID_DEC_MAX_OUT_COUNT];
    uint32_t *vir_addr;
    OutputBuffer buffer;
    struct DWLLinearMem mem;
    DecPicturePri pic_pri;
    struct DecPicturePpu picture;
    struct AVBufferRef *buffer_ref;
    struct AVBufferRef *port_ref;
};

typedef struct ESOutputPort {
    int mem_num;
    int max_mem_num;
    int mem_size;
    int pp_count;
    ESOutputMemory **output_mems;
    ESFifoQueue *frame_queue;
    ESFifoQueue *consumed_queue;
    struct AVBufferRef *port_ref;
    struct AVBufferRef *dwl_ref;
    int64_t base_time;
} ESOutputPort;

void esdec_input_buffer_init(InputBuffer *buffer,
                             uint32_t *vir_addr,
                             size_t bus_address,
                             uint32_t logical_size,
                             int max_size);
int esdec_get_input_packet_buffer(ESFifoQueue *queue, InputBuffer *buffer);
int esdec_push_input_packet_buffer(ESFifoQueue *queue, InputBuffer *buffer);
int esdec_release_input_buffer(ESFifoQueue *queue, InputBuffer *buffer);
int esdec_get_input_buffer_unitl_timeout(ESFifoQueue *queue, InputBuffer *buffer, int timeout);

int esdec_release_output_buffer(ESFifoQueue *queue, OutputBuffer *buffer);
int esdec_push_frame_output_buffer(ESFifoQueue *queue, OutputBuffer *buffer);
int esdec_get_consumed_output_buffer(ESFifoQueue *queue, OutputBuffer *buffer, int timeout_ms);
int esdec_get_output_frame_buffer(ESFifoQueue *queue, OutputBuffer *buffer, int timeout_ms);
int esdec_release_buffer_to_consume_queue(ESFifoQueue *queue, OutputBuffer *buffer);

ESInputPort *esdec_input_port_create(int mem_num);
ESOutputPort *esdec_output_port_create(int mem_num);
int esdec_input_port_enlarge(ESInputPort *port, uint32_t size);
void esdec_input_port_unref(ESInputPort **input_port);
void esdec_input_port_stop(ESInputPort *port);
void esdec_output_port_memorys_unref(ESOutputPort *port);
void esdec_output_port_unref(ESOutputPort **output_port);
void esdec_output_port_stop(ESOutputPort *port);

void esdec_clear_input_packets(ESInputPort *port);
void esdec_clear_output_frames(ESOutputPort *port);

void esdec_output_port_clear(ESOutputPort *port, void (*free_buffer)(void *opaque, uint8_t *data));

void es_reorder_queue_clear(ESQueue *queue);
int es_reorder_packet_enqueue(ESQueue *queue, ReorderPkt *pkt);
int es_reorder_packet_dequeue(ESQueue *queue, uint32_t pic_id, ReorderPkt *out_pkt);

void esdec_print_output_memory_state(ESOutputPort *port);
void esdec_timed_printf_memory_state(ESOutputPort *port);
int esdec_check_output_buffer(ESOutputPort *port, OutputBuffer *buffer);
const char *esdec_str_output_state(OutputMemoryState state);
void esdec_set_output_buffer_state(ESOutputMemory *memory, OutputMemoryState state);
ESOutputMemory *esdec_find_memory_by_vir_addr(ESOutputPort *port, uint32_t *vir_addr);

#endif