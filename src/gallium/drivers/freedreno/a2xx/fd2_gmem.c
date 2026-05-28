/*
 * Copyright © 2012 Rob Clark <robclark@freedesktop.org>
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include "pipe/p_state.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_string.h"

#include "freedreno_draw.h"
#include "freedreno_resource.h"
#include "freedreno_state.h"

#include "ir2/instr-a2xx.h"
#include "fd2_context.h"
#include "fd2_draw.h"
#include "fd2_emit.h"
#include "fd2_gmem.h"
#include "fd2_program.h"
#include "fd2_util.h"
#include "fd2_zsa.h"

static uint32_t
fmt2swap(enum pipe_format format)
{
   switch (format) {
   case PIPE_FORMAT_B8G8R8A8_UNORM:
   case PIPE_FORMAT_B8G8R8X8_UNORM:
   case PIPE_FORMAT_B5G6R5_UNORM:
   case PIPE_FORMAT_B5G5R5A1_UNORM:
   case PIPE_FORMAT_B5G5R5X1_UNORM:
   case PIPE_FORMAT_B4G4R4A4_UNORM:
   case PIPE_FORMAT_B4G4R4X4_UNORM:
   case PIPE_FORMAT_B2G3R3_UNORM:
      return 1;
   default:
      return 0;
   }
}

static bool
use_hw_binning(struct fd_batch *batch)
{
   const struct fd_gmem_stateobj *gmem = batch->gmem_state;

   /* we hardcoded a limit of 8 "pipes", we can increase this limit
    * at the cost of a slightly larger command stream
    * however very few cases will need more than 8
    * gmem->num_vsc_pipes == 0 means empty batch (TODO: does it still happen?)
    */
   if (gmem->num_vsc_pipes > 8 || !gmem->num_vsc_pipes)
      return false;

   /* a20x: shader-memexport hw binning (proven). a22x (Adreno 220): wire the
    * SAME memexport binning pass up here too. a220 is a superset of a20x and
    * the binning shader variant already emits the memexport visibility writes
    * (see extra_position_exports), so this drives a real per-tile visibility
    * pass — the freedreno "perhaps the a20x works?" TODO, finally taken.
    * The standalone a3xx-style HW VSC unit (emit_vsc_config) needs a
    * position-only binning shader we don't generate, so it is left OFF by
    * default; enabling both would make them fight over the vsc_pipe BOs.
    * FD_A22X_VSC=1 force-enables the HW VSC unit for A/B comparison instead.
    */
   if (!is_a20x(batch->ctx->screen) && !is_a22x(batch->ctx->screen))
      return false;

   return fd_binning_enabled && ((gmem->nbins_x * gmem->nbins_y) > 2);
}

/* transfer from gmem to system memory (ie. normal RAM) */

static void
emit_gmem2mem_surf(struct fd_batch *batch, uint32_t base,
                   struct pipe_surface *psurf)
{
   struct fd_ringbuffer *ring = batch->tile_store;
   struct fd_resource *rsc = fd_resource(psurf->texture);
   uint32_t offset =
      fd_resource_offset(rsc, psurf->level, psurf->first_layer);
   enum pipe_format format = fd_gmem_restore_format(psurf->format);
   uint32_t pitch = fdl2_pitch_pixels(&rsc->layout, psurf->level);

   assert((pitch & 31) == 0);
   assert((offset & 0xfff) == 0);

   if (!rsc->valid)
      return;

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_RB_COLOR_INFO));
   OUT_RING(ring, A2XX_RB_COLOR_INFO_BASE(base) |
                     A2XX_RB_COLOR_INFO_FORMAT(fd2_pipe2color(format)));

   OUT_PKT3(ring, CP_SET_CONSTANT, 5);
   OUT_RING(ring, CP_REG(REG_A2XX_RB_COPY_CONTROL));
   OUT_RING(ring, 0x00000000);             /* RB_COPY_CONTROL */
   OUT_RELOC(ring, rsc->bo, offset, 0, 0); /* RB_COPY_DEST_BASE */
   OUT_RING(ring, pitch >> 5);             /* RB_COPY_DEST_PITCH */
   OUT_RING(ring,                          /* RB_COPY_DEST_INFO */
            A2XX_RB_COPY_DEST_INFO_FORMAT(fd2_pipe2color(format)) |
               COND(!rsc->layout.tile_mode, A2XX_RB_COPY_DEST_INFO_LINEAR) |
               A2XX_RB_COPY_DEST_INFO_WRITE_RED |
               A2XX_RB_COPY_DEST_INFO_WRITE_GREEN |
               A2XX_RB_COPY_DEST_INFO_WRITE_BLUE |
               A2XX_RB_COPY_DEST_INFO_WRITE_ALPHA);

   if (!is_a20x(batch->ctx->screen)) {
      OUT_WFI(ring);

      OUT_PKT3(ring, CP_SET_CONSTANT, 3);
      OUT_RING(ring, CP_REG(REG_A2XX_VGT_MAX_VTX_INDX));
      OUT_RING(ring, 3); /* VGT_MAX_VTX_INDX */
      OUT_RING(ring, 0); /* VGT_MIN_VTX_INDX */
   }

   fd_draw(batch, ring, DI_PT_RECTLIST, IGNORE_VISIBILITY,
           DI_SRC_SEL_AUTO_INDEX, 3, 0, INDEX_SIZE_IGN, 0, 0, NULL);
}

