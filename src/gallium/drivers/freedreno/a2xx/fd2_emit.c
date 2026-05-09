/*
 * Copyright © 2012-2013 Rob Clark <robclark@freedesktop.org>
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include "pipe/p_state.h"
#include "util/u_helpers.h"
#include "util/u_memory.h"
#include "util/u_string.h"

#include "freedreno_resource.h"

#include "fd2_blend.h"
#include "fd2_context.h"
#include "fd2_emit.h"
#include "fd2_program.h"
#include "fd2_rasterizer.h"
#include "fd2_texture.h"
#include "fd2_util.h"
#include "fd2_zsa.h"

/* NOTE: just define the position for const regs statically.. the blob
 * driver doesn't seem to change these dynamically, and I can't really
 * think of a good reason to so..
 */
#define VS_CONST_BASE 0x20
#define PS_CONST_BASE 0x120

static void
emit_constants(struct fd_ringbuffer *ring, uint32_t base,
               struct fd_constbuf_stateobj *constbuf,
               struct fd2_shader_stateobj *shader)
{
   uint32_t enabled_mask = constbuf->enabled_mask;
   uint32_t start_base = base;
   unsigned i;

   /* emit user constants: */
   while (enabled_mask) {
      unsigned index = ffs(enabled_mask) - 1;
      struct pipe_constant_buffer *cb = &constbuf->cb[index];
      unsigned size = align(cb->buffer_size, 4) / 4; /* size in dwords */

      // I expect that size should be a multiple of vec4's:
      assert(size == align(size, 4));

      /* hmm, sometimes we still seem to end up with consts bound,
       * even if shader isn't using them, which ends up overwriting
       * const reg's used for immediates.. this is a hack to work
       * around that:
       */
      if (shader && ((base - start_base) >= (shader->first_immediate * 4)))
         break;

      const uint32_t *dwords;

      if (cb->user_buffer) {
         dwords = cb->user_buffer;
      } else {
         struct fd_resource *rsc = fd_resource(cb->buffer);
         dwords = fd_bo_map(rsc->bo);
      }

      dwords = (uint32_t *)(((uint8_t *)dwords) + cb->buffer_offset);

      OUT_PKT3(ring, CP_SET_CONSTANT, size + 1);
      OUT_RING(ring, base);
      for (i = 0; i < size; i++)
         OUT_RING(ring, *(dwords++));

      base += size;
      enabled_mask &= ~(1 << index);
   }

   /* emit shader immediates: */
   if (shader) {
      for (i = 0; i < shader->num_immediates; i++) {
         OUT_PKT3(ring, CP_SET_CONSTANT, 5);
         OUT_RING(ring, start_base + (4 * (shader->first_immediate + i)));
         OUT_RING(ring, shader->immediates[i].val[0]);
         OUT_RING(ring, shader->immediates[i].val[1]);
         OUT_RING(ring, shader->immediates[i].val[2]);
         OUT_RING(ring, shader->immediates[i].val[3]);
         base += 4;
      }
   }
}

typedef uint32_t texmask;

static texmask
emit_texture(struct fd_ringbuffer *ring, struct fd_context *ctx,
             struct fd_texture_stateobj *tex, unsigned samp_id, texmask emitted)
{
   unsigned const_idx = fd2_get_const_idx(ctx, tex, samp_id);
   static const struct fd2_sampler_stateobj dummy_sampler = {};
   static const struct fd2_pipe_sampler_view dummy_view = {};
   const struct fd2_sampler_stateobj *sampler;
   const struct fd2_pipe_sampler_view *view;
   struct fd_resource *rsc;

   if (emitted & (1 << const_idx))
      return 0;

   sampler = tex->samplers[samp_id]
                ? fd2_sampler_stateobj(tex->samplers[samp_id])
                : &dummy_sampler;
   view = tex->textures[samp_id] ? fd2_pipe_sampler_view(tex->textures[samp_id])
                                 : &dummy_view;

   rsc = view->base.texture ? fd_resource(view->base.texture) : NULL;

   /* Debug: log texture state */
   if (FD_DBG(MSGS) && rsc) {
      mesa_logi("  TEX[%u]: const=%u fmt=%u %ux%u gpu=0x%llx tex0=0x%08x",
                samp_id, const_idx, view->base.format,
                view->base.texture->width0, view->base.texture->height0,
                (unsigned long long)fd_bo_get_iova(rsc->bo), view->tex0);
   }

   OUT_PKT3(ring, CP_SET_CONSTANT, 7);
   OUT_RING(ring, 0x00010000 + (0x6 * const_idx));

   OUT_RING(ring, sampler->tex0 | view->tex0);
   if (rsc)
      OUT_RELOC(ring, rsc->bo, fd_resource_offset(rsc, 0, 0), view->tex1, 0);
   else
      OUT_RING(ring, 0);

   OUT_RING(ring, view->tex2);
   OUT_RING(ring, sampler->tex3 | view->tex3);
   OUT_RING(ring, sampler->tex4 | view->tex4);

