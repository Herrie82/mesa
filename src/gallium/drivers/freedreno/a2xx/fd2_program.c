/*
 * Copyright © 2012 Rob Clark <robclark@freedesktop.org>
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 *    Jonathan Marek <jonathan@marek.ca>
 */

#include "nir/tgsi_to_nir.h"
#include "pipe/p_state.h"
#include "tgsi/tgsi_dump.h"
#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_string.h"

#include "freedreno_program.h"

#include "ir2/instr-a2xx.h"
#include "fd2_program.h"
#include "fd2_texture.h"
#include "fd2_util.h"
#include "ir2.h"

static void
fd2_shader_state_delete(struct pipe_context *pctx, void *hwcso)
{
   struct fd2_shader_stateobj *so = hwcso;
   if (!so)
      return;
   ralloc_free(so->nir);
   for (int i = 0; i < ARRAY_SIZE(so->variant); i++)
      free(so->variant[i].info.dwords);
   free(so);
}

static bool
emit(struct fd_ringbuffer *ring, mesa_shader_stage type,
     struct ir2_shader_info *info, struct util_dynarray *patches)
{
   unsigned i;

   if (!info->sizedwords) {
      mesa_loge("fd2: shader has no instructions");
      return false;
   }

   OUT_PKT3(ring, CP_IM_LOAD_IMMEDIATE, 2 + info->sizedwords);
   OUT_RING(ring, type == MESA_SHADER_FRAGMENT);
   OUT_RING(ring, info->sizedwords);

   if (patches) {
      util_dynarray_append(patches, &ring->cur[info->mem_export_ptr]);
   }

   for (i = 0; i < info->sizedwords; i++)
      OUT_RING(ring, info->dwords[i]);
   return true;
}

