#define LOG_TAG "esdecbuffer"
#include "esdecbuffer.h"
#include "eslog.h"
#include "es_common.h"

#define MAX_OUTPUT_BUFFERS 40

static int esdec_queue_pop(ESFifoQueue *queue, void *buffer, int size) {
    int ret = FAILURE;
    if (!queue || !buffer || size <= 0) {
        log_error(NULL, "params error size: %d\n", size);
        return FAILURE;
    }

    ret = es_fifo_queue_pop(queue, buffer, size);
    if (ret != SUCCESS && queue->abort_request) {
        log_info(NULL, "queue pop return AVERROR_EXIT\n");
        ret = AVERROR_EXIT;
    }

    return ret;
}

static int esdec_queue_push(ESFifoQueue *queue, void *buffer, int size) {
    int ret;
    if (!queue || !buffer || size <= 0) {
        log_error(NULL, "params error size: %d\n", size);
        return FAILURE;
    }

    ret = es_fifo_queue_push(queue, buffer, size);
    return ret;
}

static int esdec_get_output_buffer(ESFifoQueue *queue, void *buffer, int size, int timeout_ms) {
    int ret;
    if (!queue || !buffer || size <= 0) {
        return FAILURE;
    }

    ret = es_fifo_queue_pop_until_timeout(queue, buffer, size, timeout_ms);
    if (ret != SUCCESS && queue->abort_request) {
        ret = AVERROR_EXIT;
    }

    return ret;
}

void esdec_input_buffer_init(InputBuffer *buffer,
                             uint32_t *vir_addr,
                             size_t bus_address,
                             uint32_t logical_size,
                             int max_size) {
    if (!buffer) {
        return;
    }

    memset(buffer, 0, sizeof(*buffer));
    buffer->max_size = max_size;
    buffer->vir_addr = vir_addr;
    buffer->bus_address = bus_address;
    buffer->logical_size = logical_size;
}

int esdec_get_input_packet_buffer(ESFifoQueue *queue, InputBuffer *buffer) {
    int ret = esdec_queue_pop(queue, buffer, sizeof(*buffer));
    return ret;
}

int esdec_push_input_packet_buffer(ESFifoQueue *queue, InputBuffer *buffer) {
    int ret = esdec_queue_push(queue, buffer, sizeof(*buffer));
    return ret;
}

int esdec_get_input_buffer_unitl_timeout(ESFifoQueue *queue, InputBuffer *buffer, int timeout) {
    int ret;
    if (!queue || !buffer) {
        log_error(NULL, "params error buffer: %p, queue: %p\n", buffer, queue);
        return FAILURE;
    }

    ret = es_fifo_queue_pop_until_timeout(queue, buffer, sizeof(InputBuffer), timeout);
    return ret;
}

int esdec_release_input_buffer(ESFifoQueue *queue, InputBuffer *buffer) {
    int ret;
    buffer->pts = 0;
    buffer->size = 0;
    buffer->reordered_opaque = 0;

    ret = esdec_queue_push(queue, buffer, sizeof(InputBuffer));
    return ret;
}

int esdec_release_output_buffer(ESFifoQueue *queue, OutputBuffer *buffer) {
    int ret;
    struct AVBufferRef *buffer_ref = NULL, *port_ref = NULL;
    if (!buffer) {
        return FAILURE;
    }

    buffer->flags = 0;
    buffer->reordered_opaque = 0;
    if (buffer->buffer_ref) {
        buffer_ref = buffer->buffer_ref;
        buffer->buffer_ref = NULL;
    } else {
        buffer->memory = NULL;
    }

    if (buffer->port_ref) {
        port_ref = buffer->port_ref;
        buffer->port_ref = NULL;
    }
    esdec_set_output_buffer_state(buffer->memory, OUTPUT_MEMORY_STATE_CONSUME_QUEUE);
    buffer->memory = NULL;
    ret = esdec_queue_push(queue, buffer, sizeof(OutputBuffer));
    if (buffer_ref) {
        av_buffer_unref(&buffer_ref);
    }
    if (port_ref) {
        av_buffer_unref(&port_ref);
    }

    return ret;
}

