/* 
 * Copyright © 2012 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Benjamin Segovia <benjamin.segovia@intel.com>
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stddef.h>

#include "intel/intel_gpgpu.h"
#include "intel/intel_defines.h"
#include "intel/intel_structs.h"
#include "intel/intel_batchbuffer.h"
#include "intel/intel_driver.h"

#include "cl_alloc.h"
#include "cl_utils.h"

#define GEN_CMD_MEDIA_OBJECT  (0x71000000)
#define MO_TS_BIT             (1 << 24)
#define MO_RETAIN_BIT         (1 << 28)
#define SAMPLER_STATE_SIZE    (16)

/* Stores both binding tables and surface states */
typedef struct surface_heap {
  uint32_t binding_table[256];
  char surface[256][sizeof(gen6_surface_state_t)];
} surface_heap_t;

#define MAX_IF_DESC    32

/* We can bind only a limited number of buffers */
enum { max_buf_n = 128 };

/* Handle GPGPU state */
struct intel_gpgpu
{
  intel_driver_t *drv;
  intel_batchbuffer_t *batch;
  cl_gpgpu_kernel *ker;
  drm_intel_bo *binded_buf[max_buf_n];  /* all buffers binded for the call */
  uint32_t binded_offset[max_buf_n];    /* their offsets in the constant buffer */
  uint32_t binded_n;                    /* number of buffers binded */

  struct {
    drm_intel_bo *bo;
    uint32_t num;
  } idrt_b;
  struct { drm_intel_bo *bo; } surface_heap_b;
  struct { drm_intel_bo *bo; } vfe_state_b;
  struct { drm_intel_bo *bo; } curbe_b;
  struct { drm_intel_bo *bo; } sampler_state_b;
  struct { drm_intel_bo *bo; } perf_b;

  struct {
    uint32_t num_cs_entries;
    uint32_t size_cs_entry;  /* size of one entry in 512bit elements */
  } urb;

  uint32_t max_threads;      /* max threads requested by the user */
};

typedef struct intel_gpgpu intel_gpgpu_t;

static void
intel_gpgpu_delete(intel_gpgpu_t *state)
{
  if (state == NULL)
    return;
  if (state->surface_heap_b.bo)
    drm_intel_bo_unreference(state->surface_heap_b.bo);
  if (state->idrt_b.bo)
    drm_intel_bo_unreference(state->idrt_b.bo);
  if (state->vfe_state_b.bo)
    drm_intel_bo_unreference(state->vfe_state_b.bo);
  if (state->curbe_b.bo)
    drm_intel_bo_unreference(state->curbe_b.bo);
  if (state->sampler_state_b.bo)
    drm_intel_bo_unreference(state->sampler_state_b.bo);
  if (state->perf_b.bo)
    drm_intel_bo_unreference(state->perf_b.bo);
  intel_batchbuffer_delete(state->batch);
  cl_free(state);
}

static intel_gpgpu_t*
intel_gpgpu_new(intel_driver_t *drv)
{
  intel_gpgpu_t *state = NULL;

  TRY_ALLOC_NO_ERR (state, CALLOC(intel_gpgpu_t));
  state->drv = drv;
  state->batch = intel_batchbuffer_new(state->drv);
  assert(state->batch);
  intel_batchbuffer_init(state->batch, state->drv);

exit:
  return state;
error:
  intel_gpgpu_delete(state);
  state = NULL;
  goto exit;
}

static void
intel_gpgpu_select_pipeline(intel_gpgpu_t *state)
{
  BEGIN_BATCH(state->batch, 1);
  OUT_BATCH(state->batch, CMD_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA);
  ADVANCE_BATCH(state->batch);
}

