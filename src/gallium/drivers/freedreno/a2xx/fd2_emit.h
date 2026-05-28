/*
 * Copyright © 2012-2013 Rob Clark <robclark@freedesktop.org>
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#ifndef FD2_EMIT_H
#define FD2_EMIT_H

#include "pipe/p_context.h"

#include "freedreno_context.h"

struct fd_ringbuffer;

struct fd2_vertex_buf {
   unsigned offset, size;
   struct pipe_resource *prsc;
};

void fd2_emit_vertex_bufs(struct fd_ringbuffer *ring, uint32_t val,
                          struct fd2_vertex_buf *vbufs, uint32_t n);
void fd2_emit_state_binning(struct fd_context *ctx,
                            const enum fd_dirty_3d_state dirty) assert_dt;
void fd2_emit_state(struct fd_context *ctx,
                    const enum fd_dirty_3d_state dirty) assert_dt;
void fd2_emit_restore(struct fd_context *ctx, struct fd_ringbuffer *ring);

/* CYCLECTR perfcounter probe (env FD_CYCPROF=1 to enable; no-op otherwise).
 * Writes the GPU cycle counter (reg 0x0ee2) into fd2_context::scratch_buf
 * at byte offset (idx*4). See fd2_emit.c for slot layout. */
bool fd2_cycprobe_active(void);
void fd2_emit_cycprobe(struct fd_context *ctx, struct fd_ringbuffer *ring,
                       unsigned idx);
/* Dump previous batch's probes (waits on the BO's fence). Called from
 * fd2_emit_tile_init BEFORE the new batch overwrites scratch_buf. */
void fd2_cycprobe_dump(struct fd_context *ctx);

void fd2_emit_init_screen(struct pipe_screen *pscreen);
void fd2_emit_init(struct pipe_context *pctx);

static inline void
fd2_emit_ib(struct fd_ringbuffer *ring, struct fd_ringbuffer *target)
{
   __OUT_IB(ring, false, target);
}

#endif /* FD2_EMIT_H */
