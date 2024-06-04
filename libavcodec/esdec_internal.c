#define LOG_TAG "esdec_internal"
#include <dectypes.h>
#include <vcdecapi.h>
#include <jpegdecapi.h>
#include "eslog.h"
#include "es_common.h"
#include "esdec_internal.h"
#include "esdecbuffer.h"
#include "esdecapi.h"
#include "esdec_wrapper.h"

static int esdec_output_buffer_fd_split(void *dwl_inst, void *dec_inst, ESOutputMemory *memory, int pp_count);

void esdec_dwl_memory_free(void *opaque, uint8_t *data) {
    struct DWLLinearMem *mem = (struct DWLLinearMem *)data;
    if (!opaque || !mem) {
        log_error(NULL, "opaque or mem is null mem: %p\n", mem);
        return;
    }

    log_info(NULL, "dwl memory free size: %d, vir_addr: %p\n", mem->size, mem->virtual_address);
    DWLFreeLinear(opaque, mem);
}

static void esdec_dwl_output_mempry_free(void *opaque, uint8_t *data) {
    int dma_fd = -1;
    ESOutputMemory *memory = (ESOutputMemory *)data;
    if (!opaque || !memory) {
        log_error(NULL, "opaque or mem is null mem: %p\n", memory);
        return;
    }

    dma_fd = ESDecGetDmaBufFd(&memory->mem);
    log_info(NULL,
             "output memory dma_fd: %d, size: %d, vir_addr: %p\n",
             dma_fd,
             memory->mem.size,
             memory->mem.virtual_address);

    for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if (memory->fd[i] >= 0) {
            if (dma_fd != memory->fd[i]) {
                log_info(NULL, "output buffer close  pp_fd[%d]: %d\n", i, memory->fd[i]);
                close(memory->fd[i]);
            }

            memory->fd[i] = -1;
        }
    }

    DWLFreeLinear(opaque, &memory->mem);
    av_free(memory);
}

void esdec_dwl_release(void *opaque, uint8_t *data) {
    void *dwl_inst = (void *)data;
    (void)opaque;
    if (!dwl_inst) {
        log_error(NULL, "dwl_inst is null\n");
        return;
    }
    log_info(NULL, "DWLRelease start dwl_inst: %p\n", dwl_inst);
    DWLRelease(dwl_inst);

    log_info(NULL, "DWLRelease success\n");
}

void esdec_stream_buffer_consumed(void *stream, void *p_user_data) {
    int found = 0;
    struct DWLLinearMem *mem;
    ESInputPort *port;

    if (!p_user_data) {
        log_error(NULL, "p_user_data is null\n");
        return;
    }

    port = (ESInputPort *)p_user_data;
    for (int i = 0; i < port->mem_num; i++) {
        mem = &port->input_mems[i]->mem;
        if ((uint8_t *)stream >= (uint8_t *)mem->virtual_address
            && (uint8_t *)stream < (uint8_t *)mem->virtual_address + mem->size) {
            found = 1;
            break;
        }
    }

    if (found) {
        InputBuffer buffer;
        buffer.max_size = mem->size;
        buffer.bus_address = mem->bus_address;
        buffer.vir_addr = mem->virtual_address;
        esdec_release_input_buffer(port->release_queue, &buffer);
        log_debug(NULL, "input buffer vir_addr: %p consumed\n", mem->virtual_address);
    } else {
        log_error(NULL, "input buffer vir_addr: %p don't release\n", stream);
    }
}

void esdec_picture_consume(void *opaque, uint8_t *data) {
    OutputBuffer *buffer;
    ESOutputPort *port = (ESOutputPort *)opaque;
    DecPicturePri *pri_pic = (DecPicturePri *)data;

    if (!pri_pic || !port) {
        log_error(NULL, "pri_pic or port is null port: %p\n", port);
        return;
    }

    buffer = (OutputBuffer *)pri_pic->hwpic;
    if (buffer) {
        esdec_release_buffer_to_consume_queue(port->consumed_queue, buffer);
    } else {
        log_error(NULL, "output buffer: %p is null\n", buffer);
    }
}

