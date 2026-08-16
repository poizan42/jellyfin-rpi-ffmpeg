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
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/dma-buf.h>
#include <drm.h>
#include <libdrm/drm_fourcc.h>
#include <math.h>
#include <inttypes.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_drm.h"
#include "libavutil/dovi_meta.h"
#include "libavutil/frame.h"
#include "libavutil/rpi_sand_fns.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/buffer.h"
#include "libavutil/thread.h"

#include "libavutil/mastering_display_metadata.h"
#include "config_components.h"
#include "avfilter.h"
#include "filters.h"
#include "formats.h"
#include "video.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "rpi_tonemap_tables.h"   /* ff_rpi_tm_luma1d/cb1d/cr1d, ff_rpi_tm_lut3d, RPI_TM_LUT3D_N */
#if CONFIG_ZSCALE_FILTER && CONFIG_TONEMAP_FILTER
#include "buffersrc.h"
#include "buffersink.h"
#endif

enum { TM_NONE = 0, TM_FAST, TM_VERYFAST, TM_ACCURATE, TM_P5, TM_P5_FAST, TM_P5_VERYFAST };
/* TM_VERYFAST: HDR10 -> same as TM_FAST (already real-time); DV P5 -> TM_P5_VERYFAST.
 * TM_P5: Dolby Vision profile 5, full 3D luma+chroma. TM_P5_FAST: 1D-luma (neutral-chroma)
 * approximation + 3D chroma — injected when a P5 frame is transcoded with tm=fast.
 * TM_P5_VERYFAST: 1D-luma + *nearest-neighbour* 3D chroma (one gather, no tetra blend) with
 * ordered (Bayer) coordinate dither — injected for P5 with tm=veryfast; ~+6% vs P5-fast, the
 * dither de-bands the grid-snapped chroma into sub-visible grain (colour-approximate). */

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

/* dma-heap uapi (avoid a hard header dependency) */
struct dma_heap_allocation_data { __u64 len; __u32 fd; __u32 fd_flags; __u64 heap_flags; };
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_allocation_data)

#define POOL_N 24   /* >= filtergraph + M2M in-flight depth */

/* Lever-0 instrumentation: run with SAND_PROF=1 to log per-phase serial cost.
 * Zero-cost when off (one cached getenv). filter_frame is single-threaded. */
static int     prof_on = -1;
static int64_t prof_ns[5];
static unsigned prof_frames;
static const char *const prof_name[5] = { "map", "setup+unpack", "outflush", "wrap", "unmap" };
static inline int64_t prof_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
#define PROF(i) do { if (prof_on) { int64_t _n = prof_now(); prof_ns[i] += _n - _t0; _t0 = _n; } } while (0)

typedef struct PoolBuf {
    int    fd;
    void  *map;
    size_t size;
    int    in_use;
} PoolBuf;

/* The dma-buf pool is refcounted independently of the filter context: an
 * emitted output frame's AVBufferRef can outlive filter uninit (it may still be
 * buffered downstream when the graph is torn down), and its free callback must
 * be able to return the slot without touching freed filter state. Both the
 * filter and every outstanding output buffer hold a reference; the pool and its
 * slots are destroyed only when the last reference drops (sandpool_free). */
typedef struct SandPool {
    AVMutex lock;
    PoolBuf pool[POOL_N];
} SandPool;

/* Lever 2: persistent mmap cache for the decoder's input SAND buffers. Mapping
 * each buffer once (instead of a fresh mmap+munmap of ~16 MB per frame in
 * av_hwframe_map) removes that per-frame churn; only the mandatory
 * cache-invalidate stays. filter_frame is single-threaded, so no lock.
 *
 * Keyed on the dma-buf's INODE, not the fd number: fd numbers are recycled by
 * the kernel once a buffer is closed, so an fd-keyed entry could hand back a
 * mapping of a *different*, already-freed buffer. A dma-buf's inode is stable
 * for the buffer's life however many fds refer to it. On a miss with a full
 * cache we evict the least recently used entry rather than giving up and
 * degrading to the per-frame map/unmap path forever, as the old code did once
 * 32 distinct buffers had been seen.
 *
 * NOTE: this is a correctness/robustness fix, NOT a fix for the 6.18 slowdown.
 * Measured on 6.18.44: identical mmap counts (124 per 60 frames) and identical
 * minor-fault counts (1.95M per 300 frames) before and after, because on that
 * kernel every dequeued frame arrives as a genuinely NEW dma-buf -- the fd
 * numbers cycle through a small set but the underlying objects differ, so no
 * cache keyed on buffer identity can hit. See
 * investigations/dv-6.18-filter-regression.md in the research repo. */
#define MAP_CACHE_N 64
typedef struct MapEnt { ino_t ino; void *addr; size_t size; uint64_t used; } MapEnt;

typedef struct BridgeContext {
    const AVClass *class;
    int      heap_fd;
    AVBufferRef *pool_ref;   /* SandPool; also referenced by each emitted OutBuf */
    AVBufferRef *drm_device; /* lazily created for the software-input fallback path */
    MapEnt   mcache[MAP_CACHE_N];
    int      mcache_n;
    uint64_t mcache_clock;   /* LRU stamp source for mcache[].used */
    unsigned mmap_count;   /* SAND_PROF: distinct input buffers mmap'd (should plateau) */
    int      tm;           /* TM_NONE / TM_FAST / TM_ACCURATE (option) */
    int      out_half;     /* out= option: 0 full, 1 half (emit input/2, drop ISP scale) */
    uint8_t  l64[64], cb64[64], cr64[64];  /* 64-entry tbl LUTs (subsampled at init) */
    uint8_t  l64_next[64];                 /* l64_next[i]=curve((i+1)*16); for the single-pass .S kernel */
    uint8_t  l64_hlg[64], cb64_hlg[64], cr64_hlg[64], l64_next_hlg[64];  /* HLG variants */
    /* Dolby Vision profile 5: per-RPU composed base-YCbCr(full-range) -> SDR 3D LUT */
    uint8_t *p5_lut;                       /* RPI_TM_LUT3D_N^3 * 3, rebuilt when the RPU changes */
    uint64_t p5_hash;                      /* hash of the RPU coeffs the bake consumed */
    int      p5_valid;                     /* p5_lut currently holds a baked LUT */
    int      p5_maxc;                      /* 2^bl_bit_depth - 1 (full-range grid extent) */
    uint8_t  p5_luma1d[1024], p5_l64[64];  /* P5 fast tier: base-Y -> SDR-Y at neutral chroma */
    /* Peak-aware PQ tone-map: LUTs regenerated at runtime for the source's actual peak
     * (via the same zscale+tonemap chain the baked ff_rpi_tm_* come from). PQ only;
     * HLG and P5 are untouched. Falls back to the baked 1000-nit tables when unavailable. */
    int      peak_opt;                     /* option: override source peak nits (0 = auto) */
    int      gen_peak;                     /* nits the g_* tables hold (0 = invalid / use baked) */
    int      warned_nopeak;                /* one-shot "no zscale" fallback warning */
    uint8_t *g_luma1d, *g_cb1d, *g_cr1d, *g_lut3d;      /* runtime PQ 1D+3D tables */
    uint8_t  g_l64[64], g_cb64[64], g_cr64[64], g_l64_next[64];  /* derived 64-entry NEON tables */
} BridgeContext;

/* Return a persistent read-only mapping of (fd,size), or NULL to signal the
 * caller to fall back to av_hwframe_map (cache full / mmap failed). */
static void *map_cached(BridgeContext *s, int fd, size_t size)
{
    struct stat st;
    int slot;

    if (fd < 0 || size == 0)   /* degenerate descriptor (e.g. a frame whose dma-buf never allocated) */
        return NULL;
    if (fstat(fd, &st) != 0)   /* no stable identity -> caller falls back */
        return NULL;

    for (int i = 0; i < s->mcache_n; i++)
        if (s->mcache[i].ino == st.st_ino && s->mcache[i].size == size) {
            s->mcache[i].used = ++s->mcache_clock;
            return s->mcache[i].addr;
        }

    if (s->mcache_n < MAP_CACHE_N) {
        slot = s->mcache_n++;
    } else {
        /* Evict the least recently used mapping. Dropping it is safe: a mapping
         * is independent of the fd it came from, and any frame still using the
         * old address has already finished with it (filter_frame is serial). */
        slot = 0;
        for (int i = 1; i < s->mcache_n; i++)
            if (s->mcache[i].used < s->mcache[slot].used)
                slot = i;
        munmap(s->mcache[slot].addr, s->mcache[slot].size);
    }

    void *m = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) {
        if (slot == s->mcache_n - 1)
            s->mcache_n--;     /* undo the slot we just claimed */
        return NULL;
    }
    s->mcache[slot] = (MapEnt){ st.st_ino, m, size, ++s->mcache_clock };
    s->mmap_count++;
    return m;
}

/* Build a SAND `mapped` view of the input DRM_PRIME frame from the cached
 * mapping (replicates hwcontext_drm.c drm_map_frame's plane + stride rework).
 * Fills sync_fds[] with the object fds to SYNC(END|READ) after use and returns
 * how many, or -1 to use the fallback path. */
static int map_input_cached(BridgeContext *s, const AVFrame *in, AVFrame *mapped,
                            int *sync_fds)
{
    if (in->format != AV_PIX_FMT_DRM_PRIME || !in->hw_frames_ctx)
        return -1;
    const AVDRMFrameDescriptor *desc = (const AVDRMFrameDescriptor *)in->data[0];
    void *base[AV_DRM_MAX_PLANES];

    /* One dma-buf object per plane is as normal as one object holding both: the
     * RPi decoder emits a single object on a 6.1 kernel and TWO (luma, chroma)
     * on 6.18. This used to bail out to the per-frame av_hwframe_map path when
     * nb_objects != 1, which silently cost ~15 ms/frame on 6.18 -- a fresh mmap
     * of the whole ~11 MB frame every frame, so the unpack then re-faulted it
     * page by page. Map every object through the cache instead. */
    if (!desc || desc->nb_objects < 1 || desc->nb_objects > AV_DRM_MAX_PLANES)
        return -1;
    for (int i = 0; i < desc->nb_objects; i++)
        if (!(base[i] = map_cached(s, desc->objects[i].fd, desc->objects[i].size)))
            return -1;

    mapped->format = ((AVHWFramesContext *)in->hw_frames_ctx->data)->sw_format;
    mapped->width  = in->width;
    mapped->height = in->height;
    int plane = 0;
    for (int i = 0; i < desc->nb_layers; i++) {
        const AVDRMLayerDescriptor *layer = &desc->layers[i];
        for (int p = 0; p < layer->nb_planes; p++) {
            int obj = layer->planes[p].object_index;
            if (obj < 0 || obj >= desc->nb_objects)
                return -1;
            mapped->data[plane]     = (uint8_t *)base[obj] + layer->planes[p].offset;
            mapped->linesize[plane] = layer->planes[p].pitch;
            plane++;
        }
    }
    /* A descriptor can carry an object but no layers — e.g. a frame the decoder
     * emitted after its dma-buf allocation failed under CMA pressure. The plane
     * loop then leaves data[] NULL, and the SAND fixup below would still stamp
     * plausible-looking strides onto it, so the unpack would read from NULL.
     * Reject it here and let the caller fail the frame cleanly. */
    if (plane < 2 || !mapped->data[0] || !mapped->data[1])
        return -1;
    if (av_rpi_is_sand_frame(mapped)) {
        int mod_stride = fourcc_mod_broadcom_param(desc->objects[0].format_modifier);
        if (mod_stride == 0) {
            mapped->linesize[3] = mapped->linesize[0];
            mapped->linesize[4] = mapped->linesize[1];
        } else {
            mapped->linesize[3] = mod_stride;
            mapped->linesize[4] = mod_stride;
        }
        mapped->linesize[0] = 128;
        mapped->linesize[1] = 128;
    }
    for (int i = 0; i < desc->nb_objects; i++)
        sync_fds[i] = desc->objects[i].fd;
    return desc->nb_objects;
}

/* Per-output-frame descriptor holder; owned by the frame's buf[0]. The dma-buf
 * itself stays in the pool (reused) — only marked free here. */
typedef struct OutBuf {
    AVDRMFrameDescriptor desc;   /* frame->data[0] points here */
    AVBufferRef *pool_ref;       /* ref to the SandPool backing this frame's slot */
    int idx;                     /* pool slot */
} OutBuf;

static void sandpool_free(void *opaque, uint8_t *data)
{
    SandPool *sp = (SandPool *)data;
    for (int i = 0; i < POOL_N; i++) {
        if (sp->pool[i].map && sp->pool[i].map != MAP_FAILED) munmap(sp->pool[i].map, sp->pool[i].size);
        if (sp->pool[i].fd >= 0) close(sp->pool[i].fd);
    }
    ff_mutex_destroy(&sp->lock);
    av_free(sp);
}

static av_cold int init(AVFilterContext *avctx)
{
    BridgeContext *s = avctx->priv;
    SandPool *sp = av_mallocz(sizeof(*sp));
    if (!sp)
        return AVERROR(ENOMEM);
    for (int i = 0; i < POOL_N; i++) sp->pool[i].fd = -1;
    ff_mutex_init(&sp->lock, NULL);
    s->pool_ref = av_buffer_create((uint8_t *)sp, sizeof(*sp), sandpool_free, NULL, 0);
    if (!s->pool_ref) {
        ff_mutex_destroy(&sp->lock);
        av_free(sp);
        return AVERROR(ENOMEM);
    }
    for (int i = 0; i < 64; i++) {   /* subsample the 1024-entry curves at code = i*16 */
        s->l64[i]      = ff_rpi_tm_luma1d[i * 16];
        s->l64_next[i] = ff_rpi_tm_luma1d[FFMIN((i + 1) * 16, 1023)];
        s->cb64[i] = ff_rpi_tm_cb1d[i * 16];
        s->cr64[i] = ff_rpi_tm_cr1d[i * 16];
        s->l64_hlg[i]      = ff_rpi_tm_luma1d_hlg[i * 16];
        s->l64_next_hlg[i] = ff_rpi_tm_luma1d_hlg[FFMIN((i + 1) * 16, 1023)];
        s->cb64_hlg[i] = ff_rpi_tm_cb1d_hlg[i * 16];
        s->cr64_hlg[i] = ff_rpi_tm_cr1d_hlg[i * 16];
    }
    /* The CMA dma-heap node name is not stable across kernels: it is
     * "linux,cma" when the region comes from that device-tree node (RPi 6.1),
     * but "default_cma_region" when it comes from the `cma=` cmdline / default
     * region (RPi 6.18 -- CONFIG_DMABUF_HEAPS_CMA_LEGACY only re-adds the old
     * name for a DT-named region). Try the known names in order. */
    {
        /* RPI_SAND_HEAP overrides the search, for A/B-ing heaps: their memory
         * attributes (cached vs write-combine) differ and that is worth being
         * able to measure without a rebuild. */
        const char *forced = getenv("RPI_SAND_HEAP");
        static const char *const heap_names[] = {
            "/dev/dma_heap/linux,cma",
            "/dev/dma_heap/default_cma_region",
            "/dev/dma_heap/reserved",
        };
        unsigned int i;

        s->heap_fd = -1;
        if (forced && *forced) {
            s->heap_fd = open(forced, O_RDWR | O_CLOEXEC);
            av_log(avctx, s->heap_fd >= 0 ? AV_LOG_VERBOSE : AV_LOG_WARNING,
                   "RPI_SAND_HEAP=%s: %s\n", forced,
                   s->heap_fd >= 0 ? "using it" : strerror(errno));
        }
        for (i = 0; s->heap_fd < 0 && i < FF_ARRAY_ELEMS(heap_names); i++) {
            s->heap_fd = open(heap_names[i], O_RDWR | O_CLOEXEC);
            if (s->heap_fd >= 0) {
                av_log(avctx, AV_LOG_VERBOSE, "Using CMA dma-heap %s\n", heap_names[i]);
                break;
            }
        }
        if (s->heap_fd < 0) {
            av_log(avctx, AV_LOG_ERROR,
                   "Cannot open a CMA dma-heap (tried linux,cma / "
                   "default_cma_region / reserved): %s\n", strerror(errno));
            return AVERROR(errno);
        }
    }
    return 0;
}