static void
intel_gpgpu_set_base_address(intel_gpgpu_t *state)
{
  const uint32_t def_cc = cc_llc_l3; /* default Cache Control value */
  BEGIN_BATCH(state->batch, 10);
  OUT_BATCH(state->batch, CMD_STATE_BASE_ADDRESS | 8);
  /* 0, Gen State Mem Obj CC, Stateless Mem Obj CC, Stateless Access Write Back */
  OUT_BATCH(state->batch, 0 | (def_cc << 8) | (def_cc << 4) | (0 << 3)| BASE_ADDRESS_MODIFY);    /* General State Base Addr   */
  /* 0, State Mem Obj CC */
  /* We use a state base address for the surface heap since IVB clamp the
   * binding table pointer at 11 bits. So, we cannot use pointers directly while
   * using the surface heap
   */
  OUT_RELOC(state->batch, state->surface_heap_b.bo,
            I915_GEM_DOMAIN_INSTRUCTION,
            I915_GEM_DOMAIN_INSTRUCTION,
            0 | (def_cc << 8) | (def_cc << 4) | (0 << 3)| BASE_ADDRESS_MODIFY);
  OUT_BATCH(state->batch, 0 | (def_cc << 8) | BASE_ADDRESS_MODIFY); /* Dynamic State Base Addr */
  OUT_BATCH(state->batch, 0 | (def_cc << 8) | BASE_ADDRESS_MODIFY); /* Indirect Obj Base Addr */
  OUT_BATCH(state->batch, 0 | (def_cc << 8) | BASE_ADDRESS_MODIFY); /* Instruction Base Addr  */
  /* If we output an AUB file, we limit the total size to 64MB */
#if USE_FULSIM
  OUT_BATCH(state->batch, 0x04000000 | BASE_ADDRESS_MODIFY); /* General State Access Upper Bound */
  OUT_BATCH(state->batch, 0x04000000 | BASE_ADDRESS_MODIFY); /* Dynamic State Access Upper Bound */
  OUT_BATCH(state->batch, 0x04000000 | BASE_ADDRESS_MODIFY); /* Indirect Obj Access Upper Bound */
  OUT_BATCH(state->batch, 0x04000000 | BASE_ADDRESS_MODIFY); /* Instruction Access Upper Bound */
#else
  OUT_BATCH(state->batch, 0 | BASE_ADDRESS_MODIFY);
  OUT_BATCH(state->batch, 0 | BASE_ADDRESS_MODIFY);
  OUT_BATCH(state->batch, 0 | BASE_ADDRESS_MODIFY);
  OUT_BATCH(state->batch, 0 | BASE_ADDRESS_MODIFY);
#endif /* USE_FULSIM */
  ADVANCE_BATCH(state->batch);
}

static void
intel_gpgpu_load_vfe_state(intel_gpgpu_t *state)
{
  BEGIN_BATCH(state->batch, 8);
  OUT_BATCH(state->batch, CMD_MEDIA_STATE_POINTERS | (8-2));

  gen6_vfe_state_inline_t* vfe = (gen6_vfe_state_inline_t*)
    intel_batchbuffer_alloc_space(state->batch,0);

  memset(vfe, 0, sizeof(struct gen6_vfe_state_inline));
  vfe->vfe1.gpgpu_mode = 1;
  vfe->vfe1.bypass_gateway_ctl = 1;
  vfe->vfe1.reset_gateway_timer = 1;
  vfe->vfe1.max_threads = state->max_threads - 1;
  vfe->vfe1.urb_entries = 64;
  vfe->vfe3.curbe_size = 480;
  vfe->vfe4.scoreboard_mask = 0;
  //vfe->vfe3.urb_size = 13;
  //vfe->vfe4.scoreboard_mask = (state->drv->gen_ver == 7 || state->drv->gen_ver == 75) ? 0 : 0x80000000;
  intel_batchbuffer_alloc_space(state->batch, sizeof(gen6_vfe_state_inline_t));
  ADVANCE_BATCH(state->batch);
}

static void
intel_gpgpu_load_constant_buffer(intel_gpgpu_t *state) 
{
  BEGIN_BATCH(state->batch, 4);
  OUT_BATCH(state->batch, CMD(2,0,1) | (4 - 2));  /* length-2 */
  OUT_BATCH(state->batch, 0);                     /* mbz */
// XXX
#if 1
  OUT_BATCH(state->batch,
            state->urb.size_cs_entry*
            state->urb.num_cs_entries*32);
#else
  OUT_BATCH(state->batch, 5120);
#endif
  OUT_RELOC(state->batch, state->curbe_b.bo, I915_GEM_DOMAIN_INSTRUCTION, 0, 0);
  ADVANCE_BATCH(state->batch);
}

