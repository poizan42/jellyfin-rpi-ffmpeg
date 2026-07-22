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
#include <linux/dma-buf.h>
#include <drm.h>
#include <libdrm/drm_fourcc.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

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
#include "libavutil/opt.h"
#include "rpi_tonemap_tables.h"   /* ff_rpi_tm_luma1d/cb1d/cr1d, ff_rpi_tm_lut3d, RPI_TM_LUT3D_N */

enum { TM_NONE = 0, TM_FAST, TM_ACCURATE };

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

/* Lever 2: persistent fd-keyed mmap cache for the decoder's input SAND buffers.
 * The decoder recycles a fixed set of dma-buf fds, so mapping them once (instead
 * of a fresh mmap+munmap of ~16 MB per frame in av_hwframe_map) removes that
 * per-frame churn; only the mandatory cache-invalidate stays. filter_frame is
 * single-threaded, so the cache needs no lock. */
#define MAP_CACHE_N 32
typedef struct MapEnt { int fd; void *addr; size_t size; } MapEnt;

typedef struct BridgeContext {
    const AVClass *class;
    int      heap_fd;
    AVMutex  lock;
    PoolBuf  pool[POOL_N];
    MapEnt   mcache[MAP_CACHE_N];
    int      mcache_n;
    unsigned mmap_count;   /* SAND_PROF: distinct input buffers mmap'd (should plateau) */
    int      tm;           /* TM_NONE / TM_FAST / TM_ACCURATE (option) */
    uint8_t  l64[64], cb64[64], cr64[64];  /* 64-entry tbl LUTs (subsampled at init) */
    uint8_t  l64_next[64];                 /* l64_next[i]=curve((i+1)*16); for the single-pass .S kernel */
} BridgeContext;

/* Return a persistent read-only mapping of (fd,size), or NULL to signal the
 * caller to fall back to av_hwframe_map (cache full / mmap failed). */
static void *map_cached(BridgeContext *s, int fd, size_t size)
{
    for (int i = 0; i < s->mcache_n; i++)
        if (s->mcache[i].fd == fd && s->mcache[i].size == size)
            return s->mcache[i].addr;
    if (s->mcache_n >= MAP_CACHE_N)
        return NULL;
    void *m = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED)
        return NULL;
    s->mcache[s->mcache_n++] = (MapEnt){ fd, m, size };
    s->mmap_count++;
    return m;
}

/* Build a SAND `mapped` view of the input DRM_PRIME frame from the cached
 * mapping (replicates hwcontext_drm.c drm_map_frame's plane + stride rework).
 * Returns the fd to SYNC(END|READ) after use, or -1 to use the fallback path. */
