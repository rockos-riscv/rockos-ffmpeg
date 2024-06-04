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

int ESDecGetDmaFdSplit(void *dwl_inst, void *dec_inst, int dmabuf_fd, int *split_fds, int fd_array_size) {
    enum DecRet rv;
    int valid_fd_num = 0;
    uint32_t buf_size = 0, offset = 0;

    log_info(NULL, "dmabuf_fd: %d, pp_count: %d\n", dmabuf_fd, fd_array_size);
    if (!split_fds) {
        return FAILURE;
    }

    for (int i = 0; i < fd_array_size; i++) {
        split_fds[i] = -1;
#ifndef MODEL_SIMULATION
        rv = ESDecGetPPXBufferSize(dec_inst, i, &buf_size);
        if (rv == DEC_OK && buf_size > 0) {
            // split_fds[i] = DWLDmaBufFdSplit(dwl_inst, dmabuf_fd, offset, buf_size);
            offset += buf_size;
            if (split_fds[i] < 0) {
                log_error(NULL, "DWLDmaBufFdSplit failed\n");
            }
            else{
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