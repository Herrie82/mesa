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

   /* Scratch BO for CP_REG_TO_MEM emission of VSC registers at submit
    * end - mirrors legacy KGSL's build_reg_save_cmds A22X (Leia) path:
    *   if (chip_id == LEIA_REV470)
    *       for reg in REG_LEIA_VSC_BIN_SIZE..REG_LEIA_VSC_PIPE_DATA_LENGTH_7
    *           emit PM4_REG_TO_MEM(reg, ctx->reg_values[j])
    * KGSL emits these REG_TO_MEM packets BETWEEN every user IB as
    * part of context-switch save.  The CP's execution of REG_TO_MEM
    * side-effects the binner state machine and drives 0x0ee2 through
    * 0x7f-namespace transitions that mainline Mesa never produces.
    * Gated on FD2_EMIT_VSC_REG_SAVE=1.
    */
   struct fd_bo *vsc_regsave_mem;
};

static inline struct fd2_context *
fd2_context(struct fd_context *ctx)
{
   return (struct fd2_context *)ctx;
}

struct pipe_context *fd2_context_create(struct pipe_screen *pscreen, void *priv,
                                        unsigned flags);

#endif /* FD2_CONTEXT_H_ */