int esdec_push_frame_output_buffer(ESFifoQueue *queue, OutputBuffer *buffer) {
    int ret = esdec_queue_push(queue, buffer, sizeof(OutputBuffer));
    return ret;
}

int esdec_get_consumed_output_buffer(ESFifoQueue *queue, OutputBuffer *buffer, int timeout_ms) {
    int ret = esdec_get_output_buffer(queue, buffer, sizeof(*buffer), timeout_ms);
    return ret;
}

int esdec_get_output_frame_buffer(ESFifoQueue *queue, OutputBuffer *buffer, int timeout_ms) {
    int ret = esdec_get_output_buffer(queue, buffer, sizeof(*buffer), timeout_ms);
    if (!ret && buffer->flags == OUTPUT_BUFFERFLAG_EOS) {
        ret = AVERROR_EOF;
    }
    return ret;
}

static void esdec_input_port_free(void *opaque, uint8_t *input_port) {
    ESInputPort *port = (ESInputPort *)input_port;
    if (!port) {
        log_error(NULL, "input port is null\n");
        return;
    }

    log_info(NULL, "enter esdec_input_port_free\n");
    for (int i = 0; i < port->mem_num; i++) {
        av_free(port->input_mems[i]);
    }
    es_fifo_queue_free(&port->packet_queue);
    es_fifo_queue_free(&port->release_queue);
    av_freep(&port->input_mems);
    if (port->dwl_ref) {
        log_info(NULL, "dwl_ref count: %d\n", av_buffer_get_ref_count(port->dwl_ref) - 1);
        av_buffer_unref(&port->dwl_ref);
    }

    av_free(port);

    log_info(NULL, "esdec_input_port_free success\n");
}

static void esdec_output_port_free(void *opaque, uint8_t *output_port) {
    ESOutputPort *port = (ESOutputPort *)output_port;
    if (!port) {
        log_error(NULL, "output port is null\n");
        return;
    }

    es_fifo_queue_free(&port->frame_queue);
    es_fifo_queue_free(&port->consumed_queue);
    av_freep(&port->output_mems);
    if (port->dwl_ref) {
        log_info(NULL, "dwl_ref count: %d\n", av_buffer_get_ref_count(port->dwl_ref) - 1);
        av_buffer_unref(&port->dwl_ref);
    }

    av_free(port);

    log_info(NULL, "esdec_output_port_free success\n");
}

void esdec_input_port_unref(ESInputPort **input_port) {
    int mem_num;
    ESInputMemory *mem;
    ESInputPort *port;
    if (!input_port || !*input_port) {
        log_error(NULL, "error input_port is null %p\n", input_port);
        return;
    }

    port = *input_port;
    mem_num = port->mem_num;
    for (int i = 0; i < mem_num; i++) {
        mem = port->input_mems[i];
        if (mem && mem->buffer_ref) {
            log_info(NULL, "i: %d buffer_ref count: %d\n", i, av_buffer_get_ref_count(mem->buffer_ref) - 1);
            av_buffer_unref(&mem->buffer_ref);
        }
        if (mem && mem->port_ref) {
            log_info(NULL, "i: %d port_ref count: %d\n", i, av_buffer_get_ref_count(mem->port_ref) - 1);
            av_buffer_unref(&mem->port_ref);
        }
    }
    if (port->port_ref) {
        log_info(NULL, "port_ref count: %d\n", av_buffer_get_ref_count(port->port_ref) - 1);
        av_buffer_unref(&port->port_ref);
    }

    *input_port = NULL;
}

void esdec_output_port_memorys_unref(ESOutputPort *port) {
    int mem_num;
    ESOutputMemory *memory;
    if (!port || port->mem_num <= 0) {
        return;
    }
    mem_num = port->mem_num;
    port->mem_num = 0;
    for (int i = 0; i < mem_num; i++) {
        memory = port->output_mems[i];
        port->output_mems[i] = NULL;
        if (memory) {
            if (memory->buffer_ref && memory->port_ref) {
                log_info(NULL,
                         "i: %d, vir_addr: %p, buffer_ref count: %d, port_ref count: %d, state: %s\n",
                         i,
                         memory->vir_addr,
                         av_buffer_get_ref_count(memory->buffer_ref) - 1,
                         av_buffer_get_ref_count(memory->port_ref) - 1,
                         esdec_str_output_state(memory->state));
            }

            av_buffer_unref(&memory->port_ref);
            av_buffer_unref(&memory->buffer_ref);
        }
    }
}

