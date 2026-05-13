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

   /* A20X uses shader-memexport-based binning; A22X uses the HW binner
    * engaged by LRZ_VSC_CONTROL=3 + the prelude in the A22X branch below.
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

   /* A22X: Disable LRZ/VSC during GMEM copy operations per KGSL behavior.
    * This ensures the Low Resolution Z and Visibility Stream Cache don't
    * interfere with the GMEM blit operations.
    */
   if (!is_a20x(ctx->screen)) {
      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_A220_RB_LRZ_VSC_CONTROL));
      OUT_RING(ring, 0);
   }

   /* A22X: WFI before changing RB_MODECONTROL to ensure pipeline is idle.
    * Mode transitions between COLOR_DEPTH and EDRAM_COPY require sync.
    */
   if (!is_a20x(ctx->screen))
      OUT_WFI(ring);

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
      /* Restore VGT_VERTEX_REUSE_BLOCK_CNTL: was 0 (disabled debug
       * leftover); restore to KGSL kernel default 0x02. */
      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_VGT_VERTEX_REUSE_BLOCK_CNTL));
      OUT_RING(ring, 0x00000002);
   }
}

static void
fd2_emit_tile_gmem2mem(struct fd_batch *batch, const struct fd_tile *tile)
{
   fd2_emit_ib(batch->gmem, batch->tile_store);

   /*
    * The tile_store IB (built by prepare_tile_fini_ib) clobbers the same
    * categories of state as fd2_emit_tile_mem2gmem - shader program (uses
    * solid_prog), vertex buffer (solid_vertexbuf), RB_DEPTHCONTROL,
    * PA_SU_SC_MODE_CNTL, PA_SC_AA_MASK, PA_CL_VPORT, PA_CL_CLIP_CNTL,
    * RB_MODECONTROL, VGT_VERTEX_REUSE_BLOCK_CNTL.
    *
    * Mark all 3D state dirty so the next user draw re-emits everything.
    * Same rationale as the dirty mark in fd2_emit_tile_mem2gmem.
    */
   fd_context_all_dirty(batch->ctx);
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

   /* A22X: Disable LRZ/VSC during mem2gmem blit per KGSL behavior.
    * This ensures the Low Resolution Z and Visibility Stream Cache don't
    * interfere with the texture restore blit operations.
    */
   if (!is_a20x(ctx->screen)) {
      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_A220_RB_LRZ_VSC_CONTROL));
      OUT_RING(ring, 0);
   }

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
    *
    * Gated on FD2_NO_CACHE_FLUSH_INV for the 0x0ee2 cycle-counter bisect.
    */
   if (!is_a20x(ctx->screen) && !getenv("FD2_NO_CACHE_FLUSH_INV")) {
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

   /*
    * The mem2gmem tile setup above forcibly overwrote a stack of RB / PA
    * registers AND the texture-sampler / shader-program / vertex-buffer
    * state behind the back of fd2_emit_state's dirty-flag tracking:
    *
    *   blend / zsa:    RB_BLEND_CONTROL, RB_COLORCONTROL, RB_DEPTHCONTROL
    *   rasterizer:     PA_SU_SC_MODE_CNTL, PA_CL_VTE_CNTL, PA_CL_CLIP_CNTL
    *   sample_mask:    PA_SC_AA_MASK
    *   scissor:        PA_SC_WINDOW_SCISSOR_*
    *   viewport:       PA_CL_VPORT_*
    *   framebuffer:    RB_COLOR_INFO, RB_SURFACE_INFO, RB_COLOR_MASK
    *   tex/sampler:    SQ_TEX_0..5 (left at SQ_TEX_FILTER_POINT for blit!)
    *   program:        fd2_program_emit installs the blit shader pair
    *   vtxbuf:         fd2_emit_vertex_bufs binds solid_vertexbuf
    *
    * fd2_emit_state only re-emits these on the matching dirty bit. Since
    * the pipe-level state objects haven't changed, those bits stay clean
    * and subsequent user draws in the next batch inherit the GMEM tile
    * setup's values - notably blend disabled with src=ONE/dst=ZERO, and
    * the blit's POINT-filter texture sampler with the FB-as-texture
    * binding.
    *
    * Symptoms on Adreno 220 / luna-surfacemanager + kmscube:
    *   - LSM translucent UI bars render with broken alpha (yellow/orange/red
    *     gradients in place of expected source-over compositing)
    *   - LSM glyph rendering: text appears in the right position but
    *     pixels are garbled - bilinear/linear sampler in user shader
    *     left at POINT filter from the GMEM blit
    *   - kmscube cube faces selectively go black, gradually filling in
    *     across frames as different draws happen to dirty other state
    *   - Touch animation transitions show transient corruption that
    *     "snaps back" once unrelated state changes
    *
    * Fix: mark all overwritten state classes dirty so the next emit_state
    * restores them.
    */
   /*
    * Maximally aggressive: mark all 3D state dirty so the next user draw
    * re-emits every register class. This is a diagnostic build to confirm
    * whether the dirty-flag mechanism is the right lever at all - if this
    * doesn't fix the post-LSM kmscube "white triangle in cube model
    * space" symptom, the bug is somewhere else (state-emit not honoring
    * dirty bits, threaded-context queue ordering, etc).
    */
   fd_context_all_dirty(ctx);
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

   /*
    * fd2_emit_restore (above) writes a stack of GPU state for the bypass
    * renderer setup that fd2_emit_state's dirty-flag tracking doesn't
    * monitor (RB_BC_CONTROL on a20x, VGT_VERTEX_REUSE_BLOCK_CNTL,
    * RBBM_PM_OVERRIDE1/2, TP0_CHICKEN, SQ_VS_CONST/PS_CONST,
    * VGT_*_VTX_INDX, SQ_CONTEXT_MISC, plus a CP_INVALIDATE_STATE that
    * resets a lot more). User draws after this need to re-emit their own
    * state to recover from the bypass-path setup. Same rationale as
    * fd2_emit_tile_mem2gmem / fd2_emit_tile_gmem2mem.
    */
   fd_context_all_dirty(ctx);
}