static int es_decode_alloc_input_memory(void *dwl_inst, ESInputMemory *memory, int size) {
    int ret = SUCCESS;
    struct DWLLinearMem *mem;
    if (!dwl_inst || !memory || size <= 0) {
        log_error(NULL, "dwl_inst: %p or memory: %p is null size: %d\n", dwl_inst, memory, size);
        return FAILURE;
    }

    mem = &memory->mem;
    mem->mem_type = DWL_MEM_TYPE_DMA_HOST_TO_DEVICE | DWL_MEM_TYPE_CPU;
    if (DWLMallocLinear(dwl_inst, size, mem) != DWL_OK) {
        ret = FAILURE;
        log_error(NULL, "DWLMallocLinear failed size: %d\n", size);
    } else {
        memory->buffer_ref =
            av_buffer_create((uint8_t *)mem, sizeof(*mem), esdec_dwl_memory_free, dwl_inst, AV_BUFFER_FLAG_READONLY);
        if (!memory->buffer_ref) {
            ret = FAILURE;
            esdec_dwl_memory_free(dwl_inst, (uint8_t *)mem);
            log_error(NULL, "av_buffer_create failed\n");
        }
    }

    return ret;
}

int es_decode_realloc_input_memory(ESInputPort *port, int size, InputBuffer *buffer) {
    int ret = FAILURE;
    void *dwl_inst;
    ESInputMemory *memory = NULL;
    if (!port || !port->release_queue || !buffer) {
        log_error(NULL, "input port or buffer is null port: %p, buffer: %p\n", port, buffer);
        return FAILURE;
    }

    if (!port->dwl_ref) {
        log_error(NULL, "input port dwl_ref is null\n");
        return FAILURE;
    }
    dwl_inst = port->dwl_ref->data;

    for (int i = 0; port->input_mems; i++) {
        if (port->input_mems[i]->mem.virtual_address == buffer->vir_addr) {
            memory = port->input_mems[i];
            break;
        }
    }

    if (memory) {
        av_buffer_unref(&memory->buffer_ref);
        ret = es_decode_alloc_input_memory(dwl_inst, memory, size);
        if (ret == SUCCESS) {
            log_info(NULL, "realloc input memory success size: %d\n", size);
            esdec_input_buffer_init(buffer,
                                    memory->mem.virtual_address,
                                    memory->mem.bus_address,
                                    memory->mem.logical_size,
                                    memory->mem.size);
        } else {
            log_error(NULL, "realloc input memory failed size: %d\n", size);
        }
    } else {
        log_error(NULL, "find memory failed vir_addr: %p\n", buffer->vir_addr);
    }

    return ret;
}

ESInputPort *esdec_allocate_input_port(ESDecCodec codec,
                                       struct AVBufferRef *dwl_ref,
                                       void *dwl_init,
                                       int32_t input_buf_num) {
    int ret = SUCCESS;
    ESInputPort *port;
    struct DWLLinearMem *mem;
    struct DWLInitParam *jpeg_dwl_init = NULL;
    InputBuffer input_buffer;
    void *dwl_inst;

    if (!dwl_ref || !dwl_ref->data) {
        log_error(NULL, "dwl_ref is null dwl_ref: %p\n", dwl_ref);
        return NULL;
    }

    if (dwl_init) {
        jpeg_dwl_init = (struct DWLInitParam *)dwl_init;
    }
    dwl_inst = dwl_ref->data;

    if (input_buf_num <= 0) {
        if (codec != ES_JPEG) {
            input_buf_num = VCDecMCGetCoreCount() + 1;
        } else {
            if (jpeg_dwl_init) {
#ifdef MODEL_SIMULATION
                input_buf_num = DWLReadAsicCoreCount() + 1;
#else
                input_buf_num = DWLReadAsicCoreCount(jpeg_dwl_init->client_type) + 1;
#endif
            } else {
                input_buf_num = JPEG_DEFAULT_INPUT_MIN_BUFFERS;
            }
        }
    }
    if (input_buf_num < NUM_OF_STREAM_BUFFERS) {
        if (codec != ES_JPEG) {
            input_buf_num = NUM_OF_STREAM_BUFFERS;
        }
    } else if (input_buf_num > MAX_STRM_BUFFERS) {
        input_buf_num = MAX_STRM_BUFFERS;
    }
    log_info(NULL, "input_buf_num: %d\n", input_buf_num);

    port = esdec_input_port_create(input_buf_num);
    if (!port) {
        return NULL;
    }

    for (int i = 0; i < input_buf_num; i++) {
        mem = &port->input_mems[i]->mem;
        ret = es_decode_alloc_input_memory(dwl_inst, port->input_mems[i], ES_DEFAULT_STREAM_BUFFER_SIZE);
        if (ret == FAILURE) {
            ret = FAILURE;
            log_error(NULL, "index: %d alloc memory failed\n", i);
            break;
        } else {
            log_info(NULL,
                     "index: %d alloc memory success size: %d, vir_addr: %p\n",
                     i,
                     ES_DEFAULT_STREAM_BUFFER_SIZE,
                     mem->virtual_address);
        }
        esdec_input_buffer_init(&input_buffer,
                                mem->virtual_address,
                                mem->bus_address,
                                mem->logical_size,
                                mem->size);
        esdec_release_input_buffer(port->release_queue, &input_buffer);
    }
    if (ret == FAILURE) {
        esdec_input_port_unref(&port);
        log_error(NULL, "allocate input port failed\n");
    } else {
        port->dwl_ref = av_buffer_ref(dwl_ref);
        log_info(NULL, "allocate input port success\n");
    }

    return port;
}

