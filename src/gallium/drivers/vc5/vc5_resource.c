/*
 * Copyright © 2014-2017 Broadcom
 * Copyright (C) 2012 Rob Clark <robclark@freedesktop.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "pipe/p_defines.h"
#include "util/u_blit.h"
#include "util/u_memory.h"
#include "util/u_format.h"
#include "util/u_inlines.h"
#include "util/u_surface.h"
#include "util/u_transfer_helper.h"
#include "util/u_upload_mgr.h"
#include "util/u_format_zs.h"

#include "drm_fourcc.h"
#include "vc5_screen.h"
#include "vc5_context.h"
#include "vc5_resource.h"
#include "vc5_tiling.h"
#include "broadcom/cle/v3d_packet_v33_pack.h"

static void
vc5_debug_resource_layout(struct vc5_resource *rsc, const char *caller)
{
        if (!(V3D_DEBUG & V3D_DEBUG_SURFACE))
                return;

        struct pipe_resource *prsc = &rsc->base;

        if (prsc->target == PIPE_BUFFER) {
                fprintf(stderr,
                        "rsc %s %p (format %s), %dx%d buffer @0x%08x-0x%08x\n",
                        caller, rsc,
                        util_format_short_name(prsc->format),
                        prsc->width0, prsc->height0,
                        rsc->bo->offset,
                        rsc->bo->offset + rsc->bo->size - 1);
                return;
        }

        static const char *const tiling_descriptions[] = {
                [VC5_TILING_RASTER] = "R",
                [VC5_TILING_LINEARTILE] = "LT",
                [VC5_TILING_UBLINEAR_1_COLUMN] = "UB1",
                [VC5_TILING_UBLINEAR_2_COLUMN] = "UB2",
                [VC5_TILING_UIF_NO_XOR] = "UIF",
                [VC5_TILING_UIF_XOR] = "UIF^",
        };

        for (int i = 0; i <= prsc->last_level; i++) {
                struct vc5_resource_slice *slice = &rsc->slices[i];

                int level_width = slice->stride / rsc->cpp;
                int level_height = slice->padded_height;
                int level_depth =
                        u_minify(util_next_power_of_two(prsc->depth0), i);

                fprintf(stderr,
                        "rsc %s %p (format %s), %dx%d: "
                        "level %d (%s) %dx%dx%d -> %dx%dx%d, stride %d@0x%08x\n",
                        caller, rsc,
                        util_format_short_name(prsc->format),
                        prsc->width0, prsc->height0,
                        i, tiling_descriptions[slice->tiling],
                        u_minify(prsc->width0, i),
                        u_minify(prsc->height0, i),
                        u_minify(prsc->depth0, i),
                        level_width,
                        level_height,
                        level_depth,
                        slice->stride,
                        rsc->bo->offset + slice->offset);
        }
}

static bool
vc5_resource_bo_alloc(struct vc5_resource *rsc)
{
        struct pipe_resource *prsc = &rsc->base;
        struct pipe_screen *pscreen = prsc->screen;
        struct vc5_bo *bo;

        bo = vc5_bo_alloc(vc5_screen(pscreen), rsc->size, "resource");
        if (bo) {
                vc5_bo_unreference(&rsc->bo);
                rsc->bo = bo;
                vc5_debug_resource_layout(rsc, "alloc");
                return true;
        } else {
                return false;
        }
}

static void
vc5_resource_transfer_unmap(struct pipe_context *pctx,
                            struct pipe_transfer *ptrans)
{
        struct vc5_context *vc5 = vc5_context(pctx);
        struct vc5_transfer *trans = vc5_transfer(ptrans);

        if (trans->map) {
                struct vc5_resource *rsc = vc5_resource(ptrans->resource);
                struct vc5_resource_slice *slice = &rsc->slices[ptrans->level];

                if (ptrans->usage & PIPE_TRANSFER_WRITE) {
                        for (int z = 0; z < ptrans->box.depth; z++) {
                                void *dst = rsc->bo->map +
                                        vc5_layer_offset(&rsc->base,
                                                         ptrans->level,
                                                         ptrans->box.z + z);
                                vc5_store_tiled_image(dst,
                                                      slice->stride,
                                                      (trans->map +
                                                       ptrans->stride *
                                                       ptrans->box.height * z),
                                                      ptrans->stride,
                                                      slice->tiling, rsc->cpp,
                                                      slice->padded_height,
                                                      &ptrans->box);
                        }
                }
                free(trans->map);
        }

        pipe_resource_reference(&ptrans->resource, NULL);
        slab_free(&vc5->transfer_pool, ptrans);
}

static void *
vc5_resource_transfer_map(struct pipe_context *pctx,
                          struct pipe_resource *prsc,
                          unsigned level, unsigned usage,
                          const struct pipe_box *box,
                          struct pipe_transfer **pptrans)
{
        struct vc5_context *vc5 = vc5_context(pctx);
        struct vc5_resource *rsc = vc5_resource(prsc);
        struct vc5_transfer *trans;
        struct pipe_transfer *ptrans;
        enum pipe_format format = prsc->format;
        char *buf;

        /* MSAA maps should have been handled by u_transfer_helper. */
        assert(prsc->nr_samples <= 1);

        /* Upgrade DISCARD_RANGE to WHOLE_RESOURCE if the whole resource is
         * being mapped.
         */
        if ((usage & PIPE_TRANSFER_DISCARD_RANGE) &&
            !(usage & PIPE_TRANSFER_UNSYNCHRONIZED) &&
            !(prsc->flags & PIPE_RESOURCE_FLAG_MAP_COHERENT) &&
            prsc->last_level == 0 &&
            prsc->width0 == box->width &&
            prsc->height0 == box->height &&
            prsc->depth0 == box->depth &&
            prsc->array_size == 1 &&
            rsc->bo->private) {
                usage |= PIPE_TRANSFER_DISCARD_WHOLE_RESOURCE;
        }

        if (usage & PIPE_TRANSFER_DISCARD_WHOLE_RESOURCE) {
                if (vc5_resource_bo_alloc(rsc)) {
                        /* If it might be bound as one of our vertex buffers
                         * or UBOs, make sure we re-emit vertex buffer state
                         * or uniforms.
                         */
                        if (prsc->bind & PIPE_BIND_VERTEX_BUFFER)
                                vc5->dirty |= VC5_DIRTY_VTXBUF;
                        if (prsc->bind & PIPE_BIND_CONSTANT_BUFFER)
                                vc5->dirty |= VC5_DIRTY_CONSTBUF;
                } else {
                        /* If we failed to reallocate, flush users so that we
                         * don't violate any syncing requirements.
                         */
                        vc5_flush_jobs_reading_resource(vc5, prsc);
                }
        } else if (!(usage & PIPE_TRANSFER_UNSYNCHRONIZED)) {
                /* If we're writing and the buffer is being used by the CL, we
                 * have to flush the CL first.  If we're only reading, we need
                 * to flush if the CL has written our buffer.
                 */
                if (usage & PIPE_TRANSFER_WRITE)
                        vc5_flush_jobs_reading_resource(vc5, prsc);
                else
                        vc5_flush_jobs_writing_resource(vc5, prsc);
        }

        if (usage & PIPE_TRANSFER_WRITE) {
                rsc->writes++;
                rsc->initialized_buffers = ~0;
        }

        trans = slab_alloc(&vc5->transfer_pool);
        if (!trans)
                return NULL;

        /* XXX: Handle DONTBLOCK, DISCARD_RANGE, PERSISTENT, COHERENT. */

        /* slab_alloc_st() doesn't zero: */
        memset(trans, 0, sizeof(*trans));
        ptrans = &trans->base;

        pipe_resource_reference(&ptrans->resource, prsc);
        ptrans->level = level;
        ptrans->usage = usage;
        ptrans->box = *box;

        /* Note that the current kernel implementation is synchronous, so no
         * need to do syncing stuff here yet.
         */

        if (usage & PIPE_TRANSFER_UNSYNCHRONIZED)
                buf = vc5_bo_map_unsynchronized(rsc->bo);
        else
                buf = vc5_bo_map(rsc->bo);
        if (!buf) {
                fprintf(stderr, "Failed to map bo\n");
                goto fail;
        }

        *pptrans = ptrans;

        /* Our load/store routines work on entire compressed blocks. */
        ptrans->box.x /= util_format_get_blockwidth(format);
        ptrans->box.y /= util_format_get_blockheight(format);
        ptrans->box.width = DIV_ROUND_UP(ptrans->box.width,
                                         util_format_get_blockwidth(format));
        ptrans->box.height = DIV_ROUND_UP(ptrans->box.height,
                                          util_format_get_blockheight(format));

        struct vc5_resource_slice *slice = &rsc->slices[level];
        if (rsc->tiled) {
                /* No direct mappings of tiled, since we need to manually
                 * tile/untile.
                 */
                if (usage & PIPE_TRANSFER_MAP_DIRECTLY)
                        return NULL;

                ptrans->stride = ptrans->box.width * rsc->cpp;
                ptrans->layer_stride = ptrans->stride * ptrans->box.height;

                trans->map = malloc(ptrans->layer_stride * ptrans->box.depth);

                if (usage & PIPE_TRANSFER_READ) {
                        for (int z = 0; z < ptrans->box.depth; z++) {
                                void *src = rsc->bo->map +
                                        vc5_layer_offset(&rsc->base,
                                                         ptrans->level,
                                                         ptrans->box.z + z);
                                vc5_load_tiled_image((trans->map +
                                                      ptrans->stride *
                                                      ptrans->box.height * z),
                                                     ptrans->stride,
                                                     src,
                                                     slice->stride,
                                                     slice->tiling, rsc->cpp,
                                                     slice->padded_height,
                                             &ptrans->box);
                        }
                }
                return trans->map;
        } else {
                ptrans->stride = slice->stride;
                ptrans->layer_stride = ptrans->stride;

                return buf + slice->offset +
                        ptrans->box.y * ptrans->stride +
                        ptrans->box.x * rsc->cpp +
                        ptrans->box.z * rsc->cube_map_stride;
        }


