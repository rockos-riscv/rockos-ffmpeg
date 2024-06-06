#define LOG_TAG "vcdec_wrapper"
#include <dectypes.h>
#include <hevcdecapi.h>
#include <h264decapi.h>
#include <jpegdecapi.h>
#include "es_common.h"
#include "eslog.h"
#include "esdec_wrapper.h"

int ESDecIsSimulation(void) {
    int result;
#ifndef MODEL_SIMULATION
    result = FALSE;
#else
    result = TRUE;
#endif

    log_info(NULL, "is simulation: %d\n", result);
    return result;
}

int ESDecGetDmaBufFd(struct DWLLinearMem *mem) {
    int fd = -1;
    if (!mem) {
        log_error(NULL, "error !!! mem is null\n");
        return -1;
    }
#ifdef SUPPORT_DMA_HEAP
    if (mem->dma_buf) {
        fd = mem->dma_buf->dmabuf_fd;
    } else {
        fd = -1;
        log_error(NULL, "dma_buf is null\n");
    }
#endif
    log_info(NULL, "dmabuf_fd: %d\n", fd);

    return fd;
}

#ifndef MODEL_SIMULATION
static enum DecRet ESDecGetPPXBufferSize(void *inst, u32 pp_index, u32 *buf_size) {
    enum DecRet rv;
    struct ESDecoderWrapper *esdec = (ESDecoderWrapper *)inst;
    if (!esdec || !esdec->inst || !buf_size) {
        log_error(NULL, "inst or buf_size is null inst: %p, buf_size: %p\ns", inst, buf_size);
        return DEC_PARAM_ERROR;
    }

    if (esdec->codec == DEC_HEVC) {
        rv = HevcDecGetPPXBufferSize(esdec->inst, pp_index, buf_size);
    } else if (esdec->codec == DEC_H264_H10P || esdec->codec == DEC_H264) {
        rv = H264DecGetPPXBufferSize(esdec->inst, pp_index, buf_size);
    } else if (esdec->codec == DEC_JPEG) {
        rv = JpegDecGetPPXBufferSize(esdec->inst, pp_index, buf_size);
    } else {
        rv = DEC_PARAM_ERROR;
    }

    log_info(NULL, "rv: %d, pp_index: %d, buf_size: %d\n", rv, pp_index, *buf_size);

    return rv;
}
#endif

// refer to HANTRODEC_IOC_DMA_HEAP_GET_IOVA & HANTRO_IOC_MAGIC
#include <sys/ioctl.h>
#define ES_IOC_MAGIC 'k'
#define ES_IOC_DMA_HEAP_FD_SPLIT _IOR(ES_IOC_MAGIC, 35, struct dmabuf_split *)

struct es_dmabuf_split {
    int dmabuf_fd;       /* dma buf fd to be splitted */
    int slice_fd;        /* splitted dma buf fd */
    unsigned int offset; /* offset of the buffer corresponding to dmabuf_fd */
    unsigned int length; /* size of the splitted buffer start from offset */
};

static int ESDecSplitFd(int dev_mem_fd, int fd, unsigned int offset, unsigned int size) {
    if (!(fd > 0 && size > 0)) {
        return 0;
    }
    if (dev_mem_fd <= 0) {
        log_error(NULL, "dev mem fd is wrong\n");
        return 0;
    }
    struct es_dmabuf_split dmaSplit = {0};
    unsigned int pgsize = getpagesize();
    if (offset % pgsize) {
        return 0;
    }
    dmaSplit.dmabuf_fd = fd;
    dmaSplit.slice_fd = -1;
    dmaSplit.offset = offset;
    dmaSplit.length = size;
    ioctl(dev_mem_fd, ES_IOC_DMA_HEAP_FD_SPLIT, &dmaSplit);
    if (dmaSplit.slice_fd < 0) {
        log_error(NULL, "split dma buf fd failed, slice_fd %d\n", dmaSplit.slice_fd);
        return 0;
    }
    return dmaSplit.slice_fd;
}

int ESDecInitDevMemFd(int *dev_mem_fd) {
    if (*dev_mem_fd <= 0) {
        *dev_mem_fd = open("/dev/es_vdec", O_RDONLY);
        if (*dev_mem_fd < 0) {
            log_error(NULL, "open dev mem fd failed\n");
            return FAILURE;
        }
    }
    return SUCCESS;
}

void ESDecDeinitDevMemFd(int *dev_mem_fd) {
    if (*dev_mem_fd > 0) {
        close(*dev_mem_fd);
        *dev_mem_fd = -1;
    }
}

int ESDecGetDmaFdSplit(void *dwl_inst, void *dec_inst, int dev_mem_fd, int dmabuf_fd, int *split_fds, int fd_array_size) {
    enum DecRet rv;
    int valid_fd_num = 0;
    uint32_t buf_size = 0, offset = 0;

    log_info(NULL, "memFd: %d dmabuf_fd: %d, pp_count: %d\n", dev_mem_fd, dmabuf_fd, fd_array_size);
    if (!split_fds) {
        return FAILURE;
    }

    for (int i = 0; i < fd_array_size; i++) {
        split_fds[i] = -1;
#ifndef MODEL_SIMULATION
        rv = ESDecGetPPXBufferSize(dec_inst, i, &buf_size);
        if (rv == DEC_OK && buf_size > 0) {
            // split_fds[i] = DWLDmaBufFdSplit(dwl_inst, dmabuf_fd, offset, buf_size);
            split_fds[i] = ESDecSplitFd(dev_mem_fd, dmabuf_fd, offset, buf_size);
            offset += buf_size;
            if (split_fds[i] < 0) {
                log_error(NULL, "DWLDmaBufFdSplit failed\n");
            } else {
                valid_fd_num++;
            }
        }
        log_info(NULL,
                 "rv: %d, split_fds[%d]: %d, buf_size: %d, offset: %d, valid_fd_num: %d\n",
                 rv,
                 i,
                 split_fds[i],
                 buf_size,
                 offset,
                 valid_fd_num);
#endif
    }

    return SUCCESS;
}