   if (rsc && rsc->b.b.last_level)
      OUT_RELOC(ring, rsc->bo, fd_resource_offset(rsc, 1, 0), view->tex5, 0);
   else
      OUT_RING(ring, view->tex5);

   return (1 << const_idx);
}

static void
emit_textures(struct fd_ringbuffer *ring, struct fd_context *ctx)
{
   struct fd_texture_stateobj *fragtex = &ctx->tex[MESA_SHADER_FRAGMENT];
   struct fd_texture_stateobj *verttex = &ctx->tex[MESA_SHADER_VERTEX];
   texmask emitted = 0;
   unsigned i;

   for (i = 0; i < verttex->num_samplers; i++)
      if (verttex->samplers[i])
         emitted |= emit_texture(ring, ctx, verttex, i, emitted);

   for (i = 0; i < fragtex->num_samplers; i++)
      if (fragtex->samplers[i])
         emitted |= emit_texture(ring, ctx, fragtex, i, emitted);
}

void
fd2_emit_vertex_bufs(struct fd_ringbuffer *ring, uint32_t val,
                     struct fd2_vertex_buf *vbufs, uint32_t n)
{
   unsigned i;

   OUT_PKT3(ring, CP_SET_CONSTANT, 1 + (2 * n));
   OUT_RING(ring, (0x1 << 16) | (val & 0xffff));
   for (i = 0; i < n; i++) {
      struct fd_resource *rsc = fd_resource(vbufs[i].prsc);
      OUT_RELOC(ring, rsc->bo, vbufs[i].offset, 3, 0);
      OUT_RING(ring, vbufs[i].size);
   }
}

void
fd2_emit_state_binning(struct fd_context *ctx,
                       const enum fd_dirty_3d_state dirty)
{
   struct fd2_blend_stateobj *blend = fd2_blend_stateobj(ctx->blend);
   struct fd_ringbuffer *ring = ctx->batch->binning;

   /* subset of fd2_emit_state needed for hw binning on a20x */

   if (dirty & (FD_DIRTY_PROG | FD_DIRTY_VTXSTATE))
      fd2_program_emit(ctx, ring, &ctx->prog);

   if (dirty & (FD_DIRTY_PROG | FD_DIRTY_CONST)) {
      emit_constants(ring, VS_CONST_BASE * 4,
                     &ctx->constbuf[MESA_SHADER_VERTEX],
                     (dirty & FD_DIRTY_PROG) ? ctx->prog.vs : NULL);
   }

   if (dirty & FD_DIRTY_VIEWPORT) {
      struct pipe_viewport_state *vp = & ctx->viewport[0];

      OUT_PKT3(ring, CP_SET_CONSTANT, 9);
      OUT_RING(ring, 0x00000184);
      OUT_RING(ring, fui(vp->translate[0]));
      OUT_RING(ring, fui(vp->translate[1]));
      OUT_RING(ring, fui(vp->translate[2]));
      OUT_RING(ring, fui(0.0f));
      OUT_RING(ring, fui(vp->scale[0]));
      OUT_RING(ring, fui(vp->scale[1]));
      OUT_RING(ring, fui(vp->scale[2]));
      OUT_RING(ring, fui(0.0f));
   }

   /* not sure why this is needed */
   if (dirty & (FD_DIRTY_BLEND | FD_DIRTY_FRAMEBUFFER)) {
      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_RB_BLEND_CONTROL));
      OUT_RING(ring, blend->rb_blendcontrol);

      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_RB_COLOR_MASK));
      OUT_RING(ring, blend->rb_colormask);
   }

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_PA_SU_SC_MODE_CNTL));
   OUT_RING(ring, A2XX_PA_SU_SC_MODE_CNTL_FACE_KILL_ENABLE);
}