static av_cold void uninit(AVFilterContext *avctx)
{
    BridgeContext *s = avctx->priv;
    for (int i = 0; i < s->mcache_n; i++)
        munmap(s->mcache[i].addr, s->mcache[i].size);
    /* Drop the filter's pool reference; sandpool_free frees the slots + lock
     * only once the last still-outstanding output frame is also released. */
    av_buffer_unref(&s->pool_ref);
    av_buffer_unref(&s->drm_device);
    if (s->heap_fd >= 0) close(s->heap_fd);
    av_freep(&s->p5_lut);
    av_freep(&s->g_luma1d);
    av_freep(&s->g_cb1d);
    av_freep(&s->g_cr1d);
    av_freep(&s->g_lut3d);
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterLink *inlink = outlink->src->inputs[0];
    BridgeContext *s = outlink->src->priv;
    /* out=half: emit input/2 directly (fused 2x2 downscale in the apply), dropping the
     * downstream ISP scale. Only for exact 2:1 (w,h multiple of 4 so luma 2x2 + chroma
     * 4x4 tile cleanly); otherwise fall back to full and let the ISP resize. */
    const int half = s->out_half && (inlink->w % 4 == 0) && (inlink->h % 4 == 0);
    if (s->out_half && !half)
        av_log(outlink->src, AV_LOG_WARNING,
               "out=half ignored: input %dx%d not a multiple of 4; emitting full size\n",
               inlink->w, inlink->h);
    outlink->w = half ? inlink->w / 2 : inlink->w;
    outlink->h = half ? inlink->h / 2 : inlink->h;   /* full: ISP scaler downstream resizes */
    outlink->time_base = inlink->time_base;
    outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;

    /* Our output is 8-bit planar YU12 (DRM_PRIME).  Advertise a frames context
     * that says so: the input's context is RPI4_10 (10-bit SAND), and reusing
     * it would mislabel the output — a consumer that trusts sw_format (e.g.
     * hwdownload) would then SAND-unpack the already-planar 8-bit data and read
     * far past the buffer.  Build a fresh DRM/YUV420P context on the same device. */
    FilterLink *inl  = ff_filter_link(inlink);
    FilterLink *outl = ff_filter_link(outlink);
    av_buffer_unref(&outl->hw_frames_ctx);

    /* The DRM device for the output frames context: normally taken from the
     * input's hw frames context (DRM_PRIME SAND). On the software-input
     * fallback path (a mid-stream switch to a format rpivid can't decode, e.g.
     * 4:2:2/4:4:4/12-bit, auto-converted to yuv420p upstream) there is no input
     * hw frames context, so create our own DRM device once and reuse it. */
    AVBufferRef *device_ref = NULL;
    if (inl->hw_frames_ctx) {
        device_ref = ((AVHWFramesContext *)inl->hw_frames_ctx->data)->device_ref;
    } else {
        if (!s->drm_device) {
            int ret = av_hwdevice_ctx_create(&s->drm_device, AV_HWDEVICE_TYPE_DRM,
                                             NULL, NULL, 0);
            if (ret < 0) {
                av_log(outlink->src, AV_LOG_ERROR,
                       "software-input path: cannot create DRM device: %s\n",
                       av_err2str(ret));
                return ret;
            }
        }
        device_ref = s->drm_device;
    }

    AVBufferRef *out_ref = av_hwframe_ctx_alloc(device_ref);
    if (!out_ref)
        return AVERROR(ENOMEM);
    AVHWFramesContext *out_fc = (AVHWFramesContext *)out_ref->data;
    out_fc->format    = AV_PIX_FMT_DRM_PRIME;
    out_fc->sw_format = AV_PIX_FMT_YUV420P;
    out_fc->width     = outlink->w;
    out_fc->height    = outlink->h;
    int ret = av_hwframe_ctx_init(out_ref);
    if (ret < 0) {
        av_buffer_unref(&out_ref);
        return ret;
    }
    outl->hw_frames_ctx = out_ref;
    return 0;
}

/* Acquire a pooled dma-buf of >= size (allocating a new slot on first need). Returns
 * the pool index and fills *fd/*map, or -1 on failure. */
static int pool_acquire(BridgeContext *s, size_t size, int *fd, void **map)
{
    SandPool *sp = (SandPool *)s->pool_ref->data;
    int idx = -1;
    ff_mutex_lock(&sp->lock);
    for (int i = 0; i < POOL_N; i++)
        if (!sp->pool[i].in_use && sp->pool[i].map && sp->pool[i].size >= size) { idx = i; break; }
    if (idx < 0) {
        for (int i = 0; i < POOL_N; i++) if (sp->pool[i].map == NULL) {   /* alloc a fresh slot */
            struct dma_heap_allocation_data a = { .len = size, .fd_flags = O_RDWR | O_CLOEXEC };
            if (ioctl(s->heap_fd, DMA_HEAP_IOCTL_ALLOC, &a) < 0) break;
            void *m = mmap(NULL, a.len, PROT_READ | PROT_WRITE, MAP_SHARED, a.fd, 0);
            if (m == MAP_FAILED) { close(a.fd); break; }
            sp->pool[i] = (PoolBuf){ .fd = a.fd, .map = m, .size = a.len, .in_use = 0 };
            idx = i; break;
        }
    }
    if (idx >= 0) { sp->pool[idx].in_use = 1; *fd = sp->pool[idx].fd; *map = sp->pool[idx].map; }
    ff_mutex_unlock(&sp->lock);
    return idx;
}

static void outbuf_free(void *opaque, uint8_t *data)
{
    OutBuf *b = (OutBuf *)data;
    SandPool *sp = (SandPool *)b->pool_ref->data;
    ff_mutex_lock(&sp->lock);
    sp->pool[b->idx].in_use = 0;   /* return to pool; dma-buf kept for reuse */
    ff_mutex_unlock(&sp->lock);
    av_buffer_unref(&b->pool_ref);   /* may trigger sandpool_free if filter already gone */
    av_free(b);
}

static void dmabuf_sync(int fd, uint64_t flags)
{
    struct dma_buf_sync s = { .flags = flags };
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);   /* best-effort */
}

/* Slice-threaded unpack. The SAND->planar detile is per-row independent (SAND is
 * column-tiled, no vertical tiling), so each thread converts a horizontal band.
 * Bands are aligned to 2 luma rows so the 4:2:0 chroma split stays exact. */
typedef struct ThreadData {
    AVFrame       *dst;   /* YU12 into the dma-buf (template; shallow-copied per band) */
    const AVFrame *src;   /* mapped SAND frame (template)                              */
    unsigned       H;     /* cropped luma height                                       */
} ThreadData;

static int unpack_slice(AVFilterContext *avctx, void *arg, int jobnr, int nb_jobs)
{
    const ThreadData *td = arg;
    const unsigned H  = td->H;
    unsigned y0 = ((uint64_t)H *  jobnr      / nb_jobs) & ~1u;
    unsigned y1 = (jobnr == nb_jobs - 1) ? H
                : (((uint64_t)H * (jobnr + 1) / nb_jobs) & ~1u);
    if (y1 <= y0)
        return 0;
    const unsigned bh = y1 - y0;

    /* Shallow views: share the underlying buffers, never unref'd. */
    AVFrame src = *td->src;
    src.crop_top    = td->src->crop_top + y0;
    src.crop_bottom = td->src->height - (td->src->crop_top + y0 + bh);

    AVFrame dst = *td->dst;
    dst.data[0] += (size_t) y0      * dst.linesize[0];
    dst.data[1] += (size_t)(y0 / 2) * dst.linesize[1];
    dst.data[2] += (size_t)(y0 / 2) * dst.linesize[2];

    return av_rpi_sand_to_planar_frame(&dst, &src);
}

static int frame_is_hdr(const AVFrame *in)
{
    return in->color_trc == AVCOL_TRC_SMPTE2084 ||
           in->color_trc == AVCOL_TRC_ARIB_STD_B67;
}

/* Tone-map apply, slice-threaded + cache-tiled: within each band, unpack a small
 * chunk of rows SAND30 -> 10-bit into an L2-resident scratch (no full-frame 10-bit
 * DRAM round-trip), then LUT it -> 8-bit YU12. Luma via the 1D curve (both tiers);
 * chroma: fast = separable 1D LUTs; accurate = chroma-res 3D LUT (trilinear, 2x2
 * block-avg luma). DRAM traffic ~ single-pass. */
#define TM_CHUNK 4   /* rows per tile; keeps the 10-bit scratch L1-resident. Swept 2/4/8/16 at 4K:
                      * 16 thrashes L2 (accurate ~44ms/frame); 4 is the knee (~37.5ms, P5-fast → ~real-time).
                      * Pure tiling granularity — output is bit-identical to any TM_CHUNK. */
typedef struct TMData {
    AVFrame       *dst;   /* 8-bit YU12 (dma-buf) */
    const AVFrame *src;   /* SAND mapped frame    */
    unsigned       H;     /* luma height (input)  */
    int            tm;
    int            half;  /* out=half: 2x2-downscale + apply at input/2, emit 1080p directly */
    const uint8_t *l64, *cb64, *cr64;  /* 64-entry tbl LUTs (fast NEON path) */
    const uint8_t *l64_next;           /* pairs with l64 for the single-pass .S kernel */
    /* Active full-res tables for this frame's transfer (PQ or HLG). */
    const uint8_t *luma1d, *cb1d, *cr1d, *lut3d;
    const uint8_t *p5_lut;             /* DV P5: base-YCbCr -> SDR 3D LUT (TM_P5) */
    int            p5_maxc;            /* P5 full-range grid extent (2^bl_bit_depth - 1) */
    const uint8_t *p5_luma1d, *p5_l64; /* P5 fast tier: 1D neutral-chroma luma curve */
} TMData;

/* Apply a smooth 1024->8 tone curve to n contiguous 10-bit samples using a
 * 64-entry table (idx=code>>4) + linear interp (code&15). Vectorised on aarch64;
 * matches the scalar 1024-entry LUT to <=1 LSB. */
static void lut1d_apply(uint8_t *dst, const uint16_t *src, unsigned n,
                        const uint8_t *t64, const uint8_t *t1024)
{
    unsigned x = 0;
#if defined(__aarch64__)
    const uint8x16x4_t T = vld1q_u8_x4(t64);
    const uint8x8_t one = vdup_n_u8(1), c63 = vdup_n_u8(63);
    for (; x + 8 <= n; x += 8) {
        uint16x8_t v   = vandq_u16(vld1q_u16(src + x), vdupq_n_u16(1023));
        uint8x8_t  idx = vmovn_u16(vshrq_n_u16(v, 4));           /* 0..63 */
        int16x8_t  fr  = vreinterpretq_s16_u16(vandq_u16(v, vdupq_n_u16(15)));
        uint8x8_t  idn = vmin_u8(vadd_u8(idx, one), c63);
        int16x8_t  lo  = vreinterpretq_s16_u16(vmovl_u8(vqtbl4_u8(T,idx)));
        int16x8_t  hi  = vreinterpretq_s16_u16(vmovl_u8(vqtbl4_u8(T,idn)));
        int16x8_t  d   = vmulq_s16(vsubq_s16(hi, lo), fr);
        int16x8_t  o   = vaddq_s16(lo, vshrq_n_s16(vaddq_s16(d, vdupq_n_s16(8)), 4));
        vst1_u8(dst + x, vqmovun_s16(o));
    }
#endif
    for (; x < n; x++)                                          /* scalar tail / fallback */
        dst[x] = t1024[src[x] & 1023];
}

/* Accurate-tier chroma: fixed-point tetrahedral 3D-LUT (Cb,Cr) for one chroma row,
 * x in [x0,cw). Scalar reference — the correctness oracle, the non-NEON fallback, and
 * the NEON tail. avgY4 = 2x2 luma block sum (co-sited with the 4:2:0 chroma sample). */
static void tm3d_chroma_scalar(uint8_t *OU, uint8_t *OV,
                               const uint16_t *Y0, const uint16_t *Y1,
                               const uint16_t *U, const uint16_t *V,
                               const uint8_t *lut3d, unsigned x0, unsigned cw)
{
    const int N = RPI_TM_LUT3D_N;
    const int64_t MY = ((int64_t)(N-1)*64 *65536 + 438) / 876;
    const int64_t MC = ((int64_t)(N-1)*256*65536 + 448) / 896;
    const int LIMQ = (N-1) << 8;
    for (unsigned x = x0; x < cw; x++) {
        int avgY4 = Y0[2*x] + Y0[2*x+1] + Y1[2*x] + Y1[2*x+1];
        int ybq = (int)(((int64_t)(avgY4 - 256) * MY + 32768) >> 16);
        int ubq = (int)(((int64_t)((int)(U[x] & 1023) - 64) * MC + 32768) >> 16);
        int vbq = (int)(((int64_t)((int)(V[x] & 1023) - 64) * MC + 32768) >> 16);
        ybq = ybq < 0 ? 0 : (ybq > LIMQ ? LIMQ : ybq);
        ubq = ubq < 0 ? 0 : (ubq > LIMQ ? LIMQ : ubq);
        vbq = vbq < 0 ? 0 : (vbq > LIMQ ? LIMQ : vbq);
        int yi = ybq>>8, ui = ubq>>8, vi = vbq>>8;
        int fy = ybq&255, fu = ubq&255, fv = vbq&255;
        int yj = yi<N-1?yi+1:yi, uj = ui<N-1?ui+1:ui, vj = vi<N-1?vi+1:vi;
#define C3(iy,iu,iv) (&lut3d[(((iy)*N+(iu))*N+(iv))*3])
        const uint8_t *c0 = C3(yi,ui,vi), *c3 = C3(yj,uj,vj), *a, *b;
        int w0, w1, w2, w3;
        if (fy >= fu) {
            if (fu >= fv)      { a=C3(yj,ui,vi); b=C3(yj,uj,vi); w0=256-fy; w1=fy-fu; w2=fu-fv; w3=fv; }
            else if (fy >= fv) { a=C3(yj,ui,vi); b=C3(yj,ui,vj); w0=256-fy; w1=fy-fv; w2=fv-fu; w3=fu; }
            else               { a=C3(yi,ui,vj); b=C3(yj,ui,vj); w0=256-fv; w1=fv-fy; w2=fy-fu; w3=fu; }
        } else {
            if (fy >= fv)      { a=C3(yi,uj,vi); b=C3(yj,uj,vi); w0=256-fu; w1=fu-fy; w2=fy-fv; w3=fv; }
            else if (fu >= fv) { a=C3(yi,uj,vi); b=C3(yi,uj,vj); w0=256-fu; w1=fu-fv; w2=fv-fy; w3=fy; }
            else               { a=C3(yi,ui,vj); b=C3(yi,uj,vj); w0=256-fv; w1=fv-fu; w2=fu-fy; w3=fy; }
        }
#undef C3
        int iu = (w0*c0[1] + w1*a[1] + w2*b[1] + w3*c3[1] + 128) >> 8;
        int iv = (w0*c0[2] + w1*a[2] + w2*b[2] + w3*c3[2] + 128) >> 8;
        OU[x] = iu<0?0:(iu>255?255:iu); OV[x] = iv<0?0:(iv>255?255:iv);
    }
}

#if defined(__aarch64__)
/* Per-4-lane coordinate map + branchless tetrahedron select. Emits the 4 corner byte
 * offsets into ff_rpi_tm_lut3d and the 4 Q8 weights. int32 math is bit-exact vs the
 * int64 scalar (products < 2^31). av_always_inline so the constants fold. */