static int
ir2_glsl_type_size(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

static void *
fd2_shader_state_create(struct pipe_context *pctx,
                        const struct pipe_shader_state *cso)
{
   struct fd2_shader_stateobj *so = CALLOC_STRUCT(fd2_shader_stateobj);
   if (!so)
      return NULL;

   so->nir = (cso->type == PIPE_SHADER_IR_NIR)
                ? cso->ir.nir
                : tgsi_to_nir(cso->tokens, pctx->screen, false);
   so->type = so->nir->info.stage;
   so->is_a20x = is_a20x(fd_context(pctx)->screen);

   NIR_PASS(_, so->nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
              ir2_glsl_type_size, 0);

   if (ir2_optimize_nir(so->nir, true))
      goto fail;

   so->first_immediate = so->nir->num_uniforms;

   if (!ir2_compile(so, 0, NULL))
      goto fail;

   /* Free FS NIR now.  VS NIR will need to stick around for the draw variant
    * later.
    */
   if (so->nir->info.stage == MESA_SHADER_FRAGMENT) {
      ralloc_free(so->nir);
      so->nir = NULL;
   }

   return so;

fail:
   fd2_shader_state_delete(pctx, so);
   return NULL;
}

static void
patch_vtx_fetch(struct fd_context *ctx, struct pipe_vertex_element *elem,
                instr_fetch_vtx_t *instr, uint16_t dst_swiz) assert_dt
{
   struct surface_format fmt = fd2_pipe2surface(elem->src_format);

   instr->dst_swiz = fd2_vtx_swiz(elem->src_format, dst_swiz);
   instr->format_comp_all = fmt.sign == SQ_TEX_SIGN_SIGNED;
   instr->num_format_all = fmt.num_format;
   instr->format = fmt.format;
   instr->exp_adjust_all = fmt.exp_adjust;
   instr->stride = elem->src_stride;
   instr->offset = elem->src_offset;
}

static void
patch_fetches(struct fd_context *ctx, struct ir2_shader_info *info,
              struct fd_vertex_stateobj *vtx,
              struct fd_texture_stateobj *tex) assert_dt
{
   for (int i = 0; i < info->num_fetch_instrs; i++) {
      struct ir2_fetch_info *fi = &info->fetch_info[i];

      instr_fetch_t *instr = (instr_fetch_t *)&info->dwords[fi->offset];
      if (instr->opc == VTX_FETCH) {
         unsigned idx =
            (instr->vtx.const_index - 20) * 3 + instr->vtx.const_index_sel;
         patch_vtx_fetch(ctx, &vtx->pipe[idx], &instr->vtx, fi->vtx.dst_swiz);
         continue;
      }

      assert(instr->opc == TEX_FETCH);
      instr->tex.const_idx = fd2_get_const_idx(ctx, tex, fi->tex.samp_id);
      instr->tex.src_swiz = fi->tex.src_swiz;
   }
}

void
fd2_program_emit(struct fd_context *ctx, struct fd_ringbuffer *ring,
                 struct fd_program_stateobj *prog)
{
   struct fd2_shader_stateobj *fp = NULL, *vp;
   struct ir2_shader_info *fpi, *vpi;
   struct ir2_frag_linkage *f;
   uint8_t vs_gprs, fs_gprs = 0, vs_export = 0;
   enum a2xx_sq_ps_vtx_mode mode = POSITION_1_VECTOR;
   bool binning = (ctx->batch && ring == ctx->batch->binning);
   unsigned variant = 0;

   vp = prog->vs;

   /* find variant matching the linked fragment shader */
   if (!binning) {
      fp = prog->fs;
      for (variant = 1; variant < ARRAY_SIZE(vp->variant); variant++) {
         /* if checked all variants, compile a new variant */
         if (!vp->variant[variant].info.sizedwords) {
            ir2_compile(vp, variant, fp);
            break;
         }

         /* check if fragment shader linkage matches */
         if (!memcmp(&vp->variant[variant].f, &fp->variant[0].f,
                     sizeof(struct ir2_frag_linkage)))
            break;
      }
      assert(variant < ARRAY_SIZE(vp->variant));
   }

   vpi = &vp->variant[variant].info;
   fpi = &fp->variant[0].info;
   f = &fp->variant[0].f;

   /* clear/gmem2mem/mem2gmem need to be changed to remove this condition */
   if (prog != &ctx->solid_prog && prog != &ctx->blit_prog[0]) {
      patch_fetches(ctx, vpi, ctx->vtx.vtx, &ctx->tex[MESA_SHADER_VERTEX]);
      if (fp)
         patch_fetches(ctx, fpi, NULL, &ctx->tex[MESA_SHADER_FRAGMENT]);
   }

   emit(ring, MESA_SHADER_VERTEX, vpi,
        binning ? &ctx->batch->shader_patches : NULL);

   if (fp) {
      emit(ring, MESA_SHADER_FRAGMENT, fpi, NULL);
      fs_gprs = (fpi->max_reg < 0) ? 0x80 : fpi->max_reg;
      vs_export = MAX2(1, f->inputs_count) - 1;

      /* Debug: warn if fragment shader has no registers allocated */
      if (fpi->max_reg < 0) {
         mesa_logw("A2XX: FS max_reg=-1 (using 0x80), sizedwords=%u, inputs=%u",
                   fpi->sizedwords, f->inputs_count);
      }
   }

   vs_gprs = (vpi->max_reg < 0) ? 0x80 : vpi->max_reg;

   /* Debug: warn if vertex shader has no registers allocated */
   if (vpi->max_reg < 0) {
      mesa_logw("A2XX: VS max_reg=-1 (using 0x80), sizedwords=%u",
                vpi->sizedwords);
   }

   if (vp->writes_psize && !binning)
      mode = POSITION_2_VECTORS_SPRITE;

   /* A22X workaround: WFI before shader program setup to ensure GPU is idle.
    * This helps prevent race conditions where shader registers are read
    * before they're fully written. Analysis shows Mesa-level state is
    * identical between smooth and faceted runs, indicating hardware timing.
    */
   if (is_a22x(ctx->screen))
      OUT_WFI(ring);

   /* set register to use for param (fragcoord/pointcoord/frontfacing) */
   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_SQ_CONTEXT_MISC));
   OUT_RING(ring,
            A2XX_SQ_CONTEXT_MISC_SC_SAMPLE_CNTL(CENTERS_ONLY) |
               COND(fp, A2XX_SQ_CONTEXT_MISC_PARAM_GEN_POS(f->inputs_count)) |
               /* we need SCREEN_XY for both fragcoord and frontfacing */
               A2XX_SQ_CONTEXT_MISC_SC_OUTPUT_SCREEN_XY);

   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_SQ_PROGRAM_CNTL));
   OUT_RING(ring,
            A2XX_SQ_PROGRAM_CNTL_PS_EXPORT_MODE(2) |
               A2XX_SQ_PROGRAM_CNTL_VS_EXPORT_MODE(mode) |
               A2XX_SQ_PROGRAM_CNTL_VS_RESOURCE |
               A2XX_SQ_PROGRAM_CNTL_PS_RESOURCE |
               A2XX_SQ_PROGRAM_CNTL_VS_EXPORT_COUNT(vs_export) |
               A2XX_SQ_PROGRAM_CNTL_PS_REGS(fs_gprs) |
               A2XX_SQ_PROGRAM_CNTL_VS_REGS(vs_gprs) |
               COND(fp && fp->need_param, A2XX_SQ_PROGRAM_CNTL_PARAM_GEN) |
               COND(!fp, A2XX_SQ_PROGRAM_CNTL_GEN_INDEX_VTX));

   /* A22X workaround: WFI to ensure PROGRAM_CNTL is processed before
    * setting INTERPOLATOR_CNTL.
    */
   if (is_a22x(ctx->screen))
      OUT_WFI(ring);

   /* Set SQ_INTERPOLATOR_CNTL AFTER SQ_PROGRAM_CNTL.
    * This register controls whether each varying uses smooth or flat
    * interpolation. 0xffffffff = all varyings use smooth interpolation.
    * Hardware may require this ordering for proper interpolation setup.
    * Also set in fd2_emit_restore() for baseline init.
    */
   OUT_PKT3(ring, CP_SET_CONSTANT, 2);
   OUT_RING(ring, CP_REG(REG_A2XX_SQ_INTERPOLATOR_CNTL));
   OUT_RING(ring, 0xffffffff);

   /* Debug: always log shader program state for A22X interpolation debugging */
   if (is_a22x(ctx->screen)) {
      static unsigned emit_count = 0;
      emit_count++;

      uint32_t sq_program_cntl =
         A2XX_SQ_PROGRAM_CNTL_PS_EXPORT_MODE(2) |
         A2XX_SQ_PROGRAM_CNTL_VS_EXPORT_MODE(mode) |
         A2XX_SQ_PROGRAM_CNTL_VS_RESOURCE |
         A2XX_SQ_PROGRAM_CNTL_PS_RESOURCE |
         A2XX_SQ_PROGRAM_CNTL_VS_EXPORT_COUNT(vs_export) |
         A2XX_SQ_PROGRAM_CNTL_PS_REGS(fs_gprs) |
         A2XX_SQ_PROGRAM_CNTL_VS_REGS(vs_gprs) |
         COND(fp && fp->need_param, A2XX_SQ_PROGRAM_CNTL_PARAM_GEN) |
         COND(!fp, A2XX_SQ_PROGRAM_CNTL_GEN_INDEX_VTX);

      mesa_logi("PROG[%u]: SQ_PROGRAM_CNTL=0x%08x mode=%d vs_export=%u",
                emit_count, sq_program_cntl, mode, vs_export);
      mesa_logi("PROG[%u]: vs_gprs=%d fs_gprs=%d variant=%u binning=%d",
                emit_count, vpi->max_reg, fp ? fpi->max_reg : -1,
                variant, binning ? 1 : 0);

      /* Log fragment shader linkage details */
      if (fp) {
         mesa_logi("PROG[%u]: FS inputs_count=%u need_param=%d has_kill=%d",
                   emit_count, f->inputs_count, fp->need_param ? 1 : 0,
                   fp->has_kill ? 1 : 0);
         for (unsigned i = 0; i < f->inputs_count && i < 8; i++) {
            mesa_logi("PROG[%u]:   input[%u] slot=%u ncomp=%u",
                      emit_count, i, f->inputs[i].slot, f->inputs[i].ncomp);
         }
      }

      /* Log shader sizes to detect if different shaders are used */
      mesa_logi("PROG[%u]: VS size=%u dwords, FS size=%u dwords",
                emit_count, vpi->sizedwords, fp ? fpi->sizedwords : 0);
   }
}