fail:
        vc5_resource_transfer_unmap(pctx, ptrans);
        return NULL;
}

static void
vc5_resource_destroy(struct pipe_screen *pscreen,
                     struct pipe_resource *prsc)
{
        struct vc5_resource *rsc = vc5_resource(prsc);

        vc5_bo_unreference(&rsc->bo);
        free(rsc);
}

static boolean
vc5_resource_get_handle(struct pipe_screen *pscreen,
                        struct pipe_context *pctx,
                        struct pipe_resource *prsc,
                        struct winsys_handle *whandle,
                        unsigned usage)
{
        struct vc5_resource *rsc = vc5_resource(prsc);
        struct vc5_bo *bo = rsc->bo;

        whandle->stride = rsc->slices[0].stride;

        /* If we're passing some reference to our BO out to some other part of
         * the system, then we can't do any optimizations about only us being
         * the ones seeing it (like BO caching).
         */
        bo->private = false;

        switch (whandle->type) {
        case DRM_API_HANDLE_TYPE_SHARED:
                return vc5_bo_flink(bo, &whandle->handle);
        case DRM_API_HANDLE_TYPE_KMS:
                whandle->handle = bo->handle;
                return TRUE;
        case DRM_API_HANDLE_TYPE_FD:
                whandle->handle = vc5_bo_get_dmabuf(bo);
                return whandle->handle != -1;
        }

        return FALSE;
}