static av_always_inline void tm3d_calc4(int32x4_t Y, int32x4_t Uu, int32x4_t Vv,
    int32_t MY, int32_t MC, int32x4_t vYoff, int32x4_t vCoff, int32x4_t v256, int32x4_t v32768, int32x4_t vLIMQ,
    int32x4_t vzero, int32x4_t vNm1, int32x4_t vone, int32x4_t v255, int32x4_t vN, int32x4_t vST,
    int32x4_t *po0, int32x4_t *poa, int32x4_t *pob, int32x4_t *po3,
    int32x4_t *pw0, int32x4_t *pw1, int32x4_t *pw2, int32x4_t *pw3)
{
    /* vYoff/vCoff: axis input offset (limited-range 256/64, or 0 for full-range P5).
     * v256: the Q8 weight base (always 256), independent of the input offset. */
    int32x4_t ybq=vshrq_n_s32(vaddq_s32(vmulq_s32(vsubq_s32(Y ,vYoff),vdupq_n_s32(MY)),v32768),16);
    int32x4_t ubq=vshrq_n_s32(vaddq_s32(vmulq_s32(vsubq_s32(Uu,vCoff),vdupq_n_s32(MC)),v32768),16);
    int32x4_t vbq=vshrq_n_s32(vaddq_s32(vmulq_s32(vsubq_s32(Vv,vCoff),vdupq_n_s32(MC)),v32768),16);
    ybq=vminq_s32(vmaxq_s32(ybq,vzero),vLIMQ);
    ubq=vminq_s32(vmaxq_s32(ubq,vzero),vLIMQ);
    vbq=vminq_s32(vmaxq_s32(vbq,vzero),vLIMQ);
    int32x4_t yi=vshrq_n_s32(ybq,8), ui=vshrq_n_s32(ubq,8), vi=vshrq_n_s32(vbq,8);
    int32x4_t fy=vandq_s32(ybq,v255), fu=vandq_s32(ubq,v255), fv=vandq_s32(vbq,v255);
    int32x4_t incy=vandq_s32(vreinterpretq_s32_u32(vcltq_s32(yi,vNm1)),vone);
    int32x4_t incu=vandq_s32(vreinterpretq_s32_u32(vcltq_s32(ui,vNm1)),vone);
    int32x4_t incv=vandq_s32(vreinterpretq_s32_u32(vcltq_s32(vi,vNm1)),vone);
    int32x4_t base=vmulq_s32(vaddq_s32(vmulq_s32(vaddq_s32(vmulq_s32(yi,vN),ui),vN),vi),vST); /* byte offset */
    int32x4_t dY=vmulq_s32(incy,vdupq_n_s32(RPI_TM_LUT3D_N*RPI_TM_LUT3D_N*3));
    int32x4_t dU=vmulq_s32(incu,vdupq_n_s32(RPI_TM_LUT3D_N*3));
    int32x4_t dV=vmulq_s32(incv,vST);
    int32x4_t allo=vaddq_s32(vaddq_s32(vaddq_s32(base,dY),dU),dV);
    int32x4_t fmax=vmaxq_s32(vmaxq_s32(fy,fu),fv);
    int32x4_t fmin=vminq_s32(vminq_s32(fy,fu),fv);
    int32x4_t fmid=vsubq_s32(vsubq_s32(vaddq_s32(vaddq_s32(fy,fu),fv),fmax),fmin);
    *pw0=vsubq_s32(v256,fmax); *pw1=vsubq_s32(fmax,fmid); *pw2=vsubq_s32(fmid,fmin); *pw3=fmin;
    uint32x4_t c_yu=vcgeq_s32(fy,fu), c_uv=vcgeq_s32(fu,fv), c_yv=vcgeq_s32(fy,fv);
    uint32x4_t ymax=vandq_u32(c_yu,c_yv), umax=vandq_u32(vmvnq_u32(c_yu),c_uv);
    int32x4_t dmax=vbslq_s32(ymax,dY,vbslq_s32(umax,dU,dV));
    uint32x4_t vmn=vandq_u32(c_uv,c_yv), umn=vandq_u32(c_yu,vmvnq_u32(c_uv));
    int32x4_t dmin=vbslq_s32(vmn,dV,vbslq_s32(umn,dU,dY));
    *po0=base; *poa=vaddq_s32(base,dmax); *pob=vsubq_s32(allo,dmin); *po3=allo;
}
#endif

/* NEON port of tm3d_chroma_scalar: 8 chroma samples/iter, bit-identical to the scalar.
 * Two tm3d_calc4() halves, a batched 8-lane software gather (Cb|Cr adjacent -> one u16
 * load per corner), then the Q8 weighted sum in u32. */
static void tm3d_chroma_row(uint8_t *OU, uint8_t *OV,
                            const uint16_t *Y0, const uint16_t *Y1,
                            const uint16_t *U, const uint16_t *V,
                            const uint8_t *lut3d, unsigned cw)
{
    unsigned x = 0;
#if defined(__aarch64__)
    enum { N = RPI_TM_LUT3D_N };
    const int32_t MY = (int32_t)(((int64_t)(N-1)*64 *65536 + 438) / 876);
    const int32_t MC = (int32_t)(((int64_t)(N-1)*256*65536 + 448) / 896);
    const int32x4_t v256=vdupq_n_s32(256), v64=vdupq_n_s32(64), v32768=vdupq_n_s32(32768);
    const int32x4_t vLIMQ=vdupq_n_s32((N-1)<<8), vzero=vdupq_n_s32(0), vNm1=vdupq_n_s32(N-1);
    const int32x4_t vone=vdupq_n_s32(1), v255=vdupq_n_s32(255), vN=vdupq_n_s32(N), vST=vdupq_n_s32(3);
    const uint16x8_t m1023q=vdupq_n_u16(1023), m8q=vdupq_n_u16(0xff);
#define CALC4(Yv,Uv,Vv,o0,oa,ob,o3,w0,w1,w2,w3) \
    tm3d_calc4(Yv,Uv,Vv,MY,MC,v256,v64,v256,v32768,vLIMQ,vzero,vNm1,vone,v255,vN,vST, \
               &o0,&oa,&ob,&o3,&w0,&w1,&w2,&w3)
    for (; x + 8 <= cw; x += 8) {
        uint16x8_t y0a=vld1q_u16(Y0+2*x), y0b=vld1q_u16(Y0+2*x+8);
        uint16x8_t y1a=vld1q_u16(Y1+2*x), y1b=vld1q_u16(Y1+2*x+8);
        uint16x8_t avg=vaddq_u16(vpaddq_u16(y0a,y0b), vpaddq_u16(y1a,y1b));   /* 8 avgY4 */
        uint16x8_t U8=vandq_u16(vld1q_u16(U+x),m1023q), V8=vandq_u16(vld1q_u16(V+x),m1023q);
        int32x4_t Ylo=vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(avg))), Yhi=vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(avg)));
        int32x4_t Ulo=vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(U8))),  Uhi=vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(U8)));
        int32x4_t Vlo=vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(V8))),  Vhi=vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(V8)));
        int32x4_t o0l,oal,obl,o3l,w0l,w1l,w2l,w3l, o0h,oah,obh,o3h,w0h,w1h,w2h,w3h;
        CALC4(Ylo,Ulo,Vlo, o0l,oal,obl,o3l, w0l,w1l,w2l,w3l);
        CALC4(Yhi,Uhi,Vhi, o0h,oah,obh,o3h, w0h,w1h,w2h,w3h);
        int32_t o0[8],oa[8],ob[8],o3[8]; uint16_t g0[8],ga[8],gb[8],g3[8];
        vst1q_s32(o0,o0l); vst1q_s32(o0+4,o0h); vst1q_s32(oa,oal); vst1q_s32(oa+4,oah);
        vst1q_s32(ob,obl); vst1q_s32(ob+4,obh); vst1q_s32(o3,o3l); vst1q_s32(o3+4,o3h);
        for (int i=0;i<8;i++) {   /* software gather: one u16 (Cb|Cr) per corner per lane */
            memcpy(&g0[i], lut3d+o0[i]+1, 2); memcpy(&ga[i], lut3d+oa[i]+1, 2);
            memcpy(&gb[i], lut3d+ob[i]+1, 2); memcpy(&g3[i], lut3d+o3[i]+1, 2);
        }
        uint16x8_t h0=vld1q_u16(g0),ha=vld1q_u16(ga),hb=vld1q_u16(gb),h3=vld1q_u16(g3);
        uint16x8_t cb0=vandq_u16(h0,m8q),cba=vandq_u16(ha,m8q),cbb=vandq_u16(hb,m8q),cb3=vandq_u16(h3,m8q);
        uint16x8_t cr0=vshrq_n_u16(h0,8),cra=vshrq_n_u16(ha,8),crb=vshrq_n_u16(hb,8),cr3=vshrq_n_u16(h3,8);
        uint32x4_t uw0l=vreinterpretq_u32_s32(w0l),uw1l=vreinterpretq_u32_s32(w1l),uw2l=vreinterpretq_u32_s32(w2l),uw3l=vreinterpretq_u32_s32(w3l);
        uint32x4_t uw0h=vreinterpretq_u32_s32(w0h),uw1h=vreinterpretq_u32_s32(w1h),uw2h=vreinterpretq_u32_s32(w2h),uw3h=vreinterpretq_u32_s32(w3h);
        uint32x4_t bl=vmulq_u32(uw0l,vmovl_u16(vget_low_u16(cb0)));
        bl=vmlaq_u32(bl,uw1l,vmovl_u16(vget_low_u16(cba))); bl=vmlaq_u32(bl,uw2l,vmovl_u16(vget_low_u16(cbb))); bl=vmlaq_u32(bl,uw3l,vmovl_u16(vget_low_u16(cb3)));
        uint32x4_t bh=vmulq_u32(uw0h,vmovl_u16(vget_high_u16(cb0)));
        bh=vmlaq_u32(bh,uw1h,vmovl_u16(vget_high_u16(cba))); bh=vmlaq_u32(bh,uw2h,vmovl_u16(vget_high_u16(cbb))); bh=vmlaq_u32(bh,uw3h,vmovl_u16(vget_high_u16(cb3)));
        uint32x4_t rl=vmulq_u32(uw0l,vmovl_u16(vget_low_u16(cr0)));
        rl=vmlaq_u32(rl,uw1l,vmovl_u16(vget_low_u16(cra))); rl=vmlaq_u32(rl,uw2l,vmovl_u16(vget_low_u16(crb))); rl=vmlaq_u32(rl,uw3l,vmovl_u16(vget_low_u16(cr3)));
        uint32x4_t rh=vmulq_u32(uw0h,vmovl_u16(vget_high_u16(cr0)));
        rh=vmlaq_u32(rh,uw1h,vmovl_u16(vget_high_u16(cra))); rh=vmlaq_u32(rh,uw2h,vmovl_u16(vget_high_u16(crb))); rh=vmlaq_u32(rh,uw3h,vmovl_u16(vget_high_u16(cr3)));
        vst1_u8(OU+x, vqmovn_u16(vcombine_u16(vrshrn_n_u32(bl,8),vrshrn_n_u32(bh,8))));  /* (acc+128)>>8 */
        vst1_u8(OV+x, vqmovn_u16(vcombine_u16(vrshrn_n_u32(rl,8),vrshrn_n_u32(rh,8))));
    }
#undef CALC4
#endif
    tm3d_chroma_scalar(OU, OV, Y0, Y1, U, V, lut3d, x, cw);   /* tail / non-NEON fallback */
}

/* ---- Dolby Vision profile 5: per-RPU bake of base-YCbCr(full-range) -> SDR ----
 * Validated against libplacebo (Gate A: max 1-2 codes; MMR math: Gate B unit test).
 * Composes the DV decode (reshape + ycc_to_rgb + PQ/LMS/PQ -> HDR10 BT.2020/PQ) with
 * the existing zscale/hable tone-map (ff_rpi_tm_luma1d + ff_rpi_tm_lut3d). */
static const double P5_HPE[9] = {  /* BT.2020 HPE LMS->RGB (from libplacebo colorspace.c) */
     3.06441879,-2.16597676, 0.10155818, -0.65612108, 1.78554118,-0.12943749,
     0.01736321,-0.04725154, 1.03004253 };
#define P5_M1 0.1593017578125
#define P5_M2 78.84375
#define P5_C1 0.8359375
#define P5_C2 18.8515625
#define P5_C3 18.6875
static inline double p5_pq_eotf(double x){ x=x<0?0:x; double a=pow(x,1.0/P5_M2), v=a-P5_C1; v=v<0?0:v; v/=(P5_C2-P5_C3*a); return pow(v<0?0:v,1.0/P5_M1); }
static inline double p5_pq_oetf(double x){ x=x<0?0:x; double v=pow(x,P5_M1); v=(P5_C1+P5_C2*v)/(1.0+P5_C3*v); return pow(v,P5_M2); }
static inline void p5_m3(const double *M,const double *v,double *o){ for(int i=0;i<3;i++)o[i]=M[3*i]*v[0]+M[3*i+1]*v[1]+M[3*i+2]*v[2]; }

/* Reshape one component (poly Horner or MMR cross-channel), inputs normalized [0,1]. */
static double p5_reshape(const AVDOVIReshapingCurve *cv, const double sig[3], int c,
                         double cs, double nrm)
{
    double s = sig[c];
    int i, np = cv->num_pivots;
    for (i = 0; i < np - 2; i++) if (s < cv->pivots[i+1] * nrm) break;
    double out;
    if (cv->mapping_idc[i] == AV_DOVI_MAPPING_POLYNOMIAL) {
        int ord = cv->poly_order[i];
        out = cv->poly_coef[i][ord] * cs;
        for (int k = ord - 1; k >= 0; k--) out = out * s + cv->poly_coef[i][k] * cs;
    } else {  /* MMR: monomials (x,y,z, xy,xz,yz,xyz), coeffs [0..2]=linear,[3..6]=cross */
        double x=sig[0], y=sig[1], z=sig[2];
        double lin[3]={x,y,z}, mono[4]={x*y, x*z, y*z, x*y*z};
        out = cv->mmr_constant[i] * cs;
        for (int j = 0; j < cv->mmr_order[i]; j++) {
            for (int t = 0; t < 3; t++) out += cv->mmr_coef[i][j][t]   * cs * pow(lin[t],  j+1);
            for (int t = 0; t < 4; t++) out += cv->mmr_coef[i][j][3+t] * cs * pow(mono[t], j+1);
        }
    }
    double lo = cv->pivots[0]*nrm, hi = cv->pivots[np-1]*nrm;
    return out < lo ? lo : (out > hi ? hi : out);
}

/* Trilinear-sample the existing HDR10->SDR chroma tone-map LUT at limited-range HDR10
 * codes (Yh,Cbh,Crh); returns SDR Cb,Cr. (Luma uses the 1D curve, as the accurate tier.) */
static void p5_tm_chroma(double Yh, double Cbh, double Crh, uint8_t *cb, uint8_t *cr)
{
    const int N = RPI_TM_LUT3D_N;
    double gy=(Yh-64)*(N-1)/876.0, gu=(Cbh-64)*(N-1)/896.0, gv=(Crh-64)*(N-1)/896.0;
    gy=gy<0?0:(gy>N-1?N-1:gy); gu=gu<0?0:(gu>N-1?N-1:gu); gv=gv<0?0:(gv>N-1?N-1:gv);
    int y0=(int)gy,u0=(int)gu,v0=(int)gv, y1=y0<N-1?y0+1:y0,u1=u0<N-1?u0+1:u0,v1=v0<N-1?v0+1:v0;
    double fy=gy-y0,fu=gu-u0,fv=gv-v0;
    double ob=0, orr=0;
    for (int dy=0;dy<2;dy++)for(int du=0;du<2;du++)for(int dv=0;dv<2;dv++){
        double w=(dy?fy:1-fy)*(du?fu:1-fu)*(dv?fv:1-fv);
        const uint8_t *e=&ff_rpi_tm_lut3d[(((dy?y1:y0)*N+(du?u1:u0))*N+(dv?v1:v0))*3];
        ob+=w*e[1]; orr+=w*e[2];
    }
    int b=(int)lround(ob), r=(int)lround(orr);
    *cb=b<0?0:(b>255?255:b); *cr=r<0?0:(r>255?255:r);
}

/* Precomputed per-RPU decode context (matrices folded once). */
typedef struct { const AVDOVIDataMapping *map; double YCC[9], OFF[3], COMB[9], cs, nrm; } P5Ctx;