void esdec_output_port_unref(ESOutputPort **output_port) {
    ESOutputPort *port;
    if (!output_port || !*output_port) {
        log_error(NULL, "output_port is null %p\n", output_port);
        return;
    }

    port = *output_port;
    esdec_output_port_memorys_unref(port);

    if (port->port_ref) {
        log_info(NULL, "port_ref count: %d\n", av_buffer_get_ref_count(port->port_ref) - 1);
        av_buffer_unref(&port->port_ref);
    }
    *output_port = NULL;
}

ESInputPort *esdec_input_port_create(int mem_num) {
    int ret = FAILURE;
    ESInputMemory *memory;
    ESInputPort *port;
    if (mem_num <= 0) {
        log_error(NULL, "mem_num: %d\n", mem_num);
        return NULL;
    }

    port = av_mallocz(sizeof(*port));
    if (!port) {
        log_error(NULL, "av_mallocz failed\n");
        return NULL;
    }

    port->port_ref =
        av_buffer_create((uint8_t *)port, sizeof(*port), esdec_input_port_free, NULL, AV_BUFFER_FLAG_READONLY);
    if (!port->port_ref) {
        esdec_input_port_free(NULL, (uint8_t *)port);
        log_error(NULL, "av_buffer_create port_ref failed\n");
        return NULL;
    }

    do {
        port->packet_queue = es_fifo_queue_create(mem_num, sizeof(InputBuffer), "packet_queue");
        if (!port->packet_queue) {
            log_error(NULL, "es_fifo_queue_create packet_queue failed\n");
            break;
        }
        port->release_queue = es_fifo_queue_create(mem_num, sizeof(InputBuffer), "release_queue");
        if (!port->release_queue) {
            log_error(NULL, "es_fifo_queue_create release_queue failed\n");
            break;
        }
        port->input_mems = av_mallocz_array(sizeof(*port->input_mems), mem_num);
        if (!port->input_mems) {
            log_error(NULL, "av_mallocz_array input_mems failed\n");
            break;
        }

        for (int i = 0; i < mem_num; i++) {
            memory = av_mallocz(sizeof(*memory));
            if (memory) {
                memory->port_ref = av_buffer_ref(port->port_ref);
                port->input_mems[i] = memory;
                port->mem_num++;
                if (port->mem_num == mem_num) {
                    log_info(NULL, "input memorys malloc success mem_num: %d\n", mem_num);
                    ret = SUCCESS;
                    break;
                }
            } else {
                log_error(NULL, "av_mallocz failed\n");
                break;
            }
        }
    } while (0);

    if (ret == FAILURE) {
        log_error(NULL, "input_port_create failed port mem_num: %d, mem_num: %d\n", port->mem_num, mem_num);
        esdec_input_port_unref(&port);
    } else {
        log_info(NULL, "esdec_input_port_create success\n");
    }

    return port;
}