int esdec_enlarge_input_port(ESDecCodec codec,
                                   ESInputPort *port,
                                   struct AVBufferRef *dwl_ref,
                                   int32_t buf_num) {
    int ret = SUCCESS;
    int rv = 0;
    struct DWLLinearMem *mem;
    InputBuffer input_buffer;
    void *dwl_inst;
    int mem_num_old = 0;

    if (!dwl_ref || !dwl_ref->data) {
        log_error(NULL, "dwl_ref is null dwl_ref: %p\n", dwl_ref);
        return FAILURE;
    }
    dwl_inst = dwl_ref->data;

    if (!port) {
        log_error(NULL, "input port is null\n");
        return FAILURE;
    }

    if (buf_num <= 0) {
        log_error(NULL, "invaild size: %d\n", buf_num);
        return FAILURE;
    }

    mem_num_old = port->mem_num;
    rv = esdec_input_port_enlarge(port, buf_num);
    if (rv < 0) {
        log_error(NULL, "esdec_input_port_enlarge failed%p\n");
        return FAILURE;
    }

    for (int i = mem_num_old; i < port->mem_num; i++) {
        mem = &port->input_mems[i]->mem;
        ret = es_decode_alloc_input_memory(dwl_inst, port->input_mems[i], ES_DEFAULT_STREAM_BUFFER_SIZE);
        if (ret == FAILURE) {
            log_error(NULL, "index: %d alloc memory failed\n", i);
            break;
        } else {
            log_info(NULL,
                     "index: %d alloc memory success size: %d, virtual_address: %p\n",
                     i,
                     ES_DEFAULT_STREAM_BUFFER_SIZE,
                     mem->virtual_address);
        }
        esdec_input_buffer_init(&input_buffer,
                                mem->virtual_address,
                                mem->bus_address,
                                mem->logical_size,
                                mem->size);
        esdec_release_input_buffer(port->release_queue, &input_buffer);
    }

    if (ret == FAILURE) {
        esdec_input_port_unref(&port);
        log_error(NULL, "enlarge input port failed\n");
    } else {
        log_info(NULL, "enlarge input port success size: %d\n", buf_num);
    }

    return ret;
}

static enum DecRet esdec_add_one_buffer(ESDecCodec codec, ESVDecInst dec_inst, ESOutputMemory *memory) {
    enum DecRet rv;
    struct DWLLinearMem *mem;
    if (!dec_inst || !memory) {
        log_error(NULL, "dec_inst: %p or memory: %p is null\n", dec_inst, memory);
        return DEC_ERROR;
    }

    if (!memory->is_added && memory->state == OUTPUT_MEMORY_STATE_INITED) {
        mem = &memory->mem;
        if (codec != ES_JPEG) {
            rv = VCDecAddBuffer(dec_inst, mem);
        } else {
            rv = JpegDecAddBuffer(dec_inst, mem);
        }
        if (rv != DEC_WAITING_FOR_BUFFER && rv != DEC_OK) {
            log_error(NULL, "VCDecAddBuffer failed\n");
            return rv;
        }
        log_info(NULL, "add buffer rv: %d, vir_addr: %p\n", rv, memory->vir_addr);
        memory->is_added = TRUE;
        esdec_set_output_buffer_state(memory, OUTPUT_MEMORY_STATE_CONSUMED);
    } else {
        log_info(NULL,
                 "memory is_added: %d, vir_addr: %p, state: %s\n",
                 memory->is_added,
                 memory->vir_addr,
                 esdec_str_output_state(memory->state));
        rv = DEC_ERROR;
    }

    return rv;
}