/* Decode one base-YCbCr sample (codes 0..maxc) -> HDR10 BT.2020/PQ YCbCr codes
 * (limited-range 10-bit). The exact math validated in scratchpad/dv_decode.c. */
static void p5_decode_hdr10(const P5Ctx *c, double Yc, double Uc, double Vc,
                            double *Yh, double *Cbh, double *Crh)
{
    double sig[3] = { Yc*c->nrm, Uc*c->nrm, Vc*c->nrm };
    double r[3] = { p5_reshape(&c->map->curves[0],sig,0,c->cs,c->nrm),
                    p5_reshape(&c->map->curves[1],sig,1,c->cs,c->nrm),
                    p5_reshape(&c->map->curves[2],sig,2,c->cs,c->nrm) };
    double t[3] = { r[0]-c->OFF[0], r[1]-c->OFF[1], r[2]-c->OFF[2] }, rgb[3]; p5_m3(c->YCC,t,rgb);
    for (int k=0;k<3;k++) rgb[k]=p5_pq_eotf(rgb[k]);
    double l[3]; p5_m3(c->COMB,rgb,l); for (int k=0;k<3;k++) l[k]=p5_pq_oetf(l[k]);
    double R=l[0],G=l[1],B=l[2], yy=0.2627*R+0.6780*G+0.0593*B;
    double cb=(B-yy)/1.8814, cr=(R-yy)/1.4746;
    *Yh=yy*876.0+64.0; *Cbh=cb*896.0+512.0; *Crh=cr*896.0+512.0;
}

/* GATE-A hook: replay the *live* decode over the validated base crop and dump the
 * HDR10 intermediate (yuv444p10le), so it can be diffed vs the libplacebo oracle.
 * Env: SAND_P5_HDRDUMP="in444:out444:W:H". Runs once, on the first bake. */
static void p5_gate_a(const P5Ctx *c)
{
    const char *spec = getenv("SAND_P5_HDRDUMP");
    if (!spec) return;
    char in[512], out[512]; int W=0,H=0;
    if (sscanf(spec, "%511[^:]:%511[^:]:%d:%d", in, out, &W, &H) != 4 || W<=0 || H<=0) return;
    FILE *fi = fopen(in,"rb"); if (!fi) { av_log(NULL,AV_LOG_ERROR,"P5 gate-A: open %s\n",in); return; }
    size_t n=(size_t)W*H; uint16_t *Y=av_malloc(n*2),*U=av_malloc(n*2),*V=av_malloc(n*2);
    uint16_t *oY=av_malloc(n*2),*oU=av_malloc(n*2),*oV=av_malloc(n*2);
    if (!Y||!U||!V||!oY||!oU||!oV) goto done;
    if (fread(Y,2,n,fi)!=n||fread(U,2,n,fi)!=n||fread(V,2,n,fi)!=n){av_log(NULL,AV_LOG_ERROR,"P5 gate-A: short read\n");goto done;}
    for (size_t i=0;i<n;i++){ double yh,cbh,crh;
        p5_decode_hdr10(c,(Y[i]&1023),(U[i]&1023),(V[i]&1023),&yh,&cbh,&crh);
        int a=(int)lround(yh),b=(int)lround(cbh),d=(int)lround(crh);
        oY[i]=a<0?0:(a>1023?1023:a); oU[i]=b<0?0:(b>1023?1023:b); oV[i]=d<0?0:(d>1023?1023:d); }
    FILE *fo=fopen(out,"wb");
    if (fo){ fwrite(oY,2,n,fo);fwrite(oU,2,n,fo);fwrite(oV,2,n,fo);fclose(fo);
             av_log(NULL,AV_LOG_INFO,"P5 gate-A: wrote HDR10 dump %s (%dx%d)\n",out,W,H); }
done:
    av_free(Y);av_free(U);av_free(V);av_free(oY);av_free(oU);av_free(oV); fclose(fi);
}

/* Build the composed base->SDR 3D LUT for the current RPU. Once per scene. */
static int p5_bake(BridgeContext *s, const AVDOVIMetadata *meta)
{
    const AVDOVIRpuDataHeader   *hdr = av_dovi_get_header(meta);
    const AVDOVIColorMetadata   *col = av_dovi_get_color(meta);
    const int N = RPI_TM_LUT3D_N;
    if (!s->p5_lut && !(s->p5_lut = av_malloc((size_t)N*N*N*3))) return AVERROR(ENOMEM);
    const int maxc = (1 << hdr->bl_bit_depth) - 1;
    P5Ctx c = { .map = av_dovi_get_mapping(meta),
                .cs = 1.0 / (double)(1ULL << hdr->coef_log2_denom), .nrm = 1.0 / maxc };
    s->p5_maxc = maxc;
    double LMSm[9];
    for (int i=0;i<9;i++){ c.YCC[i]=av_q2d(col->ycc_to_rgb_matrix[i]); LMSm[i]=av_q2d(col->rgb_to_lms_matrix[i]); }
    for (int i=0;i<3;i++) c.OFF[i]=av_q2d(col->ycc_to_rgb_offset[i]);
    for (int i=0;i<3;i++)for(int j=0;j<3;j++){ double a=0; for(int k=0;k<3;k++)a+=P5_HPE[3*i+k]*LMSm[3*k+j]; c.COMB[3*i+j]=a; }
    for (int iy=0; iy<N; iy++) for (int iu=0; iu<N; iu++) for (int iv=0; iv<N; iv++) {
        double Yh,Cbh,Crh;
        p5_decode_hdr10(&c, iy*(double)maxc/(N-1), iu*(double)maxc/(N-1), iv*(double)maxc/(N-1),
                        &Yh, &Cbh, &Crh);
        int Yhi=(int)lround(Yh); Yhi=Yhi<0?0:(Yhi>1023?1023:Yhi);
        uint8_t *e = &s->p5_lut[(((size_t)iy*N+iu)*N+iv)*3];
        e[0] = ff_rpi_tm_luma1d[Yhi];                 /* SDR Y  (1D tone curve) */
        p5_tm_chroma(Yh, Cbh, Crh, &e[1], &e[2]);     /* SDR Cb,Cr (3D tone LUT) */
    }
    /* Fast tier: 1D base-Y -> SDR-Y curve at neutral chroma (Cb=Cr=mid). Approximates the
     * cross-channel luma of the 3D LUT so the fast path can skip the full-res 3D luma lookup. */
    const double neutral = (maxc + 1) / 2;
    for (int y = 0; y < 1024; y++) {
        double Yh, Cbh, Crh;
        p5_decode_hdr10(&c, (double)FFMIN(y, maxc), neutral, neutral, &Yh, &Cbh, &Crh);
        int Yhi = (int)lround(Yh); Yhi = Yhi<0?0:(Yhi>1023?1023:Yhi);
        s->p5_luma1d[y] = ff_rpi_tm_luma1d[Yhi];
    }
    for (int i = 0; i < 64; i++) s->p5_l64[i] = s->p5_luma1d[i * 16];
    p5_gate_a(&c);
    s->p5_valid = 1;
    return 0;
}

/* FNV-1a over exactly the coeffs the bake consumes -> rebuild trigger. */
static uint64_t p5_calc_hash(const AVDOVIMetadata *meta)
{
    const AVDOVIRpuDataHeader *hdr = av_dovi_get_header(meta);
    const AVDOVIDataMapping   *map = av_dovi_get_mapping(meta);
    const AVDOVIColorMetadata *col = av_dovi_get_color(meta);
    uint64_t h = 1469598103934665603ULL;
    #define HB(p,n) do{ const uint8_t*_b=(const uint8_t*)(p); for(size_t _i=0;_i<(n);_i++){h^=_b[_i];h*=1099511628211ULL;} }while(0)
    HB(&hdr->coef_log2_denom,1); HB(&hdr->bl_bit_depth,1); HB(&hdr->bl_video_full_range_flag,1);
    HB(map, sizeof(*map));
    HB(col->ycc_to_rgb_matrix, sizeof(col->ycc_to_rgb_matrix));
    HB(col->ycc_to_rgb_offset, sizeof(col->ycc_to_rgb_offset));
    HB(col->rgb_to_lms_matrix, sizeof(col->rgb_to_lms_matrix));
    #undef HB
    return h;
}

/* Full-range Q16 axis multipliers (code -> Q8 grid coord, offset 0). MC: single full-res
 * sample; MYC: 2x2-summed luma (co-sited chroma). Round-to-nearest, matching the
 * accurate tier's constant form but with full-range extent `maxc` and zero offset. */
#define P5_MC(maxc)  ((int32_t)(((int64_t)(RPI_TM_LUT3D_N-1)*256*65536 + (maxc)/2) / (maxc)))
#define P5_MYC(maxc) ((int32_t)(((int64_t)(RPI_TM_LUT3D_N-1)*64 *65536 + (maxc)/2) / (maxc)))

/* Scalar per-sample coord map + branchless tetrahedron select (mirrors tm3d_calc4,
 * offset 0). Emits 4 corner byte offsets into the stride-3 LUT + 4 Q8 weights. */
static av_always_inline void p5_calc1(int Y, int U, int V, int MY, int MC,
    int *po0, int *poa, int *pob, int *po3, int *pw0, int *pw1, int *pw2, int *pw3)
{
    const int N = RPI_TM_LUT3D_N, LIMQ = (N-1)<<8;
    int ybq = (int)(((int64_t)Y*MY + 32768) >> 16);
    int ubq = (int)(((int64_t)U*MC + 32768) >> 16);
    int vbq = (int)(((int64_t)V*MC + 32768) >> 16);
    ybq = ybq<0?0:(ybq>LIMQ?LIMQ:ybq); ubq = ubq<0?0:(ubq>LIMQ?LIMQ:ubq); vbq = vbq<0?0:(vbq>LIMQ?LIMQ:vbq);
    int yi=ybq>>8, ui=ubq>>8, vi=vbq>>8, fy=ybq&255, fu=ubq&255, fv=vbq&255;
    int base = ((yi*N+ui)*N+vi)*3;
    int dY = (yi<N-1?1:0)*N*N*3, dU = (ui<N-1?1:0)*N*3, dV = (vi<N-1?1:0)*3;
    int allo = base+dY+dU+dV;
    int fmax=FFMAX3(fy,fu,fv), fmin=FFMIN3(fy,fu,fv), fmid=fy+fu+fv-fmax-fmin;
    *pw0=256-fmax; *pw1=fmax-fmid; *pw2=fmid-fmin; *pw3=fmin;
    int c_yu=fy>=fu, c_uv=fu>=fv, c_yv=fy>=fv;
    int dmax = (c_yu&&c_yv)?dY : ((!c_yu&&c_uv)?dU:dV);
    int dmin = (c_uv&&c_yv)?dV : ((c_yu&&!c_uv)?dU:dY);
    *po0=base; *poa=base+dmax; *pob=allo-dmin; *po3=allo;
}
/* Weighted sum of one LUT channel at 4 corners -> 8-bit, (acc+128)>>8. */
static av_always_inline uint8_t p5_wsum(const uint8_t *lut, int o0,int oa,int ob,int o3,
                                        int w0,int w1,int w2,int w3, int chan)
{
    int a = w0*lut[o0+chan]+w1*lut[oa+chan]+w2*lut[ob+chan]+w3*lut[o3+chan];
    int q = (a+128)>>8; return q<0?0:(q>255?255:q);
}
/* Pure-scalar rows (NEON tail + fallback + selfcheck oracle). */
static void p5_luma_scalar(uint8_t *O, const uint16_t *Y, const uint16_t *U, const uint16_t *V,
                           const uint8_t *lut, int MC, unsigned W, unsigned x0)
{
    for (unsigned x=x0; x<W; x++) {
        int o0,oa,ob,o3,w0,w1,w2,w3;
        p5_calc1(Y[x]&1023, U[x>>1]&1023, V[x>>1]&1023, MC, MC, &o0,&oa,&ob,&o3,&w0,&w1,&w2,&w3);
        O[x] = p5_wsum(lut,o0,oa,ob,o3,w0,w1,w2,w3,0);
    }
}
static void p5_chroma_scalar(uint8_t *OU, uint8_t *OV, const uint16_t *Y0, const uint16_t *Y1,
                             const uint16_t *U, const uint16_t *V, const uint8_t *lut,
                             int MY, int MC, unsigned cw, unsigned x0)
{
    for (unsigned x=x0; x<cw; x++) {
        int avgY4 = (Y0[2*x]&1023)+(Y0[2*x+1]&1023)+(Y1[2*x]&1023)+(Y1[2*x+1]&1023);
        int o0,oa,ob,o3,w0,w1,w2,w3;
        p5_calc1(avgY4, U[x]&1023, V[x]&1023, MY, MC, &o0,&oa,&ob,&o3,&w0,&w1,&w2,&w3);
        OU[x] = p5_wsum(lut,o0,oa,ob,o3,w0,w1,w2,w3,1);
        OV[x] = p5_wsum(lut,o0,oa,ob,o3,w0,w1,w2,w3,2);
    }
}

#if defined(__aarch64__)
/* Common NEON prelude constants for the P5 tetrahedral (full-range, offset 0). */
#define P5_NEON_CONSTS \
    enum { N = RPI_TM_LUT3D_N }; \
    const int32x4_t vz=vdupq_n_s32(0), v256=vdupq_n_s32(256), v32768=vdupq_n_s32(32768); \
    const int32x4_t vLIMQ=vdupq_n_s32((N-1)<<8), vNm1=vdupq_n_s32(N-1), vone=vdupq_n_s32(1); \
    const int32x4_t v255=vdupq_n_s32(255), vN=vdupq_n_s32(N), vST=vdupq_n_s32(3); \
    const uint16x8_t m1023q=vdupq_n_u16(1023)
#define P5_CALC4(Yv,Uv,Vv,MY,MC,o0,oa,ob,o3,w0,w1,w2,w3) \
    tm3d_calc4(Yv,Uv,Vv,MY,MC,vz,vz,v256,v32768,vLIMQ,vz,vNm1,vone,v255,vN,vST, \
               &o0,&oa,&ob,&o3,&w0,&w1,&w2,&w3)
#define P5_WIDEN(v8) int32x4_t v8##lo=vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(v8))), \
                               v8##hi=vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(v8)))
#endif