static void
prepare_tile_fini_ib(struct fd_batch *batch) assert_dt
{
   struct fd_context *ctx = batch->ctx;
   struct fd2_context *fd2_ctx = fd2_context(ctx);
   const struct fd_gmem_stateobj *gmem = batch->gmem_state;
   struct pipe_framebuffer_state *pfb = &batch->framebuffer;
   struct fd_ringbuffer *ring;

   batch->tile_store =
      fd_submit_new_ringbuffer(batch->submit, 0x1000, FD_RINGBUFFER_STREAMING);
   ring = batch->tile_store;

   fd2_emit_vertex_bufs(ring, 0x9c,
                        (struct fd2_vertex_buf[]){
                           {.prsc = fd2_ctx->solid_vertexbuf, .size = 36},
                        },
                        1);

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_PA_SC_WINDOW_OFFSET));
   OUT_RING(ring, 0x00000000); /* PA_SC_WINDOW_OFFSET */

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_VGT_INDX_OFFSET));
   OUT_RING(ring, 0);

   if (!is_a20x(ctx->screen)) {
      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_VGT_VERTEX_REUSE_BLOCK_CNTL));
      OUT_RING(ring, 0x0000028f);
   }

   fd2_program_emit(ctx, ring, &ctx->solid_prog);

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_PA_SC_AA_MASK));
   OUT_RING(ring, 0x0000ffff);

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_RB_DEPTHCONTROL));
   OUT_RING(ring, A2XX_RB_DEPTHCONTROL_EARLY_Z_ENABLE);

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_PA_SU_SC_MODE_CNTL));
   OUT_RING(
      ring,
      A2XX_PA_SU_SC_MODE_CNTL_PROVOKING_VTX_LAST | /* PA_SU_SC_MODE_CNTL */
         A2XX_PA_SU_SC_MODE_CNTL_FRONT_PTYPE(PC_DRAW_TRIANGLES) |
         A2XX_PA_SU_SC_MODE_CNTL_BACK_PTYPE(PC_DRAW_TRIANGLES));

   OUT_PKT3(ring, CP_SET_CONSTANT, 3);
   OUT_RING(ring, CP_REG(REG_A2XX_PA_SC_WINDOW_SCISSOR_TL));
   OUT_RING(ring, xy2d(0, 0));                    /* PA_SC_WINDOW_SCISSOR_TL */
   OUT_RING(ring, xy2d(pfb->width, pfb->height)); /* PA_SC_WINDOW_SCISSOR_BR */

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_PA_CL_CLIP_CNTL));
   /* A22X: Set CLIP_DISABLE during GMEM operations per KGSL behavior */
   OUT_RING(ring, is_a20x(ctx->screen) ? 0x00000000 : 0x00010000);

   OUT_PKT3(ring, CP_SET_CONSTANT, 5);
   OUT_RING(ring, CP_REG(REG_A2XX_PA_CL_VPORT_XSCALE));
   OUT_RING(ring, fui((float)gmem->bin_w / 2.0f)); /* XSCALE */
   OUT_RING(ring, fui((float)gmem->bin_w / 2.0f)); /* XOFFSET */
   OUT_RING(ring, fui((float)gmem->bin_h / 2.0f)); /* YSCALE */
   OUT_RING(ring, fui((float)gmem->bin_h / 2.0f)); /* YOFFSET */

   /* A22X: STRONG drain before the EDRAM_COPY resolve. The tile's draws must
    * have fully landed in GMEM before the resolve reads it back; a plain
    * OUT_WFI is insufficient on A220 (it does not wait for VGT DMA). Use
    * CACHE_FLUSH_TS + WFI, ONCE per tile here -- this is the per-tile
    * boundary that the resolve race actually needs, and it REPLACES the
    * per-draw drain (which serialised every draw and crippled heavy
    * multi-bin scenes to ~1 fps). The mode transition COLOR_DEPTH<->EDRAM_COPY
    * also needs this sync. (This IB is built once and replayed per tile, so
    * the flush runs once per tile, draining that tile's draws.) */
   if (!is_a20x(ctx->screen)) {
      if (fd2_ctx->scratch_buf) {
         struct fd_bo *scratch_bo = fd_resource(fd2_ctx->scratch_buf)->bo;
         OUT_PKT3(ring, CP_EVENT_WRITE, 3);
         OUT_RING(ring, CACHE_FLUSH_TS);
         OUT_RELOC(ring, scratch_bo, 0, 0, 0); /* timestamp address */
         OUT_RING(ring, ++fd2_ctx->cache_flush_seqno);
      }
      OUT_WFI(ring);
   }

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_RB_MODECONTROL));
   OUT_RING(ring, A2XX_RB_MODECONTROL_EDRAM_MODE(EDRAM_COPY));

   if (batch->resolve & (FD_BUFFER_DEPTH | FD_BUFFER_STENCIL))
      emit_gmem2mem_surf(batch, gmem->zsbuf_base[0], &pfb->zsbuf);

   if (batch->resolve & FD_BUFFER_COLOR)
      emit_gmem2mem_surf(batch, gmem->cbuf_base[0], &pfb->cbufs[0]);

   /* A22X: WFI before switching back to COLOR_DEPTH mode */
   if (!is_a20x(ctx->screen))
      OUT_WFI(ring);

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_RB_MODECONTROL));
   OUT_RING(ring, A2XX_RB_MODECONTROL_EDRAM_MODE(COLOR_DEPTH));

   if (!is_a20x(ctx->screen)) {
      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_VGT_VERTEX_REUSE_BLOCK_CNTL));
      OUT_RING(ring, 0x0000003b);
   }
}

static void
fd2_emit_tile_gmem2mem(struct fd_batch *batch, const struct fd_tile *tile)
{
   struct fd_context *ctx = batch->ctx;

   /* FD_CYCPROF=1: probe (1 + tidx*3 + 1) = end of tile draws / start of
    * resolve. tile->n is "slot within VSC pipe" (0..1 on A22X's 2-pipe
    * config), NOT the global tile index -- use (tile - gmem->tile) for
    * the actual linear index across all pipes. */
   const struct fd_gmem_stateobj *gmem = batch->gmem_state;
   unsigned tidx = tile - gmem->tile;
   fd2_emit_cycprobe(ctx, batch->gmem, 1 + tidx * 3 + 1);

   fd2_emit_ib(batch->gmem, batch->tile_store);

   /* probe (1 + tidx*3 + 2) = end of resolve. */
   fd2_emit_cycprobe(ctx, batch->gmem, 1 + tidx * 3 + 2);
}

/* transfer from system memory to gmem */

