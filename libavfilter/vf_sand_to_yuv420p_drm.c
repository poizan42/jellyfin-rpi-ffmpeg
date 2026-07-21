/*
 * NEON->ISP bridge: SAND (DRM_PRIME) -> YU12 (DRM_PRIME).
 *
 * The Pi4 fixed-function scaler (scale_v4l2m2m / bcm2835 ISP) is DRM_PRIME-in/out
 * only and 8-bit-YUV only, so it cannot ingest the decoder's 10-bit SAND. This
 * filter bridges the gap: CPU-map the decoder's SAND frame, run the (bit-exact)
 * NEON SAND->planar unpack into a pooled CMA dma-buf, and emit an 8-bit linear
 * YU12 (DRM_FORMAT_YUV420) DRM_PRIME frame that scale_v4l2m2m accepts. Pipeline:
 *   -hwaccel drm -hwaccel_output_format drm_prime -i in
 *      -vf sand_to_yuv420p_drm,scale_v4l2m2m=1280:720 -c:v h264_v4l2m2m ...
 * so the resize runs on the ISP (freeing swscale's CPU) and everything stays
 * DRM_PRIME / zero-copy into the encoder.
 *
 * This file is part of FFmpeg.  LGPL 2.1+, as the rest of libavfilter.
 */

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-buf.h>
#include <drm.h>
#include <libdrm/drm_fourcc.h>

#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_drm.h"
#include "libavutil/rpi_sand_fns.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/buffer.h"
#include "libavutil/thread.h"

#include "avfilter.h"
#include "filters.h"
#include "video.h"

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

/* dma-heap uapi (avoid a hard header dependency) */
struct dma_heap_allocation_data { __u64 len; __u32 fd; __u32 fd_flags; __u64 heap_flags; };
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_allocation_data)

#define POOL_N 24   /* >= filtergraph + M2M in-flight depth */

typedef struct PoolBuf {
    int    fd;
    void  *map;
    size_t size;
    int    in_use;
} PoolBuf;

typedef struct BridgeContext {
    const AVClass *class;
    int      heap_fd;
    AVMutex  lock;
    PoolBuf  pool[POOL_N];
} BridgeContext;

/* Per-output-frame descriptor holder; owned by the frame's buf[0]. The dma-buf
 * itself stays in the pool (reused) — only marked free here. */
typedef struct OutBuf {
    AVDRMFrameDescriptor desc;   /* frame->data[0] points here */
    BridgeContext *s;
    int idx;                     /* pool slot */
} OutBuf;

static av_cold int init(AVFilterContext *avctx)
{
    BridgeContext *s = avctx->priv;
    for (int i = 0; i < POOL_N; i++) s->pool[i].fd = -1;
    ff_mutex_init(&s->lock, NULL);
    s->heap_fd = open("/dev/dma_heap/linux,cma", O_RDWR | O_CLOEXEC);
    if (s->heap_fd < 0) {
        av_log(avctx, AV_LOG_ERROR, "Cannot open /dev/dma_heap/linux,cma: %s\n", strerror(errno));
        return AVERROR(errno);
    }
    return 0;
}

static av_cold void uninit(AVFilterContext *avctx)
{
    BridgeContext *s = avctx->priv;
    for (int i = 0; i < POOL_N; i++) {
        if (s->pool[i].map && s->pool[i].map != MAP_FAILED) munmap(s->pool[i].map, s->pool[i].size);
        if (s->pool[i].fd >= 0) close(s->pool[i].fd);
    }
    ff_mutex_destroy(&s->lock);
    if (s->heap_fd >= 0) close(s->heap_fd);
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterLink *inlink = outlink->src->inputs[0];
    outlink->w = inlink->w;
    outlink->h = inlink->h;   /* same size; the ISP scaler downstream does the resize */
    outlink->time_base = inlink->time_base;
    outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;
    return 0;
}

/* Acquire a pooled dma-buf of >= size (allocating a new slot on first need). Returns
 * the pool index and fills *fd/*map, or -1 on failure. */
static int pool_acquire(BridgeContext *s, size_t size, int *fd, void **map)
{
    int idx = -1;
    ff_mutex_lock(&s->lock);
    for (int i = 0; i < POOL_N; i++)
        if (!s->pool[i].in_use && s->pool[i].map && s->pool[i].size >= size) { idx = i; break; }
    if (idx < 0) {
        for (int i = 0; i < POOL_N; i++) if (s->pool[i].map == NULL) {   /* alloc a fresh slot */
            struct dma_heap_allocation_data a = { .len = size, .fd_flags = O_RDWR | O_CLOEXEC };
            if (ioctl(s->heap_fd, DMA_HEAP_IOCTL_ALLOC, &a) < 0) break;
            void *m = mmap(NULL, a.len, PROT_READ | PROT_WRITE, MAP_SHARED, a.fd, 0);
            if (m == MAP_FAILED) { close(a.fd); break; }
            s->pool[i] = (PoolBuf){ .fd = a.fd, .map = m, .size = a.len, .in_use = 0 };
            idx = i; break;
        }
    }
    if (idx >= 0) { s->pool[idx].in_use = 1; *fd = s->pool[idx].fd; *map = s->pool[idx].map; }
    ff_mutex_unlock(&s->lock);
    return idx;
}

static void outbuf_free(void *opaque, uint8_t *data)
{
    OutBuf *b = (OutBuf *)data;
    ff_mutex_lock(&b->s->lock);
    b->s->pool[b->idx].in_use = 0;   /* return to pool; dma-buf kept for reuse */
    ff_mutex_unlock(&b->s->lock);
    av_free(b);
}

