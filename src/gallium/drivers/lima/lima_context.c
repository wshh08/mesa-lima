/*
 * Copyright (c) 2011-2013 Luc Verhaegen <libv@skynet.be>
 * Copyright (c) 2017 Lima Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

#include "util/u_memory.h"
#include "util/u_upload_mgr.h"
#include "util/u_math.h"
#include "util/u_debug.h"
#include "util/u_transfer.h"
#include "util/ralloc.h"
#include "util/u_inlines.h"
#include "util/u_suballoc.h"

#include "lima_screen.h"
#include "lima_context.h"
#include "lima_resource.h"
#include "lima_bo.h"
#include "lima_submit.h"

#include <lima_drm.h>
#include <xf86drm.h>

uint32_t
lima_ctx_buff_va(struct lima_context *ctx, enum lima_ctx_buff buff)
{
   struct lima_ctx_buff_state *cbs = ctx->buffer_state + buff;
   struct lima_resource *res = lima_resource(cbs->res);
   lima_bo_update(res->bo, false, true);
   return res->bo->va + cbs->offset;
}

void *
lima_ctx_buff_map(struct lima_context *ctx, enum lima_ctx_buff buff)
{
   struct lima_ctx_buff_state *cbs = ctx->buffer_state + buff;
   struct lima_resource *res = lima_resource(cbs->res);
   lima_bo_update(res->bo, true, false);
   return res->bo->map + cbs->offset;
}

void *
lima_ctx_buff_alloc(struct lima_context *ctx, enum lima_ctx_buff buff,
                    unsigned size, unsigned submit, bool uploader)
{
   struct lima_ctx_buff_state *cbs = ctx->buffer_state + buff;
   void *ret = NULL;

   cbs->size = align(size, 0x40);

   if (uploader)
      u_upload_alloc(ctx->uploader, 0, cbs->size, 0x40, &cbs->offset,
                     &cbs->res, &ret);
   else
      u_suballocator_alloc(ctx->suballocator, cbs->size, 0x10,
                           &cbs->offset, &cbs->res);

   struct lima_resource *res = lima_resource(cbs->res);
   if (submit & LIMA_CTX_BUFF_SUBMIT_GP)
      lima_submit_add_bo(ctx->gp_submit, res->bo, LIMA_SUBMIT_BO_READ);
   if (submit & LIMA_CTX_BUFF_SUBMIT_PP)
      lima_submit_add_bo(ctx->pp_submit, res->bo, LIMA_SUBMIT_BO_READ);

   return ret;
}

static int
lima_context_create_drm_ctx(struct lima_screen *screen)
{
   struct drm_lima_ctx req = {
      .op = LIMA_CTX_OP_CREATE,
   };

   int ret = drmIoctl(screen->fd, DRM_IOCTL_LIMA_CTX, &req);
   if (ret)
      return errno;

   return req.id;
}

static void
lima_context_free_drm_ctx(struct lima_screen *screen, int id)
{
   struct drm_lima_ctx req = {
      .op = LIMA_CTX_OP_FREE,
      .id = id,
   };

   drmIoctl(screen->fd, DRM_IOCTL_LIMA_CTX, &req);
}

static void
lima_context_destroy(struct pipe_context *pctx)
{
   struct lima_context *ctx = lima_context(pctx);
   struct lima_screen *screen = lima_screen(pctx->screen);

   for (int i = 0; i < lima_ctx_buff_num; i++)
      pipe_resource_reference(&ctx->buffer_state[i].res, NULL);

   lima_state_fini(ctx);

   slab_destroy_child(&ctx->transfer_pool);

   if (ctx->suballocator)
      u_suballocator_destroy(ctx->suballocator);

   if (ctx->uploader)
      u_upload_destroy(ctx->uploader);

   if (ctx->share_buffer)
      lima_bo_free(ctx->share_buffer);

   if (ctx->gp_buffer)
      lima_bo_free(ctx->gp_buffer);

   if (ctx->pp_buffer)
      lima_bo_free(ctx->pp_buffer);

   lima_context_free_drm_ctx(screen, ctx->id);

   ralloc_free(ctx);
}

struct pipe_context *
lima_context_create(struct pipe_screen *pscreen, void *priv, unsigned flags)
{
   struct lima_screen *screen = lima_screen(pscreen);
   struct lima_context *ctx;

   ctx = rzalloc(screen, struct lima_context);
   if (!ctx)
      return NULL;

   ctx->id = lima_context_create_drm_ctx(screen);
   if (ctx->id < 0) {
      ralloc_free(ctx);
      return NULL;
   }

   ctx->base.screen = pscreen;
   ctx->base.destroy = lima_context_destroy;

   lima_resource_context_init(ctx);
   lima_state_init(ctx);
   lima_draw_init(ctx);
   lima_program_init(ctx);
   lima_query_init(ctx);

   slab_create_child(&ctx->transfer_pool, &screen->transfer_pool);

   ctx->uploader = u_upload_create_default(&ctx->base);
   if (!ctx->uploader)
      goto err_out;
   ctx->base.stream_uploader = ctx->uploader;
   ctx->base.const_uploader = ctx->uploader;
   ctx->base.texture_subdata = u_default_texture_subdata;

   /* for varying output which need not mmap */
   ctx->suballocator =
      u_suballocator_create(&ctx->base, 1024 * 1024, 0,
                            PIPE_USAGE_STREAM, 0, false);
   if (!ctx->suballocator)
      goto err_out;

   util_dynarray_init(&ctx->vs_cmd_array, ctx);
   util_dynarray_init(&ctx->plbu_cmd_array, ctx);

   ctx->share_buffer = lima_bo_create(screen, sh_buffer_size, 0, false, true);
   if (!ctx->share_buffer)
      goto err_out;

   ctx->gp_buffer = lima_bo_create(screen, gp_buffer_size, 0, true, true);
   if (!ctx->gp_buffer)
      goto err_out;

   /* plb address stream for plbu is static for any framebuffer */
   int max_plb = 512, block_size = 0x200;
   uint32_t *plbu_stream = ctx->gp_buffer->map + gp_plbu_plb_offset;
   for (int i = 0; i < max_plb; i++)
      plbu_stream[i] = ctx->share_buffer->va + sh_plb_offset + block_size * i;

   ctx->gp_submit = lima_submit_create(ctx, LIMA_PIPE_GP);
   if (!ctx->gp_submit)
      goto err_out;

   ctx->pp_buffer = lima_bo_create(screen, pp_buffer_size, 0, true, true);
   if (!ctx->pp_buffer)
      goto err_out;

   /* fs program for clear buffer? */
   static uint32_t pp_program[] = {
      0x00020425, 0x0000000c, 0x01e007cf, 0xb0000000, /* 0x00000000 */
      0x000005f5, 0x00000000, 0x00000000, 0x00000000, /* 0x00000010 */
   };
   memcpy(ctx->pp_buffer->map + pp_clear_program_offset, pp_program, sizeof(pp_program));

   /* is pp frame render state static? */
   uint32_t *pp_frame_rsw = ctx->pp_buffer->map + pp_frame_rsw_offset;
   memset(pp_frame_rsw, 0, 0x40);
   pp_frame_rsw[8] = 0x0000f008;
   pp_frame_rsw[9] = ctx->pp_buffer->va + pp_clear_program_offset;
   pp_frame_rsw[13] = 0x00000100;

   ctx->pp_submit = lima_submit_create(ctx, LIMA_PIPE_PP);
   if (!ctx->pp_submit)
      goto err_out;

   return &ctx->base;

err_out:
   lima_context_destroy(&ctx->base);
   return NULL;
}