int esdec_input_port_enlarge(ESInputPort *port, uint32_t size) {
    int ret = FAILURE;
    int rv = 0;
    ESInputMemory *memory;
    ESInputMemory **input_mems_new;
    uint32_t mem_size_new = 0;
    if (!port) {
        log_error(NULL, "input port is null\n");
        return ret;
    }

    if (size < 0 || size <= port->mem_num) {
        log_error(NULL, "error!!! input port mem_num: %d, enlarge to size: %d\n", port->mem_num, size);
        return ret;
    }

    do {
        rv = es_fifo_queue_enlarge(port->packet_queue, size, sizeof(InputBuffer));
        if (rv < 0) {
            log_error(NULL, "es_fifo_queue_enlarge packet_queue failed\n");
            break;
        }
        rv = es_fifo_queue_enlarge(port->release_queue, size, sizeof(InputBuffer));
        if (rv < 0) {
            log_error(NULL, "es_fifo_queue_enlarge release_queue failed\n");
            break;
        }
        input_mems_new = av_mallocz_array(sizeof(*port->input_mems), size);
        if (!input_mems_new) {
            log_error(NULL, "av_mallocz_array input_mems_new failed\n");
            break;
        }

        for(int i = 0; i < port->mem_num; i++) {
            input_mems_new[i] = port->input_mems[i];
            mem_size_new ++;
        }

        for (int i = port->mem_num; i < size; i++) {
            memory = av_mallocz(sizeof(*memory));
            if (memory) {
                memory->port_ref = av_buffer_ref(port->port_ref);
                input_mems_new[i] = memory;
                mem_size_new++;
                if (mem_size_new == size) {
                    log_info(NULL, "new input memorys malloc success mem_num: %d\n", size);
                    av_freep(&port->input_mems);
                    port->input_mems = input_mems_new;
                    port->mem_num = mem_size_new;
                    ret = SUCCESS;
                    break;
                }
            } else {
                log_error(NULL, "av_mallocz failed\n");
                break;
            }
        }

    } while (0);

    if (ret == FAILURE) {
        log_error(NULL, "input_port_enlarge failed port mem_num: %d, to size: %d\n", port->mem_num, size);
        av_freep(input_mems_new);
    } else {
        log_info(NULL, "input_port_enlarge success\n");
    }

    return ret;
}

ESOutputPort *esdec_output_port_create(int mem_num) {
    int ret = FAILURE;
    ESOutputPort *port;
    if (mem_num <= 0) {
        log_error(NULL, "mem_num: %d\n", mem_num);
        return NULL;
    }

    port = av_mallocz(sizeof(*port));
    if (!port) {
        log_error(NULL, "av_mallocz failed\n");
        return NULL;
    }

    port->max_mem_num = MAX_OUTPUT_BUFFERS;
    log_info(NULL, "output port mem_num: %d, max_mem_num: %d\n", mem_num, port->max_mem_num);
    if (mem_num > port->max_mem_num) {
        mem_num = port->max_mem_num;
    }

    port->port_ref =
        av_buffer_create((uint8_t *)port, sizeof(*port), esdec_output_port_free, NULL, AV_BUFFER_FLAG_READONLY);
    if (!port->port_ref) {
        log_error(NULL, "av_buffer_create failed\n");
        esdec_output_port_free(NULL, (uint8_t *)port);
        return NULL;
    }

    do {
        port->frame_queue = es_fifo_queue_create(MAX_OUTPUT_BUFFERS, sizeof(OutputBuffer), "frame_queue");
        if (!port->frame_queue) {
            log_error(NULL, "frame_queue create failed\n");
            break;
        }
        port->consumed_queue = es_fifo_queue_create(MAX_OUTPUT_BUFFERS, sizeof(OutputBuffer), "consumed_queue");
        if (!port->consumed_queue) {
            log_error(NULL, "consumed_queue create failed\n");
            break;
        }
        port->output_mems = av_mallocz_array(sizeof(*port->output_mems), MAX_OUTPUT_BUFFERS);
        if (!port->output_mems) {
            log_error(NULL, "av_mallocz_array output_mems failed\n");
            break;
        }
        ret = SUCCESS;
    } while (0);

    if (ret == FAILURE) {
        esdec_output_port_unref(&port);
        log_error(NULL, "output port create failed\n");
    } else {
        log_info(NULL, "output port create success\n");
    }

    return port;
}

int esdec_release_buffer_to_consume_queue(ESFifoQueue *queue, OutputBuffer *buffer) {
    int ret;
    if (!queue || !buffer) {
        log_error(NULL, "queue: %p, buffer: %p\n", queue, buffer);
        return FAILURE;
    }

    ret = esdec_release_output_buffer(queue, buffer);
    if (ret == FAILURE) {
        log_error(NULL, "esdec_release_output_buffer\n");
    }

    return ret;
}