int esdec_add_all_output_memorys(ESDecCodec codec, ESOutputPort *port, ESVDecInst dec_inst) {
    int ret = FAILURE;
    int add_count = 0;
    enum DecRet rv = DEC_WAITING_FOR_BUFFER;
    ESOutputMemory *memory;
    if (!port || !dec_inst) {
        log_error(NULL, "port: %p, dec_inst: %p\n", port, dec_inst);
        return FAILURE;
    }

    esdec_print_output_memory_state(port);

    for (int i = 0; i < port->mem_num; i++) {
        memory = port->output_mems[i];
        if (memory->is_added || memory->state != OUTPUT_MEMORY_STATE_INITED) {
            continue;
        }
        rv = esdec_add_one_buffer(codec, dec_inst, memory);
        if (rv == DEC_WAITING_FOR_BUFFER || rv == DEC_OK) {
            add_count++;
        }
    }

    if (rv == DEC_OK || rv == DEC_WAITING_FOR_BUFFER) {
        ret = SUCCESS;
        log_info(NULL, "add all memorys success add_count: %d, mem_num: %d\n", add_count, port->mem_num);
    } else if (rv == DEC_ERROR) {
        log_error(NULL, "add all output memorys failed\n");
    }

    return ret;
}

static ESOutputMemory *esdec_allocate_one_output_memory(void *dwl_inst, int mem_size) {
    int ret = FAILURE;
    AVBufferRef *buffer_ref;
    ESOutputMemory *memory = NULL;
    struct DWLLinearMem *mem;

    memory = av_mallocz(sizeof(*memory));
    do {
        if (memory) {
            memory->is_added = FALSE;
            mem = &memory->mem;
            mem->mem_type = DWL_MEM_TYPE_DPB;
            if (DWLMallocLinear(dwl_inst, mem_size, mem) != DWL_OK) {
                log_error(NULL, "DWLMallocLinear failed size: %d\n", mem_size);
                break;
            }
            if (mem->virtual_address == NULL) {
                log_error(NULL, "vir_addr is null\n");
                break;
            }

            memory->vir_addr = mem->virtual_address;
            buffer_ref = av_buffer_create(
                (uint8_t *)memory, sizeof(*memory), esdec_dwl_output_mempry_free, dwl_inst, AV_BUFFER_FLAG_READONLY);
            if (!buffer_ref) {
                esdec_dwl_output_mempry_free(dwl_inst, (uint8_t *)memory);
                memory = NULL;
                log_error(NULL, "av_buffer_create failed\n");
                break;
            }
            memory->buffer_ref = buffer_ref;
            ret = SUCCESS;
        }
    } while (0);

    if (ret == FAILURE) {
        if (memory) {
            av_free(memory);
            memory = NULL;
        }
        log_error(NULL, "allocate one memory failed\n");
    }

    return memory;
}

int esdec_allocate_all_output_memorys(ESDecCodec codec, ESOutputPort *port, ESVDecInst dec_inst, void *dwl_inst) {
    int ret = SUCCESS;
    int pp_count;
    ESOutputMemory *memory;
    if (!port || !dec_inst || !dwl_inst) {
        return FAILURE;
    }
    pp_count = port->pp_count;
    log_info(NULL, "port->mem_num: %d, pp_count: %d, mem_size: %d\n", port->mem_num, port->pp_count, port->mem_size);

    for (int i = 0; i < port->mem_num; i++) {
        memory = esdec_allocate_one_output_memory(dwl_inst, port->mem_size);
        if (memory) {
            port->output_mems[i] = memory;
            memory->port_ref = av_buffer_ref(port->port_ref);
            if (codec != ES_JPEG) {
                esdec_output_buffer_fd_split(dwl_inst, dec_inst, memory, pp_count);
            } else {
                struct ESDecoderWrapper esdec;
                esdec.codec = DEC_JPEG;
                esdec.inst = dec_inst;
                esdec_output_buffer_fd_split(dwl_inst, &esdec, memory, pp_count);
            }
            log_info(NULL, "memory: %d allocate success vir_addr: %p\n", i, memory->vir_addr);
        } else {
            ret = FAILURE;
            break;
        }
    }

    if (ret == SUCCESS) {
        log_info(NULL, "output memorys allocate success\n");
    } else {
        log_error(NULL, "output memorys allocate failed\n");
    }

    return ret;
}