static void
emit_mem2gmem_surf(struct fd_batch *batch, uint32_t base,
                   struct pipe_surface *psurf)
{
   struct fd_ringbuffer *ring = batch->gmem;
   struct fd_resource *rsc = fd_resource(psurf->texture);
   uint32_t offset =
      fd_resource_offset(rsc, psurf->level, psurf->first_layer);
   enum pipe_format format = fd_gmem_restore_format(psurf->format);
   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_RB_COLOR_INFO));
   OUT_RING(ring, A2XX_RB_COLOR_INFO_BASE(base) |
                     A2XX_RB_COLOR_INFO_FORMAT(fd2_pipe2color(format)));

   /* emit fb as a texture: */
   OUT_PKT3(ring, CP_SET_CONSTANT, 7);
   OUT_RING(ring, 0x00010000);
   OUT_RING(ring, A2XX_SQ_TEX_0_CLAMP_X(SQ_TEX_WRAP) |
                     A2XX_SQ_TEX_0_CLAMP_Y(SQ_TEX_WRAP) |
                     A2XX_SQ_TEX_0_CLAMP_Z(SQ_TEX_WRAP) |
                     A2XX_SQ_TEX_0_PITCH(
                        fdl2_pitch_pixels(&rsc->layout, psurf->level)));
   OUT_RELOC(ring, rsc->bo, offset,
             A2XX_SQ_TEX_1_FORMAT(fd2_pipe2surface(format).format) |
                A2XX_SQ_TEX_1_CLAMP_POLICY(SQ_TEX_CLAMP_POLICY_OGL),
             0);
   OUT_RING(ring, A2XX_SQ_TEX_2_WIDTH(pipe_surface_width(psurf) - 1) |
                     A2XX_SQ_TEX_2_HEIGHT(pipe_surface_height(psurf) - 1));
   OUT_RING(ring, A2XX_SQ_TEX_3_MIP_FILTER(SQ_TEX_FILTER_BASEMAP) |
                     A2XX_SQ_TEX_3_SWIZ_X(0) | A2XX_SQ_TEX_3_SWIZ_Y(1) |
                     A2XX_SQ_TEX_3_SWIZ_Z(2) | A2XX_SQ_TEX_3_SWIZ_W(3) |
                     A2XX_SQ_TEX_3_XY_MAG_FILTER(SQ_TEX_FILTER_POINT) |
                     A2XX_SQ_TEX_3_XY_MIN_FILTER(SQ_TEX_FILTER_POINT));
   OUT_RING(ring, 0x00000000);
   OUT_RING(ring, A2XX_SQ_TEX_5_DIMENSION(SQ_TEX_DIMENSION_2D));

   if (!is_a20x(batch->ctx->screen)) {
      OUT_PKT3(ring, CP_SET_CONSTANT, 3);
      OUT_RING(ring, CP_REG(REG_A2XX_VGT_MAX_VTX_INDX));
      OUT_RING(ring, 3); /* VGT_MAX_VTX_INDX */
      OUT_RING(ring, 0); /* VGT_MIN_VTX_INDX */
   }

   fd_draw(batch, ring, DI_PT_RECTLIST, IGNORE_VISIBILITY,
           DI_SRC_SEL_AUTO_INDEX, 3, 0, INDEX_SIZE_IGN, 0, 0, NULL);
}