void esdec_output_port_clear(ESOutputPort *port, void (*free)(void *opaque, uint8_t *data)) {
    int ret;
    OutputBuffer buffer = {0};
    if (!port || !port->frame_queue || !free) {
        log_error(NULL, "port or frame queue is null %p, free: %p\n", port, free);
        return;
    }

    log_info(NULL, "esdec_output_port_clear start\n");
    for (;;) {
        ret = es_fifo_queue_pop_ignore_abort(port->frame_queue, &buffer, sizeof(buffer));
        if (ret == SUCCESS) {
            if (free && buffer.memory) {
                log_info(NULL, "output frame buffer clear\n");
                free(port, (uint8_t *)(&buffer.memory->pic_pri));
            }
        } else {
            break;
        }
    }
    log_info(NULL, "esdec_output_port_clear end\n");
}

void esdec_clear_input_packets(ESInputPort *port) {
    int ret;
    InputBuffer buffer;
    if (!port || !port->packet_queue || !port->release_queue) {
        log_error(NULL, "inputport: %p is null\n", port);
        return;
    }

    for (;;) {
        ret = esdec_get_input_buffer_unitl_timeout(port->packet_queue, &buffer, 0);
        if (ret == SUCCESS) {
            log_info(NULL, "release input buffer size: %d, vir_addr: %p\n", buffer.size, buffer.vir_addr);
            esdec_release_input_buffer(port->release_queue, &buffer);
        } else {
            if (ret == AVERROR_EXIT) {
                log_info(NULL, "decode will be exit\n");
            }
            break;
        }
    }
    log_info(NULL, "clear input packets success\n");
}

void esdec_clear_output_frames(ESOutputPort *port) {
    int ret;
    OutputBuffer buffer;
    if (!port || !port->frame_queue || !port->consumed_queue) {
        log_error(NULL, "outputport: %p is null\n", port);
        return;
    }

    for (;;) {
        ret = esdec_get_output_frame_buffer(port->frame_queue, &buffer, 0);
        if (ret == SUCCESS) {
            log_info(NULL, "release output buffer vir_addr: %p\n", buffer.vir_addr);
            esdec_release_buffer_to_consume_queue(port->consumed_queue, &buffer);
        } else {
            if (ret == AVERROR_EXIT) {
                log_info(NULL, "decode will be exit\n");
            }
            break;
        }
    }
    log_info(NULL, "clear output frames success\n");
}

void esdec_output_port_stop(ESOutputPort *port) {
    if (!port) {
        return;
    }
    es_fifo_queue_abort(port->frame_queue);
    es_fifo_queue_abort(port->consumed_queue);
}

void esdec_input_port_stop(ESInputPort *port) {
    if (!port) {
        return;
    }
    es_fifo_queue_abort(port->packet_queue);
    es_fifo_queue_abort(port->release_queue);
}

int es_reorder_packet_enqueue(ESQueue *queue, ReorderPkt *pkt) {
    int ret;
    ReorderPkt *new_pkt;
    if (!queue || !pkt) {
        log_error(NULL, "queue or pkt is null queue: %p\n", queue);
        return FAILURE;
    }

    new_pkt = (ReorderPkt *)av_mallocz(sizeof(ReorderPkt));
    if (!new_pkt) {
        log_error(NULL, "malloc ReorderPkt failed\n");
        return FAILURE;
    }

    *new_pkt = *pkt;
    ret = es_queue_push_tail(queue, new_pkt);
    if (ret == SUCCESS) {
        log_debug(NULL, "es_queue_push_tail success pic_id: %d\n", pkt->pic_id);
    } else {
        log_error(NULL, "es_queue_push_tail failed pic_id: %d\n", pkt->pic_id);
    }

    return ret;
}

