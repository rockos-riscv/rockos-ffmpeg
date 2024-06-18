#define LOG_TAG "esvfbuffer"
#include "esvfbuffer.h"
#include <libavutil/log.h>
#include <libavutil/frame.h>
#include "libavutil/pixfmt.h"
#include "libavutil/imgutils.h"

static int esvf_queue_push(ESFifoQueue *queue, void *buffer, int size) {
    int ret;
    if (!queue || !buffer || size <= 0) {
        av_log(NULL, AV_LOG_ERROR, "params error size: %d\n", size);
        return FAILURE;
    }

    ret = esvf_fifo_queue_push(queue, buffer, size);
    return ret;
}

int esvf_get_buffer_unitl_timeout(ESFifoQueue *queue, ESBuffer *buffer, int timeout) {
    int ret;
    if (!queue || !buffer) {
        av_log(NULL, AV_LOG_ERROR, "params error buffer: %p, queue: %p\n", buffer, queue);
        return FAILURE;
    }
    ret = esvf_fifo_queue_pop_until_timeout(queue, buffer, sizeof(ESBuffer), timeout);
    return ret;
}

static int esvf_release_buffer(ESFifoQueue *queue, ESBuffer *buffer) {
    int ret;

    ret = esvf_queue_push(queue, buffer, sizeof(ESBuffer));
    return ret;
}

static void esvf_output_port_free(void *opaque, uint8_t *output_port) {
    ESOutputPort *port = (ESOutputPort *)output_port;
    if (!port) {
        av_log(NULL, AV_LOG_ERROR, "output port is null\n");
        return;
    }

    esvf_fifo_queue_free(&port->frame_queue);
    av_freep(&port->mems);
    av_free(port);

    av_log(NULL, AV_LOG_INFO, "esvf_output_port_free success\n");
}

static void esvf_output_port_memorys_unref(ESOutputPort *port) {
    int mem_num;
    ESMemory *memory;
    if (!port || port->mem_num <= 0) {
        return;
    }
    mem_num = port->mem_num;
    port->mem_num = 0;
    for (int i = 0; i < mem_num; i++) {
        memory = port->mems[i];
        port->mems[i] = NULL;
        if (memory) {
            if (memory->buffer_ref && memory->port_ref) {
                av_log(NULL,
                       AV_LOG_INFO,
                       "i: %d, vir_addr: %p, buffer_ref count: %d, port_ref count: %d\n",
                       i,
                       memory->vir_addr,
                       av_buffer_get_ref_count(memory->buffer_ref) - 1,
                       av_buffer_get_ref_count(memory->port_ref) - 1);
            }

            av_buffer_unref(&memory->port_ref);
            av_buffer_unref(&memory->buffer_ref);
        }
    }
}

void esvf_output_port_unref(ESOutputPort **output_port) {
    ESOutputPort *port;
    if (!output_port || !*output_port) {
        av_log(NULL, AV_LOG_ERROR, "output_port is null %p\n", output_port);
        return;
    }

    port = *output_port;
    esvf_output_port_memorys_unref(port);

    if (port->port_ref) {
        av_log(NULL, AV_LOG_INFO, "port_ref count: %d\n", av_buffer_get_ref_count(port->port_ref) - 1);
        av_buffer_unref(&port->port_ref);
    }
    if (port->mem_info) {
        av_free(port->mem_info);
    }
    *output_port = NULL;
}

static void esvf_output_mempry_free(void *opaque, uint8_t *data) {
    ESMemory *memory = (ESMemory *)data;
    if (!memory) {
        av_log(NULL, AV_LOG_ERROR, "opaque or mem is null mem: %p\n", memory);
        return;
    }

    av_log(NULL, AV_LOG_DEBUG, "esvf_output_mempry_free free mem: %p\n", memory->vir_addr);

#ifdef MODEL_SIMULATION
    av_free(memory->dma_buf);
#else
    es_dma_unmap(memory->dma_buf);
    es_dma_free(memory->dma_buf);
#endif

    av_free(memory);
}