/* P5 luma row: full-res Y + nearest (replicated) chroma, gather LUT channel 0. */
static void p5_luma_row(uint8_t *O, const uint16_t *Y, const uint16_t *U, const uint16_t *V,
                        const uint8_t *lut, int MC, unsigned W)
{
    unsigned x = 0;
#if defined(__aarch64__)
    P5_NEON_CONSTS;
    const uint16x4_t m1023h=vdup_n_u16(1023);
    for (; x + 8 <= W; x += 8) {
        uint16x8_t Yv=vandq_u16(vld1q_u16(Y+x),m1023q);
        uint16x4_t U4=vand_u16(vld1_u16(U+(x>>1)),m1023h), V4=vand_u16(vld1_u16(V+(x>>1)),m1023h);
        uint16x8_t Uv=vcombine_u16(vzip1_u16(U4,U4),vzip2_u16(U4,U4));   /* c,c,c+1,c+1,... */
        uint16x8_t Vv=vcombine_u16(vzip1_u16(V4,V4),vzip2_u16(V4,V4));
        P5_WIDEN(Yv); P5_WIDEN(Uv); P5_WIDEN(Vv);
        int32x4_t o0l,oal,obl,o3l,w0l,w1l,w2l,w3l, o0h,oah,obh,o3h,w0h,w1h,w2h,w3h;
        P5_CALC4(Yvlo,Uvlo,Vvlo,MC,MC, o0l,oal,obl,o3l, w0l,w1l,w2l,w3l);
        P5_CALC4(Yvhi,Uvhi,Vvhi,MC,MC, o0h,oah,obh,o3h, w0h,w1h,w2h,w3h);
        int32_t o0[8],oa[8],ob[8],o3[8]; uint8_t g0[8],ga[8],gb[8],g3[8];
        vst1q_s32(o0,o0l); vst1q_s32(o0+4,o0h); vst1q_s32(oa,oal); vst1q_s32(oa+4,oah);
        vst1q_s32(ob,obl); vst1q_s32(ob+4,obh); vst1q_s32(o3,o3l); vst1q_s32(o3+4,o3h);
        for (int i=0;i<8;i++){ g0[i]=lut[o0[i]]; ga[i]=lut[oa[i]]; gb[i]=lut[ob[i]]; g3[i]=lut[o3[i]]; }
        uint16x8_t h0=vmovl_u8(vld1_u8(g0)),ha=vmovl_u8(vld1_u8(ga)),hb=vmovl_u8(vld1_u8(gb)),h3=vmovl_u8(vld1_u8(g3));
        uint32x4_t yl=vmulq_u32(vreinterpretq_u32_s32(w0l),vmovl_u16(vget_low_u16(h0)));
        yl=vmlaq_u32(yl,vreinterpretq_u32_s32(w1l),vmovl_u16(vget_low_u16(ha)));
        yl=vmlaq_u32(yl,vreinterpretq_u32_s32(w2l),vmovl_u16(vget_low_u16(hb)));
        yl=vmlaq_u32(yl,vreinterpretq_u32_s32(w3l),vmovl_u16(vget_low_u16(h3)));
        uint32x4_t yh=vmulq_u32(vreinterpretq_u32_s32(w0h),vmovl_u16(vget_high_u16(h0)));
        yh=vmlaq_u32(yh,vreinterpretq_u32_s32(w1h),vmovl_u16(vget_high_u16(ha)));
        yh=vmlaq_u32(yh,vreinterpretq_u32_s32(w2h),vmovl_u16(vget_high_u16(hb)));
        yh=vmlaq_u32(yh,vreinterpretq_u32_s32(w3h),vmovl_u16(vget_high_u16(h3)));
        vst1_u8(O+x, vqmovn_u16(vcombine_u16(vrshrn_n_u32(yl,8),vrshrn_n_u32(yh,8))));
    }
#endif
    p5_luma_scalar(O, Y, U, V, lut, MC, W, x);
}

/* P5 chroma row: block-avg luma + native Cb,Cr, gather LUT channels 1,2 (Cb|Cr = one u16). */
static void p5_chroma_row(uint8_t *OU, uint8_t *OV, const uint16_t *Y0, const uint16_t *Y1,
                          const uint16_t *U, const uint16_t *V, const uint8_t *lut,
                          int MY, int MC, unsigned cw)
{
    unsigned x = 0;
#if defined(__aarch64__)
    P5_NEON_CONSTS;
    const uint16x8_t m8q=vdupq_n_u16(0xff);
    for (; x + 8 <= cw; x += 8) {
        uint16x8_t y0a=vld1q_u16(Y0+2*x), y0b=vld1q_u16(Y0+2*x+8);
        uint16x8_t y1a=vld1q_u16(Y1+2*x), y1b=vld1q_u16(Y1+2*x+8);
        uint16x8_t avg=vaddq_u16(vpaddq_u16(y0a,y0b), vpaddq_u16(y1a,y1b));   /* 8 avgY4 */
        uint16x8_t U8=vandq_u16(vld1q_u16(U+x),m1023q), V8=vandq_u16(vld1q_u16(V+x),m1023q);
        P5_WIDEN(avg); P5_WIDEN(U8); P5_WIDEN(V8);
        int32x4_t o0l,oal,obl,o3l,w0l,w1l,w2l,w3l, o0h,oah,obh,o3h,w0h,w1h,w2h,w3h;
        P5_CALC4(avglo,U8lo,V8lo,MY,MC, o0l,oal,obl,o3l, w0l,w1l,w2l,w3l);
        P5_CALC4(avghi,U8hi,V8hi,MY,MC, o0h,oah,obh,o3h, w0h,w1h,w2h,w3h);
        int32_t o0[8],oa[8],ob[8],o3[8]; uint16_t g0[8],ga[8],gb[8],g3[8];
        vst1q_s32(o0,o0l); vst1q_s32(o0+4,o0h); vst1q_s32(oa,oal); vst1q_s32(oa+4,oah);
        vst1q_s32(ob,obl); vst1q_s32(ob+4,obh); vst1q_s32(o3,o3l); vst1q_s32(o3+4,o3h);
        for (int i=0;i<8;i++){ memcpy(&g0[i],lut+o0[i]+1,2); memcpy(&ga[i],lut+oa[i]+1,2);
                               memcpy(&gb[i],lut+ob[i]+1,2); memcpy(&g3[i],lut+o3[i]+1,2); }
        uint16x8_t h0=vld1q_u16(g0),ha=vld1q_u16(ga),hb=vld1q_u16(gb),h3=vld1q_u16(g3);
        uint16x8_t cb0=vandq_u16(h0,m8q),cba=vandq_u16(ha,m8q),cbb=vandq_u16(hb,m8q),cb3=vandq_u16(h3,m8q);
        uint16x8_t cr0=vshrq_n_u16(h0,8),cra=vshrq_n_u16(ha,8),crb=vshrq_n_u16(hb,8),cr3=vshrq_n_u16(h3,8);
        uint32x4_t uw0l=vreinterpretq_u32_s32(w0l),uw1l=vreinterpretq_u32_s32(w1l),uw2l=vreinterpretq_u32_s32(w2l),uw3l=vreinterpretq_u32_s32(w3l);
        uint32x4_t uw0h=vreinterpretq_u32_s32(w0h),uw1h=vreinterpretq_u32_s32(w1h),uw2h=vreinterpretq_u32_s32(w2h),uw3h=vreinterpretq_u32_s32(w3h);
        uint32x4_t bl=vmulq_u32(uw0l,vmovl_u16(vget_low_u16(cb0)));
        bl=vmlaq_u32(bl,uw1l,vmovl_u16(vget_low_u16(cba))); bl=vmlaq_u32(bl,uw2l,vmovl_u16(vget_low_u16(cbb))); bl=vmlaq_u32(bl,uw3l,vmovl_u16(vget_low_u16(cb3)));
        uint32x4_t bh=vmulq_u32(uw0h,vmovl_u16(vget_high_u16(cb0)));
        bh=vmlaq_u32(bh,uw1h,vmovl_u16(vget_high_u16(cba))); bh=vmlaq_u32(bh,uw2h,vmovl_u16(vget_high_u16(cbb))); bh=vmlaq_u32(bh,uw3h,vmovl_u16(vget_high_u16(cb3)));
        uint32x4_t rl=vmulq_u32(uw0l,vmovl_u16(vget_low_u16(cr0)));
        rl=vmlaq_u32(rl,uw1l,vmovl_u16(vget_low_u16(cra))); rl=vmlaq_u32(rl,uw2l,vmovl_u16(vget_low_u16(crb))); rl=vmlaq_u32(rl,uw3l,vmovl_u16(vget_low_u16(cr3)));
        uint32x4_t rh=vmulq_u32(uw0h,vmovl_u16(vget_high_u16(cr0)));
        rh=vmlaq_u32(rh,uw1h,vmovl_u16(vget_high_u16(cra))); rh=vmlaq_u32(rh,uw2h,vmovl_u16(vget_high_u16(crb))); rh=vmlaq_u32(rh,uw3h,vmovl_u16(vget_high_u16(cr3)));
        vst1_u8(OU+x, vqmovn_u16(vcombine_u16(vrshrn_n_u32(bl,8),vrshrn_n_u32(bh,8))));
        vst1_u8(OV+x, vqmovn_u16(vcombine_u16(vrshrn_n_u32(rl,8),vrshrn_n_u32(rh,8))));
    }
#endif
    p5_chroma_scalar(OU, OV, Y0, Y1, U, V, lut, MY, MC, cw, x);
}

/* Ordered-dither matrix (standard Bayer 8x8, 0..63). Used by tm=veryfast to dither the
 * 3D-LUT coordinate before the nearest-cell round, so grid quantization dissolves into
 * sub-visible grain instead of hard steps. Indexed by pixel position -> deterministic. */
static const uint8_t BAYER8[64] = {
     0,32, 8,40, 2,34,10,42,
    48,16,56,24,50,18,58,26,
    12,44, 4,36,14,46, 6,38,
    60,28,52,20,62,30,54,22,
     3,35,11,43, 1,33, 9,41,
    51,19,59,27,49,17,57,25,
    15,47, 7,39,13,45, 5,37,
    63,31,55,23,61,29,53,21,
};
/* Per-axis dither bias for site (x, chroma-row yrow): full-amplitude (amp256), spanning
 * the whole [0,256) rounding interval (BAYER*4+2 -> [2,254]); axes decorrelated by tile
 * phase offsets Y(0,0) U(+3,+5) V(+6,+2). Index stays in [0,N-1] for any bias<=254. */
#define NN_BIAS(yrow,oy,x,ox) (BAYER8[(((yrow)+(oy))&7)*8 + (((x)+(ox))&7)]*4 + 2)

/* P5 chroma, NEAREST-NEIGHBOUR + ordered dither (tm=veryfast): dither-round (avgY,Cb,Cr)
 * to one grid cell -> a single gather, no tetra select / weighted sum. ~halves the chroma
 * apply arithmetic; colour-approximate but the dither de-bands the 33^3 grid (~28-code
 * cells) into grain. Opt-in speed tier; the tetrahedral p5_chroma_row stays the P5 default. */
static void p5_chroma_nn_scalar(uint8_t *OU, uint8_t *OV, const uint16_t *Y0, const uint16_t *Y1,
                                const uint16_t *U, const uint16_t *V, const uint8_t *lut,
                                int MY, int MC, unsigned cw, unsigned x0, unsigned yrow)
{
    const int N = RPI_TM_LUT3D_N, LIMQ = (N-1)<<8;
    for (unsigned x=x0; x<cw; x++) {
        int avgY4 = (Y0[2*x]&1023)+(Y0[2*x+1]&1023)+(Y1[2*x]&1023)+(Y1[2*x+1]&1023);
        int yq=(int)(((int64_t)avgY4*MY+32768)>>16);
        int uq=(int)(((int64_t)(U[x]&1023)*MC+32768)>>16);
        int vq=(int)(((int64_t)(V[x]&1023)*MC+32768)>>16);
        yq=yq<0?0:(yq>LIMQ?LIMQ:yq); uq=uq<0?0:(uq>LIMQ?LIMQ:uq); vq=vq<0?0:(vq>LIMQ?LIMQ:vq);
        int yi=(yq+NN_BIAS(yrow,0,x,0))>>8;   /* ordered-dither round to nearest cell */
        int ui=(uq+NN_BIAS(yrow,3,x,5))>>8;
        int vi=(vq+NN_BIAS(yrow,6,x,2))>>8;
        yi = yi>N-1 ? N-1 : yi;
        ui = ui>N-1 ? N-1 : ui;
        vi = vi>N-1 ? N-1 : vi;
        const uint8_t *c=&lut[((yi*N+ui)*N+vi)*3];
        OU[x]=c[1]; OV[x]=c[2];
    }
}
static void p5_chroma_nn_row(uint8_t *OU, uint8_t *OV, const uint16_t *Y0, const uint16_t *Y1,
                             const uint16_t *U, const uint16_t *V, const uint8_t *lut,
                             int MY, int MC, unsigned cw, unsigned yrow)
{
    unsigned x = 0;
#if defined(__aarch64__)
    enum { N = RPI_TM_LUT3D_N };
    const int32x4_t v32768=vdupq_n_s32(32768), vz=vdupq_n_s32(0);
    const int32x4_t vLIMQ=vdupq_n_s32((N-1)<<8), vNm1=vdupq_n_s32(N-1);
    const int32x4_t vMY=vdupq_n_s32(MY), vMC=vdupq_n_s32(MC), vN=vdupq_n_s32(N), vST=vdupq_n_s32(3);
    const uint16x8_t m1023q=vdupq_n_u16(1023), m8q=vdupq_n_u16(0xff);
    /* Ordered-dither bias, built once per row: the main loop steps x by 8 from 0 and cw%8==0,
     * so lane l always maps to Bayer column (l+ox)&7 (a fixed 8-lane pattern per row). */
    int32_t bY[8], bU[8], bV[8];
    for (int l=0;l<8;l++){ bY[l]=NN_BIAS(yrow,0,l,0); bU[l]=NN_BIAS(yrow,3,l,5); bV[l]=NN_BIAS(yrow,6,l,2); }
    const int32x4_t bYl=vld1q_s32(bY), bYh=vld1q_s32(bY+4);
    const int32x4_t bUl=vld1q_s32(bU), bUh=vld1q_s32(bU+4);
    const int32x4_t bVl=vld1q_s32(bV), bVh=vld1q_s32(bV+4);
    /* q = clamp((val*M + 32768)>>16, 0, LIMQ); idx = min((q+bias)>>8, N-1) */
#define NNIDX(val,vM,vB) vminq_s32(vshrq_n_s32(vaddq_s32( \
        vminq_s32(vmaxq_s32(vshrq_n_s32(vaddq_s32(vmulq_s32(val,vM),v32768),16),vz),vLIMQ), vB),8), vNm1)
#define NNOFF(vy,vu,vv) vmulq_s32(vaddq_s32(vmulq_s32(vaddq_s32(vmulq_s32(vy,vN),vu),vN),vv),vST)
    for (; x + 8 <= cw; x += 8) {
        uint16x8_t y0a=vld1q_u16(Y0+2*x), y0b=vld1q_u16(Y0+2*x+8);
        uint16x8_t y1a=vld1q_u16(Y1+2*x), y1b=vld1q_u16(Y1+2*x+8);
        uint16x8_t avg=vaddq_u16(vpaddq_u16(y0a,y0b), vpaddq_u16(y1a,y1b));   /* 8 avgY4 */
        uint16x8_t U8=vandq_u16(vld1q_u16(U+x),m1023q), V8=vandq_u16(vld1q_u16(V+x),m1023q);
        int32x4_t Ylo=vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(avg))), Yhi=vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(avg)));
        int32x4_t Ulo=vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(U8))),  Uhi=vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(U8)));
        int32x4_t Vlo=vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(V8))),  Vhi=vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(V8)));
        int32x4_t offl=NNOFF(NNIDX(Ylo,vMY,bYl),NNIDX(Ulo,vMC,bUl),NNIDX(Vlo,vMC,bVl));
        int32x4_t offh=NNOFF(NNIDX(Yhi,vMY,bYh),NNIDX(Uhi,vMC,bUh),NNIDX(Vhi,vMC,bVh));
        int32_t off[8]; uint16_t g[8];
        vst1q_s32(off,offl); vst1q_s32(off+4,offh);
        for (int i=0;i<8;i++) memcpy(&g[i], lut+off[i]+1, 2);   /* one u16 (Cb|Cr) per lane */
        uint16x8_t h=vld1q_u16(g);
        vst1_u8(OU+x, vqmovn_u16(vandq_u16(h,m8q)));
        vst1_u8(OV+x, vqmovn_u16(vshrq_n_u16(h,8)));
    }
#undef NNIDX
#undef NNOFF
#endif
    p5_chroma_nn_scalar(OU, OV, Y0, Y1, U, V, lut, MY, MC, cw, x, yrow);
}