static int esdec_allocate_more_output_memorys(ESDecCodec codec,
                                              ESOutputPort *port,
                                              ESVDecInst dec_inst,
                                              void *dwl_inst,
                                              int memory_count) {
    int ret = SUCCESS;
    int pp_count;
    int mem_num;
    ESOutputMemory *memory;

    if (!port || !dec_inst || !dwl_inst || memory_count <= 0) {
        return FAILURE;
    }

    if (port->mem_num + memory_count > port->max_mem_num) {
        log_error(NULL,
                  "error!!! maximum buffer limit exceeded mem_num: %d, memory_count: %d, max_mem_num: %d\n",
                  port->mem_num,
                  memory_count,
                  port->max_mem_num);
        mem_num = port->max_mem_num - port->mem_num;
    } else {
        mem_num = memory_count;
    }
    pp_count = port->pp_count;

    for (int i = 0; i < mem_num; i++) {
        memory = esdec_allocate_one_output_memory(dwl_inst, port->mem_size);
        if (memory) {
            port->output_mems[port->mem_num] = memory;
            port->mem_num++;
            memory->port_ref = av_buffer_ref(port->port_ref);
            if (codec != ES_JPEG) {
                esdec_output_buffer_fd_split(dwl_inst, dec_inst, memory, pp_count);
            } else {
                struct ESDecoderWrapper esdec;
                esdec.codec = DEC_JPEG;
                esdec.inst = dec_inst;
                esdec_output_buffer_fd_split(dwl_inst, &esdec, memory, pp_count);
            }
            log_info(NULL, "memory: %d allocate success vir_addr: %p\n", i, memory->vir_addr);
        } else {
            ret = FAILURE;
            break;
        }
    }

    if (ret == SUCCESS) {
        log_info(NULL, "output memorys allocate success mem_num: %d, memory_count: %d\n", mem_num, memory_count);
    } else {
        log_error(NULL, "output memorys allocate failed mem_num: %d, memory_count: %d\n", mem_num, memory_count);
    }

    return ret;
}

ESOutputPort *esdec_allocate_output_port(ESDecCodec codec,
                                         ESVDecInst dec_inst,
                                         struct AVBufferRef *dwl_ref,
                                         int pp_count) {
    int ret = FAILURE;
    enum DecRet rv;
    struct DecBufferInfo info = {0};
    void *dwl_inst;
    ESOutputPort *port = NULL;
    int32_t output_buf_num;
    if (!dec_inst || !dwl_ref || !dwl_ref->data) {
        log_error(NULL, "dec_inst or dwl_inst is null dec_inst: %p\n", dec_inst);
        return NULL;
    }

    DWLmemset(&info, 0, sizeof(info));
    if (codec != ES_JPEG) {
        rv = VCDecGetBufferInfo(dec_inst, &info);
    } else {
        rv = JpegDecGetBufferInfo(dec_inst, &info);
    }
    if (info.buf_to_free.virtual_address != NULL) {
        log_error(NULL, "need to free buffer rv: %d\n", rv);
    }
    log_info(NULL, "output buf_num: %d, next_buf_size: %d\n", info.buf_num, info.next_buf_size);
    dwl_inst = dwl_ref->data;

    do {
        if (info.next_buf_size != 0) {
            if (codec != ES_JPEG) {
                output_buf_num = info.buf_num;
            } else {
                output_buf_num = JPEG_DEFAULT_OUTPUT_MIN_BUFFERS;
            }

            port = esdec_output_port_create(output_buf_num);
            if (!port) {
                log_error(NULL, "esdec_output_port_create failed\n");
                return NULL;
            }
            port->mem_size = info.next_buf_size;
            port->mem_num = output_buf_num > port->max_mem_num ? port->max_mem_num : output_buf_num;
            port->pp_count = pp_count;
            ret = esdec_allocate_all_output_memorys(codec, port, dec_inst, dwl_inst);
            if (ret == FAILURE) {
                break;
            }

            ret = esdec_add_all_output_memorys(codec, port, dec_inst);
            if (ret == FAILURE) {
                break;
            }
            port->dwl_ref = av_buffer_ref(dwl_ref);
            port->pp_count = pp_count;
        }
    } while (0);

    if (ret == FAILURE) {
        log_info(NULL, "esdec_allocate_out_port failed\n");
        esdec_output_port_unref(&port);
    } else {
        log_info(NULL, "esdec_allocate_out_port success\n");
    }

    return port;
}