static void dmabuf_sync(int fd, uint64_t flags)
{
    struct dma_buf_sync s = { .flags = flags };
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);   /* best-effort */
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *avctx = inlink->dst;
    BridgeContext *s = avctx->priv;
    AVFilterLink *outlink = avctx->outputs[0];
    AVFrame *mapped = NULL, *tmp = NULL, *out = NULL;
    OutBuf *b = NULL;
    int idx = -1, fd = -1;
    void *map = NULL;
    int rv;

    if (!(mapped = av_frame_alloc())) { rv = AVERROR(ENOMEM); goto fail; }
    mapped->format = AV_PIX_FMT_NONE;
    if ((rv = av_hwframe_map(mapped, in, AV_HWFRAME_MAP_READ)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "hwframe_map(READ) failed: %s\n", av_err2str(rv));
        goto fail;
    }
    mapped->crop_top = in->crop_top;   mapped->crop_bottom = in->crop_bottom;
    mapped->crop_left = in->crop_left;  mapped->crop_right = in->crop_right;

    const unsigned w = av_frame_cropped_width(mapped);
    const unsigned h = av_frame_cropped_height(mapped);
    const unsigned bpl = w;   /* YU12 pitch == width (ISP derives width from the Y pitch) */
    const size_t ysz = (size_t)bpl * h, csz = ysz / 4, total = ysz + 2 * csz;

    if ((idx = pool_acquire(s, total, &fd, &map)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "dma-buf pool exhausted/alloc failed\n");
        rv = AVERROR(ENOMEM); goto fail;
    }

    if (!(tmp = av_frame_alloc())) { rv = AVERROR(ENOMEM); goto fail_release; }
    tmp->format = AV_PIX_FMT_YUV420P; tmp->width = w; tmp->height = h;
    tmp->data[0] = (uint8_t *)map;             tmp->linesize[0] = bpl;
    tmp->data[1] = (uint8_t *)map + ysz;       tmp->linesize[1] = bpl / 2;
    tmp->data[2] = (uint8_t *)map + ysz + csz; tmp->linesize[2] = bpl / 2;

    dmabuf_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    if (av_rpi_sand_to_planar_frame(tmp, mapped) != 0) {
        av_log(avctx, AV_LOG_ERROR, "sand->planar failed (fmt %d)\n", mapped->format);
        rv = AVERROR(EINVAL); goto fail_release;
    }
    dmabuf_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);

    if (!(b = av_mallocz(sizeof(*b)))) { rv = AVERROR(ENOMEM); goto fail_release; }
    b->s = s; b->idx = idx;
    b->desc.nb_objects = 1;
    b->desc.objects[0].fd = fd;
    b->desc.objects[0].size = s->pool[idx].size;
    b->desc.objects[0].format_modifier = DRM_FORMAT_MOD_LINEAR;
    b->desc.nb_layers = 1;
    b->desc.layers[0].format = DRM_FORMAT_YUV420;
    b->desc.layers[0].nb_planes = 3;
    b->desc.layers[0].planes[0].object_index = 0;
    b->desc.layers[0].planes[0].offset = 0;
    b->desc.layers[0].planes[0].pitch = bpl;
    b->desc.layers[0].planes[1].object_index = 0;
    b->desc.layers[0].planes[1].offset = ysz;   /* must == pitch*height */
    b->desc.layers[0].planes[1].pitch = bpl / 2;
    b->desc.layers[0].planes[2].object_index = 0;
    b->desc.layers[0].planes[2].offset = ysz + csz;
    b->desc.layers[0].planes[2].pitch = bpl / 2;

    if (!(out = av_frame_alloc())) { rv = AVERROR(ENOMEM); goto fail_release; }
    out->buf[0] = av_buffer_create((uint8_t *)b, sizeof(*b), outbuf_free, NULL, 0);
    if (!out->buf[0]) { rv = AVERROR(ENOMEM); goto fail_release; }
    b = NULL;   /* ownership -> out->buf[0] */
    out->data[0] = (uint8_t *)&((OutBuf *)out->buf[0]->data)->desc;
    out->format = AV_PIX_FMT_DRM_PRIME;
    out->width = w; out->height = h;
    av_frame_copy_props(out, in);
    out->crop_top = out->crop_left = out->crop_bottom = out->crop_right = 0;
    if (in->hw_frames_ctx)
        out->hw_frames_ctx = av_buffer_ref(in->hw_frames_ctx);

    av_frame_free(&mapped);
    av_frame_free(&tmp);
    av_frame_free(&in);
    return ff_filter_frame(outlink, out);

fail_release:
    ff_mutex_lock(&s->lock);
    if (idx >= 0) s->pool[idx].in_use = 0;
    ff_mutex_unlock(&s->lock);
fail:
    av_free(b);
    av_frame_free(&mapped);
    av_frame_free(&tmp);
    av_frame_free(&out);
    av_frame_free(&in);
    return rv;
}

static const AVOption sand_to_yuv420p_drm_options[] = { { NULL } };
AVFILTER_DEFINE_CLASS(sand_to_yuv420p_drm);

static const AVFilterPad inputs[] = {
    { .name = "default", .type = AVMEDIA_TYPE_VIDEO, .filter_frame = filter_frame },
};
static const AVFilterPad outputs[] = {
    { .name = "default", .type = AVMEDIA_TYPE_VIDEO, .config_props = config_output },
};

FFFilter ff_vf_sand_to_yuv420p_drm = {
    .p.name        = "sand_to_yuv420p_drm",
    .p.description = NULL_IF_CONFIG_SMALL("Unpack SAND (DRM_PRIME) to YU12 (DRM_PRIME) via NEON, for the ISP scaler"),
    .p.priv_class  = &sand_to_yuv420p_drm_class,
    .priv_size     = sizeof(BridgeContext),
    .init          = init,
    .uninit        = uninit,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_DRM_PRIME),
};