static void
fd2_emit_tile_mem2gmem(struct fd_batch *batch,
                       const struct fd_tile *tile) assert_dt
{
   struct fd_context *ctx = batch->ctx;
   struct fd2_context *fd2_ctx = fd2_context(ctx);
   const struct fd_gmem_stateobj *gmem = batch->gmem_state;
   struct fd_ringbuffer *ring = batch->gmem;
   struct pipe_framebuffer_state *pfb = &batch->framebuffer;
   unsigned bin_w = tile->bin_w;
   unsigned bin_h = tile->bin_h;
   float x0, y0, x1, y1;

   fd2_emit_vertex_bufs(
      ring, 0x9c,
      (struct fd2_vertex_buf[]){
         {.prsc = fd2_ctx->solid_vertexbuf, .size = 36},
         {.prsc = fd2_ctx->solid_vertexbuf, .size = 24, .offset = 36},
      },
      2);

   /* write texture coordinates to vertexbuf: */
   x0 = ((float)tile->xoff) / ((float)pfb->width);
   x1 = ((float)tile->xoff + bin_w) / ((float)pfb->width);
   y0 = ((float)tile->yoff) / ((float)pfb->height);
   y1 = ((float)tile->yoff + bin_h) / ((float)pfb->height);
   OUT_PKT3(ring, CP_MEM_WRITE, 7);
   OUT_RELOC(ring, fd_resource(fd2_ctx->solid_vertexbuf)->bo, 36, 0, 0);
   OUT_RING(ring, fui(x0));
   OUT_RING(ring, fui(y0));
   OUT_RING(ring, fui(x1));
   OUT_RING(ring, fui(y0));
   OUT_RING(ring, fui(x0));
   OUT_RING(ring, fui(y1));

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_VGT_INDX_OFFSET));
   OUT_RING(ring, 0);

   fd2_program_emit(ctx, ring, &ctx->blit_prog[0]);

   OUT_PKT0(ring, REG_A2XX_TC_CNTL_STATUS, 1);
   OUT_RING(ring, A2XX_TC_CNTL_STATUS_L2_INVALIDATE);

   /* A22X: Wait for L2 cache invalidation to complete before texture fetch.
    * Without this, stale texture data may be read during mem2gmem blit.
    */
   if (!is_a20x(batch->ctx->screen))
      OUT_WFI(ring);

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_RB_DEPTHCONTROL));
   OUT_RING(ring, A2XX_RB_DEPTHCONTROL_EARLY_Z_ENABLE);

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_PA_SU_SC_MODE_CNTL));
   OUT_RING(ring, A2XX_PA_SU_SC_MODE_CNTL_PROVOKING_VTX_LAST |
                     A2XX_PA_SU_SC_MODE_CNTL_FRONT_PTYPE(PC_DRAW_TRIANGLES) |
                     A2XX_PA_SU_SC_MODE_CNTL_BACK_PTYPE(PC_DRAW_TRIANGLES));

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_PA_SC_AA_MASK));
   OUT_RING(ring, 0x0000ffff);

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_RB_COLORCONTROL));
   OUT_RING(ring, A2XX_RB_COLORCONTROL_ALPHA_FUNC(FUNC_ALWAYS) |
                     A2XX_RB_COLORCONTROL_BLEND_DISABLE |
                     A2XX_RB_COLORCONTROL_ROP_CODE(12) |
                     A2XX_RB_COLORCONTROL_DITHER_MODE(DITHER_DISABLE) |
                     A2XX_RB_COLORCONTROL_DITHER_TYPE(DITHER_PIXEL));

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_RB_BLEND_CONTROL));
   OUT_RING(ring, A2XX_RB_BLEND_CONTROL_COLOR_SRCBLEND(FACTOR_ONE) |
                     A2XX_RB_BLEND_CONTROL_COLOR_COMB_FCN(BLEND2_DST_PLUS_SRC) |
                     A2XX_RB_BLEND_CONTROL_COLOR_DESTBLEND(FACTOR_ZERO) |
                     A2XX_RB_BLEND_CONTROL_ALPHA_SRCBLEND(FACTOR_ONE) |
                     A2XX_RB_BLEND_CONTROL_ALPHA_COMB_FCN(BLEND2_DST_PLUS_SRC) |
                     A2XX_RB_BLEND_CONTROL_ALPHA_DESTBLEND(FACTOR_ZERO));

   OUT_PKT3(ring, CP_SET_CONSTANT, 3);
   OUT_RING(ring, CP_REG(REG_A2XX_PA_SC_WINDOW_SCISSOR_TL));
   OUT_RING(ring, A2XX_PA_SC_WINDOW_OFFSET_DISABLE |
                     xy2d(0, 0));      /* PA_SC_WINDOW_SCISSOR_TL */
   OUT_RING(ring, xy2d(bin_w, bin_h)); /* PA_SC_WINDOW_SCISSOR_BR */

   OUT_PKT3(ring, CP_SET_CONSTANT, 5);
   OUT_RING(ring, CP_REG(REG_A2XX_PA_CL_VPORT_XSCALE));
   OUT_RING(ring, fui((float)bin_w / 2.0f));  /* PA_CL_VPORT_XSCALE */
   OUT_RING(ring, fui((float)bin_w / 2.0f));  /* PA_CL_VPORT_XOFFSET */
   OUT_RING(ring, fui(-(float)bin_h / 2.0f)); /* PA_CL_VPORT_YSCALE */
   OUT_RING(ring, fui((float)bin_h / 2.0f));  /* PA_CL_VPORT_YOFFSET */

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_PA_CL_VTE_CNTL));
   OUT_RING(ring, A2XX_PA_CL_VTE_CNTL_VTX_XY_FMT |
                     A2XX_PA_CL_VTE_CNTL_VTX_Z_FMT | // XXX check this???
                     A2XX_PA_CL_VTE_CNTL_VPORT_X_SCALE_ENA |
                     A2XX_PA_CL_VTE_CNTL_VPORT_X_OFFSET_ENA |
                     A2XX_PA_CL_VTE_CNTL_VPORT_Y_SCALE_ENA |
                     A2XX_PA_CL_VTE_CNTL_VPORT_Y_OFFSET_ENA);

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_PA_CL_CLIP_CNTL));
   /* A22X: Set CLIP_DISABLE during GMEM operations per KGSL behavior */
   OUT_RING(ring, is_a20x(batch->ctx->screen) ? 0x00000000 : 0x00010000);

   if (fd_gmem_needs_restore(batch, tile, FD_BUFFER_DEPTH | FD_BUFFER_STENCIL))
      emit_mem2gmem_surf(batch, gmem->zsbuf_base[0], &pfb->zsbuf);

   if (fd_gmem_needs_restore(batch, tile, FD_BUFFER_COLOR))
      emit_mem2gmem_surf(batch, gmem->cbuf_base[0], &pfb->cbufs[0]);

   /* A22X: Cache flush after mem2gmem draws as per blob driver behavior.
    * This ensures texture data written to GMEM is coherent before rendering.
    */
   if (!is_a20x(ctx->screen)) {
      OUT_PKT3(ring, CP_EVENT_WRITE, 1);
      OUT_RING(ring, CACHE_FLUSH_AND_INV_EVENT);
   }

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_PA_CL_VTE_CNTL));
   OUT_RING(ring, A2XX_PA_CL_VTE_CNTL_VTX_W0_FMT |
                     A2XX_PA_CL_VTE_CNTL_VPORT_X_SCALE_ENA |
                     A2XX_PA_CL_VTE_CNTL_VPORT_X_OFFSET_ENA |
                     A2XX_PA_CL_VTE_CNTL_VPORT_Y_SCALE_ENA |
                     A2XX_PA_CL_VTE_CNTL_VPORT_Y_OFFSET_ENA |
                     A2XX_PA_CL_VTE_CNTL_VPORT_Z_SCALE_ENA |
                     A2XX_PA_CL_VTE_CNTL_VPORT_Z_OFFSET_ENA);
}

static void
patch_draws(struct fd_batch *batch, enum pc_di_vis_cull_mode vismode)
{
   unsigned i;

   if (!is_a20x(batch->ctx->screen)) {
      /* identical to a3xx */
      for (i = 0; i < fd_patch_num_elements(&batch->draw_patches); i++) {
         struct fd_cs_patch *patch = fd_patch_element(&batch->draw_patches, i);
         *patch->cs = patch->val | DRAW(0, 0, 0, vismode, 0);
      }
      util_dynarray_clear(&batch->draw_patches);
      return;
   }

   if (vismode == USE_VISIBILITY)
      return;

   for (i = 0; i < batch->draw_patches.size / sizeof(uint32_t *); i++) {
      uint32_t *ptr =
         *util_dynarray_element(&batch->draw_patches, uint32_t *, i);
      unsigned cnt = ptr[0] >> 16 & 0xfff; /* 5 with idx buffer, 3 without */

      /* convert CP_DRAW_INDX_BIN to a CP_DRAW_INDX
       * replace first two DWORDS with NOP and move the rest down
       * (we don't want to have to move the idx buffer reloc)
       */
      ptr[0] = CP_TYPE3_PKT | (CP_NOP << 8);
      ptr[1] = 0x00000000;

      ptr[4] = ptr[2] & ~(1 << 14 | 1 << 15); /* remove cull_enable bits */
      ptr[2] = CP_TYPE3_PKT | ((cnt - 2) << 16) | (CP_DRAW_INDX << 8);
      ptr[3] = 0x00000000;
   }
}