/* P5 apply for one chunk: luma (full-res Y + co-sited chroma) + chroma (block-avg Y). */
static void p5_apply_chunk(const TMData *td, const AVFrame *S, unsigned ybase, unsigned ch)
{
    const unsigned W = td->dst->width, cw = W/2;
    AVFrame *D = td->dst;
    const int sy=S->linesize[0]/2, su=S->linesize[1]/2, sv=S->linesize[2]/2;
    const uint8_t *lut = td->p5_lut;
    const int MC = P5_MC(td->p5_maxc), MYC = P5_MYC(td->p5_maxc);
    static int selfcheck = -1;
    if (selfcheck < 0) selfcheck = !!getenv("SAND_TM_SELFCHECK");
    /* luma: TM_P5_FAST -> cheap 1D neutral-chroma curve (skips the full-res 3D lookup);
     * TM_P5 -> full-res 3D lookup with nearest (replicated) chroma. */
    const int fast = (td->tm == TM_P5_FAST || td->tm == TM_P5_VERYFAST);
    const int nn   = (td->tm == TM_P5_VERYFAST);   /* nearest-neighbour chroma */
    for (unsigned r=0; r<ch; r++) {
        const uint16_t *Y=(const uint16_t*)S->data[0]+(size_t)r*sy;
        const uint16_t *U=(const uint16_t*)S->data[1]+(size_t)(r/2)*su;
        const uint16_t *V=(const uint16_t*)S->data[2]+(size_t)(r/2)*sv;
        uint8_t *O=D->data[0]+(size_t)(ybase+r)*D->linesize[0];
        if (fast) { lut1d_apply(O, Y, W, td->p5_l64, td->p5_luma1d); continue; }
        p5_luma_row(O, Y, U, V, lut, MC, W);
        if (selfcheck) {
            uint8_t *ref=av_malloc(W); unsigned mx=0;
            if (ref){ p5_luma_scalar(ref,Y,U,V,lut,MC,W,0);
                for (unsigned x=0;x<W;x++){ unsigned d=O[x]>ref[x]?O[x]-ref[x]:ref[x]-O[x]; if(d>mx)mx=d; }
                if (mx) av_log(NULL,AV_LOG_WARNING,"P5 luma selfcheck row %u: max %u\n",r,mx);
                av_free(ref); }
        }
    }
    /* chroma: block-avg luma + native Cb,Cr */
    for (unsigned cr=0; cr<ch/2; cr++) {
        const uint16_t *Y0=(const uint16_t*)S->data[0]+(size_t)(cr*2)*sy;
        const uint16_t *Y1=(const uint16_t*)S->data[0]+(size_t)(cr*2+1)*sy;
        const uint16_t *U=(const uint16_t*)S->data[1]+(size_t)cr*su;
        const uint16_t *V=(const uint16_t*)S->data[2]+(size_t)cr*sv;
        uint8_t *OU=D->data[1]+(size_t)(ybase/2+cr)*D->linesize[1];
        uint8_t *OV=D->data[2]+(size_t)(ybase/2+cr)*D->linesize[2];
        if (nn) p5_chroma_nn_row(OU, OV, Y0, Y1, U, V, lut, MYC, MC, cw, ybase/2+cr);
        else    p5_chroma_row   (OU, OV, Y0, Y1, U, V, lut, MYC, MC, cw);
        if (selfcheck) {
            uint8_t *ru=av_malloc(cw),*rv=av_malloc(cw); unsigned mx=0;
            if (ru&&rv){ if (nn) p5_chroma_nn_scalar(ru,rv,Y0,Y1,U,V,lut,MYC,MC,cw,0,ybase/2+cr);
                         else    p5_chroma_scalar   (ru,rv,Y0,Y1,U,V,lut,MYC,MC,cw,0);
                for (unsigned x=0;x<cw;x++){ unsigned d=OU[x]>ru[x]?OU[x]-ru[x]:ru[x]-OU[x]; if(d>mx)mx=d;
                    d=OV[x]>rv[x]?OV[x]-rv[x]:rv[x]-OV[x]; if(d>mx)mx=d; }
                if (mx) av_log(NULL,AV_LOG_WARNING,"P5 chroma selfcheck row %u: max %u\n",cr,mx); }
            av_free(ru); av_free(rv);
        }
    }
}

static void tm_apply_chunk(const TMData *td, const AVFrame *S, unsigned ybase, unsigned ch)
{
    const unsigned W = td->dst->width, cw = W / 2;
    AVFrame *D = td->dst;
    const int sy = S->linesize[0] / 2, su = S->linesize[1] / 2, sv = S->linesize[2] / 2;
    for (unsigned r = 0; r < ch; r++) {
        const uint16_t *Y = (const uint16_t *)S->data[0] + (size_t)r * sy;
        uint8_t *O = D->data[0] + (size_t)(ybase + r) * D->linesize[0];
        lut1d_apply(O, Y, W, td->l64, td->luma1d);
    }
    if (td->tm == TM_FAST) {
        for (unsigned cr = 0; cr < ch / 2; cr++) {
            const uint16_t *U = (const uint16_t *)S->data[1] + (size_t)cr * su;
            const uint16_t *V = (const uint16_t *)S->data[2] + (size_t)cr * sv;
            uint8_t *OU = D->data[1] + (size_t)(ybase/2 + cr) * D->linesize[1];
            uint8_t *OV = D->data[2] + (size_t)(ybase/2 + cr) * D->linesize[2];
            lut1d_apply(OU, U, cw, td->cb64, td->cb1d);
            lut1d_apply(OV, V, cw, td->cr64, td->cr1d);
        }
    } else { /* TM_ACCURATE — chroma-resolution 3D LUT, tetrahedral (NEON, bit-exact) */
        static int selfcheck = -1;
        if (selfcheck < 0) selfcheck = !!getenv("SAND_TM_SELFCHECK");
        for (unsigned cr = 0; cr < ch / 2; cr++) {
            const uint16_t *U  = (const uint16_t *)S->data[1] + (size_t)cr * su;
            const uint16_t *V  = (const uint16_t *)S->data[2] + (size_t)cr * sv;
            const uint16_t *Y0 = (const uint16_t *)S->data[0] + (size_t)(cr*2)   * sy;
            const uint16_t *Y1 = (const uint16_t *)S->data[0] + (size_t)(cr*2+1) * sy;
            uint8_t *OU = D->data[1] + (size_t)(ybase/2 + cr) * D->linesize[1];
            uint8_t *OV = D->data[2] + (size_t)(ybase/2 + cr) * D->linesize[2];
            tm3d_chroma_row(OU, OV, Y0, Y1, U, V, td->lut3d, cw);
            if (selfcheck) {   /* DEBUG: NEON vs scalar on real frames, expect max 0 */
                uint8_t *ru = av_malloc(cw), *rv = av_malloc(cw);
                if (ru && rv) {
                    unsigned mx = 0;
                    tm3d_chroma_scalar(ru, rv, Y0, Y1, U, V, td->lut3d, 0, cw);
                    for (unsigned x = 0; x < cw; x++) {
                        unsigned d = OU[x] > ru[x] ? OU[x]-ru[x] : ru[x]-OU[x]; if (d > mx) mx = d;
                        d = OV[x] > rv[x] ? OV[x]-rv[x] : rv[x]-OV[x]; if (d > mx) mx = d;
                    }
                    if (mx) av_log(NULL, AV_LOG_WARNING, "SAND_TM 3D selfcheck row %u: max diff %u\n", cr, mx);
                }
                av_free(ru); av_free(rv);
            }
        }
    }
}

/* out=half: box-average a 2x2 of a 10-bit plane into one output row of width dw
 * ((a+b+c+d+2)>>2). s0,s1 are the two source rows (uint16). Scalar ref + NEON. */
static void box2x2_row_scalar(uint16_t *d, const uint16_t *s0, const uint16_t *s1, unsigned dw, unsigned x0)
{
    for (unsigned x = x0; x < dw; x++)
        d[x] = (uint16_t)((s0[2*x] + s0[2*x+1] + s1[2*x] + s1[2*x+1] + 2) >> 2);
}
static void box2x2_row(uint16_t *d, const uint16_t *s0, const uint16_t *s1, unsigned dw)
{
    unsigned x = 0;
#if defined(__aarch64__)
    for (; x + 8 <= dw; x += 8) {
        uint16x8_t a0 = vld1q_u16(s0 + 2*x), a1 = vld1q_u16(s0 + 2*x + 8);
        uint16x8_t b0 = vld1q_u16(s1 + 2*x), b1 = vld1q_u16(s1 + 2*x + 8);
        uint16x8_t ps0 = vpaddq_u16(a0, a1), ps1 = vpaddq_u16(b0, b1);  /* horiz pairs */
        vst1q_u16(d + x, vrshrq_n_u16(vaddq_u16(ps0, ps1), 2));         /* (sum+2)>>2 */
    }
#endif
    box2x2_row_scalar(d, s0, s1, dw, x);
}

static int tm_slice(AVFilterContext *avctx, void *arg, int jobnr, int nb_jobs)
{
    const TMData *td = arg;

    if (td->half) {
        /* out=half: unpack each TM_CHUNK band of the 4K source to a full-width 10-bit
         * scratch, box-average 2x2 -> half-res scratch, then run the EXISTING apply at
         * input/2 into the half-res dst (emitted 1080p directly, no ISP scale). */
        const unsigned H = td->H;
        const unsigned Wo = td->dst->width, cwo = Wo / 2;   /* output (half) dims */
        const unsigned Ws = Wo * 2, cws = Ws / 2;           /* source (input) dims */
        unsigned y0 = ((uint64_t)H *  jobnr      / nb_jobs) & ~3u;   /* whole 2x2 blocks */
        unsigned y1 = (jobnr == nb_jobs - 1) ? H : (((uint64_t)H * (jobnr + 1) / nb_jobs) & ~3u);
        if (y1 <= y0)
            return 0;
        uint16_t *sy = av_malloc((size_t)TM_CHUNK * Ws * 2);
        uint16_t *su = av_malloc((size_t)(TM_CHUNK/2) * cws * 2);
        uint16_t *sv = av_malloc((size_t)(TM_CHUNK/2) * cws * 2);
        uint16_t *hy = av_malloc((size_t)(TM_CHUNK/2) * Wo * 2);
        uint16_t *hu = av_malloc((size_t)(TM_CHUNK/4 + 1) * cwo * 2);
        uint16_t *hv = av_malloc((size_t)(TM_CHUNK/4 + 1) * cwo * 2);
        if (!sy || !su || !sv || !hy || !hu || !hv) {
            av_free(sy); av_free(su); av_free(sv); av_free(hy); av_free(hu); av_free(hv);
            return AVERROR(ENOMEM);
        }
        AVFrame s10 = { 0 }, s10h = { 0 };
        s10.format = AV_PIX_FMT_YUV420P10; s10.width = Ws;
        s10.data[0] = (uint8_t *)sy; s10.linesize[0] = Ws  * 2;
        s10.data[1] = (uint8_t *)su; s10.linesize[1] = cws * 2;
        s10.data[2] = (uint8_t *)sv; s10.linesize[2] = cws * 2;
        s10h.format = AV_PIX_FMT_YUV420P10; s10h.width = Wo;
        s10h.data[0] = (uint8_t *)hy; s10h.linesize[0] = Wo  * 2;
        s10h.data[1] = (uint8_t *)hu; s10h.linesize[1] = cwo * 2;
        s10h.data[2] = (uint8_t *)hv; s10h.linesize[2] = cwo * 2;
        int rv = 0;
        for (unsigned y = y0; y < y1 && !rv; y += TM_CHUNK) {
            unsigned ch = FFMIN((unsigned)TM_CHUNK, y1 - y);   /* multiple of 4 (aligned) */
            s10.height = ch;
            AVFrame src = *td->src;
            src.crop_top    = td->src->crop_top + y;
            src.crop_bottom = td->src->height - (td->src->crop_top + y + ch);
            rv = av_rpi_sand_to_planar_frame(&s10, &src);      /* SAND30 -> 10-bit scratch */
            if (rv) break;
            const unsigned lh = ch / 2, chr = ch / 4;          /* half luma / chroma rows */
            for (unsigned oy = 0; oy < lh; oy++)
                box2x2_row(hy + (size_t)oy*Wo, sy + (size_t)(2*oy)*Ws, sy + (size_t)(2*oy+1)*Ws, Wo);
            for (unsigned oc = 0; oc < chr; oc++) {
                box2x2_row(hu + (size_t)oc*cwo, su + (size_t)(2*oc)*cws, su + (size_t)(2*oc+1)*cws, cwo);
                box2x2_row(hv + (size_t)oc*cwo, sv + (size_t)(2*oc)*cws, sv + (size_t)(2*oc+1)*cws, cwo);
            }
            s10h.height = lh;
            if (td->tm == TM_NONE) {                            /* SDR / passthrough: 10->8 narrow */
                for (unsigned r = 0; r < lh; r++) {
                    uint8_t *O = td->dst->data[0] + (size_t)(y/2 + r) * td->dst->linesize[0];
                    const uint16_t *Y = hy + (size_t)r*Wo;
                    for (unsigned x = 0; x < Wo; x++) O[x] = Y[x] >> 2;
                }
                for (unsigned r = 0; r < chr; r++) {
                    uint8_t *OU = td->dst->data[1] + (size_t)(y/4 + r) * td->dst->linesize[1];
                    uint8_t *OV = td->dst->data[2] + (size_t)(y/4 + r) * td->dst->linesize[2];
                    const uint16_t *U = hu + (size_t)r*cwo, *V = hv + (size_t)r*cwo;
                    for (unsigned x = 0; x < cwo; x++) { OU[x] = U[x] >> 2; OV[x] = V[x] >> 2; }
                }
            } else if (td->tm == TM_P5 || td->tm == TM_P5_FAST || td->tm == TM_P5_VERYFAST) {
                p5_apply_chunk(td, &s10h, y/2, lh);
            } else {
                tm_apply_chunk(td, &s10h, y/2, lh);
            }
        }
        av_free(sy); av_free(su); av_free(sv); av_free(hy); av_free(hu); av_free(hv);
        return rv;
    }

    const unsigned H = td->H, W = td->dst->width, cw = W / 2;
    unsigned y0 = ((uint64_t)H *  jobnr      / nb_jobs) & ~1u;
    unsigned y1 = (jobnr == nb_jobs - 1) ? H : (((uint64_t)H * (jobnr + 1) / nb_jobs) & ~1u);
    if (y1 <= y0)
        return 0;

    if (td->tm == TM_FAST) {
        /* Fast tier: luma is a single-pass SAND30 -> tone-mapped 8-bit (the .S
         * kernel folds the 1D curve into the unpack — no 10-bit intermediate).
         * Chroma still needs a small L2-resident 10-bit scratch for the
         * separable Cb/Cr LUTs (1/3 of the samples, so cheap). */
        const AVFrame *src = td->src;
        const unsigned stride1 = av_rpi_sand_frame_stride1(src);
        av_rpi_sand30_to_planar_y8_lut(
            td->dst->data[0] + (size_t)y0 * td->dst->linesize[0], td->dst->linesize[0],
            src->data[0], stride1, av_rpi_sand_frame_stride2_y(src),
            src->crop_left, src->crop_top + y0, W, y1 - y0, td->l64, td->l64_next);

        uint16_t *su = av_malloc((size_t)(TM_CHUNK/2) * cw * 2);
        uint16_t *sv = av_malloc((size_t)(TM_CHUNK/2) * cw * 2);
        if (!su || !sv) { av_free(su); av_free(sv); return AVERROR(ENOMEM); }
        for (unsigned y = y0; y < y1; y += TM_CHUNK) {
            unsigned ch = FFMIN((unsigned)TM_CHUNK, y1 - y);
            av_rpi_sand30_to_planar_c16((uint8_t *)su, cw * 2, (uint8_t *)sv, cw * 2,
                src->data[1], stride1, av_rpi_sand_frame_stride2_c(src),
                src->crop_left / 2, (src->crop_top + y) / 2, cw, ch / 2);
            for (unsigned r = 0; r < ch / 2; r++) {
                uint8_t *OU = td->dst->data[1] + (size_t)(y/2 + r) * td->dst->linesize[1];
                uint8_t *OV = td->dst->data[2] + (size_t)(y/2 + r) * td->dst->linesize[2];
                lut1d_apply(OU, su + (size_t)r * cw, cw, td->cb64, td->cb1d);
                lut1d_apply(OV, sv + (size_t)r * cw, cw, td->cr64, td->cr1d);
            }
        }
        av_free(su); av_free(sv);
        return 0;
    }

    /* L2-resident 10-bit scratch for TM_CHUNK rows */
    uint16_t *sy = av_malloc((size_t)TM_CHUNK * W * 2);
    uint16_t *su = av_malloc((size_t)(TM_CHUNK/2) * cw * 2);
    uint16_t *sv = av_malloc((size_t)(TM_CHUNK/2) * cw * 2);
    if (!sy || !su || !sv) { av_free(sy); av_free(su); av_free(sv); return AVERROR(ENOMEM); }
    AVFrame s10 = { 0 };
    s10.format = AV_PIX_FMT_YUV420P10; s10.width = W;
    s10.data[0] = (uint8_t *)sy; s10.linesize[0] = W * 2;
    s10.data[1] = (uint8_t *)su; s10.linesize[1] = cw * 2;
    s10.data[2] = (uint8_t *)sv; s10.linesize[2] = cw * 2;
    int rv = 0;
    for (unsigned y = y0; y < y1 && !rv; y += TM_CHUNK) {
        unsigned ch = FFMIN((unsigned)TM_CHUNK, y1 - y);
        s10.height = ch;
        AVFrame src = *td->src;                    /* SAND view cropped to this chunk */
        src.crop_top    = td->src->crop_top + y;
        src.crop_bottom = td->src->height - (td->src->crop_top + y + ch);
        rv = av_rpi_sand_to_planar_frame(&s10, &src);   /* SAND30 -> 10-bit scratch */
        if (!rv) {                                      /* 10-bit -> 8-bit YU12       */
            if (td->tm == TM_P5 || td->tm == TM_P5_FAST || td->tm == TM_P5_VERYFAST) p5_apply_chunk(td, &s10, y, ch);
            else                 tm_apply_chunk(td, &s10, y, ch);
        }
    }
    av_free(sy); av_free(su); av_free(sv);
    return rv;
}