static int map_input_cached(BridgeContext *s, const AVFrame *in, AVFrame *mapped)
{
    if (in->format != AV_PIX_FMT_DRM_PRIME || !in->hw_frames_ctx)
        return -1;
    const AVDRMFrameDescriptor *desc = (const AVDRMFrameDescriptor *)in->data[0];
    if (!desc || desc->nb_objects != 1)
        return -1;
    void *base = map_cached(s, desc->objects[0].fd, desc->objects[0].size);
    if (!base)
        return -1;

    mapped->format = ((AVHWFramesContext *)in->hw_frames_ctx->data)->sw_format;
    mapped->width  = in->width;
    mapped->height = in->height;
    int plane = 0;
    for (int i = 0; i < desc->nb_layers; i++) {
        const AVDRMLayerDescriptor *layer = &desc->layers[i];
        for (int p = 0; p < layer->nb_planes; p++) {
            mapped->data[plane]     = (uint8_t *)base + layer->planes[p].offset;
            mapped->linesize[plane] = layer->planes[p].pitch;
            plane++;
        }
    }
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
    return desc->objects[0].fd;
}

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
    for (int i = 0; i < 64; i++) {   /* subsample the 1024-entry curves at code = i*16 */
        s->l64[i]      = ff_rpi_tm_luma1d[i * 16];
        s->l64_next[i] = ff_rpi_tm_luma1d[FFMIN((i + 1) * 16, 1023)];
        s->cb64[i] = ff_rpi_tm_cb1d[i * 16];
        s->cr64[i] = ff_rpi_tm_cr1d[i * 16];
    }
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
    for (int i = 0; i < s->mcache_n; i++)
        munmap(s->mcache[i].addr, s->mcache[i].size);
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

    /* Our output is 8-bit planar YU12 (DRM_PRIME).  Advertise a frames context
     * that says so: the input's context is RPI4_10 (10-bit SAND), and reusing
     * it would mislabel the output — a consumer that trusts sw_format (e.g.
     * hwdownload) would then SAND-unpack the already-planar 8-bit data and read
     * far past the buffer.  Build a fresh DRM/YUV420P context on the same device. */
    FilterLink *inl  = ff_filter_link(inlink);
    FilterLink *outl = ff_filter_link(outlink);
    av_buffer_unref(&outl->hw_frames_ctx);
    if (inl->hw_frames_ctx) {
        AVHWFramesContext *in_fc = (AVHWFramesContext *)inl->hw_frames_ctx->data;
        AVBufferRef *out_ref = av_hwframe_ctx_alloc(in_fc->device_ref);
        if (!out_ref)
            return AVERROR(ENOMEM);
        AVHWFramesContext *out_fc = (AVHWFramesContext *)out_ref->data;
        out_fc->format    = AV_PIX_FMT_DRM_PRIME;
        out_fc->sw_format = AV_PIX_FMT_YUV420P;
        out_fc->width     = inlink->w;
        out_fc->height    = inlink->h;
        int ret = av_hwframe_ctx_init(out_ref);
        if (ret < 0) {
            av_buffer_unref(&out_ref);
            return ret;
        }
        outl->hw_frames_ctx = out_ref;
    }
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
#define TM_CHUNK 16   /* rows per tile; scratch (10-bit) stays in L2 */
typedef struct TMData {
    AVFrame       *dst;   /* 8-bit YU12 (dma-buf) */
    const AVFrame *src;   /* SAND mapped frame    */
    unsigned       H;     /* luma height          */
    int            tm;
    const uint8_t *l64, *cb64, *cr64;  /* 64-entry tbl LUTs (fast NEON path) */
    const uint8_t *l64_next;           /* pairs with l64 for the single-pass .S kernel */
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
                               unsigned x0, unsigned cw)
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
#define C3(iy,iu,iv) (&ff_rpi_tm_lut3d[(((iy)*N+(iu))*N+(iv))*3])
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
    int32_t MY, int32_t MC, int32x4_t v256, int32x4_t v64, int32x4_t v32768, int32x4_t vLIMQ,
    int32x4_t vzero, int32x4_t vNm1, int32x4_t vone, int32x4_t v255, int32x4_t vN, int32x4_t vST,
    int32x4_t *po0, int32x4_t *poa, int32x4_t *pob, int32x4_t *po3,
    int32x4_t *pw0, int32x4_t *pw1, int32x4_t *pw2, int32x4_t *pw3)
{
    int32x4_t ybq=vshrq_n_s32(vaddq_s32(vmulq_s32(vsubq_s32(Y ,v256),vdupq_n_s32(MY)),v32768),16);
    int32x4_t ubq=vshrq_n_s32(vaddq_s32(vmulq_s32(vsubq_s32(Uu,v64 ),vdupq_n_s32(MC)),v32768),16);
    int32x4_t vbq=vshrq_n_s32(vaddq_s32(vmulq_s32(vsubq_s32(Vv,v64 ),vdupq_n_s32(MC)),v32768),16);
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
                            const uint16_t *U, const uint16_t *V, unsigned cw)
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
    tm3d_calc4(Yv,Uv,Vv,MY,MC,v256,v64,v32768,vLIMQ,vzero,vNm1,vone,v255,vN,vST, \
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
            memcpy(&g0[i], ff_rpi_tm_lut3d+o0[i]+1, 2); memcpy(&ga[i], ff_rpi_tm_lut3d+oa[i]+1, 2);
            memcpy(&gb[i], ff_rpi_tm_lut3d+ob[i]+1, 2); memcpy(&g3[i], ff_rpi_tm_lut3d+o3[i]+1, 2);
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
    tm3d_chroma_scalar(OU, OV, Y0, Y1, U, V, x, cw);   /* tail / non-NEON fallback */
}