static void
fd2_emit_sysmem_prep(struct fd_batch *batch)
{
   struct fd_context *ctx = batch->ctx;
   struct fd_ringbuffer *ring = batch->gmem;
   struct pipe_framebuffer_state *pfb = &batch->framebuffer;
   struct pipe_surface *psurf = &pfb->cbufs[0];

   if (!psurf->texture)
      return;

   struct fd_resource *rsc = fd_resource(psurf->texture);
   uint32_t offset =
      fd_resource_offset(rsc, psurf->level, psurf->first_layer);
   uint32_t pitch = fdl2_pitch_pixels(&rsc->layout, psurf->level);

   assert((pitch & 31) == 0);
   assert((offset & 0xfff) == 0);

   fd2_emit_restore(ctx, ring);

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_RB_SURFACE_INFO));
   OUT_RING(ring, A2XX_RB_SURFACE_INFO_SURFACE_PITCH(pitch));

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_RB_COLOR_INFO));
   OUT_RELOC(ring, rsc->bo, offset,
             COND(!rsc->layout.tile_mode, A2XX_RB_COLOR_INFO_LINEAR) |
                A2XX_RB_COLOR_INFO_SWAP(fmt2swap(psurf->format)) |
                A2XX_RB_COLOR_INFO_FORMAT(fd2_pipe2color(psurf->format)),
             0);

   OUT_PKT3(ring, CP_SET_CONSTANT, 3);
   OUT_RING(ring, CP_REG(REG_A2XX_PA_SC_SCREEN_SCISSOR_TL));
   OUT_RING(ring, A2XX_PA_SC_SCREEN_SCISSOR_TL_WINDOW_OFFSET_DISABLE);
   OUT_RING(ring, A2XX_PA_SC_SCREEN_SCISSOR_BR_X(pfb->width) |
                     A2XX_PA_SC_SCREEN_SCISSOR_BR_Y(pfb->height));

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_PA_SC_WINDOW_OFFSET));
   OUT_RING(ring,
            A2XX_PA_SC_WINDOW_OFFSET_X(0) | A2XX_PA_SC_WINDOW_OFFSET_Y(0));

   patch_draws(batch, IGNORE_VISIBILITY);
   util_dynarray_clear(&batch->draw_patches);
   util_dynarray_clear(&batch->shader_patches);
}

/*
 * Configure the A220 hardware VSC tile-binner.
 *
 * Unlike a20x (which freedreno drives with a memexport visibility shader
 * + separate binning IB) the A220 VSC binner is the same block as a3xx and
 * is *always in the primitive path*. freedreno never programmed it, so it
 * came up with garbage power-on pipe state and corrupted tile coverage —
 * the deterministic "period-8" cycle where 7 of 8 otherwise-identical
 * renders dropped subsets of GMEM tiles.
 *
 * The proprietary webOS stack renders correctly on the same silicon by
 * fully configuring the VSC at context/render setup (register capture in
 * the kernel reports): VSC_BIN_SIZE, the 8 VSC_PIPE config/addr/len
 * triples with backing BOs, and VSC_SIZE_ADDRESS — yet it issues only
 * plain DRAW_INDX, never DRAW_INDX_BIN, and runs no separate binning pass.
 * The vis-streams it produces are non-empty, i.e. the binner runs during
 * the normal draw. So the fix is to give the binner valid state, not to
 * add a binning pass.
 *
 * We reuse freedreno's own per-pipe bin layout (gmem->vsc_pipe[], computed
 * for every GMEM setup) so the binner's notion of bins matches the SW tile
 * loop exactly. VSC_PIPE.CONFIG / VSC_BIN_SIZE encodings are identical to
 * a3xx (this mirrors fd3_gmem.c update_vsc_pipe()); the captured webOS
 * values (BIN_SIZE 0x108 for 256x256, 0x187 for 224x384, PIPE_CONFIG
 * X[0:9] Y[10:19] W[20:23] H[24:27]) confirm the layout.
 *
 * Note: webOS also pulses SQ_GPR_MANAGEMENT=0x0007f010 (VTX=127,PIX=1)
 * here, but that is its binning-shader GPR split; with PIX=1 it would
 * starve our real pixel shaders. freedreno relies on the kernel's static
 * 0x00040400 (VTX=64,PIX=64) and runs no binning shader, so we
 * deliberately leave SQ_GPR_MANAGEMENT alone.
 */
static void
emit_vsc_config(struct fd_batch *batch) assert_dt
{
   struct fd_context *ctx = batch->ctx;
   struct fd2_context *fd2_ctx = fd2_context(ctx);
   const struct fd_gmem_stateobj *gmem = batch->gmem_state;
   struct fd_ringbuffer *ring = batch->gmem;
   int i;

   /* bin dimensions (already 32-aligned and sized to fit color[+depth] in
    * GMEM by freedreno_gmem.c — the same budget constraint webOS uses) */
   OUT_PKT0(ring, REG_A2XX_A220_VSC_BIN_SIZE, 1);
   OUT_RING(ring, A2XX_A220_VSC_BIN_SIZE_WIDTH(gmem->bin_w) |
                     A2XX_A220_VSC_BIN_SIZE_HEIGHT(gmem->bin_h));

   /* feedback buffer for per-pipe vis-stream byte counts */
   OUT_PKT0(ring, REG_A2XX_VSC_SIZE_ADDRESS, 1);
   OUT_RELOC(ring, fd2_ctx->vsc_size_mem, 0, 0, 0);

   /* 8 pipes: CONFIG (bin x/y/w/h) + backing BO address + length.
    * 256 KB per pipe matches the captured webOS DATA_LENGTH (0x40000)
    * and a3xx. Unused pipes (>= num_vsc_pipes) are zeroed in gmem and
    * get CONFIG=0, which is harmless. */
   for (i = 0; i < 8; i++) {
      const struct fd_vsc_pipe *pipe = &gmem->vsc_pipe[i];

      if (!ctx->vsc_pipe_bo[i]) {
         ctx->vsc_pipe_bo[i] =
            fd_bo_new(ctx->dev, 0x40000, 0, "vsc_pipe[%u]", i);
      }

      OUT_PKT0(ring, REG_A2XX_VSC_PIPE(i), 3);
      OUT_RING(ring, A2XX_VSC_PIPE_CONFIG_X(pipe->x) |
                        A2XX_VSC_PIPE_CONFIG_Y(pipe->y) |
                        A2XX_VSC_PIPE_CONFIG_W(pipe->w) |
                        A2XX_VSC_PIPE_CONFIG_H(pipe->h));
      OUT_RELOC(ring, ctx->vsc_pipe_bo[i], 0, 0, 0); /* DATA_ADDRESS */
      OUT_RING(ring,
               fd_bo_size(ctx->vsc_pipe_bo[i]) - 32); /* DATA_LENGTH */
   }

   /* Enable the binner. webOS drives LRZ_VSC_CONTROL=3 transiently while
    * configuring and settles to 1 for steady render; 1 is the run value. */
   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_A220_RB_LRZ_VSC_CONTROL));
   OUT_RING(ring, 0x00000001);

   /* DEBUG (FD_MESA_DEBUG=msgs): dump the emitted VSC grid so it can be diffed
    * against the webOS hardware capture. webOS depth scene = BIN_SIZE 0x187
    * (224x384), 5x2 bins, PIPE_CONFIG 0x01100000/0x01100001/0x01200400/...
    * (low16 = (y<<10)|x). If freedreno's grid or CONFIG encoding differs, that
    * mismatch is the a220 binner-wedge bug on heavy scenes (e.g. glmark blur). */
   if (FD_DBG(MSGS)) {
      DBG("a22x VSC: bin=%dx%d nbins=%dx%d num_vsc_pipes=%d BIN_SIZE=0x%08x",
          gmem->bin_w, gmem->bin_h, gmem->nbins_x, gmem->nbins_y,
          gmem->num_vsc_pipes,
          A2XX_A220_VSC_BIN_SIZE_WIDTH(gmem->bin_w) |
             A2XX_A220_VSC_BIN_SIZE_HEIGHT(gmem->bin_h));
      for (int dp = 0; dp < 8; dp++) {
         const struct fd_vsc_pipe *pp = &gmem->vsc_pipe[dp];
         DBG("a22x VSC pipe[%d]: x=%d y=%d w=%d h=%d CONFIG=0x%08x", dp,
             pp->x, pp->y, pp->w, pp->h,
             A2XX_VSC_PIPE_CONFIG_X(pp->x) | A2XX_VSC_PIPE_CONFIG_Y(pp->y) |
                A2XX_VSC_PIPE_CONFIG_W(pp->w) | A2XX_VSC_PIPE_CONFIG_H(pp->h));
      }
   }
}