#define PAGE_UB_ROWS (VC5_UIFCFG_PAGE_SIZE / VC5_UIFBLOCK_ROW_SIZE)
#define PAGE_UB_ROWS_TIMES_1_5 ((PAGE_UB_ROWS * 3) >> 1)
#define PAGE_CACHE_UB_ROWS (VC5_PAGE_CACHE_SIZE / VC5_UIFBLOCK_ROW_SIZE)
#define PAGE_CACHE_MINUS_1_5_UB_ROWS (PAGE_CACHE_UB_ROWS - PAGE_UB_ROWS_TIMES_1_5)

/**
 * Computes the HW's UIFblock padding for a given height/cpp.
 *
 * The goal of the padding is to keep pages of the same color (bank number) at
 * least half a page away from each other vertically when crossing between
 * between columns of UIF blocks.
 */
static uint32_t
vc5_get_ub_pad(struct vc5_resource *rsc, uint32_t height)
{
        uint32_t utile_h = vc5_utile_height(rsc->cpp);
        uint32_t uif_block_h = utile_h * 2;
        uint32_t height_ub = height / uif_block_h;

        uint32_t height_offset_in_pc = height_ub % PAGE_CACHE_UB_ROWS;

        /* For the perfectly-aligned-for-UIF-XOR case, don't add any pad. */
        if (height_offset_in_pc == 0)
                return 0;

        /* Try padding up to where we're offset by at least half a page. */
        if (height_offset_in_pc < PAGE_UB_ROWS_TIMES_1_5) {
                /* If we fit entirely in the page cache, don't pad. */
                if (height_ub < PAGE_CACHE_UB_ROWS)
                        return 0;
                else
                        return PAGE_UB_ROWS_TIMES_1_5 - height_offset_in_pc;
        }

        /* If we're close to being aligned to page cache size, then round up
         * and rely on XOR.
         */
        if (height_offset_in_pc > PAGE_CACHE_MINUS_1_5_UB_ROWS)
                return PAGE_CACHE_UB_ROWS - height_offset_in_pc;

        /* Otherwise, we're far enough away (top and bottom) to not need any
         * padding.
         */
        return 0;
}