#if CONFIG_ZSCALE_FILTER && CONFIG_TONEMAP_FILTER
/* Peak-aware PQ tone-map LUT generation (PQ/HDR10 only). Mirrors the build-time
 * libavfilter/rpi_tonemap_gen.py at runtime, parameterized by the source peak, using the
 * same zscale+tonemap=hable chain the baked ff_rpi_tm_* tables come from — so peak=1000
 * reproduces them. The generated tables are applied by the identical NEON kernels (zero
 * per-frame cost); generation happens once per stream (or when the peak changes). */
#define TM_GAIN 1.5   /* fast-tier chroma saturation gain (matches rpi_tonemap_gen.py) */

static AVFrame *mk444(int w, int h)
{
    AVFrame *f = av_frame_alloc();
    if (!f) return NULL;
    f->format = AV_PIX_FMT_YUV444P10LE; f->width = w; f->height = h;
    if (av_frame_get_buffer(f, 32) < 0) { av_frame_free(&f); return NULL; }
    f->pts = 0;
    f->color_trc = AVCOL_TRC_SMPTE2084; f->color_primaries = AVCOL_PRI_BT2020;
    f->colorspace = AVCOL_SPC_BT2020_NCL; f->color_range = AVCOL_RANGE_MPEG;
    return f;
}
static inline void setp(AVFrame *f, int p, int x, int y, uint16_t v)
{ ((uint16_t *)(f->data[p] + (size_t)y * f->linesize[p]))[x] = v; }
static inline uint8_t getp8(const AVFrame *f, int p, int x, int y)
{ return f->data[p][(size_t)y * f->linesize[p] + x]; }

/* Push one synthetic BT.2020-PQ yuv444p10le frame through
 * zscale=t=linear:npl=100,tonemap=hable:peak=P,zscale=t=bt709...,format=yuv444p ; return the
 * yuv444p (8-bit) output. Consumes `inp`. NULL on failure. */
static AVFrame *tm_gen_run(void *logctx, double peak, AVFrame *inp)
{
    AVFilterGraph *g = avfilter_graph_alloc();
    AVFilterContext *src = NULL, *sink = NULL;
    AVFilterInOut *outs = NULL, *ins = NULL;
    AVFrame *out = NULL;
    char args[256], chain[256];
    if (!g) { av_frame_free(&inp); return NULL; }
    snprintf(args, sizeof args,
             "video_size=%dx%d:pix_fmt=%d:time_base=1/25:pixel_aspect=1/1",
             inp->width, inp->height, AV_PIX_FMT_YUV444P10LE);
    if (avfilter_graph_create_filter(&src, avfilter_get_by_name("buffer"),  "in",  args, NULL, g) < 0) goto done;
    if (avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "out", NULL, NULL, g) < 0) goto done;
    snprintf(chain, sizeof chain,
             "zscale=t=linear:npl=100,tonemap=hable:peak=%.6f,"
             "zscale=t=bt709:m=bt709:p=bt709:r=tv,format=yuv444p", peak);
    outs = avfilter_inout_alloc(); ins = avfilter_inout_alloc();
    if (!outs || !ins) goto done;
    outs->name = av_strdup("in");  outs->filter_ctx = src;  outs->pad_idx = 0; outs->next = NULL;
    ins->name  = av_strdup("out"); ins->filter_ctx  = sink; ins->pad_idx  = 0; ins->next  = NULL;
    if (avfilter_graph_parse_ptr(g, chain, &ins, &outs, NULL) < 0) goto done;
    if (avfilter_graph_config(g, NULL) < 0) goto done;
    if (av_buffersrc_add_frame(src, inp) < 0) { inp = NULL; goto done; }
    inp = NULL;                                   /* consumed by buffersrc */
    if (av_buffersrc_add_frame(src, NULL) < 0) goto done;
    out = av_frame_alloc();
    if (out && av_buffersink_get_frame(sink, out) < 0) av_frame_free(&out);
done:
    avfilter_inout_free(&outs); avfilter_inout_free(&ins);
    avfilter_graph_free(&g);
    av_frame_free(&inp);
    return out;
}

/* Regenerate the PQ tables for peak_nits into the s->g_* buffers. Returns 0 on success. */
static int pq_lut_regen(AVFilterContext *avctx, BridgeContext *s, int peak_nits)
{
    const int N = RPI_TM_LUT3D_N;
    const double peak = peak_nits / 100.0;
    AVFrame *in, *out;
    int rv = AVERROR(ENOMEM);

    if (!s->g_luma1d) {
        s->g_luma1d = av_malloc(1024); s->g_cb1d = av_malloc(1024);
        s->g_cr1d   = av_malloc(1024); s->g_lut3d = av_malloc((size_t)N*N*N*3);
        if (!s->g_luma1d || !s->g_cb1d || !s->g_cr1d || !s->g_lut3d) goto fail;
    }

    /* 1D luma ramp: Y=0..1023, neutral chroma. */
    if (!(in = mk444(1024, 2))) goto fail;
    for (int y = 0; y < 2; y++) for (int x = 0; x < 1024; x++)
        { setp(in,0,x,y,x); setp(in,1,x,y,512); setp(in,2,x,y,512); }
    if (!(out = tm_gen_run(avctx, peak, in))) goto fail;
    for (int i = 0; i < 1024; i++) s->g_luma1d[i] = getp8(out, 0, i, 0);
    av_frame_free(&out);

    /* 1D Cb ramp (U=0..1023), then bake the fast-tier 1.5x saturation gain around 128. */
    if (!(in = mk444(1024, 2))) goto fail;
    for (int y = 0; y < 2; y++) for (int x = 0; x < 1024; x++)
        { setp(in,0,x,y,512); setp(in,1,x,y,x); setp(in,2,x,y,512); }
    if (!(out = tm_gen_run(avctx, peak, in))) goto fail;
    for (int i = 0; i < 1024; i++)
        s->g_cb1d[i] = av_clip_uint8(lrint(128 + TM_GAIN * ((int)getp8(out,1,i,0) - 128)));
    av_frame_free(&out);

    /* 1D Cr ramp (V=0..1023). */
    if (!(in = mk444(1024, 2))) goto fail;
    for (int y = 0; y < 2; y++) for (int x = 0; x < 1024; x++)
        { setp(in,0,x,y,512); setp(in,1,x,y,512); setp(in,2,x,y,x); }
    if (!(out = tm_gen_run(avctx, peak, in))) goto fail;
    for (int i = 0; i < 1024; i++)
        s->g_cr1d[i] = av_clip_uint8(lrint(128 + TM_GAIN * ((int)getp8(out,2,i,0) - 128)));
    av_frame_free(&out);

    /* 33^3 identity grid, limited-range 10-bit: Y in [64,940], C in [64,960].
     * Row iy holds all (Cb,Cr) combos for luma level iy at column icb*N+icr. */
    if (!(in = mk444(N * N, N))) goto fail;
    for (int iy = 0; iy < N; iy++) {
        int Yc = lrint(64 + iy * (940.0 - 64.0) / (N - 1));
        for (int icb = 0; icb < N; icb++) {
            int Ccb = lrint(64 + icb * (960.0 - 64.0) / (N - 1));
            for (int icr = 0; icr < N; icr++) {
                int Ccr = lrint(64 + icr * (960.0 - 64.0) / (N - 1));
                int col = icb * N + icr;
                setp(in,0,col,iy,Yc); setp(in,1,col,iy,Ccb); setp(in,2,col,iy,Ccr);
            }
        }
    }
    if (!(out = tm_gen_run(avctx, peak, in))) goto fail;
    for (int iy = 0; iy < N; iy++)
        for (int icb = 0; icb < N; icb++)
            for (int icr = 0; icr < N; icr++) {
                int col = icb * N + icr;
                uint8_t *p = &s->g_lut3d[(((size_t)iy * N + icb) * N + icr) * 3];
                p[0] = getp8(out,0,col,iy); p[1] = getp8(out,1,col,iy); p[2] = getp8(out,2,col,iy);
            }
    av_frame_free(&out);

    /* Derive the 64-entry NEON tables (identical to init's subsample). */
    for (int i = 0; i < 64; i++) {
        s->g_l64[i]      = s->g_luma1d[i * 16];
        s->g_l64_next[i] = s->g_luma1d[FFMIN((i + 1) * 16, 1023)];
        s->g_cb64[i]     = s->g_cb1d[i * 16];
        s->g_cr64[i]     = s->g_cr1d[i * 16];
    }
    s->gen_peak = peak_nits;
    return 0;
fail:
    return rv;
}
#endif /* CONFIG_ZSCALE_FILTER && CONFIG_TONEMAP_FILTER */

/* Software-input fallback path. The source switched mid-stream to a format
 * rpivid cannot decode (4:2:2/4:4:4/12-bit/>4K), so the decoder produces
 * software frames; fftools auto-converts them to yuv420p ahead of us. Copy that
 * planar frame into a pooled dma-buf and emit the same DRM_PRIME YU12 descriptor
 * the SAND path emits, so the fixed hardware tail (ISP scale + H.264 encode) is
 * unchanged. No tone-map here (tm=none on the SW path) — it is a rare, already
 * sub-real-time path; HDR-on-SW is a later refinement. */