/* end of tile loop - emit "release slot" events to deallocate the 16-entry
 * VPC / TC / faceness pool that the GPU consumes one slot of per Mesa batch.
 *
 * Background (reports/gemini-summary-2026-05-13-cycle-register.md):
 *   Register word 0x0ee2 advances deterministically per Mesa render with
 *   period 16 - one slot per submit, wrap to slot 0 every 16th render.
 *   1 in 16 caps produces the correct frame (slot 0 = boot state).
 *   Mesa is leaking pool slots; vendor drivers don't (they emit different
 *   events or use a "global" submit path).
 *
 * webOS strings survey: emits CP_EVENT_WRITE for 0x06 (CACHE_FLUSH),
 *   0x17 (PERFCOUNTER_START), 0x18 (PERFCOUNTER_STOP), 0x1c
 *   (FACENESS_FLUSH).  Mesa freedreno A2XX emits only 0x16
 *   (CACHE_FLUSH_AND_INV_EVENT).
 *
 * Three env vars bisect which event(s) collapse the cycle:
 *   FD2_END_CTX_DONE     emit CP_EVENT_WRITE 0x05 (CONTEXT_DONE)
 *   FD2_END_FACENESS     emit CP_EVENT_WRITE 0x1c (FACENESS_FLUSH)
 *   FD2_END_DEALLOC      emit 0x00 (VS_DEALLOC) + 0x01 (PS_DEALLOC)
 *
 * Default (no env vars) = byte-identical to prior behaviour (no events).
 */
static void
fd2_emit_tile_fini(struct fd_batch *batch) assert_dt
{
   struct fd_context *ctx = batch->ctx;
   struct fd_ringbuffer *ring = batch->gmem;

   if (!is_a22x(ctx->screen))
      return;

   if (getenv("FD2_END_CTX_DONE")) {
      OUT_PKT3(ring, CP_EVENT_WRITE, 1);
      OUT_RING(ring, 0x05);  /* CONTEXT_DONE */
   }

   if (getenv("FD2_END_FACENESS")) {
      OUT_PKT3(ring, CP_EVENT_WRITE, 1);
      OUT_RING(ring, 0x1c);  /* FACENESS_FLUSH */
   }

   /* FD2_END_DEALLOC=N emits N pairs of VS_DEALLOC + PS_DEALLOC.
    *
    * Round-3 test showed N=1 (single pair) shifts the period-16 cycle
    * by exactly 1 cap (5adc3160 moves cap 1 -> cap 2).  Gemini round-4
    * hypothesis: each render consumes 2 slots (binning draw + render
    * draw), so we need N=2 to pin the cycle.  Test by setting
    * FD2_END_DEALLOC=2 (or higher to verify saturation).
    * FD2_END_DEALLOC=1 keeps the prior single-pair behaviour for
    * direct comparison.
    */
   const char *dealloc_env = getenv("FD2_END_DEALLOC");
   if (dealloc_env) {
      int n = atoi(dealloc_env);
      if (n <= 0) n = 1;
      for (int i = 0; i < n; i++) {
         OUT_PKT3(ring, CP_EVENT_WRITE, 1);
         OUT_RING(ring, 0x00);  /* VS_DEALLOC */
         OUT_PKT3(ring, CP_EVENT_WRITE, 1);
         OUT_RING(ring, 0x01);  /* PS_DEALLOC */
      }
   }

   /* FD2_END_CACHE_FLUSH_INV_COUNT=N: emit N extra
    * CACHE_FLUSH_AND_INV_EVENT at end of tile loop.
    * Tests whether 0x16 itself is what advances 0x0ee2 by
    * +0x00020200/render.  If yes, emitting 14 here should wrap
    * the counter mod-16 and pin the cycle.
    */
   const char *cfi_env = getenv("FD2_END_CACHE_FLUSH_INV_COUNT");
   if (cfi_env) {
      int n = atoi(cfi_env);
      for (int i = 0; i < n; i++) {
         OUT_PKT3(ring, CP_EVENT_WRITE, 1);
         OUT_RING(ring, 0x16);  /* CACHE_FLUSH_AND_INV_EVENT */
      }
   }

   /* FD2_END_TC_INV=N: emit N inline TC_CNTL_STATUS = L2_INVALIDATE
    * writes at end of tile loop (PM4 type-0 register write).
    *
    * Decompiled vendor libGLESv2 binaries (HTC/Samsung/Xiaomi/webOS)
    * all have a leia_cmdbuffer_inserttexcacheinvalid helper that
    * emits exactly this:
    *
    *   *param_1 = 0xe00;   (PM4 type-0 header, write 1 dword to
    *                        REG_A2XX_TC_CNTL_STATUS word 0x0e00)
    *   param_1[1] = 1;     (value = L2_INVALIDATE bit 0)
    *
    * We tested writing this register via debugfs (CPU-side direct
    * MMIO write) and got no effect on 0x0ee2 cycle.  But that path
    * may hit GPU while clock-gated and silently fail.  Inline-in-
    * cmdstream emission means CP executes the write during active
    * render - different timing, possibly different semantics.
    */
   const char *tcinv_env = getenv("FD2_END_TC_INV");
   if (tcinv_env) {
      int n = atoi(tcinv_env);
      if (n <= 0) n = 1;
      for (int i = 0; i < n; i++) {
         OUT_PKT0(ring, REG_A2XX_TC_CNTL_STATUS, 1);
         OUT_RING(ring, A2XX_TC_CNTL_STATUS_L2_INVALIDATE);
      }
   }

   /* FD2_EMIT_VSC_REG_SAVE=1: emit CP_REG_TO_MEM packets for VSC_PIPE
    * registers at end of tile loop, mirroring legacy KGSL's
    * build_reg_save_cmds A22X path (kgsl_drawctxt.c lines 620-630):
    *
    *   if (chip_id == LEIA_REV470) {
    *       for (i = REG_LEIA_VSC_BIN_SIZE; i <= REG_LEIA_VSC_PIPE_DATA_LENGTH_7; i++) {
    *           *cmd++ = pm4_type3_packet(PM4_REG_TO_MEM, 2);
    *           *cmd++ = i;
    *           *cmd++ = ctx->reg_values[j++];
    *       }
    *   }
    *
    * KGSL emits these REG_TO_MEM packets BETWEEN every user IB as part
    * of context-switch save.  The CP's execution of REG_TO_MEM has the
    * side effect of advancing the binner state machine through
    * 0x7f-namespace transitions in 0x0ee2 that mainline Mesa never
    * produces - and is the most likely cause of the period-16 cycle.
    *
    * Range emitted: VSC_BIN_SIZE (0x0c01) + per-pipe CONFIG/DATA_ADDR/
    * DATA_LEN for 8 pipes (0x0c06..0x0c1d) = 25 register reads total.
    *
    * Cost: 25 CP_REG_TO_MEM packets * 3 dwords = 75 extra dwords per
    * batch.  Scratch BO (vsc_regsave_mem) is allocated lazily on first
    * use and reused for all subsequent batches - no per-batch malloc.
    */
   if (is_a22x(ctx->screen) && getenv("FD2_EMIT_VSC_REG_SAVE")) {
      struct fd2_context *fd2_ctx = fd2_context(ctx);
      if (!fd2_ctx->vsc_regsave_mem) {
         fd2_ctx->vsc_regsave_mem = fd_bo_new(
            ctx->screen->dev, 256, 0, "vsc_regsave");
      }
      if (fd2_ctx->vsc_regsave_mem) {
         /* VSC_BIN_SIZE @ 0x0c01 -> offset 0 */
         OUT_PKT3(ring, CP_REG_TO_MEM, 2);
         OUT_RING(ring, REG_A2XX_A220_VSC_BIN_SIZE);
         OUT_RELOC(ring, fd2_ctx->vsc_regsave_mem, 0, 0, 0);
         /* Per-pipe VSC_PIPE_CONFIG / DATA_ADDRESS / DATA_LENGTH
          * @ 0x0c06..0x0c1d (0x18 = 3 regs x 8 pipes) -> offsets 4..96 */
         for (int p = 0; p < 8; p++) {
            OUT_PKT3(ring, CP_REG_TO_MEM, 2);
            OUT_RING(ring, REG_A2XX_VSC_PIPE_CONFIG(p));
            OUT_RELOC(ring, fd2_ctx->vsc_regsave_mem, 4 + p * 12, 0, 0);
            OUT_PKT3(ring, CP_REG_TO_MEM, 2);
            OUT_RING(ring, REG_A2XX_VSC_PIPE_DATA_ADDRESS(p));
            OUT_RELOC(ring, fd2_ctx->vsc_regsave_mem, 8 + p * 12, 0, 0);
            OUT_PKT3(ring, CP_REG_TO_MEM, 2);
            OUT_RING(ring, REG_A2XX_VSC_PIPE_DATA_LENGTH(p));
            OUT_RELOC(ring, fd2_ctx->vsc_regsave_mem, 12 + p * 12, 0, 0);
         }
      }
   }
}