static void
vc5_setup_slices(struct vc5_resource *rsc)
{
        struct pipe_resource *prsc = &rsc->base;
        uint32_t width = prsc->width0;
        uint32_t height = prsc->height0;
        uint32_t depth = prsc->depth0;
        /* Note that power-of-two padding is based on level 1.  These are not
         * equivalent to just util_next_power_of_two(dimension), because at a
         * level 0 dimension of 9, the level 1 power-of-two padded value is 4,
         * not 8.
         */
        uint32_t pot_width = 2 * util_next_power_of_two(u_minify(width, 1));
        uint32_t pot_height = 2 * util_next_power_of_two(u_minify(height, 1));
        uint32_t pot_depth = 2 * util_next_power_of_two(u_minify(depth, 1));
        uint32_t offset = 0;
        uint32_t utile_w = vc5_utile_width(rsc->cpp);
        uint32_t utile_h = vc5_utile_height(rsc->cpp);
        uint32_t uif_block_w = utile_w * 2;
        uint32_t uif_block_h = utile_h * 2;
        uint32_t block_width = util_format_get_blockwidth(prsc->format);
        uint32_t block_height = util_format_get_blockheight(prsc->format);
        bool msaa = prsc->nr_samples > 1;
        /* MSAA textures/renderbuffers are always laid out as single-level
         * UIF.
         */
        bool uif_top = msaa;

        for (int i = prsc->last_level; i >= 0; i--) {
                struct vc5_resource_slice *slice = &rsc->slices[i];

                uint32_t level_width, level_height, level_depth;
                if (i < 2) {
                        level_width = u_minify(width, i);
                        level_height = u_minify(height, i);
                } else {
                        level_width = u_minify(pot_width, i);
                        level_height = u_minify(pot_height, i);
                }
                if (i < 1)
                        level_depth = u_minify(depth, i);
                else
                        level_depth = u_minify(pot_depth, i);

                if (msaa) {
                        level_width *= 2;
                        level_height *= 2;
                }

                level_width = DIV_ROUND_UP(level_width, block_width);
                level_height = DIV_ROUND_UP(level_height, block_height);

                if (!rsc->tiled) {
                        slice->tiling = VC5_TILING_RASTER;
                        if (prsc->target == PIPE_TEXTURE_1D)
                                level_width = align(level_width, 64 / rsc->cpp);
                } else {
                        if ((i != 0 || !uif_top) &&
                            (level_width <= utile_w ||
                             level_height <= utile_h)) {
                                slice->tiling = VC5_TILING_LINEARTILE;
                                level_width = align(level_width, utile_w);
                                level_height = align(level_height, utile_h);
                        } else if ((i != 0 || !uif_top) &&
                                   level_width <= uif_block_w) {
                                slice->tiling = VC5_TILING_UBLINEAR_1_COLUMN;
                                level_width = align(level_width, uif_block_w);
                                level_height = align(level_height, uif_block_h);
                        } else if ((i != 0 || !uif_top) &&
                                   level_width <= 2 * uif_block_w) {
                                slice->tiling = VC5_TILING_UBLINEAR_2_COLUMN;
                                level_width = align(level_width, 2 * uif_block_w);
                                level_height = align(level_height, uif_block_h);
                        } else {
                                /* We align the width to a 4-block column of
                                 * UIF blocks, but we only align height to UIF
                                 * blocks.
                                 */
                                level_width = align(level_width,
                                                    4 * uif_block_w);
                                level_height = align(level_height,
                                                     uif_block_h);

                                slice->ub_pad = vc5_get_ub_pad(rsc,
                                                               level_height);
                                level_height += slice->ub_pad * uif_block_h;

                                /* If the padding set us to to be aligned to
                                 * the page cache size, then the HW will use
                                 * the XOR bit on odd columns to get us
                                 * perfectly misaligned
                                 */
                                if ((level_height / uif_block_h) %
                                    (VC5_PAGE_CACHE_SIZE /
                                     VC5_UIFBLOCK_ROW_SIZE) == 0) {
                                        slice->tiling = VC5_TILING_UIF_XOR;
                                } else {
                                        slice->tiling = VC5_TILING_UIF_NO_XOR;
                                }
                        }
                }

                slice->offset = offset;
                slice->stride = level_width * rsc->cpp;
                slice->padded_height = level_height;
                slice->size = level_height * slice->stride;

                uint32_t slice_total_size = slice->size * level_depth;

                /* The HW aligns level 1's base to a page if any of level 1 or
                 * below could be UIF XOR.  The lower levels then inherit the
                 * alignment for as long as necesary, thanks to being power of
                 * two aligned.
                 */
                if (i == 1 &&
                    level_width > 4 * uif_block_w &&
                    level_height > PAGE_CACHE_MINUS_1_5_UB_ROWS * uif_block_h) {
                        slice_total_size = align(slice_total_size,
                                                 VC5_UIFCFG_PAGE_SIZE);
                }

                offset += slice_total_size;

        }
        rsc->size = offset;

        /* UIF/UBLINEAR levels need to be aligned to UIF-blocks, and LT only
         * needs to be aligned to utile boundaries.  Since tiles are laid out
         * from small to big in memory, we need to align the later UIF slices
         * to UIF blocks, if they were preceded by non-UIF-block-aligned LT
         * slices.
         *
         * We additionally align to 4k, which improves UIF XOR performance.
         */
        uint32_t page_align_offset = (align(rsc->slices[0].offset, 4096) -
                                      rsc->slices[0].offset);
        if (page_align_offset) {
                rsc->size += page_align_offset;
                for (int i = 0; i <= prsc->last_level; i++)
                        rsc->slices[i].offset += page_align_offset;
        }

        /* Arrays and cube textures have a stride which is the distance from
         * one full mipmap tree to the next (64b aligned).  For 3D textures,
         * we need to program the stride between slices of miplevel 0.
         */
        if (prsc->target != PIPE_TEXTURE_3D) {
                rsc->cube_map_stride = align(rsc->slices[0].offset +
                                             rsc->slices[0].size, 64);
                rsc->size += rsc->cube_map_stride * (prsc->array_size - 1);
        } else {
                rsc->cube_map_stride = rsc->slices[0].size;
        }
}