static void
intel_gpgpu_load_idrt(intel_gpgpu_t *state) 
{
  BEGIN_BATCH(state->batch, 4);
  OUT_BATCH(state->batch, CMD(2,0,2) | (4 - 2)); /* length-2 */
  OUT_BATCH(state->batch, 0);                    /* mbz */
  OUT_BATCH(state->batch, state->idrt_b.num << 5);
  OUT_RELOC(state->batch, state->idrt_b.bo, I915_GEM_DOMAIN_INSTRUCTION, 0, 0);
  ADVANCE_BATCH(state->batch);
}

static const uint32_t gpgpu_l3_config_reg1[] =
{
              // SLM    URB     DC      RO      I/S     C       T
  0x00080040, //{ 0,    256,    0,      256,    0,      0,      0,      }
  0x02040040, //{ 0,    256,    128,    128,    0,      0,      0,      }
  0x00800040, //{ 0,    256,    32,     0,      64,     32,     128,    }
  0x01000038, //{ 0,    224,    64,     0,      64,     32,     128,    }
  0x02000030, //{ 0,    224,    128,    0,      64,     32,     64,     }
  0x01000038, //{ 0,    224,    64,     0,      128,    32,     64,     }
  0x00000038, //{ 0,    224,    0,      0,      128,    32,     128,    }
  0x00000040, //{ 0,    256,    0,      0,      128,    0,      128,    }
  0x0A140091, //{ 128,  128,    128,    128,    0,      0,      0,      }
  0x09100091, //{ 128,  128,    64,     0,      64,     64,     64,     }
  0x08900091, //{ 128,  128,    32,     0,      64,     32,     128,    }
  0x08900091  //{ 128,  128,    32,     0,      128,    32,     64,     }
};

static const uint32_t gpgpu_l3_config_reg2[] =
{
              // SLM    URB     DC      RO      I/S     C       T
  0x00000000, //{ 0,    256,    0,      256,    0,      0,      0,      }
  0x00000000, //{ 0,    256,    128,    128,    0,      0,      0,      }
  0x00080410, //{ 0,    256,    32,     0,      64,     32,     128,    }
  0x00080410, //{ 0,    224,    64,     0,      64,     32,     128,    }
  0x00040410, //{ 0,    224,    128,    0,      64,     32,     64,     }
  0x00040420, //{ 0,    224,    64,     0,      128,    32,     64,     }
  0x00080420, //{ 0,    224,    0,      0,      128,    32,     128,    }
  0x00080020, //{ 0,    256,    0,      0,      128,    0,      128,    }
  0x00204080, //{ 128,  128,    128,    128,    0,      0,      0,      }
  0x00244890, //{ 128,  128,    64,     0,      64,     64,     64,     }
  0x00284490, //{ 128,  128,    32,     0,      64,     32,     128,    }
  0x002444A0  //{ 128,  128,    32,     0,      128,    32,     64,     }
};

// L3 cache stuff 
#define L3_CNTL_REG2_ADDRESS_OFFSET         (0xB020)
#define L3_CNTL_REG3_ADDRESS_OFFSET         (0xB024)

enum INSTRUCTION_PIPELINE
{
  PIPE_COMMON       = 0x0,
  PIPE_SINGLE_DWORD = 0x1,
  PIPE_COMMON_CTG   = 0x1,
  PIPE_MEDIA        = 0x2,
  PIPE_3D           = 0x3
};

enum GFX_OPCODE
{
  GFXOP_PIPELINED     = 0x0,
  GFXOP_NONPIPELINED  = 0x1,
  GFXOP_3DPRIMITIVE   = 0x3
};

enum INSTRUCTION_TYPE
{
  INSTRUCTION_MI      = 0x0,
  INSTRUCTION_TRUSTED = 0x1,
  INSTRUCTION_2D      = 0x2,
  INSTRUCTION_GFX     = 0x3
};

enum GFX3DCONTROL_SUBOPCODE
{
  GFX3DSUBOP_3DCONTROL    = 0x00
};

enum GFX3D_OPCODE
{
  GFX3DOP_3DSTATE_PIPELINED       = 0x0,
  GFX3DOP_3DSTATE_NONPIPELINED    = 0x1,
  GFX3DOP_3DCONTROL               = 0x2,
  GFX3DOP_3DPRIMITIVE             = 0x3
};