/* before first tile */
static void
fd2_emit_tile_init(struct fd_batch *batch) assert_dt
{
   struct fd_context *ctx = batch->ctx;
   struct fd_ringbuffer *ring = batch->gmem;
   struct pipe_framebuffer_state *pfb = &batch->framebuffer;
   const struct fd_gmem_stateobj *gmem = batch->gmem_state;
   enum pipe_format format = pipe_surface_format(&pfb->cbufs[0]);
   uint32_t reg;

   /* FD_CYCPROF=1: dump previous batch's CYCLECTR probes before scratch_buf
    * gets overwritten by the new batch's probes below. */
   fd2_cycprobe_dump(ctx);

   fd2_emit_restore(ctx, ring);

   prepare_tile_fini_ib(batch);

   /* FD_CYCPROF=1: probe 0 = batch start (after restore/setup, before tile loop).
    * Record nbins so the dump (next batch) knows how many tile slots to read. */
   fd2_emit_cycprobe(ctx, ring, 0);
   if (fd2_cycprobe_active())
      fd2_context(ctx)->cycprobe_nbins = gmem->nbins_x * gmem->nbins_y;

   OUT_PKT3(ring, CP_SET_CONSTANT, 4);
   OUT_RING(ring, CP_REG(REG_A2XX_RB_SURFACE_INFO));
   OUT_RING(ring, gmem->bin_w); /* RB_SURFACE_INFO */
   OUT_RING(ring, A2XX_RB_COLOR_INFO_SWAP(fmt2swap(format)) |
                     A2XX_RB_COLOR_INFO_FORMAT(fd2_pipe2color(format)));
   reg = A2XX_RB_DEPTH_INFO_DEPTH_BASE(gmem->zsbuf_base[0]);
   if (pfb->zsbuf.texture)
      reg |= A2XX_RB_DEPTH_INFO_DEPTH_FORMAT(fd_pipe2depth(pfb->zsbuf.format));
   OUT_RING(ring, reg); /* RB_DEPTH_INFO */

   /* fast clear patches */
   int depth_size = -1;
   int color_size = -1;

   if (pfb->cbufs[0].texture)
      color_size = util_format_get_blocksizebits(format) == 32 ? 4 : 2;

   if (pfb->zsbuf.texture)
      depth_size = fd_pipe2depth(pfb->zsbuf.format) == 1 ? 4 : 2;

   for (int i = 0; i < fd_patch_num_elements(&batch->gmem_patches); i++) {
      struct fd_cs_patch *patch = fd_patch_element(&batch->gmem_patches, i);
      uint32_t color_base = 0, depth_base = gmem->zsbuf_base[0];
      uint32_t size, lines;

      /* note: 1 "line" is 512 bytes in both color/depth areas (1K total) */
      switch (patch->val) {
      case GMEM_PATCH_FASTCLEAR_COLOR:
         size = align(gmem->bin_w * gmem->bin_h * color_size, 0x8000);
         lines = size / 1024;
         depth_base = size / 2;
         break;
      case GMEM_PATCH_FASTCLEAR_DEPTH:
         size = align(gmem->bin_w * gmem->bin_h * depth_size, 0x8000);
         lines = size / 1024;
         color_base = depth_base;
         depth_base = depth_base + size / 2;
         break;
      case GMEM_PATCH_FASTCLEAR_COLOR_DEPTH:
         lines =
            align(gmem->bin_w * gmem->bin_h * color_size * 2, 0x8000) / 1024;
         break;
      case GMEM_PATCH_RESTORE_INFO:
         patch->cs[0] = gmem->bin_w;
         patch->cs[1] = A2XX_RB_COLOR_INFO_SWAP(fmt2swap(format)) |
                        A2XX_RB_COLOR_INFO_FORMAT(fd2_pipe2color(format));
         patch->cs[2] = A2XX_RB_DEPTH_INFO_DEPTH_BASE(gmem->zsbuf_base[0]);
         if (pfb->zsbuf.texture)
            patch->cs[2] |= A2XX_RB_DEPTH_INFO_DEPTH_FORMAT(
               fd_pipe2depth(pfb->zsbuf.format));
         continue;
      default:
         continue;
      }

      patch->cs[0] = A2XX_PA_SC_SCREEN_SCISSOR_BR_X(32) |
                     A2XX_PA_SC_SCREEN_SCISSOR_BR_Y(lines);
      patch->cs[4] = A2XX_RB_COLOR_INFO_BASE(color_base) |
                     A2XX_RB_COLOR_INFO_FORMAT(COLORX_8_8_8_8);
      patch->cs[5] = A2XX_RB_DEPTH_INFO_DEPTH_BASE(depth_base) |
                     A2XX_RB_DEPTH_INFO_DEPTH_FORMAT(1);
   }
   util_dynarray_clear(&batch->gmem_patches);

   /* set to zero, for some reason hardware doesn't like certain values */
   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_VGT_CURRENT_BIN_ID_MIN));
   OUT_RING(ring, 0);

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_VGT_CURRENT_BIN_ID_MAX));
   OUT_RING(ring, 0);

   if (use_hw_binning(batch)) {
      /* patch out unneeded memory exports by changing EXEC CF to EXEC_END
       *
       * in the shader compiler, we guarantee that the shader ends with
       * a specific pattern of ALLOC/EXEC CF pairs for the hw binning exports
       *
       * the since patches point only to dwords and CFs are 1.5 dwords
       * the patch is aligned and might point to a ALLOC CF
       */
      for (int i = 0; i < batch->shader_patches.size / sizeof(void *); i++) {
         instr_cf_t *cf =
            *util_dynarray_element(&batch->shader_patches, instr_cf_t *, i);
         if (cf->opc == ALLOC)
            cf++;
         assert(cf->opc == EXEC);
         assert(cf[ctx->screen->info->num_vsc_pipes * 2 - 2].opc == EXEC_END);
         cf[2 * (gmem->num_vsc_pipes - 1)].opc = EXEC_END;
      }

      patch_draws(batch, USE_VISIBILITY);

      /* initialize shader constants for the binning memexport */
      OUT_PKT3(ring, CP_SET_CONSTANT, 1 + gmem->num_vsc_pipes * 4);
      OUT_RING(ring, 0x0000000C);

      for (int i = 0; i < gmem->num_vsc_pipes; i++) {
         /* allocate in 64k increments to avoid reallocs */
         uint32_t bo_size = align(batch->num_vertices, 0x10000);
         if (!ctx->vsc_pipe_bo[i] ||
             fd_bo_size(ctx->vsc_pipe_bo[i]) < bo_size) {
            if (ctx->vsc_pipe_bo[i])
               fd_bo_del(ctx->vsc_pipe_bo[i]);
            ctx->vsc_pipe_bo[i] =
               fd_bo_new(ctx->dev, bo_size, 0, "vsc_pipe[%u]", i);
            assert(ctx->vsc_pipe_bo[i]);
         }

         /* memory export address (export32):
          * .x: (base_address >> 2) | 0x40000000 (?)
          * .y: index (float) - set by shader
          * .z: 0x4B00D000 (?)
          * .w: 0x4B000000 (?) | max_index (?)
          */
         OUT_RELOC(ring, ctx->vsc_pipe_bo[i], 0, 0x40000000, -2);
         OUT_RING(ring, 0x00000000);
         OUT_RING(ring, 0x4B00D000);
         OUT_RING(ring, 0x4B000000 | bo_size);
      }

      OUT_PKT3(ring, CP_SET_CONSTANT, 1 + gmem->num_vsc_pipes * 8);
      OUT_RING(ring, 0x0000018C);

      for (int i = 0; i < gmem->num_vsc_pipes; i++) {
         const struct fd_vsc_pipe *pipe = &gmem->vsc_pipe[i];
         float off_x, off_y, mul_x, mul_y;

         /* const to tranform from [-1,1] to bin coordinates for this pipe
          * for x/y, [0,256/2040] = 0, [256/2040,512/2040] = 1, etc
          * 8 possible values on x/y axis,
          * to clip at binning stage: only use center 6x6
          * TODO: set the z parameters too so that hw binning
          * can clip primitives in Z too
          */

         mul_x = 1.0f / (float)(gmem->bin_w * 8);
         mul_y = 1.0f / (float)(gmem->bin_h * 8);
         off_x = -pipe->x * (1.0f / 8.0f) + 0.125f - mul_x * gmem->minx;
         off_y = -pipe->y * (1.0f / 8.0f) + 0.125f - mul_y * gmem->miny;

         OUT_RING(ring, fui(off_x * (256.0f / 255.0f)));
         OUT_RING(ring, fui(off_y * (256.0f / 255.0f)));
         OUT_RING(ring, 0x3f000000);
         OUT_RING(ring, fui(0.0f));

         OUT_RING(ring, fui(mul_x * (256.0f / 255.0f)));
         OUT_RING(ring, fui(mul_y * (256.0f / 255.0f)));
         OUT_RING(ring, fui(0.0f));
         OUT_RING(ring, fui(0.0f));
      }

      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_VGT_VERTEX_REUSE_BLOCK_CNTL));
      OUT_RING(ring, 0);

      fd2_emit_ib(ring, batch->binning);

      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_VGT_VERTEX_REUSE_BLOCK_CNTL));
      OUT_RING(ring, 0x00000002);
   } else {
      patch_draws(batch, IGNORE_VISIBILITY);
   }

   /* A22X now runs a real binning pass through the a20x memexport path above
    * (use_hw_binning). The standalone a3xx-style HW VSC unit is left disabled
    * (LRZ_VSC_CONTROL stays 0 from emit_restore) so the two binning mechanisms
    * can't fight over the vsc_pipe BOs. FD_A22X_VSC=1 force-enables the HW VSC
    * unit instead, for A/B comparison — do NOT combine it with the memexport
    * pass (it will double-write the visibility BOs). */
   if (is_a22x(ctx->screen) &&
       debug_get_bool_option("FD_A22X_VSC", false))
      emit_vsc_config(batch);

   util_dynarray_clear(&batch->draw_patches);
   util_dynarray_clear(&batch->shader_patches);
}