int esdec_output_port_change(ESDecCodec codec, ESOutputPort *port, ESVDecInst dec_inst, int pp_count, int new_hdr) {
    int ret;
    int free_buf_count = 0;
    enum DecRet rv;
    void *dwl_inst;
    struct DecBufferInfo info = {0};
    if (!dec_inst || !port || !port->dwl_ref) {
        log_error(NULL, "dec_inst or dwl_inst is null dec_inst: %p\n", dec_inst);
        return FAILURE;
    }

    DWLmemset(&info, 0, sizeof(info));
    do {
        if (codec != ES_JPEG) {
            rv = VCDecGetBufferInfo(dec_inst, &info);
        } else {
            rv = JpegDecGetBufferInfo(dec_inst, &info);
        }
        if (info.buf_to_free.virtual_address != NULL) {
            log_info(NULL, "need to free buffer rv: %d, vir_addr: %p\n", rv, info.buf_to_free.virtual_address);
            free_buf_count++;
        }
    } while (info.buf_to_free.virtual_address);

    log_info(NULL,
             "free_buf_count: %d, port mem_num: %d, pp_count: %d, new pp_count: %d, next_buf_size: %d, buf_num: %d\n",
             free_buf_count,
             port->mem_num,
             port->pp_count,
             pp_count,
             info.next_buf_size,
             info.buf_num);
    if (free_buf_count == 0 && new_hdr) {
        free_buf_count = port->mem_num;
        log_info(NULL, "hevc should be here\n");
    }

    if (free_buf_count > 0 && free_buf_count != port->mem_num) {
        log_error(NULL, "free_buf_count error !!!!\n");
        // TODO
    }
    dwl_inst = port->dwl_ref->data;

    if (info.next_buf_size <= 0) {
        log_error(NULL, "next_buf_size: %d, buf_num: %d\n", info.next_buf_size, info.buf_num);
        return FAILURE;
    } else if (free_buf_count > 0) {
        esdec_output_port_memorys_unref(port);
        port->mem_num = info.buf_num > port->max_mem_num ? port->max_mem_num : info.buf_num;
        port->mem_size = info.next_buf_size;
        port->pp_count = pp_count;

        ret = esdec_allocate_all_output_memorys(codec, port, dec_inst, dwl_inst);
        if (ret == FAILURE) {
            log_error(NULL, "esdec_allocate_all_output_memorys failed\n");
            return ret;
        }
        ret = esdec_add_all_output_memorys(codec, port, dec_inst);
    } else if (free_buf_count == 0) {
        log_info(NULL, "abort should be here\n");
        ret = esdec_add_all_output_memorys_until_timeout(codec, port, dec_inst, 100 /*ms*/);
    } else {
        log_error(NULL, "free_buf_count: %d\n", free_buf_count);
    }

    return ret;
}

int esdec_enlarge_output_port(ESDecCodec codec,
                              ESOutputPort *port,
                              void* dec_inst,
                              struct AVBufferRef *dwl_ref,
                              int buf_num) {
    struct DecBufferInfo info = {0};
    int ret = FAILURE;
    enum DecRet rv;
    void *dwl_inst;

    if (!port || !dec_inst || !dwl_ref || !dwl_ref->data) {
        log_error(NULL, "port dec_inst or dwl_inst is null dec_inst: %p\n", dec_inst);
        return ret;
    }

    dwl_inst = dwl_ref->data;

    DWLmemset(&info, 0, sizeof(info));
    if (codec != ES_JPEG) {
        rv = VCDecGetBufferInfo(dec_inst, &info);
    } else {
        rv = JpegDecGetBufferInfo(dec_inst, &info);
    }

    do {
        if (port->mem_size > 0) {
            ret = esdec_allocate_more_output_memorys(codec,
                                                     port,
                                                     dec_inst,
                                                     dwl_inst,
                                                     buf_num);
            if (ret == FAILURE) {
                break;
            }

            ret = esdec_add_all_output_memorys_until_timeout(codec, port, dec_inst, 100 /*ms*/);
            if (ret == FAILURE) {
                break;
            }
        }
    } while (0);

    if (ret == FAILURE) {
        log_error(NULL, "esdec_enlarge_out_port failed\n");
    } else {
        log_info(NULL, "esdec_allocate_out_port success\n");
    }

    return ret;
}