static ESOutputPort *esvf_output_port_create(int mem_num) {
    int ret = FAILURE;
    ESOutputPort *port;
    if (mem_num <= 0) {
        av_log(NULL, AV_LOG_ERROR, "mem_num: %d\n", mem_num);
        return NULL;
    }

    port = av_mallocz(sizeof(*port));
    if (!port) {
        av_log(NULL, AV_LOG_ERROR, "av_mallocz failed\n");
        return NULL;
    }

    port->max_mem_num = mem_num;

    port->port_ref =
        av_buffer_create((uint8_t *)port, sizeof(*port), esvf_output_port_free, NULL, AV_BUFFER_FLAG_READONLY);
    if (!port->port_ref) {
        av_log(NULL, AV_LOG_ERROR, "av_buffer_create failed\n");
        esvf_output_port_free(NULL, (uint8_t *)port);
        return NULL;
    }

    do {
        port->frame_queue = esvf_fifo_queue_create(port->max_mem_num, sizeof(ESBuffer), "frame_queue");
        if (!port->frame_queue) {
            av_log(NULL, AV_LOG_ERROR, "frame_queue create failed\n");
            break;
        }
        port->mems = av_mallocz_array(sizeof(*port->mems), port->max_mem_num);
        if (!port->mems) {
            av_log(NULL, AV_LOG_ERROR, "av_mallocz_array mems failed\n");
            break;
        }
        ret = SUCCESS;
    } while (0);

    if (ret == FAILURE) {
        esvf_output_port_unref(&port);
        av_log(NULL, AV_LOG_ERROR, "output port create failed\n");
    } else {
        av_log(NULL, AV_LOG_INFO, "output port create success\n");
    }

    return port;
}