/* before mem2gmem */
static void
fd2_emit_tile_prep(struct fd_batch *batch, const struct fd_tile *tile)
{
   struct fd_ringbuffer *ring = batch->gmem;
   struct pipe_framebuffer_state *pfb = &batch->framebuffer;
   enum pipe_format format = pipe_surface_format(&pfb->cbufs[0]);

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_RB_COLOR_INFO));
   OUT_RING(ring, A2XX_RB_COLOR_INFO_SWAP(1) | /* RB_COLOR_INFO */
                     A2XX_RB_COLOR_INFO_FORMAT(fd2_pipe2color(format)));

   /* setup screen scissor for current tile (same for mem2gmem): */
   OUT_PKT3(ring, CP_SET_CONSTANT, 3);
   OUT_RING(ring, CP_REG(REG_A2XX_PA_SC_SCREEN_SCISSOR_TL));
   OUT_RING(ring, A2XX_PA_SC_SCREEN_SCISSOR_TL_X(0) |
                     A2XX_PA_SC_SCREEN_SCISSOR_TL_Y(0));
   OUT_RING(ring, A2XX_PA_SC_SCREEN_SCISSOR_BR_X(tile->bin_w) |
                     A2XX_PA_SC_SCREEN_SCISSOR_BR_Y(tile->bin_h));
}