enum GFX3DSTATE_PIPELINED_SUBOPCODE
{
  GFX3DSUBOP_3DSTATE_PIPELINED_POINTERS       = 0x00,
  GFX3DSUBOP_3DSTATE_BINDING_TABLE_POINTERS   = 0x01,
  GFX3DSUBOP_3DSTATE_STATE_POINTER_INVALIDATE = 0x02,
  GFX3DSUBOP_3DSTATE_VERTEX_BUFFERS           = 0x08,
  GFX3DSUBOP_3DSTATE_VERTEX_ELEMENTS          = 0x09,
  GFX3DSUBOP_3DSTATE_INDEX_BUFFER             = 0x0A,
  GFX3DSUBOP_3DSTATE_VF_STATISTICS            = 0x0B,
  GFX3DSUBOP_3DSTATE_CC_STATE_POINTERS        = 0x0E
};

static void
intel_gpgpu_pipe_control(intel_gpgpu_t *state)
{
  BEGIN_BATCH(state->batch, SIZEOF32(gen6_pipe_control_t));
  gen6_pipe_control_t* pc = (gen6_pipe_control_t*)
    intel_batchbuffer_alloc_space(state->batch, 0);
  memset(pc, 0, sizeof(*pc));
  pc->dw0.length = SIZEOF32(gen6_pipe_control_t) - 2;
  pc->dw0.instruction_subopcode = GFX3DSUBOP_3DCONTROL;
  pc->dw0.instruction_opcode = GFX3DOP_3DCONTROL;
  pc->dw0.instruction_pipeline = PIPE_3D;
  pc->dw0.instruction_type = INSTRUCTION_GFX;
  pc->dw1.render_target_cache_flush_enable = 1;
  pc->dw1.cs_stall = 1;
  pc->dw1.dc_flush_enable = 1;
  ADVANCE_BATCH(state->batch);
}

static void
intel_gpgpu_set_L3(intel_gpgpu_t *state, uint32_t use_barrier)
{
  BEGIN_BATCH(state->batch, 6);
  OUT_BATCH(state->batch, CMD_LOAD_REGISTER_IMM | 1); /* length - 2 */
  OUT_BATCH(state->batch, L3_CNTL_REG2_ADDRESS_OFFSET);
  if (use_barrier)
    OUT_BATCH(state->batch, gpgpu_l3_config_reg1[8]);
  else
    OUT_BATCH(state->batch, gpgpu_l3_config_reg1[4]);

  OUT_BATCH(state->batch, CMD_LOAD_REGISTER_IMM | 1); /* length - 2 */
  OUT_BATCH(state->batch, L3_CNTL_REG3_ADDRESS_OFFSET);
  if (use_barrier)
    OUT_BATCH(state->batch, gpgpu_l3_config_reg2[8]);
  else
    OUT_BATCH(state->batch, gpgpu_l3_config_reg2[4]);
  ADVANCE_BATCH(state->batch);

  intel_gpgpu_pipe_control(state);
}

static void
intel_gpgpu_batch_start(intel_gpgpu_t *state)
{
  intel_batchbuffer_start_atomic(state->batch, 256);
  intel_gpgpu_pipe_control(state);
  if (state->drv->gen_ver == 7 || state->drv->gen_ver == 75)
    intel_gpgpu_set_L3(state, state->ker->use_barrier);
  intel_gpgpu_select_pipeline(state);
  intel_gpgpu_set_base_address(state);
  intel_gpgpu_load_vfe_state(state);
  intel_gpgpu_load_constant_buffer(state);
  intel_gpgpu_load_idrt(state);

  if (state->perf_b.bo) {
    BEGIN_BATCH(state->batch, 3);
    OUT_BATCH(state->batch,
              (0x28 << 23) | /* MI_REPORT_PERF_COUNT */
              (3 - 2));      /* length-2 */
    OUT_RELOC(state->batch, state->perf_b.bo,
              I915_GEM_DOMAIN_RENDER,
              I915_GEM_DOMAIN_RENDER,
              0 |  /* Offset for the start "counters" */
              1);  /* Use GTT and not PGTT */
    OUT_BATCH(state->batch, 0);
    ADVANCE_BATCH(state->batch);
  }
}

