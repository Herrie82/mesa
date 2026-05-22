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
};

static inline struct fd2_context *
fd2_context(struct fd_context *ctx)
{
   return (struct fd2_context *)ctx;
}

struct pipe_context *fd2_context_create(struct pipe_screen *pscreen, void *priv,
                                        unsigned flags);

#endif /* FD2_CONTEXT_H_ */