static int filter_frame_sw(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *avctx = inlink->dst;
    BridgeContext *s = avctx->priv;
    AVFilterLink *outlink = avctx->outputs[0];
    AVFrame *out = NULL;
    OutBuf *b = NULL;
    int idx = -1, fd = -1, rv;
    void *map = NULL;

    const unsigned w = av_frame_cropped_width(in);
    const unsigned h = av_frame_cropped_height(in);
    const unsigned wo = w, ho = h;   /* full size; the ISP scaler downscales */
    const unsigned bpl = wo;
    const size_t ysz = (size_t)bpl * ho, csz = ysz / 4, total = ysz + 2 * csz;

    if ((idx = pool_acquire(s, total, &fd, &map)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "dma-buf pool exhausted/alloc failed\n");
        rv = AVERROR(ENOMEM); goto fail;
    }

    {   /* copy the yuv420p planes into the YU12 dma-buf, honouring input crop */
        const int cw = ((int)wo + 1) / 2, ch = ((int)ho + 1) / 2;
        const uint8_t *sy = in->data[0] + in->crop_top * in->linesize[0] + in->crop_left;
        const uint8_t *su = in->data[1] + (in->crop_top / 2) * in->linesize[1] + in->crop_left / 2;
        const uint8_t *sv = in->data[2] + (in->crop_top / 2) * in->linesize[2] + in->crop_left / 2;
        dmabuf_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
        av_image_copy_plane((uint8_t *)map,             bpl,     sy, in->linesize[0], wo, ho);
        av_image_copy_plane((uint8_t *)map + ysz,       bpl / 2, su, in->linesize[1], cw, ch);
        av_image_copy_plane((uint8_t *)map + ysz + csz, bpl / 2, sv, in->linesize[2], cw, ch);
        dmabuf_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    }

    if (!(b = av_mallocz(sizeof(*b)))) { rv = AVERROR(ENOMEM); goto fail_release; }
    b->idx = idx;
    if (!(b->pool_ref = av_buffer_ref(s->pool_ref))) { rv = AVERROR(ENOMEM); goto fail_release; }
    b->desc.nb_objects = 1;
    b->desc.objects[0].fd = fd;
    b->desc.objects[0].size = ((SandPool *)s->pool_ref->data)->pool[idx].size;
    b->desc.objects[0].format_modifier = DRM_FORMAT_MOD_LINEAR;
    b->desc.nb_layers = 1;
    b->desc.layers[0].format = DRM_FORMAT_YUV420;
    b->desc.layers[0].nb_planes = 3;
    b->desc.layers[0].planes[0].object_index = 0;
    b->desc.layers[0].planes[0].offset = 0;
    b->desc.layers[0].planes[0].pitch = bpl;
    b->desc.layers[0].planes[1].object_index = 0;
    b->desc.layers[0].planes[1].offset = ysz;
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
    out->width = wo; out->height = ho;
    av_frame_copy_props(out, in);
    out->crop_top = out->crop_left = out->crop_bottom = out->crop_right = 0;
    {
        FilterLink *outl = ff_filter_link(outlink);
        if (outl->hw_frames_ctx)
            out->hw_frames_ctx = av_buffer_ref(outl->hw_frames_ctx);
    }
    av_frame_free(&in);
    return ff_filter_frame(outlink, out);

fail_release:
    {
        SandPool *sp = (SandPool *)s->pool_ref->data;
        ff_mutex_lock(&sp->lock);
        if (idx >= 0) sp->pool[idx].in_use = 0;
        ff_mutex_unlock(&sp->lock);
    }
fail:
    if (b) av_buffer_unref(&b->pool_ref);
    av_free(b);
    av_frame_free(&out);
    av_frame_free(&in);
    return rv;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *avctx = inlink->dst;
    BridgeContext *s = avctx->priv;
    AVFilterLink *outlink = avctx->outputs[0];
    AVFrame *mapped = NULL, *tmp = NULL, *out = NULL;
    OutBuf *b = NULL;
    int idx = -1, fd = -1;
    int sync_fds[AV_DRM_MAX_PLANES], n_sync = -1;
    void *map = NULL;
    int rv;
    int64_t _t0;

    /* Software-input fallback (mid-stream HW->SW): handled separately, emitting
     * the same DRM_PRIME YU12 so the hardware tail is unchanged. */
    if (in->format != AV_PIX_FMT_DRM_PRIME)
        return filter_frame_sw(inlink, in);

    if (prof_on < 0) prof_on = !!getenv("SAND_PROF");
    _t0 = prof_on ? prof_now() : 0;

    if (!(mapped = av_frame_alloc())) { rv = AVERROR(ENOMEM); goto fail; }
    n_sync = map_input_cached(s, in, mapped, sync_fds);
    if (n_sync > 0) {
        /* cached fast path: persistent mmap, invalidate for this frame's read */
        for (int i = 0; i < n_sync; i++)
            dmabuf_sync(sync_fds[i], DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    } else {
        /* fail-safe: fresh per-frame map (mmap + invalidate + munmap on free) */
        mapped->format = AV_PIX_FMT_NONE;
        if ((rv = av_hwframe_map(mapped, in, AV_HWFRAME_MAP_READ)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "hwframe_map(READ) failed: %s\n", av_err2str(rv));
            goto fail;
        }
    }
    /* Both mapping paths must yield a usable two-plane SAND view. A decoder that
     * hit a dma-buf allocation failure (CMA exhaustion, e.g. a second concurrent
     * 4K session) can still emit a frame whose backing buffer never materialised;
     * unpacking that would read from a NULL/short mapping and segfault. Fail the
     * frame instead — the caller drops it and the transcode continues or exits
     * cleanly. */
    if (!mapped->data[0] || !mapped->data[1] || mapped->linesize[0] <= 0) {
        av_log(avctx, AV_LOG_ERROR,
               "input frame has no usable mapping (decoder buffer allocation failed?)\n");
        rv = AVERROR(EINVAL);
        goto fail;
    }
    mapped->crop_top = in->crop_top;   mapped->crop_bottom = in->crop_bottom;
    mapped->crop_left = in->crop_left;  mapped->crop_right = in->crop_right;
    PROF(0);

    const unsigned w = av_frame_cropped_width(mapped);
    const unsigned h = av_frame_cropped_height(mapped);
    /* out=half: emit input/2 (fused 2x2 downscale in the apply), dropping the ISP scale.
     * Only for exact 2:1 (w,h multiple of 4); else full size + downstream ISP resize. */
    const int half = s->out_half && (w % 4 == 0) && (h % 4 == 0);
    const unsigned wo = half ? w / 2 : w, ho = half ? h / 2 : h;
    const unsigned bpl = wo;  /* YU12 pitch == width (ISP/encoder derive width from Y pitch) */
    const size_t ysz = (size_t)bpl * ho, csz = ysz / 4, total = ysz + 2 * csz;

    if ((idx = pool_acquire(s, total, &fd, &map)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "dma-buf pool exhausted/alloc failed\n");
        rv = AVERROR(ENOMEM); goto fail;
    }

    if (!(tmp = av_frame_alloc())) { rv = AVERROR(ENOMEM); goto fail_release; }
    tmp->format = AV_PIX_FMT_YUV420P; tmp->width = wo; tmp->height = ho;
    tmp->data[0] = (uint8_t *)map;             tmp->linesize[0] = bpl;
    tmp->data[1] = (uint8_t *)map + ysz;       tmp->linesize[1] = bpl / 2;
    tmp->data[2] = (uint8_t *)map + ysz + csz; tmp->linesize[2] = bpl / 2;

    /* Leave one core for the (parallel) decode/encode threads: on the 4-core
     * Pi, using all cores for the unpack oversubscribes and slows the whole
     * pipeline (~5%). The unpack is memory-latency-bound, so it reaches the
     * shared-bus ceiling below core count anyway. */
    int pool = ff_filter_get_nb_threads(avctx);
    int nb = FFMIN(pool > 1 ? pool - 1 : 1, (int)(h / 2));
    nb = av_clip(nb, 1, 64);
    int rets[64] = { 0 };
    /* Dolby Vision profile 5: RPU present + base layer NOT HDR10-tagged. Its base
     * is Dolby's reshaped IPT-PQ signal, so it must be reconstructed to HDR10 (via
     * the per-RPU 3D LUT) before tone-mapping — the generic PQ path corrupts colour.
     * Engage regardless of `tm` (even tm=none) so P5 never passes through wrong. */
    const AVFrameSideData *dovi_sd = av_frame_get_side_data(in, AV_FRAME_DATA_DOVI_METADATA);
    const int is_p5 = dovi_sd && !frame_is_hdr(in);
    const int do_tm = is_p5 || ((s->tm != TM_NONE) && frame_is_hdr(in));
    /* HLG (ARIB_STD_B67) uses a different transfer than PQ, so its tone-map LUTs are
     * baked separately (ff_rpi_tm_*_hlg). Select by the frame's transfer. (P5 never
     * reaches here as HLG — is_p5 requires !frame_is_hdr; and its own path uses p5_lut.) */
    const int is_hlg = in->color_trc == AVCOL_TRC_ARIB_STD_B67;

    /* Effective apply tier for this frame. On the out=half path the nn+dither chroma
     * bands (the ISP no longer averages it back), so veryfast collapses to the tetra
     * P5_FAST; SDR/passthrough (only routed through the scratch path when half) is TM_NONE. */
    int eff_tm;
    if (!do_tm)      eff_tm = TM_NONE;
    else if (is_p5)  eff_tm = half ? ((s->tm == TM_FAST || s->tm == TM_VERYFAST) ? TM_P5_FAST : TM_P5)
                                   : (s->tm == TM_VERYFAST ? TM_P5_VERYFAST :
                                      s->tm == TM_FAST     ? TM_P5_FAST     : TM_P5);
    else             eff_tm = (s->tm == TM_VERYFAST) ? TM_FAST : s->tm;

    if (is_p5) {
        const AVDOVIMetadata *meta = (const AVDOVIMetadata *)dovi_sd->data;
        uint64_t hash = p5_calc_hash(meta);
        if (!s->p5_valid || hash != s->p5_hash) {   /* rebuild on scene/RPU change */
            if ((rv = p5_bake(s, meta)) < 0) goto fail_release;
            s->p5_hash = hash;
            av_log(avctx, AV_LOG_VERBOSE, "DV P5: baked base->SDR 3D LUT (RPU %016"PRIx64")\n", hash);
        }
    }

    /* Peak-aware tone-map (plain PQ/HDR10 only): if the authored source peak differs from
     * the baked 1000-nit tables, regenerate the PQ LUTs for the real peak — once per stream,
     * and again whenever the peak changes (e.g. a spliced/live stream whose mastering/MaxCLL
     * SEI updates at a new coded-video-sequence). Rebuild-on-change is keyed on gen_peak, like
     * the DV-P5 RPU rebuild above; the HEVC decoder makes the SEI sticky per-CVS, so a frame
     * mid-segment reports its segment's peak (no per-frame thrash — see hevcdec set_side_data).
     * Untagged PQ and ~1000-nit content keep the baked tables. */
    int use_gen = 0;
    if (do_tm && !is_p5 && !is_hlg) {
        int peak_nits = s->peak_opt;
        if (peak_nits <= 0) {
            const AVFrameSideData *cll = av_frame_get_side_data(in, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
            const AVFrameSideData *mdm = av_frame_get_side_data(in, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
            if (cll) peak_nits = ((const AVContentLightMetadata *)cll->data)->MaxCLL;
            if (peak_nits <= 0 && mdm) {
                const AVMasteringDisplayMetadata *m = (const AVMasteringDisplayMetadata *)mdm->data;
                if (m->has_luminance) peak_nits = lrint(av_q2d(m->max_luminance));
            }
            if (peak_nits <= 0) peak_nits = 1000;   /* untagged: keep baked (no darkening) */
        }
        /* Explicit peak= always (re)generates (honour the override, and lets peak=1000
         * validate that generation reproduces the baked tables); auto-detected peaks within
         * ~1000 nits keep the baked tables. */
        if (s->peak_opt > 0 || abs(peak_nits - 1000) > 50) {
#if CONFIG_ZSCALE_FILTER && CONFIG_TONEMAP_FILTER
            if (s->gen_peak != peak_nits) {
                if (pq_lut_regen(avctx, s, peak_nits) < 0) {
                    av_log(avctx, AV_LOG_WARNING,
                           "peak-aware tone-map LUT gen failed for %d nits; using 1000-nit tables\n", peak_nits);
                    s->gen_peak = 0;
                } else {
                    av_log(avctx, AV_LOG_VERBOSE,
                           "tone-map: generated LUTs for source peak %d nits\n", peak_nits);
                }
            }
            use_gen = (s->gen_peak == peak_nits);
#else
            if (!s->warned_nopeak) {
                av_log(avctx, AV_LOG_WARNING,
                       "source peak %d nits: peak-aware tone-map needs an ffmpeg built with "
                       "zscale+tonemap; using 1000-nit tables (highlights >1000 nits roll off)\n", peak_nits);
                s->warned_nopeak = 1;
            }
#endif
        }
    }

    if (!do_tm && !half) {
        /* SDR / tm=none, full size: single-pass SAND -> 8-bit YU12. */
        ThreadData td = { .dst = tmp, .src = mapped, .H = h };
        dmabuf_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
        ff_filter_execute(avctx, unpack_slice, &td, rets, nb);
        PROF(1);
        dmabuf_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
        PROF(2);
        for (int i = 0; i < nb; i++)
            if (rets[i] != 0) {
                av_log(avctx, AV_LOG_ERROR, "sand->planar failed (fmt %d)\n", mapped->format);
                rv = AVERROR(EINVAL); goto fail_release;
            }
    } else {
        /* Tone-map (or SDR when out=half): cache-tiled SAND -> 10-bit scratch ->
         * [2x2 box-average when half] -> LUT/narrow -> 8-bit YU12. */
        TMData tdm = { .dst = tmp, .src = mapped, .H = h, .half = half,
                       .tm = eff_tm,
                       .l64      = is_hlg ? s->l64_hlg      : (use_gen ? s->g_l64      : s->l64),
                       .cb64     = is_hlg ? s->cb64_hlg     : (use_gen ? s->g_cb64     : s->cb64),
                       .cr64     = is_hlg ? s->cr64_hlg     : (use_gen ? s->g_cr64     : s->cr64),
                       .l64_next = is_hlg ? s->l64_next_hlg : (use_gen ? s->g_l64_next : s->l64_next),
                       .luma1d = is_hlg ? ff_rpi_tm_luma1d_hlg : (use_gen ? s->g_luma1d : ff_rpi_tm_luma1d),
                       .cb1d   = is_hlg ? ff_rpi_tm_cb1d_hlg   : (use_gen ? s->g_cb1d   : ff_rpi_tm_cb1d),
                       .cr1d   = is_hlg ? ff_rpi_tm_cr1d_hlg   : (use_gen ? s->g_cr1d   : ff_rpi_tm_cr1d),
                       .lut3d  = is_hlg ? ff_rpi_tm_lut3d_hlg  : (use_gen ? s->g_lut3d  : ff_rpi_tm_lut3d),
                       .p5_lut = s->p5_lut, .p5_maxc = s->p5_maxc,
                       .p5_luma1d = s->p5_luma1d, .p5_l64 = s->p5_l64 };
        dmabuf_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
        ff_filter_execute(avctx, tm_slice, &tdm, rets, nb);
        PROF(1);
        dmabuf_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
        PROF(2);
        for (int i = 0; i < nb; i++)
            if (rets[i] != 0) {
                av_log(avctx, AV_LOG_ERROR, "tone-map failed (fmt %d)\n", mapped->format);
                rv = AVERROR(EINVAL); goto fail_release;
            }
    }

    if (!(b = av_mallocz(sizeof(*b)))) { rv = AVERROR(ENOMEM); goto fail_release; }
    b->idx = idx;
    if (!(b->pool_ref = av_buffer_ref(s->pool_ref))) { rv = AVERROR(ENOMEM); goto fail_release; }
    b->desc.nb_objects = 1;
    b->desc.objects[0].fd = fd;
    b->desc.objects[0].size = ((SandPool *)s->pool_ref->data)->pool[idx].size;
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
    out->width = wo; out->height = ho;
    av_frame_copy_props(out, in);
    out->crop_top = out->crop_left = out->crop_bottom = out->crop_right = 0;
    /* Use our own YU12 frames context (built in config_output) so the frame's
     * sw_format matches its actual 8-bit planar contents, not the RPI4_10 input. */
    FilterLink *outl = ff_filter_link(outlink);
    if (outl->hw_frames_ctx)
        out->hw_frames_ctx = av_buffer_ref(outl->hw_frames_ctx);
    else if (in->hw_frames_ctx)
        out->hw_frames_ctx = av_buffer_ref(in->hw_frames_ctx);
    PROF(3);

    for (int i = 0; i < n_sync; i++)
        dmabuf_sync(sync_fds[i], DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    av_frame_free(&mapped);   /* cached: frees struct only (no buf); fallback: unmaps */
    PROF(4);
    av_frame_free(&tmp);
    av_frame_free(&in);

    if (prof_on && ++prof_frames % 100 == 0) {
        av_log(avctx, AV_LOG_INFO, "SAND_PROF us/frame:");
        for (int i = 0; i < 5; i++) {
            av_log(avctx, AV_LOG_INFO, " %s=%.0f", prof_name[i], prof_ns[i] / 100.0 / 1000.0);
            prof_ns[i] = 0;
        }
        av_log(avctx, AV_LOG_INFO, " [mmaps=%u]\n", s->mmap_count);
    }
    return ff_filter_frame(outlink, out);

fail_release:
    {
        SandPool *sp = (SandPool *)s->pool_ref->data;
        ff_mutex_lock(&sp->lock);
        if (idx >= 0) sp->pool[idx].in_use = 0;
        ff_mutex_unlock(&sp->lock);
    }
fail:
    if (b) av_buffer_unref(&b->pool_ref);
    av_free(b);
    av_frame_free(&mapped);
    av_frame_free(&tmp);
    av_frame_free(&out);
    av_frame_free(&in);
    return rv;
}

#define OFFSET(x) offsetof(BridgeContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM)
static const AVOption sand_to_yuv420p_drm_options[] = {
    { "tm", "HDR->SDR tone-map (HDR10 sources only; SDR passes through)", OFFSET(tm),
      AV_OPT_TYPE_INT, { .i64 = TM_NONE }, TM_NONE, TM_ACCURATE, FLAGS, .unit = "tm" },
        { "none",     "no tone-map (10->8 truncation)",       0, AV_OPT_TYPE_CONST, { .i64 = TM_NONE },     0, 0, FLAGS, .unit = "tm" },
        { "fast",     "separable, real-time, colour approx.", 0, AV_OPT_TYPE_CONST, { .i64 = TM_FAST },     0, 0, FLAGS, .unit = "tm" },
        { "veryfast", "like fast; DV P5 uses nearest chroma (faster, colour-approx)", 0, AV_OPT_TYPE_CONST, { .i64 = TM_VERYFAST }, 0, 0, FLAGS, .unit = "tm" },
        { "accurate", "3D-LUT, luma-aware (matches zscale)",  0, AV_OPT_TYPE_CONST, { .i64 = TM_ACCURATE }, 0, 0, FLAGS, .unit = "tm" },
    { "out", "output size: full, or half (emit input/2 directly, no ISP scale; needs exact 2:1)", OFFSET(out_half),
      AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, FLAGS, .unit = "out" },
        { "full", "same size as input (downstream ISP does any resize)", 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 0, FLAGS, .unit = "out" },
        { "half", "half width+height (fused 2x2 downscale; skip scale_v4l2m2m)", 0, AV_OPT_TYPE_CONST, { .i64 = 1 }, 0, 0, FLAGS, .unit = "out" },
    { "peak", "override HDR source peak luminance in nits for the PQ tone-map (0 = auto from mastering/MaxCLL)",
      OFFSET(peak_opt), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 100000, FLAGS },
    { NULL }
};
AVFILTER_DEFINE_CLASS(sand_to_yuv420p_drm);

/* Input: DRM_PRIME SAND (the rpivid HW path) OR software yuv420p (the mid-stream
 * HW->SW fallback — the decoder drops to software for formats rpivid can't handle
 * and fftools auto-converts them to yuv420p ahead of us). Output is always the
 * DRM_PRIME YU12 the ISP/encoder consume. Advertising yuv420p input lets the
 * filtergraph re-negotiate across a HW->SW switch instead of failing; the HW path
 * still negotiates DRM_PRIME (it matches the decoder output, no conversion). */
static int query_formats(const AVFilterContext *avctx,
                         AVFilterFormatsConfig **cfg_in,
                         AVFilterFormatsConfig **cfg_out)
{
    static const enum AVPixelFormat in_fmts[]  =
        { AV_PIX_FMT_DRM_PRIME, AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };
    static const enum AVPixelFormat out_fmts[] =
        { AV_PIX_FMT_DRM_PRIME, AV_PIX_FMT_NONE };
    int ret;
    if ((ret = ff_formats_ref(ff_make_format_list(in_fmts),  &cfg_in[0]->formats))  < 0 ||
        (ret = ff_formats_ref(ff_make_format_list(out_fmts), &cfg_out[0]->formats)) < 0)
        return ret;
    return 0;
}

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
    .p.flags       = AVFILTER_FLAG_SLICE_THREADS,
    /* We emit a different sw_format (YUV420P) than we consume (RPI4_10), so we
     * set the output link's hw_frames_ctx ourselves in config_output rather than
     * letting the framework propagate the input's. That requires this flag. */
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
    .priv_size     = sizeof(BridgeContext),
    .init          = init,
    .uninit        = uninit,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(outputs),
    FILTER_QUERY_FUNC2(query_formats),
};