static ESMemory *esvf_allocate_one_output_memory(int mem_size) {
    int ret = FAILURE;
    AVBufferRef *buffer_ref;
    ESMemory *memory = NULL;
#ifdef MODEL_SIMULATION
    void *dma_buf = NULL;
#else
    es_dma_buf *dma_buf = NULL;
#endif

    memory = av_mallocz(sizeof(*memory));
    do {
        if (memory) {
#ifdef MODEL_SIMULATION
            dma_buf = av_malloc(mem_size);
            if (!dma_buf) {
                av_log(NULL, AV_LOG_ERROR, "allocate mem failed\n");
                av_free(memory);
                memory = NULL;
                break;
            }
            memory->dma_buf = dma_buf;
            memory->vir_addr = dma_buf;
            memory->fd = (uint64_t)memory->vir_addr;
#else
            dma_buf = es_dma_alloc(DMA_TYPE_MMZ_0, mem_size, UNCACHED_BUF, 0);
            if (!dma_buf) {
                av_log(NULL, AV_LOG_ERROR, "allocate dma_buf failed\n");
                av_free(memory);
                memory = NULL;
                break;
            }
            es_dma_map(dma_buf, UNCACHED_BUF);
            memory->dma_buf = dma_buf;
            memory->vir_addr = dma_buf->vir_addr;
            memory->fd = dma_buf->dmabuf_fd;
#endif
            memory->size = mem_size;

            buffer_ref = av_buffer_create(
                (uint8_t *)memory, sizeof(*memory), esvf_output_mempry_free, NULL, AV_BUFFER_FLAG_READONLY);
            if (!buffer_ref) {
                esvf_output_mempry_free(NULL, (uint8_t *)memory);
                memory = NULL;
                av_log(NULL, AV_LOG_ERROR, "av_buffer_create failed\n");
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
        av_log(NULL, AV_LOG_ERROR, "allocate one memory failed\n");
    }

    return memory;
}

static void esvf_output_buffer_init(ESBuffer *buffer, ESOutputPort *port, ESMemory *memory) {
    if (!buffer || !port || !memory) {
        return;
    }

    memset(buffer, 0, sizeof(*buffer));
    buffer->max_size = memory->size;
    buffer->vir_addr = memory->vir_addr;
    buffer->memory = memory;
    memory->buffer = *buffer;
}

int esvf_allocate_one_output_memorys(ESOutputPort *port) {
    int ret = SUCCESS;
    int index = 0;
    ESMemory *memory;
    ESBuffer buffer;
    if (!port) {
        return FAILURE;
    }

    if (port->mem_num >= port->max_mem_num) {
        av_log(NULL,
               AV_LOG_INFO,
               "output port can not allocate any memory anyway, max_mem_num: %d, mem_num: %d\n",
               port->max_mem_num,
               port->mem_num);
        return FAILURE;
    }

    index = port->mem_num;
    memory = esvf_allocate_one_output_memory(port->mem_size);
    if (memory) {
        port->mems[index] = memory;
        memory->port_ref = av_buffer_ref(port->port_ref);
        port->mem_num++;
        esvf_output_buffer_init(&buffer, port, memory);
        esvf_release_buffer(port->frame_queue, &buffer);
    } else {
        ret = FAILURE;
    }

    if (ret == SUCCESS) {
        av_log(NULL, AV_LOG_INFO, "output memorys allocate success\n");
    } else {
        av_log(NULL, AV_LOG_ERROR, "output memorys allocate failed\n");
    }

    return ret;
}

int esvf_allocate_all_output_memorys(ESOutputPort *port) {
    int ret = SUCCESS;
    int index = 0;
    ESMemory *memory;
    ESBuffer buffer;
    if (!port) {
        return FAILURE;
    }

    for (int i = 0; i < port->max_mem_num; i++) {
        index = port->mem_num;
        memory = esvf_allocate_one_output_memory(port->mem_size);
        if (memory) {
            port->mems[index] = memory;
            memory->port_ref = av_buffer_ref(port->port_ref);
            port->mem_num++;
            esvf_output_buffer_init(&buffer, port, memory);
            esvf_release_buffer(port->frame_queue, &buffer);
        } else {
            ret = FAILURE;
            break;
        }
    }

    if (ret == SUCCESS) {
        av_log(NULL, AV_LOG_INFO, "output memorys allocate success\n");
    } else {
        av_log(NULL, AV_LOG_ERROR, "output memorys allocate failed\n");
    }

    return ret;
}

static int esvf_init_meminfo(
    MemInfo *mem_info, enum AVPixelFormat out_fmt, uint32_t pic_width, uint32_t pic_height, uint32_t stride_align) {
    AVPixFmtDescriptor *desc;
    int height = 0;
    int buffer_size = 0;

    if (!mem_info) {
        av_log(NULL, AV_LOG_ERROR, "esvf_init_meminfo invaild paras, mem_info: %p.\n", mem_info);
        return FAILURE;
    }

    desc = av_pix_fmt_desc_get(out_fmt);
    if (!desc) {
        av_log(NULL,
               AV_LOG_ERROR,
               "esvf_init_meminfo get fmt: %s AVPixFmtDescriptor failed.\n",
               av_get_pix_fmt_name(out_fmt));
        return FAILURE;
    }

    av_image_fill_linesizes(mem_info->linesize, out_fmt, FFALIGN(pic_width, stride_align));

    for (int i = 0; i < 4 && mem_info->linesize[i]; i++) {
        height = pic_height;
        if (i == 1 || i == 2) {
            height = AV_CEIL_RSHIFT(height, desc->log2_chroma_h);
        }
        mem_info->offset[i] = buffer_size;
        mem_info->datasize[i] = mem_info->linesize[i] * height;
        buffer_size += mem_info->datasize[i];
    }
    mem_info->w = pic_width;
    mem_info->h = pic_height;
    mem_info->size = buffer_size;

    return SUCCESS;
}

ESOutputPort *esvf_allocate_output_port(
    int mem_num, enum AVPixelFormat out_fmt, uint32_t width, uint32_t height, uint32_t stride_align) {
    int ret = FAILURE;
    ESOutputPort *port = NULL;
    int32_t output_buf_num;
    MemInfo *mem_info;

    output_buf_num = mem_num;

    do {
        if (mem_num <= 0) {
            output_buf_num = DEFAULT_OUTPUT_BUFFERS;
        } else {
            output_buf_num = mem_num > MAX_OUTPUT_BUFFERS ? MAX_OUTPUT_BUFFERS : mem_num;
        }

        port = esvf_output_port_create(output_buf_num);
        if (!port) {
            av_log(NULL, AV_LOG_ERROR, "esvf_output_port_create failed\n");
            return NULL;
        }

        mem_info = (MemInfo *)av_malloc(sizeof(MemInfo));
        if (!mem_info) {
            av_log(NULL, AV_LOG_ERROR, "mem_info_alloc failed \n");
            return FAILURE;
        }
        ret = esvf_init_meminfo(mem_info, out_fmt, width, height, stride_align);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "esvf_init_meminfo faild\n");
            return FAILURE;
        }

        port->mem_info = mem_info;
        port->mem_size = FFALIGN(mem_info->size, stride_align);
        port->mem_num = 0;

        ret = esvf_allocate_all_output_memorys(port);
        if (ret == FAILURE) {
            av_log(NULL, AV_LOG_ERROR, "esvf_allocate_all_output_memorys failed\n");
            ret = FAILURE;
            break;
        }

        ret = SUCCESS;
    } while (0);

    if (ret == FAILURE) {
        av_log(NULL, AV_LOG_INFO, "esvf_allocate_out_port failed\n");
        esvf_output_port_unref(&port);
    } else {
        av_log(NULL, AV_LOG_INFO, "esvf_allocate_out_port success\n");
    }

    return port;
}

