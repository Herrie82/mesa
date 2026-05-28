/*
 * Copyright © 2013 Rob Clark <robclark@freedesktop.org>
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#ifndef FD2_CONTEXT_H_
#define FD2_CONTEXT_H_

#include "freedreno_context.h"

struct fd2_context {
   struct fd_context base;

   /* vertex buf used for clear/gmem->mem vertices, and mem->gmem
    * vertices and tex coords:
    */
   struct pipe_resource *solid_vertexbuf;

   /* A22X hardware VSC tile-binner feedback buffer: the binner writes
    * per-pipe visibility-stream byte counts here (VSC_SIZE_ADDRESS).
    * Only allocated/used on a22x. See fd2_gmem.c emit_vsc_config().
    */
   struct fd_bo *vsc_size_mem;

   /* Scratch buffer for A22X cache flush timestamp verification.
    * CACHE_FLUSH_TS writes a timestamp here when the flush completes,
    * and the following WFI drains the pipeline so the per-tile resolve
    * can't race ahead of the draw's GMEM writes. Only used on a22x.
    *
    * Also doubles as the CYCLECTR probe buffer (FD_CYCPROF=1) -- see
    * fd2_emit_cycprobe() / fd2_cycprobe_dump() in fd2_emit.c. 512 bytes
    * total, 128 probe slots.
    */
   struct pipe_resource *scratch_buf;
   uint32_t cache_flush_seqno;

   /* CYCLECTR probe tracking (FD_CYCPROF=1). Set at end of fd2_emit_tile_init
    * and consumed at the start of the NEXT batch's fd2_emit_tile_init (where
    * the previous batch's fence has signalled and scratch_buf is safe to read).
    * Zero when no probes were emitted last batch (e.g. first batch / sysmem).
    */
   uint32_t cycprobe_nbins;  /* nbins from the previous batch, 0 if none */
};

static inline struct fd2_context *
fd2_context(struct fd_context *ctx)
{
   return (struct fd2_context *)ctx;
}

struct pipe_context *fd2_context_create(struct pipe_screen *pscreen, void *priv,
                                        unsigned flags);

#endif /* FD2_CONTEXT_H_ */