static void
intel_gpgpu_batch_end(intel_gpgpu_t *state, int32_t flush_mode)
{
  /* Insert the performance counter command */
  if (state->perf_b.bo) {
    BEGIN_BATCH(state->batch, 3);
    OUT_BATCH(state->batch,
              (0x28 << 23) | /* MI_REPORT_PERF_COUNT */
              (3 - 2));      /* length-2 */
    OUT_RELOC(state->batch, state->perf_b.bo,
              I915_GEM_DOMAIN_RENDER,
              I915_GEM_DOMAIN_RENDER,
              512 |  /* Offset for the end "counters" */
              1);    /* Use GTT and not PGTT */
    OUT_BATCH(state->batch, 0);
    ADVANCE_BATCH(state->batch);
  }

  if(flush_mode) intel_gpgpu_pipe_control(state);
  intel_batchbuffer_end_atomic(state->batch);
}

static void
intel_gpgpu_batch_reset(intel_gpgpu_t *state, size_t sz)
{
  intel_batchbuffer_reset(state->batch, sz);
}

static void
intel_gpgpu_flush(intel_gpgpu_t *state)
{
  intel_batchbuffer_flush(state->batch);
}

static void
intel_gpgpu_state_init(intel_gpgpu_t *state,
                       uint32_t max_threads,
                       uint32_t size_cs_entry)
{
  dri_bo *bo;

  /* Binded buffers */
  state->binded_n = 0;

  /* URB */
  state->urb.num_cs_entries = 64;
  state->urb.size_cs_entry = size_cs_entry;
  state->max_threads = max_threads;

  /* constant buffer */
  if(state->curbe_b.bo)
    dri_bo_unreference(state->curbe_b.bo);
  uint32_t size_cb = state->urb.num_cs_entries * state->urb.size_cs_entry * 64;
  size_cb = ALIGN(size_cb, 4096);
  bo = dri_bo_alloc(state->drv->bufmgr,
                    "CONSTANT_BUFFER",
                    size_cb,
                    64);
  assert(bo);
  state->curbe_b.bo = bo;

  /* surface state */
  if(state->surface_heap_b.bo)
    dri_bo_unreference(state->surface_heap_b.bo);
  bo = dri_bo_alloc(state->drv->bufmgr, 
                    "SURFACE_HEAP",
                    sizeof(surface_heap_t),
                    32);
  assert(bo);
  dri_bo_map(bo, 1);
  memset(bo->virtual, 0, sizeof(surface_heap_t));
  state->surface_heap_b.bo = bo;

  /* Interface descriptor remap table */
  if(state->idrt_b.bo)
    dri_bo_unreference(state->idrt_b.bo);
  bo = dri_bo_alloc(state->drv->bufmgr, 
                    "IDRT",
                    MAX_IF_DESC * sizeof(struct gen6_interface_descriptor),
                    32);
  assert(bo);
  state->idrt_b.bo = bo;

  /* vfe state */
  if(state->vfe_state_b.bo)
    dri_bo_unreference(state->vfe_state_b.bo);
  state->vfe_state_b.bo = NULL;

  /* sampler state */
  if (state->sampler_state_b.bo)
    dri_bo_unreference(state->sampler_state_b.bo);
  bo = dri_bo_alloc(state->drv->bufmgr, 
                    "sample states",
                    GEN_MAX_SAMPLERS * sizeof(gen6_sampler_state_t),
                    32);
  assert(bo);
  dri_bo_map(bo, 1);
  memset(bo->virtual, 0, sizeof(gen6_sampler_state_t) * GEN_MAX_SAMPLERS);
  state->sampler_state_b.bo = bo;
}

static void
intel_gpgpu_set_buf_reloc_gen7(intel_gpgpu_t *state, int32_t index, dri_bo* obj_bo)
{
  surface_heap_t *heap = state->surface_heap_b.bo->virtual;
  heap->binding_table[index] = offsetof(surface_heap_t, surface) +
                               index * sizeof(gen7_surface_state_t);
  dri_bo_emit_reloc(state->surface_heap_b.bo,
                    I915_GEM_DOMAIN_RENDER,
                    I915_GEM_DOMAIN_RENDER,
                    0,
                    heap->binding_table[index] +
                    offsetof(gen7_surface_state_t, ss1),
                    obj_bo);
}

/* Map address space with two 2GB surfaces. One surface for untyped message and
 * one surface for byte scatters / gathers. Actually the HW will not require teo
 * surface but Fulsim complains
 */