uint32_t
vc5_layer_offset(struct pipe_resource *prsc, uint32_t level, uint32_t layer)
{
        struct vc5_resource *rsc = vc5_resource(prsc);
        struct vc5_resource_slice *slice = &rsc->slices[level];

        if (prsc->target == PIPE_TEXTURE_3D)
                return slice->offset + layer * slice->size;
        else
                return slice->offset + layer * rsc->cube_map_stride;
}

static struct vc5_resource *
vc5_resource_setup(struct pipe_screen *pscreen,
                   const struct pipe_resource *tmpl)
{
        struct vc5_screen *screen = vc5_screen(pscreen);
        struct vc5_resource *rsc = CALLOC_STRUCT(vc5_resource);
        if (!rsc)
                return NULL;
        struct pipe_resource *prsc = &rsc->base;

        *prsc = *tmpl;

        pipe_reference_init(&prsc->reference, 1);
        prsc->screen = pscreen;

        if (prsc->nr_samples <= 1 ||
            util_format_is_depth_or_stencil(prsc->format)) {
                rsc->cpp = util_format_get_blocksize(prsc->format) *
                        MAX2(prsc->nr_samples, 1);
        } else {
                assert(vc5_rt_format_supported(&screen->devinfo, prsc->format));
                uint32_t output_image_format =
                        vc5_get_rt_format(&screen->devinfo, prsc->format);
                uint32_t internal_type;
                uint32_t internal_bpp;
                vc5_get_internal_type_bpp_for_output_format(&screen->devinfo,
                                                            output_image_format,
                                                            &internal_type,
                                                            &internal_bpp);
                switch (internal_bpp) {
                case V3D_INTERNAL_BPP_32:
                        rsc->cpp = 4;
                        break;
                case V3D_INTERNAL_BPP_64:
                        rsc->cpp = 8;
                        break;
                case V3D_INTERNAL_BPP_128:
                        rsc->cpp = 16;
                        break;
                }
        }

        assert(rsc->cpp);

        return rsc;
}