void
fd2_emit_state(struct fd_context *ctx, const enum fd_dirty_3d_state dirty)
{
   struct fd2_blend_stateobj *blend = fd2_blend_stateobj(ctx->blend);
   struct fd2_zsa_stateobj *zsa = fd2_zsa_stateobj(ctx->zsa);
   struct fd2_shader_stateobj *fs = ctx->prog.fs;
   struct fd_ringbuffer *ring = ctx->batch->draw;

   /* NOTE: we probably want to eventually refactor this so each state
    * object handles emitting it's own state..  although the mapping of
    * state to registers is not always orthogonal, sometimes a single
    * register contains bitfields coming from multiple state objects,
    * so not sure the best way to deal with that yet.
    */

   if (dirty & FD_DIRTY_SAMPLE_MASK) {
      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_PA_SC_AA_MASK));
      OUT_RING(ring, ctx->sample_mask);
   }

   if (dirty & (FD_DIRTY_ZSA | FD_DIRTY_STENCIL_REF | FD_DIRTY_PROG)) {
      struct pipe_stencil_ref *sr = &ctx->stencil_ref;
      uint32_t val = zsa->rb_depthcontrol;

      if (fs->has_kill)
         val &= ~A2XX_RB_DEPTHCONTROL_EARLY_Z_ENABLE;

      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_RB_DEPTHCONTROL));
      OUT_RING(ring, val);

      OUT_PKT3(ring, CP_SET_CONSTANT, 4);
      OUT_RING(ring, CP_REG(REG_A2XX_RB_STENCILREFMASK_BF));
      OUT_RING(ring, zsa->rb_stencilrefmask_bf |
                        A2XX_RB_STENCILREFMASK_STENCILREF(sr->ref_value[1]));
      OUT_RING(ring, zsa->rb_stencilrefmask |
                        A2XX_RB_STENCILREFMASK_STENCILREF(sr->ref_value[0]));
      OUT_RING(ring, zsa->rb_alpha_ref);
   }

   if (ctx->rasterizer && dirty & FD_DIRTY_RASTERIZER) {
      struct fd2_rasterizer_stateobj *rasterizer =
         fd2_rasterizer_stateobj(ctx->rasterizer);
      OUT_PKT3(ring, CP_SET_CONSTANT, 3);
      OUT_RING(ring, CP_REG(REG_A2XX_PA_CL_CLIP_CNTL));
      OUT_RING(ring, rasterizer->pa_cl_clip_cntl);
      OUT_RING(ring, rasterizer->pa_su_sc_mode_cntl |
                        A2XX_PA_SU_SC_MODE_CNTL_VTX_WINDOW_OFFSET_ENABLE);

      OUT_PKT3(ring, CP_SET_CONSTANT, 5);
      OUT_RING(ring, CP_REG(REG_A2XX_PA_SU_POINT_SIZE));
      OUT_RING(ring, rasterizer->pa_su_point_size);
      OUT_RING(ring, rasterizer->pa_su_point_minmax);
      OUT_RING(ring, rasterizer->pa_su_line_cntl);
      OUT_RING(ring, rasterizer->pa_sc_line_stipple);

      OUT_PKT3(ring, CP_SET_CONSTANT, 6);
      OUT_RING(ring, CP_REG(REG_A2XX_PA_SU_VTX_CNTL));
      OUT_RING(ring, rasterizer->pa_su_vtx_cntl);
      OUT_RING(ring, fui(1.0f)); /* PA_CL_GB_VERT_CLIP_ADJ */
      OUT_RING(ring, fui(1.0f)); /* PA_CL_GB_VERT_DISC_ADJ */
      OUT_RING(ring, fui(1.0f)); /* PA_CL_GB_HORZ_CLIP_ADJ */
      OUT_RING(ring, fui(1.0f)); /* PA_CL_GB_HORZ_DISC_ADJ */

      if (rasterizer->base.offset_tri) {
         /* TODO: why multiply scale by 2 ? without it deqp test fails
          * deqp/piglit tests aren't very precise
          */
         OUT_PKT3(ring, CP_SET_CONSTANT, 5);
         OUT_RING(ring, CP_REG(REG_A2XX_PA_SU_POLY_OFFSET_FRONT_SCALE));
         OUT_RING(ring,
                  fui(rasterizer->base.offset_scale * 2.0f)); /* FRONT_SCALE */
         OUT_RING(ring, fui(rasterizer->base.offset_units));  /* FRONT_OFFSET */
         OUT_RING(ring,
                  fui(rasterizer->base.offset_scale * 2.0f)); /* BACK_SCALE */
         OUT_RING(ring, fui(rasterizer->base.offset_units));  /* BACK_OFFSET */
      }
   }

   /* NOTE: scissor enabled bit is part of rasterizer state: */
   if (dirty & (FD_DIRTY_SCISSOR | FD_DIRTY_RASTERIZER)) {
      struct pipe_scissor_state *scissor = fd_context_get_scissor(ctx);

      OUT_PKT3(ring, CP_SET_CONSTANT, 3);
      OUT_RING(ring, CP_REG(REG_A2XX_PA_SC_WINDOW_SCISSOR_TL));
      OUT_RING(ring, xy2d(scissor->minx, /* PA_SC_WINDOW_SCISSOR_TL */
                          scissor->miny));
      OUT_RING(ring, xy2d(scissor->maxx, /* PA_SC_WINDOW_SCISSOR_BR */
                          scissor->maxy));

      ctx->batch->max_scissor.minx =
         MIN2(ctx->batch->max_scissor.minx, scissor->minx);
      ctx->batch->max_scissor.miny =
         MIN2(ctx->batch->max_scissor.miny, scissor->miny);
      ctx->batch->max_scissor.maxx =
         MAX2(ctx->batch->max_scissor.maxx, scissor->maxx);
      ctx->batch->max_scissor.maxy =
         MAX2(ctx->batch->max_scissor.maxy, scissor->maxy);
   }

   if (dirty & FD_DIRTY_VIEWPORT) {
      struct pipe_viewport_state *vp = & ctx->viewport[0];

      OUT_PKT3(ring, CP_SET_CONSTANT, 7);
      OUT_RING(ring, CP_REG(REG_A2XX_PA_CL_VPORT_XSCALE));
      OUT_RING(ring, fui(vp->scale[0]));     /* PA_CL_VPORT_XSCALE */
      OUT_RING(ring, fui(vp->translate[0])); /* PA_CL_VPORT_XOFFSET */
      OUT_RING(ring, fui(vp->scale[1]));     /* PA_CL_VPORT_YSCALE */
      OUT_RING(ring, fui(vp->translate[1])); /* PA_CL_VPORT_YOFFSET */
      OUT_RING(ring, fui(vp->scale[2]));     /* PA_CL_VPORT_ZSCALE */
      OUT_RING(ring, fui(vp->translate[2])); /* PA_CL_VPORT_ZOFFSET */

      /* set viewport in C65/C66, for a20x hw binning and fragcoord.z */
      OUT_PKT3(ring, CP_SET_CONSTANT, 9);
      OUT_RING(ring, 0x00000184);

      OUT_RING(ring, fui(vp->translate[0]));
      OUT_RING(ring, fui(vp->translate[1]));
      OUT_RING(ring, fui(vp->translate[2]));
      OUT_RING(ring, fui(0.0f));

      OUT_RING(ring, fui(vp->scale[0]));
      OUT_RING(ring, fui(vp->scale[1]));
      OUT_RING(ring, fui(vp->scale[2]));
      OUT_RING(ring, fui(0.0f));
   }

   if (dirty & (FD_DIRTY_PROG | FD_DIRTY_VTXSTATE | FD_DIRTY_TEXSTATE))
      fd2_program_emit(ctx, ring, &ctx->prog);

   if (dirty & (FD_DIRTY_PROG | FD_DIRTY_CONST)) {
      emit_constants(ring, VS_CONST_BASE * 4,
                     &ctx->constbuf[MESA_SHADER_VERTEX],
                     (dirty & FD_DIRTY_PROG) ? ctx->prog.vs : NULL);
      emit_constants(ring, PS_CONST_BASE * 4,
                     &ctx->constbuf[MESA_SHADER_FRAGMENT],
                     (dirty & FD_DIRTY_PROG) ? ctx->prog.fs : NULL);

      /* A22X workaround: WFI after constant emission to ensure shader
       * constants are fully written before draw. Without this, shaders
       * may read stale constant values causing incorrect colors.
       */
      if (is_a22x(ctx->screen))
         OUT_WFI(ring);
   }

   if (dirty & (FD_DIRTY_BLEND | FD_DIRTY_ZSA)) {
      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_RB_COLORCONTROL));
      OUT_RING(ring, zsa->rb_colorcontrol | blend->rb_colorcontrol);
   }

   if (dirty & (FD_DIRTY_BLEND | FD_DIRTY_FRAMEBUFFER)) {
      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_RB_BLEND_CONTROL));
      OUT_RING(ring, blend->rb_blendcontrol);

      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_RB_COLOR_MASK));
      OUT_RING(ring, blend->rb_colormask);
   }

   if (dirty & FD_DIRTY_BLEND_COLOR) {
      OUT_PKT3(ring, CP_SET_CONSTANT, 5);
      OUT_RING(ring, CP_REG(REG_A2XX_RB_BLEND_RED));
      OUT_RING(ring, float_to_ubyte(ctx->blend_color.color[0]));
      OUT_RING(ring, float_to_ubyte(ctx->blend_color.color[1]));
      OUT_RING(ring, float_to_ubyte(ctx->blend_color.color[2]));
      OUT_RING(ring, float_to_ubyte(ctx->blend_color.color[3]));
   }

   if (dirty & (FD_DIRTY_TEX | FD_DIRTY_PROG))
      emit_textures(ring, ctx);
}