int es_reorder_packet_dequeue(ESQueue *queue, uint32_t pic_id, ReorderPkt *out_pkt) {
    int found = FALSE;
    int ret = FAILURE;
    List *list;
    ReorderPkt *pkt = NULL;
    if (!out_pkt || !queue) {
        return FAILURE;
    }

    list = queue->head;
    while (list) {
        pkt = (ReorderPkt *)list->data;
        if (pkt->pic_id == pic_id) {
            found = TRUE;
            break;
        }
        list = list->next;
    }
    if (!found) {
        log_error(NULL, "find pic_id: %d failed\n", pic_id);
    } else {
        ret = SUCCESS;
        *out_pkt = *pkt;
        es_queue_delete_data(queue, pkt);
        av_free(pkt);
    }

    return ret;
}

void es_reorder_queue_clear(ESQueue *queue) {
    ReorderPkt *pkt;
    while (pkt = (ReorderPkt *)es_queue_pop_head(queue)) {
        log_info(NULL, "reorder queue pic_id: %d\n", pkt->pic_id);
        av_free(pkt);
    }
}

const char *esdec_str_output_state(OutputMemoryState state) {
    switch (state) {
        case OUTPUT_MEMORY_STATE_INITED:
            return "inited";
        case OUTPUT_MEMORY_STATE_CONSUMED:
            return "consumed";
        case OUTPUT_MEMORY_STATE_CONSUME_QUEUE:
            return "consume_queue";
        case OUTPUT_MEMORY_STATE_FRAME_QUEUE:
            return "frame_queue";
        case OUTPUT_MEMORY_STATE_FFMPEG:
            return "ffmpeg";
        case OUTPUT_MEMORY_STATE_ERROR:
            return "erronr";
        default:
            return "unknown";
    }
}

void esdec_set_output_buffer_state(ESOutputMemory *memory, OutputMemoryState state) {
    OutputMemoryState old_state;
    if (!memory) {
        log_error(NULL, "memory is null\n");
        return;
    }

    old_state = memory->state;
    memory->state = state;
    log_debug(NULL,
              "vir_addr: %p, old_state: %s, memory new_state: %s\n",
              memory->vir_addr,
              esdec_str_output_state(old_state),
              esdec_str_output_state(state));
}

void esdec_print_output_memory_state(ESOutputPort *port) {
    ESOutputMemory *memory;
    if (!port || !port->output_mems || port->mem_num == 0) {
        return;
    }

    for (int i = 0; i < port->mem_num; i++) {
        memory = port->output_mems[i];
        log_info(NULL,
                 "memory: %d, vir_addr: %p, is_added: %d, state: %s\n",
                 i,
                 memory->vir_addr,
                 memory->is_added,
                 esdec_str_output_state(memory->state));
    }
}

void esdec_timed_printf_memory_state(ESOutputPort *port) {
    int64_t now_time, start_time;

    if (!port) {
        return;
    }
    start_time = port->base_time;

    now_time = av_gettime_relative();
    if (now_time - start_time >= 5000000) {
        port->base_time = now_time;
        esdec_print_output_memory_state(port);
    }
}

int esdec_check_output_buffer(ESOutputPort *port, OutputBuffer *buffer) {
    int ret = FAILURE;
    if (!port || !port->output_mems || !buffer) {
        log_error(NULL, "port or buffer is null port: %p, buffer: %p\n", port, buffer);
        return FAILURE;
    }

    for (int i = 0; i < port->mem_num; i++) {
        if (buffer->vir_addr == port->output_mems[i]->vir_addr) {
            ret = SUCCESS;
            break;
        }
    }

    if (ret == FAILURE) {
        log_error(NULL, "outbuffer check failed vir_addr: %p\n", buffer->vir_addr);
    }

    return ret;
}

ESOutputMemory *esdec_find_memory_by_vir_addr(ESOutputPort *port, uint32_t *vir_addr) {
    ESOutputMemory *memory = NULL;
    if (!port || port->mem_num <= 0 || !vir_addr) {
        log_error(NULL, "port: %p, vir_addr: %p\n", port, vir_addr);
        return NULL;
    }

    for (int i = 0; i < port->mem_num; i++) {
        memory = port->output_mems[i];
        if (memory->vir_addr == vir_addr) {
            break;
        }
    }

    if (!memory) {
        log_error(NULL, "find memory failed vir_addr: %p\n", vir_addr);
    }

    return memory;
}