int esdec_consumed_one_output_buffer(ESDecCodec codec, ESOutputPort *port, ESVDecInst dec_inst, OutputBuffer *buffer) {
    int ret = SUCCESS;
    enum DecRet rv;
    JpegDecOutput jpic;
    if (!port || !dec_inst || !buffer) {
        log_error(NULL, "port: %p or dec_inst: %p or buffer: %p is null\n", port, dec_inst, buffer);
        return FAILURE;
    }

    if (esdec_check_output_buffer(port, buffer) == SUCCESS) {
        ESOutputMemory *memory = esdec_find_memory_by_vir_addr(port, buffer->vir_addr);
        if (!memory) {
            ret = FAILURE;
            log_error(NULL, "memory is null\n");
        } else if (memory->is_added) {
            if (codec != ES_JPEG) {
                rv = VCDecPictureConsumed(dec_inst, &memory->picture);
            } else {
                DWLmemset(&jpic, 0, sizeof(JpegDecOutput));
                for (int i = 0; i < DEC_MAX_OUT_COUNT;i++) {
                    jpic.pictures[i].output_picture_y = memory->picture.pictures[i].luma;
                }
                rv = JpegDecPictureConsumed(dec_inst, &jpic);
            }
            esdec_set_output_buffer_state(memory, OUTPUT_MEMORY_STATE_CONSUMED);
            log_debug(NULL, "VCDecPictureConsumed rv: %d\n", rv);
        } else if (!memory->is_added) {
            log_info(NULL, "esdec_add_one_buffer\n");
            esdec_set_output_buffer_state(memory, OUTPUT_MEMORY_STATE_INITED);
            esdec_add_one_buffer(codec, dec_inst, memory);
            // TODO
        }
    } else {
        ret = FAILURE;
        log_error(NULL, "esdec_check_output_buffer failed vir_addr: %p\n", buffer->vir_addr);
    }

    return ret;
}

int esdec_wait_picture_consumed_until_timeout(ESDecCodec codec,
                                              ESVDecInst dec_inst,
                                              ESOutputPort *port,
                                              int timeout_ms) {
    int ret;
    OutputBuffer buffer;
    if (!port || !dec_inst || timeout_ms < 0) {
        return FAILURE;
    }

    ret = esdec_get_consumed_output_buffer(port->consumed_queue, &buffer, timeout_ms);
    if (ret == SUCCESS) {
        esdec_consumed_one_output_buffer(codec, port, dec_inst, &buffer);
    }

    return ret;
}

int esdec_wait_all_pictures_consumed_unitl_timeout(ESDecCodec codec,
                                                   ESVDecInst dec_inst,
                                                   ESOutputPort *port,
                                                   int timeout_ms) {
    int ret;
    int success_count = 0;
    do {
        ret = esdec_wait_picture_consumed_until_timeout(codec, dec_inst, port, timeout_ms);
        if (ret == SUCCESS) {
            success_count++;
        }
        if (timeout_ms > 0) {
            timeout_ms = 0;
        }

    } while (ret == SUCCESS);

    if (ret != AVERROR_EXIT && success_count > 0) {
        ret = SUCCESS;
    }

    return ret;
}

int esdec_wait_release_picture_add_buffer(ESDecCodec codec, ESVDecInst dec_inst, ESOutputPort *port, int timeout_ms) {
    int ret;
    OutputBuffer buffer;
    if (!port || !dec_inst || timeout_ms < 0) {
        return FAILURE;
    }

    ret = esdec_get_consumed_output_buffer(port->consumed_queue, &buffer, timeout_ms);
    if (ret == SUCCESS) {
        enum DecRet rv;
        ESOutputMemory *memory = esdec_find_memory_by_vir_addr(port, buffer.vir_addr);
        if (!memory) {
            ret = FAILURE;
            return ret;
        }

        esdec_set_output_buffer_state(memory, OUTPUT_MEMORY_STATE_INITED);
        rv = esdec_add_one_buffer(codec, dec_inst, memory);
        if (rv == DEC_OK || rv == DEC_WAITING_FOR_BUFFER) {
            ret = SUCCESS;
        } else {
            ret = FAILURE;
        }
    } else if (ret != AVERROR_EXIT) {
        ret = EAGAIN;
    }

    return ret;
}