static bool
find_modifier(uint64_t needle, const uint64_t *haystack, int count)
{
        int i;

        for (i = 0; i < count; i++) {
                if (haystack[i] == needle)
                        return true;
        }

        return false;
}

static struct pipe_resource *
vc5_resource_create_with_modifiers(struct pipe_screen *pscreen,
                                   const struct pipe_resource *tmpl,
                                   const uint64_t *modifiers,
                                   int count)
{
        bool linear_ok = find_modifier(DRM_FORMAT_MOD_LINEAR, modifiers, count);
        struct vc5_resource *rsc = vc5_resource_setup(pscreen, tmpl);
        struct pipe_resource *prsc = &rsc->base;
        /* Use a tiled layout if we can, for better 3D performance. */
        bool should_tile = true;

        /* VBOs/PBOs are untiled (and 1 height). */
        if (tmpl->target == PIPE_BUFFER)
                should_tile = false;

        /* Cursors are always linear, and the user can request linear as well.
         */
        if (tmpl->bind & (PIPE_BIND_LINEAR | PIPE_BIND_CURSOR))
                should_tile = false;

        /* 1D and 1D_ARRAY textures are always raster-order. */
        if (tmpl->target == PIPE_TEXTURE_1D ||
            tmpl->target == PIPE_TEXTURE_1D_ARRAY)
                should_tile = false;

        /* Scanout BOs for simulator need to be linear for interaction with
         * i965.
         */
        if (using_vc5_simulator &&
            tmpl->bind & (PIPE_BIND_SHARED | PIPE_BIND_SCANOUT))
                should_tile = false;

        /* No user-specified modifier; determine our own. */
        if (count == 1 && modifiers[0] == DRM_FORMAT_MOD_INVALID) {
                linear_ok = true;
                rsc->tiled = should_tile;
        } else if (should_tile &&
                   find_modifier(DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED,
                                 modifiers, count)) {
                rsc->tiled = true;
        } else if (linear_ok) {
                rsc->tiled = false;
        } else {
                fprintf(stderr, "Unsupported modifier requested\n");
                return NULL;
        }

        rsc->internal_format = prsc->format;

        vc5_setup_slices(rsc);
        if (!vc5_resource_bo_alloc(rsc))
                goto fail;

        return prsc;
fail:
        vc5_resource_destroy(pscreen, prsc);
        return NULL;
}

struct pipe_resource *
vc5_resource_create(struct pipe_screen *pscreen,
                    const struct pipe_resource *tmpl)
{
        const uint64_t mod = DRM_FORMAT_MOD_INVALID;
        return vc5_resource_create_with_modifiers(pscreen, tmpl, &mod, 1);
}

