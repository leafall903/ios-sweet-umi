#ifndef UMI_GPU_SMMU_GUARD_H
#define UMI_GPU_SMMU_GUARD_H
#include <stdint.h>
#include <stddef.h>

/* Guard for the GPU SMMU only (0x03da0000), not the UFS SMMU.
 * rd/wr must perform one 32-bit MMIO access; caller holds its GPU lock.
 * Zero-initialize the snapshot. acquire is read-only. Mark dirty immediately
 * BEFORE the first driver write; never reacquire a dirty snapshot.
 * restore requires the caller to have stopped/reset GMU/CP and drained DMA.
 * W1C fault flags and historical TLB contents cannot be restored. Existing
 * context fault flags are rejected; restore invalidates relevant ASIDs.
 */
enum UmiGpuSmmuResult {
    UMI_GPU_SMMU_OK = 0,
    UMI_GPU_SMMU_ARGUMENT,
    UMI_GPU_SMMU_GEOMETRY,
    UMI_GPU_SMMU_RANGE,
    UMI_GPU_SMMU_TARGET_VALID,
    UMI_GPU_SMMU_STREAM_OVERLAP,
    UMI_GPU_SMMU_TARGET_REFERENCED,
    UMI_GPU_SMMU_CONTEXT_ENABLED,
    UMI_GPU_SMMU_PENDING_FAULT,
    UMI_GPU_SMMU_NOT_QUIESCED,
    UMI_GPU_SMMU_OWNERSHIP_CHANGED,
    UMI_GPU_SMMU_TLB_TIMEOUT,
    UMI_GPU_SMMU_RESTORE_MISMATCH,
    UMI_GPU_SMMU_GLOBAL_FAULT
};
struct UmiGpuSmmuIo {
    void *cookie;
    uint32_t (*rd)(void *, uint32_t);
    void (*wr)(void *, uint32_t, uint32_t);
    void (*barrier)(void *);
    void (*relax)(void *); /* optional bounded polling delay */
};
struct UmiGpuSmmuDiagnostic {
    enum UmiGpuSmmuResult result;
    uint32_t index, observed, expected;
};
#define UMI_GPU_SMMU_CB_FIELDS 9u
static const uint32_t umi_gpu_smmu_cb_offsets[UMI_GPU_SMMU_CB_FIELDS] = {
    0x00u, 0x10u, 0x20u, 0x24u, 0x28u, 0x2cu, 0x30u, 0x38u, 0x3cu
};
struct UmiGpuSmmuSnapshot {
    uint32_t span, page, pages, contexts, matches;
    uint32_t idr0, idr1, global_fault;
    uint32_t smr[2], s2cr[2], cbar[2], cba2r[2];
    uint32_t cb[2][UMI_GPU_SMMU_CB_FIELDS];
    uint32_t acquired, dirty;
};
static inline enum UmiGpuSmmuResult
umi_gpu_smmu_result(struct UmiGpuSmmuDiagnostic *d, enum UmiGpuSmmuResult r,
    uint32_t i, uint32_t got, uint32_t want)
{
    if (d) { d->result = r; d->index = i; d->observed = got; d->expected = want; }
    return r;
}
static inline int umi_gpu_smmu_has(uint32_t span, uint64_t off)
{
    return !(off & 3u) && off <= span && span - off >= 4u;
}
static inline int umi_gpu_smmu_overlap(uint32_t a, uint32_t b)
{
    uint32_t masks = ((a >> 16) | (b >> 16)) & 0x7fffu;
    return (((a ^ b) & 0x7fffu) & ~masks) == 0;
}
static inline uint32_t umi_gpu_smmu_wanted_smr(unsigned i)
{
    return i ? 0x84000005u : 0x84010000u;
}
static inline enum UmiGpuSmmuResult
umi_gpu_smmu_guard_acquire(struct UmiGpuSmmuSnapshot *s,
    const struct UmiGpuSmmuIo *io, uint32_t span, struct UmiGpuSmmuDiagnostic *d)
{
    if (!s || !io || !io->rd || !io->wr || !io->barrier || span < 0x1000u)
        return umi_gpu_smmu_result(d, UMI_GPU_SMMU_ARGUMENT, 0, span, 0x1000u);
    if (s->acquired && s->dirty)
        return umi_gpu_smmu_result(d, UMI_GPU_SMMU_ARGUMENT, 0, 1, 0);
    s->acquired = s->dirty = 0;
    s->span = span;
    s->idr0 = io->rd(io->cookie, 0x20u);
    s->idr1 = io->rd(io->cookie, 0x24u);
    s->page = (s->idr1 >> 31) ? 0x10000u : 0x1000u;
    s->pages = 2u << ((s->idr1 >> 28) & 7u);
    s->contexts = s->idr1 & 0xffu;
    s->matches = s->idr0 & 0xffu;
    if ((uint64_t)2u * s->pages * s->page != span ||
        s->contexts < 2u || s->matches < 2u)
        return umi_gpu_smmu_result(d, UMI_GPU_SMMU_GEOMETRY, 0, s->idr1, span);
    if (!umi_gpu_smmu_has(span, 0x800u + 4ull * (s->matches - 1u)) ||
        !umi_gpu_smmu_has(span, 0xc00u + 4ull * (s->matches - 1u)) ||
        !umi_gpu_smmu_has(span, (uint64_t)(s->pages + 1u) * s->page + 0x7f4u) ||
        !umi_gpu_smmu_has(span, (uint64_t)s->page + 0x804u))
        return umi_gpu_smmu_result(d, UMI_GPU_SMMU_RANGE, 0, s->page, span);
    s->global_fault = io->rd(io->cookie, 0x48u);
    if (s->global_fault)
        return umi_gpu_smmu_result(d, UMI_GPU_SMMU_GLOBAL_FAULT, 0, s->global_fault, 0);
    for (unsigned i = 0; i < 2; ++i) {
        s->smr[i] = io->rd(io->cookie, 0x800u + 4u * i);
        s->s2cr[i] = io->rd(io->cookie, 0xc00u + 4u * i);
        if (s->smr[i] & 0x80000000u)
            return umi_gpu_smmu_result(d, UMI_GPU_SMMU_TARGET_VALID, i, s->smr[i], 0);
    }
    for (unsigned i = 2; i < s->matches; ++i) {
        uint32_t smr = io->rd(io->cookie, 0x800u + 4u * i);
        if (!(smr & 0x80000000u)) continue;
        uint32_t route = io->rd(io->cookie, 0xc00u + 4u * i);
        if (umi_gpu_smmu_overlap(smr, umi_gpu_smmu_wanted_smr(0)) ||
            umi_gpu_smmu_overlap(smr, umi_gpu_smmu_wanted_smr(1)))
            return umi_gpu_smmu_result(d, UMI_GPU_SMMU_STREAM_OVERLAP, i, smr, route);
        if (((route >> 16) & 3u) == 0 && (route & 0xffu) < 2u)
            return umi_gpu_smmu_result(d, UMI_GPU_SMMU_TARGET_REFERENCED, i, smr, route);
    }
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t cb = (s->pages + i) * s->page;
        for (unsigned k = 0; k < UMI_GPU_SMMU_CB_FIELDS; ++k)
            s->cb[i][k] = io->rd(io->cookie, cb + umi_gpu_smmu_cb_offsets[k]);
        s->cbar[i] = io->rd(io->cookie, s->page + 4u * i);
        s->cba2r[i] = io->rd(io->cookie, s->page + 0x800u + 4u * i);
        if (s->cb[i][0] & 1u)
            return umi_gpu_smmu_result(d, UMI_GPU_SMMU_CONTEXT_ENABLED, i, s->cb[i][0], 0);
        uint32_t fsr = io->rd(io->cookie, cb + 0x58u);
        if (fsr & 0xc00001feu)
            return umi_gpu_smmu_result(d, UMI_GPU_SMMU_PENDING_FAULT, i, fsr, 0);
    }
    s->acquired = 1;
    return umi_gpu_smmu_result(d, UMI_GPU_SMMU_OK, 0, 0, 0);
}
static inline int umi_gpu_smmu_guard_mark_dirty(struct UmiGpuSmmuSnapshot *s)
{
    if (!s || !s->acquired) return 0;
    s->dirty = 1;
    return 1;
}
static inline int umi_gpu_smmu_sync(const struct UmiGpuSmmuIo *io, uint32_t cb, uint32_t asid)
{
    io->wr(io->cookie, cb + 0x610u, asid);
    io->wr(io->cookie, cb + 0x7f0u, 1u);
    io->barrier(io->cookie);
    for (unsigned n = 0; n < 10000u; ++n) {
        if (!(io->rd(io->cookie, cb + 0x7f4u) & 1u)) return 1;
        if (io->relax) io->relax(io->cookie);
    }
    return 0;
}
static inline enum UmiGpuSmmuResult
umi_gpu_smmu_guard_restore(struct UmiGpuSmmuSnapshot *s,
    const struct UmiGpuSmmuIo *io, int quiesced, struct UmiGpuSmmuDiagnostic *d)
{
    if (!s || !io || !io->rd || !io->wr || !io->barrier || !s->acquired)
        return umi_gpu_smmu_result(d, UMI_GPU_SMMU_ARGUMENT, 0, 0, 0);
    if (!s->dirty) return umi_gpu_smmu_result(d, UMI_GPU_SMMU_OK, 0, 0, 0);
    if (!quiesced) return umi_gpu_smmu_result(d, UMI_GPU_SMMU_NOT_QUIESCED, 0, 0, 1);
    /* Refuse to clear a route that another driver changed after acquisition. */
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t smr = io->rd(io->cookie, 0x800u + 4u * i);
        uint32_t route = io->rd(io->cookie, 0xc00u + 4u * i);
        if ((smr != s->smr[i] && smr != umi_gpu_smmu_wanted_smr(i) &&
             smr != (umi_gpu_smmu_wanted_smr(i) & 0x7fffffffu)) ||
            (route != s->s2cr[i] && route != i && route != (2u << 16)))
            return umi_gpu_smmu_result(d, UMI_GPU_SMMU_OWNERSHIP_CHANGED, i, smr, route);
    }
    for (unsigned i = 0; i < 2; ++i) {
        io->wr(io->cookie, 0x800u + 4u * i, io->rd(io->cookie, 0x800u + 4u * i) & 0x7fffffffu);
        uint32_t cb = (s->pages + i) * s->page;
        io->wr(io->cookie, cb, io->rd(io->cookie, cb) & ~1u);
    }
    io->barrier(io->cookie);
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t cb = (s->pages + i) * s->page;
        uint32_t current_asid = io->rd(io->cookie, cb + 0x24u) >> 16;
        if (!umi_gpu_smmu_sync(io, cb, current_asid))
            return umi_gpu_smmu_result(d, UMI_GPU_SMMU_TLB_TIMEOUT, i, 1, 0);
        io->wr(io->cookie, s->page + 0x800u + 4u * i, s->cba2r[i]);
        io->wr(io->cookie, s->page + 4u * i, s->cbar[i]);
        for (unsigned k = 1; k < UMI_GPU_SMMU_CB_FIELDS; ++k)
            io->wr(io->cookie, cb + umi_gpu_smmu_cb_offsets[k], s->cb[i][k]);
        if (!umi_gpu_smmu_sync(io, cb, s->cb[i][3] >> 16))
            return umi_gpu_smmu_result(d, UMI_GPU_SMMU_TLB_TIMEOUT, i, 1, 0);
        io->wr(io->cookie, 0xc00u + 4u * i, s->s2cr[i]);
        io->wr(io->cookie, 0x800u + 4u * i, s->smr[i]);
        io->wr(io->cookie, cb, s->cb[i][0]);
    }
    io->barrier(io->cookie);
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t cb = (s->pages + i) * s->page;
        for (unsigned k = 0; k < UMI_GPU_SMMU_CB_FIELDS; ++k)
            if (io->rd(io->cookie, cb + umi_gpu_smmu_cb_offsets[k]) != s->cb[i][k])
                return umi_gpu_smmu_result(d, UMI_GPU_SMMU_RESTORE_MISMATCH, i, k, s->cb[i][k]);
        if (io->rd(io->cookie, 0x800u + 4u * i) != s->smr[i] ||
            io->rd(io->cookie, 0xc00u + 4u * i) != s->s2cr[i] ||
            io->rd(io->cookie, s->page + 4u * i) != s->cbar[i] ||
            io->rd(io->cookie, s->page + 0x800u + 4u * i) != s->cba2r[i])
            return umi_gpu_smmu_result(d, UMI_GPU_SMMU_RESTORE_MISMATCH, i, 0, 0);
    }
    s->dirty = 0;
    return umi_gpu_smmu_result(d, UMI_GPU_SMMU_OK, 0, 0, 0);
}
#endif