static void tm_apply_chunk(const TMData *td, const AVFrame *S, unsigned ybase, unsigned ch)
{
    const unsigned W = td->dst->width, cw = W / 2;
    AVFrame *D = td->dst;
    const int sy = S->linesize[0] / 2, su = S->linesize[1] / 2, sv = S->linesize[2] / 2;
    for (unsigned r = 0; r < ch; r++) {
        const uint16_t *Y = (const uint16_t *)S->data[0] + (size_t)r * sy;
        uint8_t *O = D->data[0] + (size_t)(ybase + r) * D->linesize[0];
        lut1d_apply(O, Y, W, td->l64, ff_rpi_tm_luma1d);
    }
    if (td->tm == TM_FAST) {
        for (unsigned cr = 0; cr < ch / 2; cr++) {
            const uint16_t *U = (const uint16_t *)S->data[1] + (size_t)cr * su;
            const uint16_t *V = (const uint16_t *)S->data[2] + (size_t)cr * sv;
            uint8_t *OU = D->data[1] + (size_t)(ybase/2 + cr) * D->linesize[1];
            uint8_t *OV = D->data[2] + (size_t)(ybase/2 + cr) * D->linesize[2];
            lut1d_apply(OU, U, cw, td->cb64, ff_rpi_tm_cb1d);
            lut1d_apply(OV, V, cw, td->cr64, ff_rpi_tm_cr1d);
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
            tm3d_chroma_row(OU, OV, Y0, Y1, U, V, cw);
            if (selfcheck) {   /* DEBUG: NEON vs scalar on real frames, expect max 0 */
                uint8_t *ru = av_malloc(cw), *rv = av_malloc(cw);
                if (ru && rv) {
                    unsigned mx = 0;
                    tm3d_chroma_scalar(ru, rv, Y0, Y1, U, V, 0, cw);
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

static int tm_slice(AVFilterContext *avctx, void *arg, int jobnr, int nb_jobs)
{
    const TMData *td = arg;
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
                lut1d_apply(OU, su + (size_t)r * cw, cw, td->cb64, ff_rpi_tm_cb1d);
                lut1d_apply(OV, sv + (size_t)r * cw, cw, td->cr64, ff_rpi_tm_cr1d);
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
        if (!rv) tm_apply_chunk(td, &s10, y, ch);       /* 10-bit -> 8-bit YU12       */
    }
    av_free(sy); av_free(su); av_free(sv);
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
    int sync_fd = -1;
    void *map = NULL;
    int rv;
    int64_t _t0;

    if (prof_on < 0) prof_on = !!getenv("SAND_PROF");
    _t0 = prof_on ? prof_now() : 0;

    if (!(mapped = av_frame_alloc())) { rv = AVERROR(ENOMEM); goto fail; }
    sync_fd = map_input_cached(s, in, mapped);
    if (sync_fd >= 0) {
        /* cached fast path: persistent mmap, invalidate for this frame's read */
        dmabuf_sync(sync_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    } else {
        /* fail-safe: fresh per-frame map (mmap + invalidate + munmap on free) */
        mapped->format = AV_PIX_FMT_NONE;
        if ((rv = av_hwframe_map(mapped, in, AV_HWFRAME_MAP_READ)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "hwframe_map(READ) failed: %s\n", av_err2str(rv));
            goto fail;
        }
    }
    mapped->crop_top = in->crop_top;   mapped->crop_bottom = in->crop_bottom;
    mapped->crop_left = in->crop_left;  mapped->crop_right = in->crop_right;
    PROF(0);

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

    /* Leave one core for the (parallel) decode/encode threads: on the 4-core
     * Pi, using all cores for the unpack oversubscribes and slows the whole
     * pipeline (~5%). The unpack is memory-latency-bound, so it reaches the
     * shared-bus ceiling below core count anyway. */
    int pool = ff_filter_get_nb_threads(avctx);
    int nb = FFMIN(pool > 1 ? pool - 1 : 1, (int)(h / 2));
    nb = av_clip(nb, 1, 64);
    int rets[64] = { 0 };
    const int do_tm = (s->tm != TM_NONE) && frame_is_hdr(in);

    if (!do_tm) {
        /* SDR / tm=none: single-pass SAND -> 8-bit YU12. */
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
        /* HDR tone-map: cache-tiled SAND -> 10-bit scratch -> LUT -> 8-bit YU12
         * (single-pass DRAM traffic; scratch stays L2-resident). */
        TMData tdm = { .dst = tmp, .src = mapped, .H = h, .tm = s->tm,
                       .l64 = s->l64, .cb64 = s->cb64, .cr64 = s->cr64,
                       .l64_next = s->l64_next };
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
    /* Use our own YU12 frames context (built in config_output) so the frame's
     * sw_format matches its actual 8-bit planar contents, not the RPI4_10 input. */
    FilterLink *outl = ff_filter_link(outlink);
    if (outl->hw_frames_ctx)
        out->hw_frames_ctx = av_buffer_ref(outl->hw_frames_ctx);
    else if (in->hw_frames_ctx)
        out->hw_frames_ctx = av_buffer_ref(in->hw_frames_ctx);
    PROF(3);

    if (sync_fd >= 0) dmabuf_sync(sync_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
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

#define OFFSET(x) offsetof(BridgeContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM)
static const AVOption sand_to_yuv420p_drm_options[] = {
    { "tm", "HDR->SDR tone-map (HDR10 sources only; SDR passes through)", OFFSET(tm),
      AV_OPT_TYPE_INT, { .i64 = TM_NONE }, TM_NONE, TM_ACCURATE, FLAGS, .unit = "tm" },
        { "none",     "no tone-map (10->8 truncation)",       0, AV_OPT_TYPE_CONST, { .i64 = TM_NONE },     0, 0, FLAGS, .unit = "tm" },
        { "fast",     "separable, real-time, colour approx.", 0, AV_OPT_TYPE_CONST, { .i64 = TM_FAST },     0, 0, FLAGS, .unit = "tm" },
        { "accurate", "3D-LUT, luma-aware (matches zscale)",  0, AV_OPT_TYPE_CONST, { .i64 = TM_ACCURATE }, 0, 0, FLAGS, .unit = "tm" },
    { NULL }
};
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
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_DRM_PRIME),
};
