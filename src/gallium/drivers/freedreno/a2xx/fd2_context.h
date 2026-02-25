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

   /* Scratch buffer for A22X cache flush timestamp verification.
    * CACHE_FLUSH_TS writes a timestamp here when flush completes,
    * allowing us to poll for completion. This matches the legacy
    * KGSL pattern for explicit cache flush verification.
    */
   struct pipe_resource *scratch_buf;
   uint32_t cache_flush_seqno;
};

static inline struct fd2_context *
fd2_context(struct fd_context *ctx)
{
   return (struct fd2_context *)ctx;
}

struct pipe_context *fd2_context_create(struct pipe_screen *pscreen, void *priv,
                                        unsigned flags);

#endif /* FD2_CONTEXT_H_ */