static struct pipe_resource *
vc5_resource_from_handle(struct pipe_screen *pscreen,
                         const struct pipe_resource *tmpl,
                         struct winsys_handle *whandle,
                         unsigned usage)
{
        struct vc5_screen *screen = vc5_screen(pscreen);
        struct vc5_resource *rsc = vc5_resource_setup(pscreen, tmpl);
        struct pipe_resource *prsc = &rsc->base;
        struct vc5_resource_slice *slice = &rsc->slices[0];

        if (!rsc)
                return NULL;

        switch (whandle->modifier) {
        case DRM_FORMAT_MOD_LINEAR:
                rsc->tiled = false;
                break;
        /* XXX: UIF */
        default:
                fprintf(stderr,
                        "Attempt to import unsupported modifier 0x%llx\n",
                        (long long)whandle->modifier);
                goto fail;
        }

        if (whandle->offset != 0) {
                fprintf(stderr,
                        "Attempt to import unsupported winsys offset %u\n",
                        whandle->offset);
                goto fail;
        }

        switch (whandle->type) {
        case DRM_API_HANDLE_TYPE_SHARED:
                rsc->bo = vc5_bo_open_name(screen,
                                           whandle->handle, whandle->stride);
                break;
        case DRM_API_HANDLE_TYPE_FD:
                rsc->bo = vc5_bo_open_dmabuf(screen,
                                             whandle->handle, whandle->stride);
                break;
        default:
                fprintf(stderr,
                        "Attempt to import unsupported handle type %d\n",
                        whandle->type);
                goto fail;
        }

        if (!rsc->bo)
                goto fail;

        vc5_setup_slices(rsc);
        vc5_debug_resource_layout(rsc, "import");

        if (whandle->stride != slice->stride) {
                static bool warned = false;
                if (!warned) {
                        warned = true;
                        fprintf(stderr,
                                "Attempting to import %dx%d %s with "
                                "unsupported stride %d instead of %d\n",
                                prsc->width0, prsc->height0,
                                util_format_short_name(prsc->format),
                                whandle->stride,
                                slice->stride);
                }
                goto fail;
        }

        return prsc;

fail:
        vc5_resource_destroy(pscreen, prsc);
        return NULL;
}

static struct pipe_surface *
vc5_create_surface(struct pipe_context *pctx,
                   struct pipe_resource *ptex,
                   const struct pipe_surface *surf_tmpl)
{
        struct vc5_context *vc5 = vc5_context(pctx);
        struct vc5_screen *screen = vc5->screen;
        struct vc5_surface *surface = CALLOC_STRUCT(vc5_surface);
        struct vc5_resource *rsc = vc5_resource(ptex);

        if (!surface)
                return NULL;

        assert(surf_tmpl->u.tex.first_layer == surf_tmpl->u.tex.last_layer);

        struct pipe_surface *psurf = &surface->base;
        unsigned level = surf_tmpl->u.tex.level;
        struct vc5_resource_slice *slice = &rsc->slices[level];

        pipe_reference_init(&psurf->reference, 1);
        pipe_resource_reference(&psurf->texture, ptex);

        psurf->context = pctx;
        psurf->format = surf_tmpl->format;
        psurf->width = u_minify(ptex->width0, level);
        psurf->height = u_minify(ptex->height0, level);
        psurf->u.tex.level = level;
        psurf->u.tex.first_layer = surf_tmpl->u.tex.first_layer;
        psurf->u.tex.last_layer = surf_tmpl->u.tex.last_layer;

        surface->offset = vc5_layer_offset(ptex, level,
                                           psurf->u.tex.first_layer);
        surface->tiling = slice->tiling;

        surface->format = vc5_get_rt_format(&screen->devinfo, psurf->format);

        if (util_format_is_depth_or_stencil(psurf->format)) {
                switch (psurf->format) {
                case PIPE_FORMAT_Z16_UNORM:
                        surface->internal_type = V3D_INTERNAL_TYPE_DEPTH_16;
                        break;
                case PIPE_FORMAT_Z32_FLOAT:
                case PIPE_FORMAT_Z32_FLOAT_S8X24_UINT:
                        surface->internal_type = V3D_INTERNAL_TYPE_DEPTH_32F;
                        break;
                default:
                        surface->internal_type = V3D_INTERNAL_TYPE_DEPTH_24;
                }
        } else {
                uint32_t bpp, type;
                vc5_get_internal_type_bpp_for_output_format(&screen->devinfo,
                                                            surface->format,
                                                            &type, &bpp);
                surface->internal_type = type;
                surface->internal_bpp = bpp;
        }

        if (surface->tiling == VC5_TILING_UIF_NO_XOR ||
            surface->tiling == VC5_TILING_UIF_XOR) {
                surface->padded_height_of_output_image_in_uif_blocks =
                        (slice->padded_height /
                         (2 * vc5_utile_height(rsc->cpp)));
        }

        if (rsc->separate_stencil) {
                surface->separate_stencil =
                        vc5_create_surface(pctx, &rsc->separate_stencil->base,
                                           surf_tmpl);
        }

        return &surface->base;
}