void
fd2_prog_init(struct pipe_context *pctx)
{
   struct fd_context *ctx = fd_context(pctx);
   struct fd_program_stateobj *prog;
   struct fd2_shader_stateobj *so;
   struct ir2_shader_info *info;
   instr_fetch_vtx_t *instr;

   pctx->create_fs_state = fd2_shader_state_create;
   pctx->delete_fs_state = fd2_shader_state_delete;

   pctx->create_vs_state = fd2_shader_state_create;
   pctx->delete_vs_state = fd2_shader_state_delete;

   fd_prog_init(pctx);

   /* XXX maybe its possible to reuse patch_vtx_fetch somehow? */

   prog = &ctx->solid_prog;
   so = prog->vs;
   ir2_compile(prog->vs, 1, prog->fs);

#define IR2_FETCH_SWIZ_XY01 0xb08
#define IR2_FETCH_SWIZ_XYZ1 0xa88

   info = &so->variant[1].info;

   instr = (instr_fetch_vtx_t *)&info->dwords[info->fetch_info[0].offset];
   instr->const_index = 26;
   instr->const_index_sel = 0;
   instr->format = FMT_32_32_32_FLOAT;
   instr->format_comp_all = false;
   instr->stride = 12;
   instr->num_format_all = true;
   instr->dst_swiz = IR2_FETCH_SWIZ_XYZ1;

   prog = &ctx->blit_prog[0];
   so = prog->vs;
   ir2_compile(prog->vs, 1, prog->fs);

   info = &so->variant[1].info;

   instr = (instr_fetch_vtx_t *)&info->dwords[info->fetch_info[0].offset];
   instr->const_index = 26;
   instr->const_index_sel = 1;
   instr->format = FMT_32_32_FLOAT;
   instr->format_comp_all = false;
   instr->stride = 8;
   instr->num_format_all = false;
   instr->dst_swiz = IR2_FETCH_SWIZ_XY01;

   instr = (instr_fetch_vtx_t *)&info->dwords[info->fetch_info[1].offset];
   instr->const_index = 26;
   instr->const_index_sel = 0;
   instr->format = FMT_32_32_32_FLOAT;
   instr->format_comp_all = false;
   instr->stride = 12;
   instr->num_format_all = false;
   instr->dst_swiz = IR2_FETCH_SWIZ_XYZ1;
}