ESOutputMemory *esdec_find_memory_by_picture(ESOutputPort *port, struct DecPicturePpu *pic) {
    ESOutputMemory *memory = NULL;
    uint32_t *vir_addr = NULL;
    if (!port || !pic) {
        log_error(NULL, "ctx or out_buffers or picture is null\n");
        return NULL;
    }

    for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if (pic->pictures[i].luma.virtual_address != NULL) {
            vir_addr = pic->pictures[i].luma.virtual_address;
            log_debug(NULL, "index: %d, vir_addr: %p\n", i, pic->pictures[i].luma.virtual_address);
            break;
        }
    }

    memory = esdec_find_memory_by_vir_addr(port, vir_addr);

    if (!memory) {
        log_error(NULL, "find vir_addr failed from picture vir_addr: %p\n", vir_addr);
        return NULL;
    }

    return memory;
}

int esdec_end_stream(ESVDecInst dec_inst) {
    enum DecRet rv;
    if (!dec_inst) {
        log_error(NULL, "dec_inst is null");
        return FAILURE;
    }

    rv = VCDecEndOfStream(dec_inst);
    if (rv == DEC_OK) {
        log_info(NULL, "esdec_end_stream success\n");
        return SUCCESS;
    }

    log_info(NULL, "esdec_end_stream failed rv: %d\n", rv);
    return FAILURE;
}

static int esdec_output_buffer_fd_split(void *dwl_inst, void *dec_inst, ESOutputMemory *memory, int pp_count) {
    int ret = SUCCESS;
    int dma_fd;

    if (!dwl_inst || !dec_inst || !memory) {
        log_error(NULL, "dwl_inst: %p, dec_inst: %p, memory: %p\n", dwl_inst, dec_inst, memory);
        return FAILURE;
    }

    for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        memory->fd[i] = -1;
    }

    if (ESDecIsSimulation()) {
        return SUCCESS;
    }

    dma_fd = ESDecGetDmaBufFd(&memory->mem);
    if (dma_fd < 0) {
        log_error(NULL, "dma fd is error dma_fd: %d\n", dma_fd);
        return FAILURE;
    }

    for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        memory->fd[i] = dma_fd;
    }
    log_debug(NULL, "pp_count: %d, dma_fd: %d\n", pp_count, dma_fd);

    if (pp_count == ES_VID_DEC_MAX_OUT_COUNT) {
        ret = ESDecGetDmaFdSplit(dwl_inst, dec_inst, dma_fd, memory->fd, ES_VID_DEC_MAX_OUT_COUNT);
    }

    return ret;
}

void esdec_reset_output_memorys(ESOutputPort *port) {
    int ret;
    ESOutputMemory *memory;
    OutputBuffer buffer;
    if (!port || !port->consumed_queue) {
        log_error(NULL, "errnor !! port: %p or consumed_queue is null\n", port);
        return;
    }

    for (;;) {
        ret = esdec_get_consumed_output_buffer(port->consumed_queue, &buffer, 0);
        if (ret == SUCCESS) {
            memory = esdec_find_memory_by_vir_addr(port, buffer.vir_addr);
            if (memory) {
                esdec_set_output_buffer_state(memory, OUTPUT_MEMORY_STATE_INITED);
            }
        } else {
            break;
        }
    }

    for (int i = 0; i < port->mem_num; i++) {
        memory = port->output_mems[i];
        if (!memory) {
            log_error(NULL, "memory is null\n");
            continue;
        }
        memory->is_added = FALSE;
        if (memory->state == OUTPUT_MEMORY_STATE_CONSUMED) {
            esdec_set_output_buffer_state(memory, OUTPUT_MEMORY_STATE_INITED);
        }

        log_info(
            NULL, "memory: %d , vir_addr: %p, state: %s\n", i, memory->vir_addr, esdec_str_output_state(memory->state));
    }
}

int esdec_add_all_output_memorys_until_timeout(ESDecCodec codec, ESOutputPort *port, ESVDecInst dec_inst, int timeout) {
    int ret;
    int is_wait = FALSE;
    ESOutputMemory *memory;
    if (!port || port->mem_num <= 0 || !dec_inst) {
        log_error(NULL, "port: %p, dec_inst: %p\n", port, dec_inst);
        return FAILURE;
    }
    esdec_add_all_output_memorys(codec, port, dec_inst);

    for (int i = 0; i < port->mem_num; i++) {
        memory = port->output_mems[i];
        if (!memory->is_added) {
            is_wait = TRUE;
            break;
        }
    }

    if (is_wait) {
        for (;;) {
            ret = esdec_wait_release_picture_add_buffer(codec, dec_inst, port, timeout /*ms*/);
            if (ret == SUCCESS) {
                timeout = 0;
                log_info(NULL, "add buffer ok\n");
            } else {
                break;
            }
        }
    }

    return SUCCESS;
}