static int esvf_release_buffer_to_frame_queue(ESFifoQueue *queue, ESBuffer *buffer) {
    struct AVBufferRef *buffer_ref = NULL;
    struct AVBufferRef *port_ref = NULL;
    int ret;
    if (!queue || !buffer) {
        av_log(NULL, AV_LOG_ERROR, "queue: %p, buffer: %p\n", queue, buffer);
        return FAILURE;
    }

    if (buffer->buffer_ref) {
        buffer_ref = buffer->buffer_ref;
        buffer->buffer_ref = NULL;
    }

    if (buffer->port_ref) {
        port_ref = buffer->port_ref;
        buffer->port_ref = NULL;
    }

    ret = esvf_release_buffer(queue, buffer);
    if (ret == FAILURE) {
        av_log(NULL, AV_LOG_ERROR, "esvf_release_output_buffer failed\n");
    }

    if (buffer_ref) {
        av_buffer_unref(&buffer_ref);
    }
    if (port_ref) {
        av_buffer_unref(&port_ref);
    }

    return ret;
}

void esvf_buffer_consume(void *opaque, uint8_t *data) {
    ESBuffer *buffer;
    ESOutputPort *port = (ESOutputPort *)opaque;
    ESMemory *memory = (ESMemory *)data;

    if (!memory || !port) {
        av_log(NULL, AV_LOG_ERROR, "memory or port is null port: %p\n", port);
        return;
    }

    buffer = &memory->buffer;

    if (buffer) {
        esvf_release_buffer_to_frame_queue(port->frame_queue, buffer);
    } else {
        av_log(NULL, AV_LOG_ERROR, "output buffer: %p is null\n", buffer);
    }
}

static int esvf_get_video_buffer(ESOutputPort *port, ESBuffer *buffer) {
    int ret;

    if (!port) {
        av_log(NULL, AV_LOG_ERROR, "params error port: %p\n", port);
        return FAILURE;
    }

    if (port->mem_num < port->max_mem_num) {
        ret = esvf_get_buffer_unitl_timeout(port->frame_queue, buffer, 0);
        if (ret != SUCCESS) {
            ret = esvf_allocate_one_output_memorys(port);
            if (ret == FAILURE) {
                av_log(NULL, AV_LOG_ERROR, "esvf_allocate_one_output_memorys failed\n");
            }
            ret = esvf_get_buffer_unitl_timeout(port->frame_queue, buffer, -1);
        }
    } else {
        ret = esvf_get_buffer_unitl_timeout(port->frame_queue, buffer, -1);
    }

    return ret;
}