static void
intel_gpgpu_map_address_space(intel_gpgpu_t *state)
{
  surface_heap_t *heap = state->surface_heap_b.bo->virtual;
  gen7_surface_state_t *ss0 = (gen7_surface_state_t *) heap->surface[0];
  gen7_surface_state_t *ss1 = (gen7_surface_state_t *) heap->surface[1];
  memset(ss0, 0, sizeof(gen7_surface_state_t));
  memset(ss1, 0, sizeof(gen7_surface_state_t));
  ss1->ss0.surface_type = ss0->ss0.surface_type = I965_SURFACE_BUFFER;
  ss1->ss0.surface_format = ss0->ss0.surface_format = I965_SURFACEFORMAT_RAW;
  ss1->ss2.width  = ss0->ss2.width  = 127;   /* bits 6:0 of sz */
  ss1->ss2.height = ss0->ss2.height = 16383; /* bits 20:7 of sz */
  ss0->ss3.depth  = 1023; /* bits 30:21 of sz */
  ss1->ss3.depth  = 510;  /* bits 30:21 of sz */
  ss1->ss5.cache_control = ss0->ss5.cache_control = cc_llc_l3;
  heap->binding_table[0] = offsetof(surface_heap_t, surface);
  heap->binding_table[1] = sizeof(gen7_surface_state_t) + offsetof(surface_heap_t, surface);
}

static void
intel_gpgpu_bind_image2D_gen7(intel_gpgpu_t *state,
                              int32_t index,
                              dri_bo* obj_bo,
                              uint32_t format,
                              int32_t w,
                              int32_t h,
                              int32_t pitch,
                              int32_t tiling)
{
  surface_heap_t *heap = state->surface_heap_b.bo->virtual;
  gen7_surface_state_t *ss = (gen7_surface_state_t *) heap->surface[index];
  memset(ss, 0, sizeof(*ss));
  ss->ss0.surface_type = I965_SURFACE_2D;
  ss->ss0.surface_format = format;
  ss->ss1.base_addr = obj_bo->offset;
  ss->ss2.width = w - 1;
  ss->ss2.height = h - 1;
  ss->ss3.pitch = pitch - 1;
  ss->ss5.cache_control = cc_llc_l3;
  if (tiling == GPGPU_TILE_X) {
    ss->ss0.tiled_surface = 1;
    ss->ss0.tile_walk = I965_TILEWALK_XMAJOR;
  } else if (tiling == GPGPU_TILE_Y) {
    ss->ss0.tiled_surface = 1;
    ss->ss0.tile_walk = I965_TILEWALK_YMAJOR;
  }
  intel_gpgpu_set_buf_reloc_gen7(state, index, obj_bo);
}

static void
intel_gpgpu_bind_buf(intel_gpgpu_t *gpgpu, drm_intel_bo *buf, uint32_t offset, uint32_t cchint)
{
  assert(gpgpu->binded_n < max_buf_n);
  gpgpu->binded_buf[gpgpu->binded_n] = buf;
  gpgpu->binded_offset[gpgpu->binded_n] = offset;
  gpgpu->binded_n++;
#if 0
  const uint32_t size = obj_bo->size;
  assert(index < GEN_MAX_SURFACES);
  if (state->drv->gen_ver == 7 || state->drv->gen_ver == 75)
    intel_gpgpu_bind_buf_gen7(state, index, obj_bo, size, cchint);
  else
    NOT_IMPLEMENTED;
#endif
}

static void
intel_gpgpu_bind_image2D(intel_gpgpu_t *state,
                         int32_t index,
                         cl_buffer *obj_bo,
                         uint32_t format,
                         int32_t w,
                         int32_t h,
                         int32_t pitch,
                         cl_gpgpu_tiling tiling)
{
  assert(index < GEN_MAX_SURFACES);
  if (state->drv->gen_ver == 7 || state->drv->gen_ver == 75)
    intel_gpgpu_bind_image2D_gen7(state, index, (drm_intel_bo*) obj_bo, format, w, h, pitch, tiling);
  else
    NOT_IMPLEMENTED;
}