/* before IB to rendering cmds: */
static void
fd2_emit_tile_renderprep(struct fd_batch *batch,
                         const struct fd_tile *tile) assert_dt
{
   struct fd_context *ctx = batch->ctx;
   struct fd2_context *fd2_ctx = fd2_context(ctx);
   struct fd_ringbuffer *ring = batch->gmem;
   struct pipe_framebuffer_state *pfb = &batch->framebuffer;
   enum pipe_format format = pipe_surface_format(&pfb->cbufs[0]);

   /* A22X: the per-tile state reprogramming below (RB_COLOR_INFO, window
    * offset/scissor, ...) races this tile's mem2gmem restore and the
    * previous tile's gmem2mem resolve, which are still in flight -- there
    * is no implicit sync between tiles. On heavy depth/restore scenes
    * (e.g. glmark2 desktop: 16 bins, batch_restore=20) that race leaves the
    * 3D back-end stuck busy (hangcheck "gpu lockup" with the fence still
    * creeping). The per-draw CACHE_FLUSH_TS+WFI does not cover the restore/
    * resolve blits, so serialise tiles here: wait for the GPU to go idle
    * before reprogramming. A plain OUT_WFI is insufficient on A22X (see the
    * pre-draw VGT-DMA RBBM_STATUS poll in fd2_draw.c), so reuse the same
    * RBBM_STATUS poll, waiting for GUI_ACTIVE (bit 31) to clear -- i.e. the
    * previous tile's draws + resolve have fully drained. (Idle RBBM_STATUS
    * reads 0x110; busy reads 0xc40103xx.) */
   if (is_a22x(ctx->screen)) {
      OUT_PKT3(ring, CP_WAIT_REG_EQ, 4);
      OUT_RING(ring, 0x000005d0); /* RBBM_STATUS */
      OUT_RING(ring, 0x00000000); /* reference value: idle */
      OUT_RING(ring, 0x80000000); /* mask: GUI_ACTIVE */
      OUT_RING(ring, 0x00000001); /* poll interval */
   }

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_RB_COLOR_INFO));
   OUT_RING(ring, A2XX_RB_COLOR_INFO_SWAP(fmt2swap(format)) |
                     A2XX_RB_COLOR_INFO_FORMAT(fd2_pipe2color(format)));

   /* setup window scissor and offset for current tile (different
    * from mem2gmem):
    */
   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_PA_SC_WINDOW_OFFSET));
   OUT_RING(ring, A2XX_PA_SC_WINDOW_OFFSET_X(-tile->xoff) |
                     A2XX_PA_SC_WINDOW_OFFSET_Y(-tile->yoff));

   /* write SCISSOR_BR to memory so fast clear path can restore from it */
   OUT_PKT3(ring, CP_MEM_WRITE, 2);
   OUT_RELOC(ring, fd_resource(fd2_ctx->solid_vertexbuf)->bo, 60, 0, 0);
   OUT_RING(ring, A2XX_PA_SC_SCREEN_SCISSOR_BR_X(tile->bin_w) |
                     A2XX_PA_SC_SCREEN_SCISSOR_BR_Y(tile->bin_h));

   /* set the copy offset for gmem2mem */
   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_RB_COPY_DEST_OFFSET));
   OUT_RING(ring, A2XX_RB_COPY_DEST_OFFSET_X(tile->xoff) |
                     A2XX_RB_COPY_DEST_OFFSET_Y(tile->yoff));

   /* tile offset for gl_FragCoord on a20x (C64 in fragment shader) */
   if (is_a20x(ctx->screen)) {
      OUT_PKT3(ring, CP_SET_CONSTANT, 5);
      OUT_RING(ring, 0x00000580);
      OUT_RING(ring, fui(tile->xoff));
      OUT_RING(ring, fui(tile->yoff));
      OUT_RING(ring, fui(0.0f));
      OUT_RING(ring, fui(0.0f));
   }

   if (use_hw_binning(batch)) {
      struct fd_bo *pipe_bo = ctx->vsc_pipe_bo[tile->p];

      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_VGT_CURRENT_BIN_ID_MIN));
      OUT_RING(ring, tile->n);

      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_VGT_CURRENT_BIN_ID_MAX));
      OUT_RING(ring, tile->n);

      /* TODO only emit this when tile->p changes */
      OUT_PKT3(ring, CP_SET_DRAW_INIT_FLAGS, 1);
      OUT_RELOC(ring, pipe_bo, 0, 0, 0);
   }

   /* FD_CYCPROF=1: probe (1 + tidx*3 + 0) = end of renderprep / start of
    * the per-tile batch->draw replay. tile->n is slot-within-pipe, not
    * the global tile index -- compute the global index from gmem->tile. */
   {
      const struct fd_gmem_stateobj *gmem = batch->gmem_state;
      unsigned tidx = tile - gmem->tile;
      fd2_emit_cycprobe(ctx, ring, 1 + tidx * 3 + 0);
   }
}

void
fd2_gmem_init(struct pipe_context *pctx) disable_thread_safety_analysis
{
   struct fd_context *ctx = fd_context(pctx);

   ctx->emit_sysmem_prep = fd2_emit_sysmem_prep;
   ctx->emit_tile_init = fd2_emit_tile_init;
   ctx->emit_tile_prep = fd2_emit_tile_prep;
   ctx->emit_tile_mem2gmem = fd2_emit_tile_mem2gmem;
   ctx->emit_tile_renderprep = fd2_emit_tile_renderprep;
   ctx->emit_tile_gmem2mem = fd2_emit_tile_gmem2mem;
}
