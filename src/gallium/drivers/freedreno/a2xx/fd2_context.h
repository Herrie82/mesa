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

   /* A22X HW binner visibility-stream size BO.  The binner writes one
    * u32 per pipe (8 pipes, 4 bytes each) to *(vsc_size_mem + pipe*4);
    * the per-tile CP_SET_BIN_DATA packet then loads that length so the
    * visibility filter sees how many bytes of the pipe data BO are
    * valid for this tile's pipe.  Allocated lazily on first batch
    * with use_hw_binning && is_a22x.
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