static void
intel_gpgpu_build_idrt(intel_gpgpu_t *state, cl_gpgpu_kernel *kernel)
{
  gen6_interface_descriptor_t *desc;
  drm_intel_bo *bo = NULL, *ker_bo = NULL;

  bo = state->idrt_b.bo;
  dri_bo_map(bo, 1);
  assert(bo->virtual);
  desc = (gen6_interface_descriptor_t*) bo->virtual;

  memset(desc, 0, sizeof(*desc));
  ker_bo = (drm_intel_bo *) kernel->bo;
  desc->desc0.kernel_start_pointer = ker_bo->offset >> 6; /* reloc */
  desc->desc1.single_program_flow = 1;
  desc->desc2.sampler_state_pointer = state->sampler_state_b.bo->offset >> 5;
  desc->desc3.binding_table_entry_count = 0; /* no prefetch */
  desc->desc3.binding_table_pointer = 0;
  desc->desc4.curbe_read_len = kernel->cst_sz / 32;
  desc->desc4.curbe_read_offset = 0;

  /* Barriers / SLM are automatically handled on Gen7+ */
  if (state->drv->gen_ver == 7 || state->drv->gen_ver == 75) {
    size_t slm_sz = kernel->slm_sz;
    desc->desc5.group_threads_num = kernel->use_barrier ? kernel->thread_n : 0;
    desc->desc5.barrier_enable = kernel->use_barrier;
    if (slm_sz > 0) {
      if (slm_sz <= 4 * KB)
        slm_sz = 4 * KB; //4KB
      else if (slm_sz <= 8 * KB)
        slm_sz = 8 * KB; //8KB
      else if (slm_sz <= 16 * KB)
        slm_sz = 16 * KB; //16KB
      else if (slm_sz <= 32 * KB)
        slm_sz = 32 * KB; //32KB
      else if (slm_sz <= 64 * KB)
        slm_sz = 64 * KB; //64KB
      slm_sz = slm_sz >> 12;
    }
    desc->desc5.slm_sz = slm_sz;
  }
  else
    desc->desc5.group_threads_num = kernel->barrierID; /* BarrierID on GEN6 */

  dri_bo_emit_reloc(bo,
                    I915_GEM_DOMAIN_INSTRUCTION, 0,
                    0,
                    offsetof(gen6_interface_descriptor_t, desc0),
                    ker_bo);

  dri_bo_emit_reloc(bo,
                    I915_GEM_DOMAIN_INSTRUCTION, 0,
                    0,
                    offsetof(gen6_interface_descriptor_t, desc2),
                    state->sampler_state_b.bo);
  state->idrt_b.num = 1;
  dri_bo_unmap(bo);
}

static void
intel_gpgpu_upload_constants(intel_gpgpu_t *gpgpu, const void* data, uint32_t size)
{
  unsigned char *constant_buffer = NULL;
  cl_gpgpu_kernel *k = gpgpu->ker;
  uint32_t i, j;

  /* Upload the data first */
  dri_bo_map(gpgpu->curbe_b.bo, 1);
  assert(gpgpu->curbe_b.bo->virtual);
  constant_buffer = (unsigned char *) gpgpu->curbe_b.bo->virtual;
  memcpy(constant_buffer, data, size);
  dri_bo_unmap(gpgpu->curbe_b.bo);

  /* Now put all the relocations for our flat address space */
  for (i = 0; i < k->thread_n; ++i)
    for (j = 0; j < gpgpu->binded_n; ++j)
      drm_intel_bo_emit_reloc(gpgpu->curbe_b.bo,
                              gpgpu->binded_offset[j]+i*k->cst_sz,
                              gpgpu->binded_buf[j],
                              0,
                              I915_GEM_DOMAIN_RENDER,
                              I915_GEM_DOMAIN_RENDER);
}

static void
intel_gpgpu_upload_samplers(intel_gpgpu_t *gpgpu, const void *data, uint32_t n)
{
  if (n) {
    const size_t sz = n * sizeof(gen6_sampler_state_t);
    memcpy(gpgpu->sampler_state_b.bo->virtual, data, sz);
  }
}

static void
intel_gpgpu_states_setup(intel_gpgpu_t *gpgpu, cl_gpgpu_kernel *kernel)
{
  gpgpu->ker = kernel;
  intel_gpgpu_build_idrt(gpgpu, kernel);
  intel_gpgpu_map_address_space(gpgpu);
  dri_bo_unmap(gpgpu->surface_heap_b.bo);
  dri_bo_unmap(gpgpu->sampler_state_b.bo);
}