/* before first tile */
static void
fd2_emit_tile_init(struct fd_batch *batch) assert_dt
{
   struct fd_context *ctx = batch->ctx;
   struct fd2_context *fd2_ctx = fd2_context(ctx);
   struct fd_ringbuffer *ring = batch->gmem;
   struct pipe_framebuffer_state *pfb = &batch->framebuffer;
   const struct fd_gmem_stateobj *gmem = batch->gmem_state;
   enum pipe_format format = pipe_surface_format(&pfb->cbufs[0]);
   uint32_t reg;

   /* DIAGNOSTIC: CP_SCRATCH milestone markers for hang localization.
    *
    * a2xx_recover() in the kernel dumps SCRATCH_REG0..7 when hangcheck
    * fires. By writing distinctive values at key points in the cmdstream
    * we can see how far the CP got before the hang.
    *
    *   REG0=0x10001 entered fd2_emit_tile_init
    *   REG1=0x10002 after fd2_emit_restore returned
    *   REG2=0x10003 after Fork D prelude (binner engaged)
    *   REG3=0x10004 just before fd2_emit_ib(batch->binning)
    *   REG4=0x10005 just after fd2_emit_ib(batch->binning)
    *   REG5=0x10006 after binner disengage (LRZ_VSC_CONTROL=0)
    *   REG6=0x10007 end of fd2_emit_tile_init
    *
    * Any SCRATCH_REG[N] that's 0 in the kernel hang dump means the CP
    * did not reach that milestone.
    */
   OUT_PKT0(ring, REG_AXXX_CP_SCRATCH_REG0, 1);
   OUT_RING(ring, 0x10001);

   fd2_emit_restore(ctx, ring);

   OUT_PKT0(ring, REG_AXXX_CP_SCRATCH_REG1, 1);
   OUT_RING(ring, 0x10002);

   prepare_tile_fini_ib(batch);

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

   /* set to zero, for some reason hardware doesn't like certain values
    *
    * A22X: skip the zero-write. The 0-value-is-bad warning in the
    * original comment is real - the A22X hardware binner enters a
    * cycling-state-machine mode when BIN_ID=0, producing the period-8
    * tile-coverage cycle. Per-tile non-zero BIN_ID writes happen in
    * fd2_emit_tile_renderprep below; let those be the only writes so
    * the binner never sees BIN_ID=0.
    */
   if (!is_a22x(ctx->screen)) {
      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_VGT_CURRENT_BIN_ID_MIN));
      OUT_RING(ring, 0);

      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_VGT_CURRENT_BIN_ID_MAX));
      OUT_RING(ring, 0);
   }

   /* A22X hw binning prelude (Fork D + Fork A/B integration 2026-05-11)
    *
    * Only emit when we're actually going to run the binning IB
    * (use_hw_binning true). Without the IB, engaging the binner
    * with no work would hang.
    *
    * Emit the binning state setup matching webOS proprietary libGLESv2.so
    * leia_configure_binning_pass (decomp @ 0x124a50):
    *   1. VSC_BIN_SIZE  -- tile dimensions in 32px units
    *   2. VSC_PIPE[0..7] CONFIG + DATA_ADDRESS + DATA_LENGTH for 8 pipes
    *   3. A220_VSC_ENABLE @ 0xC00 = 1   (undocumented master enable)
    *   4. A220_RB_LRZ_VSC_CONTROL = 3   (engage the binner)
    *
    * Phase 0 (LRZ_VSC_CONTROL=3 alone) collapsed the 8-cycle to 1 hash but
    * hung the GPU because the binner had nowhere to write. This block adds
    * the pipe BOs + VSC config so the binner has buffers, removing the hang.
    *
    * Fork C intentionally does NOT emit:
    *   - SQ_GPR_MANAGEMENT = 0x7f010 (binning shader register partition).
    *     We use the regular VS+PS shaders for both binning and rendering,
    *     so the shader's own SQ_PROGRAM_CNTL handling stays in effect.
    *   - A separate binning shader variant or two-pass rendering. The
    *     binner produces visibility data passively during the normal
    *     draw stream; tile filtering relies on the per-tile BIN_ID
    *     writes from patch 0090.
    *
    * Reference: reports/a22x-hw-binning-port-analysis.md (sections 1-4).
    */
   /* A22X Fork A/B baseline (2026-05-12, post-bisect):
    *   Default behaviour for A22X is "Test 4" — proven stable across
    *   30+ caps with no hangs:
    *     use_hw_binning  TRUE (per-tile DRAW_INIT_FLAGS for A20X path)
    *     prelude         RUN  (engage HW binner before binning IB)
    *     binning IB      RUN  (VS-only binning shader populates VSC pipe BOs)
    *     disengage       RUN  (LRZ_VSC_CONTROL=0 after binning IB)
    *     visibility flt  SKIP for A22X (A20X keeps its working filter)
    *
    *   The visibility filter (patch_draws(USE_VISIBILITY) + per-tile
    *   CP_SET_DRAW_INIT_FLAGS) hard-hangs A22X regardless of whether
    *   the binning IB populated the BOs. webOS likely uses a different
    *   A22X-specific visibility-consume packet — TBD.
    *
    * Bisect env-var toggles (diagnostic only):
    *   FD2_SKIP_PRELUDE       skip Fork D prelude (LRZ_VSC_CONTROL=3, etc)
    *   FD2_SKIP_DISENGAGE     skip LRZ_VSC_CONTROL=0 after binning IB
    *   FD2_SKIP_BINNING_IB    skip fd2_emit_ib(batch->binning)
    *   FD2_FORCE_VISIBILITY   re-enable patch_draws(USE_VIS) + per-tile
    *                          CP_SET_DRAW_INIT_FLAGS for A22X (WILL HANG)
    *   FD2_USE_BIN_DATA       enable webOS-style per-tile CP_SET_BIN_DATA
    *                          consume (implies FULL_DISENGAGE + 0xC04=0).
    *                          Patches draws to USE_VISIBILITY so the
    *                          filter reads the bound stream.
    *                          v5 default emits SET_BIN_DATA only, no WAIT.
    *   FD2_BIN_DATA_NO_VIS    v6 diagnostic: emits SET_BIN_DATA per
    *                          tile and the full-disengage / 0xC04=0
    *                          prelude, but keeps patch_draws as
    *                          IGNORE_VISIBILITY.  Lets us tell whether
    *                          the hang is in SET_BIN_DATA itself or in
    *                          the visibility-filter draws.
    *   FD2_BIN_WAIT           also emit the per-tile CP_WAIT_REG_EQ on
    *                          RBBM_DEBUG.A220_BINNER_DONE before each
    *                          SET_BIN_DATA (v3 behaviour).  Diagnostic
    *                          only — hangs the GPU because bit 24
    *                          never latches on Mesa's path.
    *   FD2_BIN_FULL_DISENGAGE add 0xC00=0 + CACHE_FLUSH event +
    *                          CP_WAIT_REG_EQ RBBM_STATUS to the
    *                          disengage block (v3 — required for the
    *                          binner to actually flush its FIFOs;
    *                          collapses the period-16 cycle on its own)
    *   FD2_VSC_DUMP           dump VSC pipe BO contents per batch (diag)
    *   FD2_PIN_BIN_ID=<n>     v8 diagnostic: override per-tile
    *                          VGT_CURRENT_BIN_ID with a fixed value <n>
    *                          for all tiles.  Tests whether the SQ
    *                          wavefront slot is keyed off bin_id (and
    *                          therefore whether per-tile slot rotation
    *                          is the source of the period-N cycle).
    */
   /* Allocate VSC pipe BOs + the per-context VSC_SIZE BO for A22X
    * when use_hw_binning is active.  Per-tile renderprep emits
    * CP_SET_BIN_DATA referencing both when FD2_USE_BIN_DATA /
    * FD2_BIN_DATA_NO_VIS is set, so they must exist by then.
    *
    * v7: also gate the vsc_size_mem allocation behind the same env
    * vars.  v6 default unconditionally allocated it and wrote
    * VSC_SIZE_ADDRESS in the prelude; that perturbed the binner state
    * enough to drift the hash pool and (after a 15 s idle) wedge the
    * device during runtime-PM suspend.  v7 default never allocates
    * size_mem and never touches VSC_SIZE_ADDRESS — byte-identical to
    * pre-0098 Test 4 baseline.
    */
   bool use_bin_data = is_a22x(ctx->screen) &&
                       (getenv("FD2_USE_BIN_DATA") ||
                        getenv("FD2_BIN_DATA_NO_VIS") ||
                        getenv("FD2_BIN_FULL_DISENGAGE"));
   if (is_a22x(ctx->screen) && use_hw_binning(batch)) {
      for (int i = 0; i < 8; i++) {
         if (!ctx->vsc_pipe_bo[i]) {
            ctx->vsc_pipe_bo[i] = fd_bo_new(ctx->dev, 0x40000, 0,
                                            "a22x_vsc_pipe[%u]", i);
            assert(ctx->vsc_pipe_bo[i]);
         }
      }
      if (use_bin_data && !fd2_ctx->vsc_size_mem) {
         /* 32 bytes is enough (8 pipes × 4 bytes), but match the A3XX
          * 0x1000 page allocation so the BO lands on its own page and
          * can never share a cache line with unrelated state. */
         fd2_ctx->vsc_size_mem =
            fd_bo_new(ctx->dev, 0x1000, 0, "a22x_vsc_size");
         assert(fd2_ctx->vsc_size_mem);
      }
   }

   if (is_a22x(ctx->screen) && !getenv("FD2_SKIP_PRELUDE")) {
      const unsigned num_pipes = 8;

      /* DIAGNOSTIC: dump VSC pipe BO contents from the PREVIOUS batch
       * (which contains the binner's visibility stream output for that
       * batch's tile geometry) before we re-use the BOs for this batch.
       *
       * Enabled when env var FD2_VSC_DUMP is set. First batch in a
       * process will dump zeros (BOs freshly allocated below); the
       * second and subsequent batches will see the prior binner output.
       *
       * Output: /tmp/vsc_pipe<P>_batch<N>.dump (first 4KB per pipe).
       *
       * Used to determine whether the period-8 render cycle has its
       * source in the binner (different VSC byte content per phase)
       * or downstream (rasterizer/RB processing same binner data
       * differently each cycle).
       */
      static int dump_batch_counter = 0;
      if (getenv("FD2_VSC_DUMP")) {
         for (int i = 0; i < num_pipes; i++) {
            if (!ctx->vsc_pipe_bo[i])
               continue;
            /* Wait for GPU to finish any pending writes to this BO. */
            fd_bo_cpu_prep(ctx->vsc_pipe_bo[i], ctx->pipe, FD_BO_PREP_READ);
            void *map = fd_bo_map(ctx->vsc_pipe_bo[i]);
            if (!map)
               continue;
            char path[64];
            snprintf(path, sizeof(path), "/tmp/vsc_pipe%d_batch%d.dump",
                     i, dump_batch_counter);
            FILE *f = fopen(path, "wb");
            if (f) {
               fwrite(map, 4096, 1, f);
               fclose(f);
            }
         }
         dump_batch_counter++;
      }

      /* Allocate one 256KB VSC pipe BO per pipe (matches webOS initial
       * allocation in leia_binning_grow_vis_stream_buffer). Buffers are
       * reused across batches; grown lazily if a future patch tracks
       * pipe data length overflow.
       */
      for (int i = 0; i < num_pipes; i++) {
         if (!ctx->vsc_pipe_bo[i]) {
            ctx->vsc_pipe_bo[i] = fd_bo_new(ctx->dev, 0x40000, 0,
                                            "a22x_vsc_pipe[%u]", i);
            assert(ctx->vsc_pipe_bo[i]);
         }
      }

      /* VSC_BIN_SIZE: bin_w / bin_h in 32-pixel units.
       *
       * NOTE: VSC registers live at 0x0C00..0x0C1D which is BELOW the
       * 3D state block (0x2000-0x2FFF) that CP_SET_CONSTANT uses. Mesa's
       * CP_REG() macro does (reg - 0x2000) and only works for registers
       * >= 0x2000. For VSC registers we must use raw PM4 type-0 packets
       * via OUT_PKT0. webOS does the same (type-0 packet 0x170c06 for
       * the 24-reg VSC_PIPE block, 0x000c00 for the master enable).
       */
      OUT_PKT0(ring, REG_A2XX_A220_VSC_BIN_SIZE, 1);
      OUT_RING(ring, A2XX_A220_VSC_BIN_SIZE_WIDTH(gmem->bin_w) |
                     A2XX_A220_VSC_BIN_SIZE_HEIGHT(gmem->bin_h));

      /* VSC_SIZE_ADDRESS: tell the binner where to write the per-pipe
       * visibility-stream length (one u32 per pipe).  Mirrors A3XX
       * update_vsc_pipe(), but using OUT_PKT0 because A2XX VSC regs
       * are below 0x2000 (CP_SET_CONSTANT range).  The per-tile
       * CP_SET_BIN_DATA packet later loads from this BO at offset
       * tile->p * 4 to feed the visibility filter.
       *
       * v5 ordering: webOS leia_configure_binid_groups (decomp line 3358)
       * writes the VSC configs in this exact order, per renderpass:
       *   0xC01 (VSC_BIN_SIZE) -> 0xC02 (VSC_SIZE_ADDRESS) ->
       *   0xC04 = 0 (undocumented A22X-only binner control) ->
       *   0xC06+ (VSC_PIPE array, 24 dwords for 8 pipes).
       * The 0xC04 = 0 write is critical: KGSL inits the full 0xC01..0xC1D
       * range at context start, but mainline drm/msm a2xx doesn't, so
       * 0xC04 holds stale state and CP_SET_BIN_DATA later trips on it.
       */
      /* v7: gate the VSC_SIZE_ADDRESS write behind the same env-vars
       * that gate the rest of the binner-consume work.  Default path
       * never writes this register — pre-0098 baseline restored.
       */
      if (use_bin_data) {
         OUT_PKT0(ring, REG_A2XX_A220_VSC_SIZE_ADDRESS, 1);
         OUT_RELOC(ring, fd2_ctx->vsc_size_mem, 0, 0, 0);
      }

      /* v5: undocumented A22X-only binner control register @ 0xC04.
       * webOS writes 0 here once per renderpass.  Without this, the
       * binner / visibility filter operates on stale state and
       * CP_SET_BIN_DATA hangs the GPU (confirmed v4 on-device test
       * 2026-05-12: 0098 v4 with FD2_USE_BIN_DATA=1 caused hw_init
       * reset loop and USB drop).  No symbolic name in any reference
       * we've seen; KGSL's adreno_a2xx.c snapshot range includes it
       * (REG_A220_VSC_BIN_SIZE..REG_A220_VSC_PIPE_DATA_LENGTH_7) so
       * it is a real register, just undocumented.
       *
       * v6: gated behind FD2_USE_BIN_DATA / FD2_BIN_FULL_DISENGAGE.
       * v5 emitted this unconditionally, which broke FD2_VSC_DUMP
       * double-render mode (confirmed v5 on-device 2026-05-12: device
       * full lockup, rapid-fire hangchecks, recover_worker stuck).
       * Default path (no env vars) must remain a no-op vs pre-0098
       * behaviour.
       */
      if (use_bin_data) {
         OUT_PKT0(ring, 0x0c04, 1);
         OUT_RING(ring, 0x00000000);
      }

      /* VSC_PIPE[0..7]: 24 consecutive regs at 0xC06 (CONFIG, ADDRESS, LENGTH)
       * CONFIG encoding from webOS decomp: (h<<24)|(w<<20)|(y<<10)|x.
       * Bit layout independently verified against documented A3XX
       * VSC_PIPE_CONFIG (X[0:9], Y[10:19], W[20:23], H[24:27]).
       */
      OUT_PKT0(ring, REG_A2XX_VSC_PIPE_CONFIG(0), num_pipes * 3);
      for (int i = 0; i < num_pipes; i++) {
         const struct fd_vsc_pipe *pipe = &gmem->vsc_pipe[i];
         uint32_t config = ((pipe->h & 0xf) << 24) |
                           ((pipe->w & 0xf) << 20) |
                           ((pipe->y & 0x3ff) << 10) |
                           (pipe->x & 0x3ff);
         OUT_RING(ring, config);
         OUT_RELOC(ring, ctx->vsc_pipe_bo[i], 0, 0, 0);
         OUT_RING(ring, fd_bo_size(ctx->vsc_pipe_bo[i]));
      }

      /* Explicit disengage of any previously-engaged binner before
       * re-engaging in this batch.
       *
       * Hangs investigation 2026-05-11: with FD2_VSC_DUMP=1 enabling
       * double-render mode, every gl-cap invocation hangs on the
       * second batch's submission. netconsole shows fence delta = 1
       * across 62 hangchecks (batch N+1 hangs every time). Cause:
       * the binner stays engaged across batches; the second batch's
       * LRZ_VSC_CONTROL=3 write on an already-engaged binner deadlocks.
       *
       * clear_state_restore writes LRZ_VSC_CONTROL=0 at batch start but
       * that path may not include a WFI, so the disengage doesn't commit
       * before our prelude runs. Force a clean disengage with explicit
       * WFI here so the engage that follows starts from a known state.
       */
      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_A220_RB_LRZ_VSC_CONTROL));
      OUT_RING(ring, 0x00000000);
      OUT_WFI(ring);

      /* A220_VSC_PIPE_PARTITIONING.ENABLE = 1 -- master engage for the
       * HW binner.  webOS writes this via a type-0 PM4 packet at the
       * start of leia_configure_binning_pass; without it the binner does
       * not advance past stream-out setup.
       */
      OUT_PKT0(ring, REG_A2XX_A220_VSC_PIPE_PARTITIONING, 1);
      OUT_RING(ring, A2XX_A220_VSC_PIPE_PARTITIONING_ENABLE);

      /* Engage the binner: A220_RB_LRZ_VSC_CONTROL = 3 (webOS value).
       * WFI follows immediately after, per webOS ordering — earlier v2
       * placed WFI BEFORE this write which was the wrong position.
       */
      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_A220_RB_LRZ_VSC_CONTROL));
      OUT_RING(ring, 0x00000003);

      OUT_WFI(ring);

      /* Fork D: pulse SQ_GPR_MANAGEMENT to the binning shader partition
       * (REG_SIZE_VTX=127, REG_SIZE_PIX=1, REG_DYNAMIC=0) then restore
       * to Mesa's dynamic value (0x00040401 = VS=64/PS=64 dynamic) so
       * the regular VS+PS shaders that run for the actual draws have
       * enough register budget.
       *
       * Hypothesis (Phase 1 analysis section 9, and 2c65153c "5/6 correct"
       * observation): the A22X binner samples SQ_GPR_MANAGEMENT at engage
       * time to define wavefront-slot boundaries for its visibility
       * stream. With an unbounded/random initial value, the slot pointer
       * rotates through 8 stable mis-indexed states (the period-8 cycle).
       * Pulsing the boundary at engage clamps the slot allocator. Restore
       * before draws ensures PS has the GPRs it needs.
       *
       * webOS writes this AFTER LRZ_VSC_CONTROL=3 + WFI without an
       * explicit restore in the binning_pass cmdbuf — but webOS's render-
       * pass shader load (rb_gpuprogram_loadexecutable_internal) emits
       * its own SQ_GPR_MANAGEMENT for the real shader. Mesa's regular
       * shader path doesn't re-emit SQ_GPR_MANAGEMENT per draw, so we
       * restore here.
       */
      OUT_PKT0(ring, REG_A2XX_SQ_GPR_MANAGEMENT, 1);
      OUT_RING(ring, 0x0007f010);   /* VTX=127, PIX=1, static */

      OUT_WFI(ring);

      OUT_PKT0(ring, REG_A2XX_SQ_GPR_MANAGEMENT, 1);
      OUT_RING(ring, 0x00040401);   /* VS=64, PS=64, REG_DYNAMIC=1 */

      /* DIAGNOSTIC: marker 3 = passed Fork D prelude */
      OUT_PKT0(ring, REG_AXXX_CP_SCRATCH_REG2, 1);
      OUT_RING(ring, 0x10003);
   }

   if (use_hw_binning(batch)) {
      /* A22X default = IGNORE_VISIBILITY (Test 4 shipping baseline).
       * Env-vars that switch the A22X consume path:
       *
       *   FD2_USE_BIN_DATA       webOS-style CP_SET_BIN_DATA per tile.
       *                          Implies USE_VISIBILITY so the draws
       *                          actually consume the stream the
       *                          packet binds.
       *   FD2_BIN_DATA_NO_VIS    same as above BUT keep patch_draws as
       *                          IGNORE_VISIBILITY (v6 diagnostic):
       *                          emits SET_BIN_DATA so the binner
       *                          state machine "consumes" each pipe,
       *                          but the draws don't actually filter
       *                          on the visibility stream.  If
       *                          USE_BIN_DATA hangs but NO_VIS does
       *                          not, the bug is in the draw-side
       *                          visibility filter consumption, not
       *                          in SET_BIN_DATA itself.
       *   FD2_FORCE_VISIBILITY   legacy A20X CP_SET_DRAW_INIT_FLAGS
       *                          path.  Confirmed to hard-hang on A22X.
       *                          Falsifier knob only.
       *
       * A20X always uses USE_VISIBILITY (its CP_SET_DRAW_INIT_FLAGS path
       * is the original, working freedreno binning consume).
       */
      bool a22x = is_a22x(ctx->screen);
      bool a22x_visibility = a22x && !getenv("FD2_BIN_DATA_NO_VIS") &&
                             (getenv("FD2_USE_BIN_DATA") ||
                              getenv("FD2_FORCE_VISIBILITY"));
      patch_draws(batch,
                  (!a22x || a22x_visibility) ? USE_VISIBILITY
                                             : IGNORE_VISIBILITY);

      if (is_a20x(ctx->screen)) {
         /* A20X hw binning: shader-memexport mechanism.
          *
          * Patch out unneeded memory exports by changing EXEC CF to EXEC_END.
          * In the shader compiler, we guarantee that the shader ends with
          * a specific pattern of ALLOC/EXEC CF pairs for the hw binning exports.
          *
          * The patches point only to dwords and CFs are 1.5 dwords so the
          * patch is aligned and might point to an ALLOC CF.
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
      }
      /* For A22X, the hw binner produces visibility data itself. The
       * BO allocation + VSC_PIPE_CONFIG + LRZ_VSC_CONTROL=3 prelude
       * happens earlier in this function (Fork D block). The binning
       * IB below will run while the binner is engaged.
       *
       * Note: A22X's binning shader (ir2 variant 0) still has the 8
       * memexport CFs from extra_position_exports() in ir2_nir.c.
       * Without A20X's memexport address constants (skipped above),
       * those CFs would write to undefined addresses. For now we don't
       * skip them — the HW binner is the actual visibility producer;
       * the memexport CFs are stray writes that may corrupt memory but
       * shouldn't change rasterization. This is a known issue to fix
       * in a follow-up (modify ir2_nir.c to skip memexport for A22X).
       */

      /* Disable vertex reuse during binning (legitimate - binning has
       * its own vertex processing constraints). */
      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_VGT_VERTEX_REUSE_BLOCK_CNTL));
      OUT_RING(ring, 0);

      /* DIAGNOSTIC: marker 4 = about to dispatch binning IB */
      OUT_PKT0(ring, REG_AXXX_CP_SCRATCH_REG3, 1);
      OUT_RING(ring, 0x10004);

      /* A22X: run the binning IB by default. The IB executes safely
       * when visibility filter is disabled (the default config). Set
       * FD2_SKIP_BINNING_IB to skip it (diagnostic).
       *
       * FD2_SPLIT_BINNING=1: do NOT emit the binning IB here inline.
       * Instead, flush_ring() in freedreno_gmem.c issues batch->binning
       * as its own separate MSM_SUBMIT BEFORE the main batch.  This
       * matches webOS's submit-topology where binning is a distinct
       * ioctl, triggering the GPU's 0x7f-namespace transition in
       * register 0x0ee2 (see reports/webos-ib-decode-2026-05-13.md).
       */
      if ((!is_a22x(ctx->screen) || !getenv("FD2_SKIP_BINNING_IB")) &&
          !getenv("FD2_SPLIT_BINNING")) {
         fd2_emit_ib(ring, batch->binning);
      }

      /* DIAGNOSTIC: marker 5 = binning IB returned */
      OUT_PKT0(ring, REG_AXXX_CP_SCRATCH_REG4, 1);
      OUT_RING(ring, 0x10005);

      /* Restore VGT_VERTEX_REUSE_BLOCK_CNTL after binning - was 0
       * (debug leftover that was never reverted); restore to KGSL
       * kernel default 0x02. */
      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_VGT_VERTEX_REUSE_BLOCK_CNTL));
      OUT_RING(ring, 0x00000002);

      if (is_a22x(ctx->screen) && !getenv("FD2_SKIP_DISENGAGE")) {
         /* Disengage the A22X HW binner after the binning IB has run.
          * Subsequent per-tile rendering uses the visibility data the
          * binner wrote to VSC_PIPE BOs, but the binner itself should
          * not be re-engaged for render-pass draws.
          *
          * webOS-equivalent block (leia_perform_resolve decomp, 12 dwords
          * gated on `binner_used`):
          *   1. LRZ_VSC_CONTROL = 0                  (disengage binner)
          *   2. A220_VSC_PIPE_PARTITIONING (0xC00)=0 (master disengage)
          *   3. CP_EVENT_WRITE CACHE_FLUSH (event 6) (flush binner FIFOs;
          *                                            sets RBBM_DEBUG.A220_BINNER_DONE)
          *   4. CP_WAIT_REG_EQ RBBM_STATUS, mask=0x5f601000  (3D pipe idle)
          *
          * v2 of this patch only emitted (1) + WFI; per-tile
          * CP_WAIT_REG_EQ then polled RBBM_DEBUG bit 24 forever because
          * the CACHE_FLUSH event was missing.  v3 adds (2), (3), (4) to
          * mirror webOS exactly.
          *
          * Default-disabled via FD2_BIN_FULL_DISENGAGE so the v2 path
          * remains the fallback if v3 still doesn't unblock the bit.
          */
         OUT_PKT3(ring, CP_SET_CONSTANT, 2);
         OUT_RING(ring, CP_REG(REG_A2XX_A220_RB_LRZ_VSC_CONTROL));
         OUT_RING(ring, 0x00000000);

         if (use_bin_data) {
            /* (2) Master disengage of the A220 binner module. */
            OUT_PKT0(ring, REG_A2XX_A220_VSC_PIPE_PARTITIONING, 1);
            OUT_RING(ring, 0);

            /* (3) CACHE_FLUSH event — flushes binner internal FIFOs and
             *     latches RBBM_DEBUG.A220_BINNER_DONE (bit 24).
             */
            OUT_PKT3(ring, CP_EVENT_WRITE, 1);
            OUT_RING(ring, CACHE_FLUSH);

            /* (4) Explicit poll for full 3D pipeline idle.  webOS uses
             *     this in place of WFI; the mask covers VGT/SX/TPC/SC/PA/
             *     RB/SQ_CNTX0+17/VGT_NO_DMA — every stage that could
             *     still be holding binner state.  WFI alone only waits
             *     on the CP queue and is not sufficient here.
             */
            OUT_PKT3(ring, CP_WAIT_REG_EQ, 4);
            OUT_RING(ring, REG_A2XX_RBBM_STATUS);
            OUT_RING(ring, 0x00000000);
            OUT_RING(ring, 0x5f601000);
            OUT_RING(ring, 0x00000001);
         } else {
            OUT_WFI(ring);
         }

         /* DIAGNOSTIC: marker 6 = disengage complete */
         OUT_PKT0(ring, REG_AXXX_CP_SCRATCH_REG5, 1);
         OUT_RING(ring, 0x10006);
      }
   } else {
      patch_draws(batch, IGNORE_VISIBILITY);
   }

   /* DIAGNOSTIC: marker 7 = end of fd2_emit_tile_init */
   OUT_PKT0(ring, REG_AXXX_CP_SCRATCH_REG6, 1);
   OUT_RING(ring, 0x10007);

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

      /* v8 diagnostic: pin VGT_CURRENT_BIN_ID to a single value across
       * all tiles to test the SQ-wavefront-slot-via-bin_id hypothesis.
       *
       * Visual analysis of v6 T1 default captures shows the period-N
       * cycle is per-tile vertex-attribute corruption — each tile picks
       * a different SQ slot, only one has correct state.  tile->n
       * cycles 0..N-1 for the N visible tiles and is the obvious
       * candidate for "what selects the SQ slot".
       *
       * If FD2_PIN_BIN_ID=<n> is set, all tiles emit the same bin_id
       * value.  All tiles would then land on the same SQ slot; if that
       * slot holds the user's correct state, ALL tiles should render
       * correctly and we'd see a clean 5adc3160 (or a single-hash
       * collapse to a non-blank correct render).
       *
       * If pinning collapses the cycle and the result is a real
       * triangle: SQ-slot-via-bin_id hypothesis CONFIRMED.
       * If pinning collapses to blank/wrong: bin_id IS the slot
       * selector but no slot holds the correct state.
       * If pinning doesn't collapse: hypothesis FALSIFIED.
       */
      const char *pin = getenv("FD2_PIN_BIN_ID");
      uint32_t bin_id = pin ? (uint32_t)atoi(pin) : tile->n;

      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_VGT_CURRENT_BIN_ID_MIN));
      OUT_RING(ring, bin_id);

      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_VGT_CURRENT_BIN_ID_MAX));
      OUT_RING(ring, bin_id);

      if (is_a22x(ctx->screen)) {
         /* A22X visibility-stream consume.  Matches webOS
          * leia_perform_resolve per-tile sequence:
          *
          *   CP_WAIT_REG_EQ  reg=RBBM_DEBUG mask=0x01000000 val=0x01000000
          *   CP_SET_BIN_DATA pipe_data_addr=VSC_PIPE[p].DATA_ADDRESS
          *                   bin_size_addr =VSC_SIZE_ADDRESS + p*4
          *
          * The WAIT_REG_EQ stalls the CP until the binner reports its
          * per-pipe stream is complete (RBBM_DEBUG.A220_BINNER_DONE).
          * Then CP_SET_BIN_DATA tells the visibility filter where to
          * find the stream + how many bytes are valid for this tile's
          * pipe.  Mesa's CP_SET_DRAW_INIT_FLAGS path (used on A20X) is
          * the wrong consume packet for A22X and hangs the GPU.
          *
          * Default-disabled: needs FD2_USE_BIN_DATA=1 (or v6's
          * FD2_BIN_DATA_NO_VIS=1) in the environment so we can bisect
          * against the Test 4 baseline (IGNORE_VISIBILITY, no per-tile
          * consume) without rebuild.
          */
         if (getenv("FD2_USE_BIN_DATA") || getenv("FD2_BIN_DATA_NO_VIS")) {
            /* v4: per-tile CP_WAIT_REG_EQ default-OFF.
             *
             * v3 test (2026-05-12, 10 caps): adding the WAIT here hangs
             * the GPU 4 times per cap — hangcheck fires after 3s and
             * recover_worker resets the pipeline.  The poll never sees
             * bit 24 latch even with CACHE_FLUSH event in the disengage
             * block.  The cycle DID collapse (16 hashes → 1 hash, all
             * d1dd210d) so the binner is working, but the output is the
             * post-recovery state, not a real render.
             *
             * Set FD2_BIN_WAIT=1 to re-enable the WAIT (diagnostic for
             * finding the right poll target).
             */
            if (getenv("FD2_BIN_WAIT")) {
               OUT_PKT3(ring, CP_WAIT_REG_EQ, 4);
               OUT_RING(ring, REG_A2XX_RBBM_DEBUG);
               OUT_RING(ring, 0x01000000);  /* A220_BINNER_DONE = 1 */
               OUT_RING(ring, 0x01000000);  /* mask */
               OUT_RING(ring, 0x00000001);  /* poll interval */
            }

            OUT_PKT3(ring, CP_SET_BIN_DATA, 2);
            OUT_RELOC(ring, pipe_bo, 0, 0, 0);       /* BIN_DATA_ADDR */
            OUT_RELOC(ring, fd2_ctx->vsc_size_mem,   /* BIN_SIZE_ADDR */
                      tile->p * 4, 0, 0);
         }
      } else {
         /* A20X path: CP_SET_DRAW_INIT_FLAGS points the rasterizer at
          * the VSC pipe BO.  Confirmed working for A20X.
          */
         OUT_PKT3(ring, CP_SET_DRAW_INIT_FLAGS, 1);
         OUT_RELOC(ring, pipe_bo, 0, 0, 0);
      }
   } else if (is_a22x(ctx->screen)) {
      /*
       * A22X period-8 render-cycle fix: explicitly write per-tile bin ID.
       *
       * Without hardware binning enabled (use_hw_binning() returns false
       * for A22X - "TODO" in this file), Mesa previously left
       * VGT_CURRENT_BIN_ID_MIN = MAX = 0 throughout the tile loop (set
       * once in fd2_emit_tile_init with the comment "for some reason
       * hardware doesn't like certain values"). The A22X hardware binner
       * is nevertheless active under the hood; with bin_id pinned at 0
       * it uses some internal-counter state to decide tile coverage,
       * producing a deterministic 8-pattern visibility cycle observed
       * across many investigations (see reports/ pattern of 8 distinct
       * pixel-hash outputs where 1/8 is bit-exact correct and 7/8 are
       * partial tile coverage).
       *
       * Encoding (revised 2026-05-11 from webOS libGLESv2.so decomp):
       *   bin_id = (col + 1) | (row << 3)
       *
       * Initial v1 attempted the A20X reference encoding
       * (((row+1) << 3) | (col+1) from freedreno_gmem.c:392) which
       * produced no effect. Deeper analysis of the webOS proprietary
       * decomp showed the pattern:
       *   uVar24 = uVar27 + 1 | iVar5 * 8 | uVar24;
       * which decodes as (col+1) | (row<<3) - col gets the +1
       * offset, row does not. This produces bin_id=0x01 for the
       * first tile rather than 0x09.
       *
       * For the standard 1024x768 / 2x3 tile layout this produces
       * bin_ids 0x01, 0x02, 0x09, 0x0a, 0x11, 0x12 - all non-zero,
       * all distinct. The (col+1) ensures the col-zero case lands
       * on 1 instead of 0 (the "reserved" value the binner cycles
       * around).
       */
      uint32_t col = tile->xoff / tile->bin_w;
      uint32_t row = tile->yoff / tile->bin_h;
      uint32_t bin_id = (col + 1) | (row << 3);

      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_VGT_CURRENT_BIN_ID_MIN));
      OUT_RING(ring, bin_id);

      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_VGT_CURRENT_BIN_ID_MAX));
      OUT_RING(ring, bin_id);
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
   ctx->emit_tile_fini = fd2_emit_tile_fini;
}