/* emit per-context initialization:
 */
void
fd2_emit_restore(struct fd_context *ctx, struct fd_ringbuffer *ring)
{
   /* Debug: Log when GPU state restore is called. This helps track whether
    * GPU initialization is happening consistently across batches.
    */
   if (FD_DBG(MSGS)) {
      static uint32_t restore_count = 0;
      mesa_logi("A2XX: fd2_emit_restore called (count=%u, is_a20x=%d)",
                ++restore_count, is_a20x(ctx->screen));
   }

   /*
    * Drain the pipeline before issuing the state-restore register writes.
    * Without an explicit WFI here, the writes can race the previous
    * client's in-flight retirement. This is what KGSL effectively does
    * via PM4_CONTEXT_UPDATE / PM4_LOAD_CONSTANT_CONTEXT (both of which
    * include implicit pipeline drains and which mainline freedreno
    * cannot use safely on a2xx because the hardware shadow-memory
    * mechanism causes hangs).
    *
    * Mainline mainstream a2xx already has a CP_WAIT_REG_EQ on
    * RBBM_STATUS later in this function, but that fires AFTER the
    * VGT_VERTEX_REUSE_BLOCK_CNTL / RBBM_PM_OVERRIDE / TP0_CHICKEN /
    * SQ_VS_CONST / SQ_PS_CONST / etc. writes have already gone through.
    * The previous client's work could be touching some of those very
    * same blocks while we write them.
    *
    * Adding an explicit drain here is cheap (one CP packet, no perf
    * cost when the pipeline is already empty) and aligned with the
    * "context-switch firewall" pattern Gemini's analysis recommends
    * for state-pollution issues that survive cmdstream-byte-identity.
    */
   OUT_PKT3(ring, CP_WAIT_FOR_IDLE, 1);
   OUT_RING(ring, 0x00000000);

   /*
    * Aggressive cache flush + invalidate at batch start (A22X only).
    *
    * gl-capture pixel-diff experiments isolated cross-process GPU-side
    * nondeterminism: 10 consecutive runs of the *same* deterministic
    * test program emit byte-identical PM4 cmdstreams (FD_RD_DUMP
    * confirmed - md5 of all rd files is identical) but produce
    * different pixel output. Mesa is doing its job; the hardware is
    * producing different output for the same input across runs.
    *
    * The remaining sources of run-to-run variance for byte-identical
    * cmdstreams are GPU-internal caches/SRAMs that retain data from a
    * previous submit (potentially across processes / DRM clients):
    *   - VPC vertex parameter cache (drained via VS_FETCH_DONE event)
    *   - Texture/L2 caches (TC_CNTL_STATUS L2_INVALIDATE - already
    *     present below)
    *   - SQ instruction & shader-constant caches (CACHE_FLUSH_AND_INV
    *     event flushes both)
    *   - General "wait for everything to settle" between submits
    *     (SC_WAIT_WC for write-coalesce drain)
    *
    * Issue the strongest A22X-available flush sequence here, before
    * any state-restore writes, so each batch starts from a clean GPU
    * state regardless of what previous submits or clients left
    * behind. Cheap once-per-batch.
    */
   if (!is_a20x(ctx->screen)) {
      OUT_PKT3(ring, CP_EVENT_WRITE, 1);
      OUT_RING(ring, CACHE_FLUSH_AND_INV_EVENT);

      OUT_PKT3(ring, CP_EVENT_WRITE, 1);
      OUT_RING(ring, VS_FETCH_DONE);

      OUT_PKT3(ring, CP_EVENT_WRITE, 1);
      OUT_RING(ring, SC_WAIT_WC);

      OUT_PKT3(ring, CP_WAIT_FOR_IDLE, 1);
      OUT_RING(ring, 0x00000000);
   }

   if (is_a20x(ctx->screen)) {
      OUT_PKT0(ring, REG_A2XX_RB_BC_CONTROL, 1);
      OUT_RING(ring, A2XX_RB_BC_CONTROL_ACCUM_TIMEOUT_SELECT(3) |
                        A2XX_RB_BC_CONTROL_DISABLE_LZ_NULL_ZCMD_DROP |
                        A2XX_RB_BC_CONTROL_ENABLE_CRC_UPDATE |
                        A2XX_RB_BC_CONTROL_ACCUM_DATA_FIFO_LIMIT(8) |
                        A2XX_RB_BC_CONTROL_MEM_EXPORT_TIMEOUT_SELECT(3));

      /* not sure why this is required */
      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_PA_SC_VIZ_QUERY));
      OUT_RING(ring, A2XX_PA_SC_VIZ_QUERY_VIZ_QUERY_ID(16));

      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_VGT_VERTEX_REUSE_BLOCK_CNTL));
      OUT_RING(ring, 0x00000002);

      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_VGT_OUT_DEALLOC_CNTL));
      OUT_RING(ring, 0x00000002);
   } else {
      /*
       * A22X (Adreno 220 / Leia): VGT_VERTEX_REUSE_BLOCK_CNTL.
       *
       * This was previously set to 0x00000000 ("disable vertex reuse")
       * with a comment "Try disabling vertex reuse entirely to debug
       * black faces issue". That was a temporary debug change that was
       * never reverted - and disabling vertex reuse on a22x is *exactly*
       * the kind of pessimization that hurts performance on heavy
       * geometry (every vertex re-shaded with no cache).
       *
       * Cross-reference of vendor proprietary drivers (Ghidra of
       * webOS libGLESv2.so + Samsung libGLESv2_adreno200.so) shows:
       *   - Samsung Q1 (Adreno 220) leia_perform_resolve writes 0x3b
       *     per draw - this is the value the proprietary userspace
       *     driver actually uses
       *   - KGSL kernel default (a2xx_drawctxt + adreno_drawctxt) is
       *     0x02 (low reuse depth)
       *   - Mesa a20x (the other branch above) writes 0x02 already
       *
       * Pick 0x02 (KGSL kernel default + Mesa a20x parity) rather than
       * 0x3b (Samsung userspace value) for now - smaller change from
       * the kernel-level baseline, less likely to introduce a
       * regression in some untested edge case. If 0x02 doesn't fix
       * the per-vertex color residuals, try 0x3b.
       */
      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_VGT_VERTEX_REUSE_BLOCK_CNTL));
      OUT_RING(ring, 0x00000002);

      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_VGT_OUT_DEALLOC_CNTL));
      OUT_RING(ring, 0x00000002);
   }

   /* enable perfcntrs */
   OUT_PKT0(ring, REG_A2XX_CP_PERFMON_CNTL, 1);
   OUT_RING(ring, COND(FD_DBG(PERFC), 1));

   /* note: perfcntrs don't work without the PM_OVERRIDE bit
    * A22X (Leia) requires RBBM_PM_OVERRIDE2=0x1a0 per KGSL driver, not 0xfff.
    * Using incorrect clock gating overrides causes timing issues that manifest
    * as intermittent faceted rendering (varyings not smoothly interpolated).
    */
   /* Force all GPU clocks/power domains on to test if clock gating
    * causes intermittent faceted rendering. KGSL uses 0xfffffffe/0xff
    * during init, then relaxes to 0/0x1a0 after. Try forcing full power.
    */
   OUT_PKT0(ring, REG_A2XX_RBBM_PM_OVERRIDE1, 2);
   OUT_RING(ring, 0xfffffffe);  /* Force all clocks on */
   OUT_RING(ring, 0xffffffff);  /* Force all power domains on */

   OUT_PKT0(ring, REG_A2XX_TP0_CHICKEN, 1);
   OUT_RING(ring, 0x00000002);

   OUT_PKT3(ring, CP_INVALIDATE_STATE, 1);
   OUT_RING(ring, 0x00007fff);

   /*
    * Invalidate the L2 texture cache (TC) at every batch start.
    *
    * Mesa's dirty-flag tracking + patch 0038 (mark all state dirty after
    * GMEM tile setup/resolve/sysmem_prep) ensures the cmdstream emits
    * the correct register values across batch boundaries, but the GPU
    * L2 texture cache is keyed by physical address and persists across
    * processes. Kernel-side gpummu_unmap invalidates the MMU TLB
    * (MH_MMU_INVALIDATE_TC = MMU translation cache) but NOT the L2
    * texture cache.
    *
    * When a GLES client (e.g. kmscube) inherits CMA-allocated BOs at
    * physical pages a previous client (e.g. luna-surfacemanager) just
    * vacated, the GPU TC may still hold the previous client's texture
    * data at those physical addresses. CPU writes via glTexImage2D do
    * not invalidate GPU TC, so the GPU samples stale data.
    *
    * Symptoms on Adreno 220 / HP TouchPad: kmscube cube faces fade in
    * and out across runs after LSM exits; LSM glyph text shows wrong
    * pixels; glmark2 texture test renders incorrectly on first
    * post-LSM cycle.
    *
    * Invalidating L2 at batch start (where fd2_emit_restore runs) is
    * the cheapest correct fix - one packet, no perf cost when there's
    * nothing to invalidate, and once-per-batch frequency aligns with
    * the natural state-restore rhythm.
    */
   OUT_PKT0(ring, REG_A2XX_TC_CNTL_STATUS, 1);
   OUT_RING(ring, A2XX_TC_CNTL_STATUS_L2_INVALIDATE);

   /* Wait for L2 invalidate to complete before any subsequent fetch. */
   OUT_WFI(ring);

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_SQ_VS_CONST));
   OUT_RING(ring, A2XX_SQ_VS_CONST_BASE(VS_CONST_BASE) |
                     A2XX_SQ_VS_CONST_SIZE(0x100));

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_SQ_PS_CONST));
   OUT_RING(ring,
            A2XX_SQ_PS_CONST_BASE(PS_CONST_BASE) | A2XX_SQ_PS_CONST_SIZE(0xe0));

   OUT_PKT3(ring, CP_SET_CONSTANT, 3);
   OUT_RING(ring, CP_REG(REG_A2XX_VGT_MAX_VTX_INDX));
   OUT_RING(ring, 0xffffffff); /* VGT_MAX_VTX_INDX */
   OUT_RING(ring, 0x00000000); /* VGT_MIN_VTX_INDX */

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_VGT_INDX_OFFSET));
   OUT_RING(ring, 0x00000000);

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_SQ_CONTEXT_MISC));
   OUT_RING(ring, A2XX_SQ_CONTEXT_MISC_SC_SAMPLE_CNTL(CENTERS_ONLY));

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_SQ_INTERPOLATOR_CNTL));
   OUT_RING(ring, 0xffffffff);

   /* Initialize PA_SU_SC_MODE_CNTL to a known state with perspective
    * correction ENABLED (PERSP_CORR_DIS bit 20 = 0). This register controls
    * polygon setup including culling, provoking vertex, and critically,
    * whether perspective-correct interpolation is used for varyings.
    *
    * If PERSP_CORR_DIS is set (by a previous context or GPU operation),
    * varyings will be interpolated linearly instead of perspective-correctly,
    * causing faceted/flat shading appearance even when SQ_INTERPOLATOR_CNTL
    * requests smooth interpolation.
    *
    * Set a safe default: no culling, last vertex provoking, perspective
    * correction enabled (bit 20 NOT set).
    */
   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_PA_SU_SC_MODE_CNTL));
   OUT_RING(ring, A2XX_PA_SU_SC_MODE_CNTL_VTX_WINDOW_OFFSET_ENABLE |
                  A2XX_PA_SU_SC_MODE_CNTL_PROVOKING_VTX_LAST |
                  A2XX_PA_SU_SC_MODE_CNTL_FRONT_PTYPE(PC_DRAW_TRIANGLES) |
                  A2XX_PA_SU_SC_MODE_CNTL_BACK_PTYPE(PC_DRAW_TRIANGLES));

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_PA_SC_AA_CONFIG));
   OUT_RING(ring, 0x00000000);

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_PA_SC_LINE_CNTL));
   OUT_RING(ring, 0x00000000);

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_PA_SC_WINDOW_OFFSET));
   OUT_RING(ring, 0x00000000);

   // XXX we change this dynamically for draw/clear.. vs gmem<->mem..
   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_RB_MODECONTROL));
   OUT_RING(ring, A2XX_RB_MODECONTROL_EDRAM_MODE(COLOR_DEPTH));

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_RB_SAMPLE_POS));
   OUT_RING(ring, 0x88888888);

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_RB_COLOR_DEST_MASK));
   OUT_RING(ring, 0xffffffff);

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_RB_COPY_DEST_INFO));
   OUT_RING(ring, A2XX_RB_COPY_DEST_INFO_FORMAT(COLORX_4_4_4_4) |
                     A2XX_RB_COPY_DEST_INFO_WRITE_RED |
                     A2XX_RB_COPY_DEST_INFO_WRITE_GREEN |
                     A2XX_RB_COPY_DEST_INFO_WRITE_BLUE |
                     A2XX_RB_COPY_DEST_INFO_WRITE_ALPHA);

   OUT_PKT3(ring, CP_SET_CONSTANT, 3);
   OUT_RING(ring, CP_REG(REG_A2XX_SQ_WRAPPING_0));
   OUT_RING(ring, 0x00000000); /* SQ_WRAPPING_0 */
   OUT_RING(ring, 0x00000000); /* SQ_WRAPPING_1 */

   OUT_PKT3(ring, CP_SET_DRAW_INIT_FLAGS, 1);
   OUT_RING(ring, 0x00000000);

   OUT_PKT3(ring, CP_WAIT_REG_EQ, 4);
   OUT_RING(ring, 0x000005d0);
   OUT_RING(ring, 0x00000000);
   OUT_RING(ring, 0x5f601000);
   OUT_RING(ring, 0x00000001);

   /* A22X: Initialize SQ_GPR_MANAGEMENT for shader execution.
    *
    * Register layout (0x0d00):
    *   REG_DYNAMIC (bit 0):     1 = HW dynamically allocates per shader
    *   REG_SIZE_PIX (bits 4-11): 0x40 = 64 GPRs PS hint
    *   REG_SIZE_VTX (bits 12-19): 0x40 = 64 GPRs VS hint
    *
    * Setting REG_DYNAMIC=1 lets the hardware adjust the VS/PS GPR
    * split based on the shader's actual needs (the proprietary KGSL-
    * userspace driver also relies on dynamic allocation, computing
    * the split per shader in libGLESv2.so leia_perform_resolve).
    * Without REG_DYNAMIC, the static 64/64 split under-allocates
    * vertex GPRs on shaders with heavy vertex computation (Phong
    * lighting, multi-light scenes, complex glyph shaders) and
    * computations spill or read wrong slots — symptom is
    * per-vertex color confusion (e.g. kmscube gears partially
    * red/blue per vertex).
    *
    * An earlier attempt (commit 3fcf3b9b0b0, reverted) tried to
    * dynamically calculate the split per-shader and update this
    * register from fd2_program_emit. That hung the GPU - likely
    * the recalculated value path was hardware-incompatible. This
    * patch is the conservative version: keep the same VS=64/PS=64
    * hint but flip REG_DYNAMIC=1 so the hardware can adjust on
    * its own. One-bit change, no new register writes, no extra
    * WFI - lowest possible risk surface.
    */
   OUT_PKT0(ring, REG_A2XX_SQ_GPR_MANAGEMENT, 1);
   OUT_RING(ring, 0x00040401);

   /* Debug: Log GPR management setup. This is critical for shader execution. */
   if (FD_DBG(MSGS)) {
      mesa_logi("A2XX: SQ_GPR_MANAGEMENT=0x00040401 (VS=64 PS=64 + REG_DYNAMIC)");
   }

   OUT_PKT0(ring, REG_A2XX_SQ_INST_STORE_MANAGMENT, 1);
   OUT_RING(ring, 0x00000180);

   /* Debug: Log instruction store management. */
   if (FD_DBG(MSGS)) {
      mesa_logi("A2XX: SQ_INST_STORE_MANAGMENT=0x00000180");
   }

   /* NOTE: SQ_PIX_IN_CNTL (0x0d0c) and SQ_RESOURCE_MANAGMENT (0x0d03) are
    * Leia-specific registers defined in KGSL but writing to them causes
    * GPU hang. DO NOT initialize these registers.
    */

   OUT_PKT3(ring, CP_INVALIDATE_STATE, 1);
   OUT_RING(ring, 0x00000300);

   OUT_PKT3(ring, CP_SET_SHADER_BASES, 1);
   OUT_RING(ring, 0x80000180);

   /* not sure what this form of CP_SET_CONSTANT is.. */
   OUT_PKT3(ring, CP_SET_CONSTANT, 13);
   OUT_RING(ring, 0x00000000);
   OUT_RING(ring, 0x00000000);
   OUT_RING(ring, 0x00000000);
   OUT_RING(ring, 0x00000000);
   OUT_RING(ring, 0x00000000);
   OUT_RING(ring, 0x469c4000);
   OUT_RING(ring, 0x3f800000);
   OUT_RING(ring, 0x3f000000);
   OUT_RING(ring, 0x00000000);
   OUT_RING(ring, 0x40000000);
   OUT_RING(ring, 0x3f400000);
   OUT_RING(ring, 0x3ec00000);
   OUT_RING(ring, 0x3e800000);

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_RB_COLOR_MASK));
   OUT_RING(ring,
            A2XX_RB_COLOR_MASK_WRITE_RED | A2XX_RB_COLOR_MASK_WRITE_GREEN |
               A2XX_RB_COLOR_MASK_WRITE_BLUE | A2XX_RB_COLOR_MASK_WRITE_ALPHA);

   OUT_PKT3(ring, CP_SET_CONSTANT, 5);
   OUT_RING(ring, CP_REG(REG_A2XX_RB_BLEND_RED));
   OUT_RING(ring, 0x00000000); /* RB_BLEND_RED */
   OUT_RING(ring, 0x00000000); /* RB_BLEND_GREEN */
   OUT_RING(ring, 0x00000000); /* RB_BLEND_BLUE */
   OUT_RING(ring, 0x000000ff); /* RB_BLEND_ALPHA */

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_PA_CL_VTE_CNTL));
   OUT_RING(ring, A2XX_PA_CL_VTE_CNTL_VTX_W0_FMT |
                     A2XX_PA_CL_VTE_CNTL_VPORT_X_SCALE_ENA |
                     A2XX_PA_CL_VTE_CNTL_VPORT_X_OFFSET_ENA |
                     A2XX_PA_CL_VTE_CNTL_VPORT_Y_SCALE_ENA |
                     A2XX_PA_CL_VTE_CNTL_VPORT_Y_OFFSET_ENA |
                     A2XX_PA_CL_VTE_CNTL_VPORT_Z_SCALE_ENA |
                     A2XX_PA_CL_VTE_CNTL_VPORT_Z_OFFSET_ENA);

   /* A22X: Initialize VSC (Visibility Stream Cache) registers.
    * KGSL saves/restores these during context switches for Leia (A220).
    * Without initialization, stale values may cause visibility corruption
    * leading to intermittent rendering issues like faceted shading.
    *
    * Registers (from leia_reg.h):
    *   A220_VSC_BIN_SIZE (0x0C01): Binning size
    *   VSC_PIPE[0-7] (0x0C06-0x0C1D): 8 pipes, 3 regs each (CONFIG, ADDR, LEN)
    *   A220_RB_LRZ_VSC_CONTROL (0x2209): LRZ/VSC control
    */
   if (is_a22x(ctx->screen)) {
      /* Initialize A220_RB_LRZ_VSC_CONTROL to 0 (disabled) */
      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_A220_RB_LRZ_VSC_CONTROL));
      OUT_RING(ring, 0x00000000);

      /* Initialize A220_GRAS_CONTROL to 0.
       * KGSL restores this register in context switch (range 0x2208-0x2210).
       * Without explicit initialization, compositor can change this register
       * and cause faceted rendering in other GL contexts.
       */
      OUT_PKT3(ring, CP_SET_CONSTANT, 2);
      OUT_RING(ring, CP_REG(REG_A2XX_A220_GRAS_CONTROL));
      OUT_RING(ring, 0x00000000);

      /* Initialize A220_VSC_BIN_SIZE to 0 */
      OUT_PKT0(ring, REG_A2XX_A220_VSC_BIN_SIZE, 1);
      OUT_RING(ring, 0x00000000);

      /* Initialize all 8 VSC_PIPE entries (each has CONFIG, DATA_ADDRESS, DATA_LENGTH)
       * VSC_PIPE array starts at 0x0c06, stride=3, length=8
       */
      for (int i = 0; i < 8; i++) {
         OUT_PKT0(ring, REG_A2XX_VSC_PIPE_CONFIG(i), 3);
         OUT_RING(ring, 0x00000000);  /* CONFIG */
         OUT_RING(ring, 0x00000000);  /* DATA_ADDRESS */
         OUT_RING(ring, 0x00000000);  /* DATA_LENGTH */
      }

      if (FD_DBG(MSGS)) {
         mesa_logi("A22X: VSC/GRAS registers initialized (VSC_BIN_SIZE=0, LRZ_VSC_CONTROL=0, GRAS_CONTROL=0, all pipes=0)");
      }
   }
}

void
fd2_emit_init_screen(struct pipe_screen *pscreen)
{
   struct fd_screen *screen = fd_screen(pscreen);
   screen->emit_ib = fd2_emit_ib;
}

void
fd2_emit_init(struct pipe_context *pctx)
{
}