static void
intel_gpgpu_set_perf_counters(intel_gpgpu_t *gpgpu, cl_buffer *perf)
{
  if (gpgpu->perf_b.bo)
    drm_intel_bo_unreference(gpgpu->perf_b.bo);
  drm_intel_bo_reference((drm_intel_bo*) perf);
  gpgpu->perf_b.bo = (drm_intel_bo*) perf;
}

static void
intel_gpgpu_walker(intel_gpgpu_t *gpgpu,
                   uint32_t simd_sz,
                   uint32_t thread_n,
                   const size_t global_wk_off[3],
                   const size_t global_wk_sz[3],
                   const size_t local_wk_sz[3])
{
  const uint32_t global_wk_dim[3] = {
    global_wk_sz[0] / local_wk_sz[0],
    global_wk_sz[1] / local_wk_sz[1],
    global_wk_sz[2] / local_wk_sz[2]
  };
  assert(simd_sz == 8 || simd_sz == 16);
  BEGIN_BATCH(gpgpu->batch, 11);
  OUT_BATCH(gpgpu->batch, CMD_GPGPU_WALKER | 9);
  OUT_BATCH(gpgpu->batch, 0);                        /* kernel index == 0 */
  if (simd_sz == 16)
    OUT_BATCH(gpgpu->batch, (1 << 30) | (thread_n-1)); /* SIMD16 | thread max */
  else
    OUT_BATCH(gpgpu->batch, (0 << 30) | (thread_n-1)); /* SIMD8  | thread max */
  OUT_BATCH(gpgpu->batch, global_wk_off[0]);
  OUT_BATCH(gpgpu->batch, global_wk_dim[0]);
  OUT_BATCH(gpgpu->batch, global_wk_off[1]);
  OUT_BATCH(gpgpu->batch, global_wk_dim[1]);
  OUT_BATCH(gpgpu->batch, global_wk_off[2]);
  OUT_BATCH(gpgpu->batch, global_wk_dim[2]);
  OUT_BATCH(gpgpu->batch, ~0x0);
  OUT_BATCH(gpgpu->batch, ~0x0);
  ADVANCE_BATCH(gpgpu->batch);

  BEGIN_BATCH(gpgpu->batch, 2);
  OUT_BATCH(gpgpu->batch, CMD_MEDIA_STATE_FLUSH | 0);
  OUT_BATCH(gpgpu->batch, 0);                        /* kernel index == 0 */
  ADVANCE_BATCH(gpgpu->batch);
}

LOCAL void
intel_set_gpgpu_callbacks(void)
{
  cl_gpgpu_new = (cl_gpgpu_new_cb *) intel_gpgpu_new;
  cl_gpgpu_delete = (cl_gpgpu_delete_cb *) intel_gpgpu_delete;
  cl_gpgpu_bind_image2D = (cl_gpgpu_bind_image2D_cb *) intel_gpgpu_bind_image2D;
  cl_gpgpu_bind_buf = (cl_gpgpu_bind_buf_cb *) intel_gpgpu_bind_buf;
  cl_gpgpu_state_init = (cl_gpgpu_state_init_cb *) intel_gpgpu_state_init;
  cl_gpgpu_set_perf_counters = (cl_gpgpu_set_perf_counters_cb *) intel_gpgpu_set_perf_counters;
  cl_gpgpu_upload_constants = (cl_gpgpu_upload_constants_cb *) intel_gpgpu_upload_constants;
  cl_gpgpu_states_setup = (cl_gpgpu_states_setup_cb *) intel_gpgpu_states_setup;
  cl_gpgpu_upload_samplers = (cl_gpgpu_upload_samplers_cb *) intel_gpgpu_upload_samplers;
  cl_gpgpu_batch_reset = (cl_gpgpu_batch_reset_cb *) intel_gpgpu_batch_reset;
  cl_gpgpu_batch_start = (cl_gpgpu_batch_start_cb *) intel_gpgpu_batch_start;
  cl_gpgpu_batch_end = (cl_gpgpu_batch_end_cb *) intel_gpgpu_batch_end;
  cl_gpgpu_flush = (cl_gpgpu_flush_cb *) intel_gpgpu_flush;
  cl_gpgpu_walker = (cl_gpgpu_walker_cb *) intel_gpgpu_walker;
}