static void
vc5_surface_destroy(struct pipe_context *pctx, struct pipe_surface *psurf)
{
        struct vc5_surface *surf = vc5_surface(psurf);

        if (surf->separate_stencil)
                pipe_surface_reference(&surf->separate_stencil, NULL);

        pipe_resource_reference(&psurf->texture, NULL);
        FREE(psurf);
}

static void
vc5_flush_resource(struct pipe_context *pctx, struct pipe_resource *resource)
{
        /* All calls to flush_resource are followed by a flush of the context,
         * so there's nothing to do.
         */
}

static enum pipe_format
vc5_resource_get_internal_format(struct pipe_resource *prsc)
{
        return vc5_resource(prsc)->internal_format;
}

static void
vc5_resource_set_stencil(struct pipe_resource *prsc,
                         struct pipe_resource *stencil)
{
        vc5_resource(prsc)->separate_stencil = vc5_resource(stencil);
}

static struct pipe_resource *
vc5_resource_get_stencil(struct pipe_resource *prsc)
{
        struct vc5_resource *rsc = vc5_resource(prsc);

        return &rsc->separate_stencil->base;
}

static const struct u_transfer_vtbl transfer_vtbl = {
        .resource_create          = vc5_resource_create,
        .resource_destroy         = vc5_resource_destroy,
        .transfer_map             = vc5_resource_transfer_map,
        .transfer_unmap           = vc5_resource_transfer_unmap,
        .transfer_flush_region    = u_default_transfer_flush_region,
        .get_internal_format      = vc5_resource_get_internal_format,
        .set_stencil              = vc5_resource_set_stencil,
        .get_stencil              = vc5_resource_get_stencil,
};

void
vc5_resource_screen_init(struct pipe_screen *pscreen)
{
        pscreen->resource_create_with_modifiers =
                vc5_resource_create_with_modifiers;
        pscreen->resource_create = u_transfer_helper_resource_create;
        pscreen->resource_from_handle = vc5_resource_from_handle;
        pscreen->resource_get_handle = vc5_resource_get_handle;
        pscreen->resource_destroy = u_transfer_helper_resource_destroy;
        pscreen->transfer_helper = u_transfer_helper_create(&transfer_vtbl,
                                                            true, true, true);
}

void
vc5_resource_context_init(struct pipe_context *pctx)
{
        pctx->transfer_map = u_transfer_helper_transfer_map;
        pctx->transfer_flush_region = u_transfer_helper_transfer_flush_region;
        pctx->transfer_unmap = u_transfer_helper_transfer_unmap;
        pctx->buffer_subdata = u_default_buffer_subdata;
        pctx->texture_subdata = u_default_texture_subdata;
        pctx->create_surface = vc5_create_surface;
        pctx->surface_destroy = vc5_surface_destroy;
        pctx->resource_copy_region = util_resource_copy_region;
        pctx->blit = vc5_blit;
        pctx->flush_resource = vc5_flush_resource;
}
