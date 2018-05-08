/*
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

#ifndef H_LIMA_SCREEN
#define H_LIMA_SCREEN

#include "util/slab.h"
#include "util/list.h"
#include "os/os_thread.h"

#include "pipe/p_screen.h"

extern bool lima_dump_command_stream;
extern int lima_ctx_num_plb;

/* max texture size is 4096x4096 */
#define LIMA_MAX_MIP_LEVELS 13

struct ra_regs;

struct lima_screen {
   struct pipe_screen base;
   struct renderonly *ro;

   int refcnt;
   void *winsys_priv;

   int fd;
   int gpu_type;
   int num_pp;

   /* va mgr */
   mtx_t va_lock;
   struct list_head va_holes;

   /* bo table */
   mtx_t bo_table_lock;
   struct util_hash_table *bo_handles;
   struct util_hash_table *bo_flink_names;

   struct slab_parent_pool transfer_pool;

   struct ra_regs *pp_ra;

   struct lima_bo *gp_buffer;
   #define gp_tile_heap_offset       0x000000
   #define gp_buffer_size            0x100000

   struct lima_bo *pp_buffer;
   #define pp_frame_rsw_offset       0x0000
   #define pp_clear_program_offset   0x0040
   #define pp_stack_offset           0x0080
   #define pp_buffer_size            0x2000
};

static inline struct lima_screen *
lima_screen(struct pipe_screen *pscreen)
{
   return (struct lima_screen *)pscreen;
}

struct pipe_screen *
lima_screen_create(int fd, struct renderonly *ro);

#endif
