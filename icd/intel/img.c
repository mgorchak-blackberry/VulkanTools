/*
 * XGL
 *
 * Copyright (C) 2014 LunarG, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "ilo_screen.h"
#include "ilo_resource.h"

/* use PIPE_BIND_CUSTOM to indicate MCS */
#define ILO_BIND_MCS PIPE_BIND_CUSTOM

struct tex_layout {
   const struct ilo_dev_info *dev;
   const struct pipe_resource *templ;

   bool has_depth, has_stencil;
   bool hiz, separate_stencil;

   enum pipe_format format;
   unsigned block_width, block_height, block_size;
   bool compressed;

   enum intel_tiling_mode tiling;
   unsigned valid_tilings; /* bitmask of valid tiling modes */

   bool array_spacing_full;
   bool interleaved;

   struct {
      int w, h, d;
      struct ilo_texture_slice *slices;
   } levels[PIPE_MAX_TEXTURE_LEVELS];

   int align_i, align_j;
   int qpitch;

   int width, height;

   int bo_stride, bo_height;
   int hiz_stride, hiz_height;
};

/*
 * From the Ivy Bridge PRM, volume 1 part 1, page 105:
 *
 *     "In addition to restrictions on maximum height, width, and depth,
 *      surfaces are also restricted to a maximum size in bytes. This
 *      maximum is 2 GB for all products and all surface types."
 */
static const size_t max_resource_size = 1u << 31;

static const char *
resource_get_bo_name(const struct pipe_resource *templ)
{
   static const char *target_names[PIPE_MAX_TEXTURE_TYPES] = {
      [PIPE_BUFFER] = "buf",
      [PIPE_TEXTURE_1D] = "tex-1d",
      [PIPE_TEXTURE_2D] = "tex-2d",
      [PIPE_TEXTURE_3D] = "tex-3d",
      [PIPE_TEXTURE_CUBE] = "tex-cube",
      [PIPE_TEXTURE_RECT] = "tex-rect",
      [PIPE_TEXTURE_1D_ARRAY] = "tex-1d-array",
      [PIPE_TEXTURE_2D_ARRAY] = "tex-2d-array",
      [PIPE_TEXTURE_CUBE_ARRAY] = "tex-cube-array",
   };
   const char *name = target_names[templ->target];

   if (templ->target == PIPE_BUFFER) {
      switch (templ->bind) {
      case PIPE_BIND_VERTEX_BUFFER:
         name = "buf-vb";
         break;
      case PIPE_BIND_INDEX_BUFFER:
         name = "buf-ib";
         break;
      case PIPE_BIND_CONSTANT_BUFFER:
         name = "buf-cb";
         break;
      case PIPE_BIND_STREAM_OUTPUT:
         name = "buf-so";
         break;
      default:
         break;
      }
   }

   return name;
}

static enum intel_domain_flag
resource_get_bo_initial_domain(const struct pipe_resource *templ)
{
   return (templ->bind & (PIPE_BIND_DEPTH_STENCIL |
                          PIPE_BIND_RENDER_TARGET |
                          PIPE_BIND_STREAM_OUTPUT)) ?
      INTEL_DOMAIN_RENDER : 0;
}

static void
tex_layout_init_qpitch(struct tex_layout *layout)
{
   const struct pipe_resource *templ = layout->templ;
   int h0, h1;

   if (templ->array_size <= 1)
      return;

   h0 = align(layout->levels[0].h, layout->align_j);

   if (!layout->array_spacing_full) {
      layout->qpitch = h0;
      return;
   }

   h1 = align(layout->levels[1].h, layout->align_j);

   /*
    * From the Sandy Bridge PRM, volume 1 part 1, page 115:
    *
    *     "The following equation is used for surface formats other than
    *      compressed textures:
    *
    *        QPitch = (h0 + h1 + 11j)"
    *
    *     "The equation for compressed textures (BC* and FXT1 surface formats)
    *      follows:
    *
    *        QPitch = (h0 + h1 + 11j) / 4"
    *
    *     "[DevSNB] Errata: Sampler MSAA Qpitch will be 4 greater than the
    *      value calculated in the equation above, for every other odd Surface
    *      Height starting from 1 i.e. 1,5,9,13"
    *
    * From the Ivy Bridge PRM, volume 1 part 1, page 111-112:
    *
    *     "If Surface Array Spacing is set to ARYSPC_FULL (note that the depth
    *      buffer and stencil buffer have an implied value of ARYSPC_FULL):
    *
    *        QPitch = (h0 + h1 + 12j)
    *        QPitch = (h0 + h1 + 12j) / 4 (compressed)
    *
    *      (There are many typos or missing words here...)"
    *
    * To access the N-th slice, an offset of (Stride * QPitch * N) is added to
    * the base address.  The PRM divides QPitch by 4 for compressed formats
    * because the block height for those formats are 4, and it wants QPitch to
    * mean the number of memory rows, as opposed to texel rows, between
    * slices.  Since we use texel rows in tex->slice_offsets, we do not need
    * to divide QPitch by 4.
    */
   layout->qpitch = h0 + h1 +
      ((layout->dev->gen >= ILO_GEN(7)) ? 12 : 11) * layout->align_j;

   if (layout->dev->gen == ILO_GEN(6) && templ->nr_samples > 1 &&
       templ->height0 % 4 == 1)
      layout->qpitch += 4;
}

static void
tex_layout_init_alignments(struct tex_layout *layout)
{
   const struct pipe_resource *templ = layout->templ;

   /*
    * From the Sandy Bridge PRM, volume 1 part 1, page 113:
    *
    *     "surface format           align_i     align_j
    *      YUV 4:2:2 formats        4           *see below
    *      BC1-5                    4           4
    *      FXT1                     8           4
    *      all other formats        4           *see below"
    *
    *     "- align_j = 4 for any depth buffer
    *      - align_j = 2 for separate stencil buffer
    *      - align_j = 4 for any render target surface is multisampled (4x)
    *      - align_j = 4 for any render target surface with Surface Vertical
    *        Alignment = VALIGN_4
    *      - align_j = 2 for any render target surface with Surface Vertical
    *        Alignment = VALIGN_2
    *      - align_j = 2 for all other render target surface
    *      - align_j = 2 for any sampling engine surface with Surface Vertical
    *        Alignment = VALIGN_2
    *      - align_j = 4 for any sampling engine surface with Surface Vertical
    *        Alignment = VALIGN_4"
    *
    * From the Sandy Bridge PRM, volume 4 part 1, page 86:
    *
    *     "This field (Surface Vertical Alignment) must be set to VALIGN_2 if
    *      the Surface Format is 96 bits per element (BPE)."
    *
    * They can be rephrased as
    *
    *                                  align_i        align_j
    *   compressed formats             block width    block height
    *   PIPE_FORMAT_S8_UINT            4              2
    *   other depth/stencil formats    4              4
    *   4x multisampled                4              4
    *   bpp 96                         4              2
    *   others                         4              2 or 4
    */

   /*
    * From the Ivy Bridge PRM, volume 1 part 1, page 110:
    *
    *     "surface defined by      surface format     align_i     align_j
    *      3DSTATE_DEPTH_BUFFER    D16_UNORM          8           4
    *                              not D16_UNORM      4           4
    *      3DSTATE_STENCIL_BUFFER  N/A                8           8
    *      SURFACE_STATE           BC*, ETC*, EAC*    4           4
    *                              FXT1               8           4
    *                              all others         (set by SURFACE_STATE)"
    *
    * From the Ivy Bridge PRM, volume 4 part 1, page 63:
    *
    *     "- This field (Surface Vertical Aligment) is intended to be set to
    *        VALIGN_4 if the surface was rendered as a depth buffer, for a
    *        multisampled (4x) render target, or for a multisampled (8x)
    *        render target, since these surfaces support only alignment of 4.
    *      - Use of VALIGN_4 for other surfaces is supported, but uses more
    *        memory.
    *      - This field must be set to VALIGN_4 for all tiled Y Render Target
    *        surfaces.
    *      - Value of 1 is not supported for format YCRCB_NORMAL (0x182),
    *        YCRCB_SWAPUVY (0x183), YCRCB_SWAPUV (0x18f), YCRCB_SWAPY (0x190)
    *      - If Number of Multisamples is not MULTISAMPLECOUNT_1, this field
    *        must be set to VALIGN_4."
    *      - VALIGN_4 is not supported for surface format R32G32B32_FLOAT."
    *
    *     "- This field (Surface Horizontal Aligment) is intended to be set to
    *        HALIGN_8 only if the surface was rendered as a depth buffer with
    *        Z16 format or a stencil buffer, since these surfaces support only
    *        alignment of 8.
    *      - Use of HALIGN_8 for other surfaces is supported, but uses more
    *        memory.
    *      - This field must be set to HALIGN_4 if the Surface Format is BC*.
    *      - This field must be set to HALIGN_8 if the Surface Format is
    *        FXT1."
    *
    * They can be rephrased as
    *
    *                                  align_i        align_j
    *  compressed formats              block width    block height
    *  PIPE_FORMAT_Z16_UNORM           8              4
    *  PIPE_FORMAT_S8_UINT             8              8
    *  other depth/stencil formats     4 or 8         4
    *  2x or 4x multisampled           4 or 8         4
    *  tiled Y                         4 or 8         4 (if rt)
    *  PIPE_FORMAT_R32G32B32_FLOAT     4 or 8         2
    *  others                          4 or 8         2 or 4
    */

   if (layout->compressed) {
      /* this happens to be the case */
      layout->align_i = layout->block_width;
      layout->align_j = layout->block_height;
   }
   else if (layout->has_depth || layout->has_stencil) {
      if (layout->dev->gen >= ILO_GEN(7)) {
         switch (layout->format) {
         case PIPE_FORMAT_Z16_UNORM:
            layout->align_i = 8;
            layout->align_j = 4;
            break;
         case PIPE_FORMAT_S8_UINT:
            layout->align_i = 8;
            layout->align_j = 8;
            break;
         default:
            layout->align_i = 4;
            layout->align_j = 4;
            break;
         }
      }
      else {
         switch (layout->format) {
         case PIPE_FORMAT_S8_UINT:
            layout->align_i = 4;
            layout->align_j = 2;
            break;
         default:
            layout->align_i = 4;
            layout->align_j = 4;
            break;
         }
      }
   }
   else {
      const bool valign_4 = (templ->nr_samples > 1) ||
         (layout->dev->gen >= ILO_GEN(7) &&
          layout->tiling == INTEL_TILING_Y &&
          (templ->bind & PIPE_BIND_RENDER_TARGET));

      if (valign_4)
         assert(layout->block_size != 12);

      layout->align_i = 4;
      layout->align_j = (valign_4) ? 4 : 2;
   }

   /*
    * the fact that align i and j are multiples of block width and height
    * respectively is what makes the size of the bo a multiple of the block
    * size, slices start at block boundaries, and many of the computations
    * work.
    */
   assert(layout->align_i % layout->block_width == 0);
   assert(layout->align_j % layout->block_height == 0);

   /* make sure align() works */
   assert(util_is_power_of_two(layout->align_i) &&
          util_is_power_of_two(layout->align_j));
   assert(util_is_power_of_two(layout->block_width) &&
          util_is_power_of_two(layout->block_height));
}

static void
tex_layout_init_levels(struct tex_layout *layout)
{
   const struct pipe_resource *templ = layout->templ;
   int last_level, lv;

   last_level = templ->last_level;

   /* need at least 2 levels to compute full qpitch */
   if (last_level == 0 && templ->array_size > 1 && layout->array_spacing_full)
      last_level++;

   /* compute mip level sizes */
   for (lv = 0; lv <= last_level; lv++) {
      int w, h, d;

      w = u_minify(templ->width0, lv);
      h = u_minify(templ->height0, lv);
      d = u_minify(templ->depth0, lv);

      /*
       * From the Sandy Bridge PRM, volume 1 part 1, page 114:
       *
       *     "The dimensions of the mip maps are first determined by applying
       *      the sizing algorithm presented in Non-Power-of-Two Mipmaps
       *      above. Then, if necessary, they are padded out to compression
       *      block boundaries."
       */
      w = align(w, layout->block_width);
      h = align(h, layout->block_height);

      /*
       * From the Sandy Bridge PRM, volume 1 part 1, page 111:
       *
       *     "If the surface is multisampled (4x), these values must be
       *      adjusted as follows before proceeding:
       *
       *        W_L = ceiling(W_L / 2) * 4
       *        H_L = ceiling(H_L / 2) * 4"
       *
       * From the Ivy Bridge PRM, volume 1 part 1, page 108:
       *
       *     "If the surface is multisampled and it is a depth or stencil
       *      surface or Multisampled Surface StorageFormat in SURFACE_STATE
       *      is MSFMT_DEPTH_STENCIL, W_L and H_L must be adjusted as follows
       *      before proceeding:
       *
       *        #samples  W_L =                    H_L =
       *        2         ceiling(W_L / 2) * 4     HL [no adjustment]
       *        4         ceiling(W_L / 2) * 4     ceiling(H_L / 2) * 4
       *        8         ceiling(W_L / 2) * 8     ceiling(H_L / 2) * 4
       *        16        ceiling(W_L / 2) * 8     ceiling(H_L / 2) * 8"
       *
       * For interleaved samples (4x), where pixels
       *
       *   (x, y  ) (x+1, y  )
       *   (x, y+1) (x+1, y+1)
       *
       * would be is occupied by
       *
       *   (x, y  , si0) (x+1, y  , si0) (x, y  , si1) (x+1, y  , si1)
       *   (x, y+1, si0) (x+1, y+1, si0) (x, y+1, si1) (x+1, y+1, si1)
       *   (x, y  , si2) (x+1, y  , si2) (x, y  , si3) (x+1, y  , si3)
       *   (x, y+1, si2) (x+1, y+1, si2) (x, y+1, si3) (x+1, y+1, si3)
       *
       * Thus the need to
       *
       *   w = align(w, 2) * 2;
       *   y = align(y, 2) * 2;
       */
      if (layout->interleaved) {
         switch (templ->nr_samples) {
         case 0:
         case 1:
            break;
         case 2:
            w = align(w, 2) * 2;
            break;
         case 4:
            w = align(w, 2) * 2;
            h = align(h, 2) * 2;
            break;
         case 8:
            w = align(w, 2) * 4;
            h = align(h, 2) * 2;
            break;
         case 16:
            w = align(w, 2) * 4;
            h = align(h, 2) * 4;
            break;
         default:
            assert(!"unsupported sample count");
            break;
         }
      }

      layout->levels[lv].w = w;
      layout->levels[lv].h = h;
      layout->levels[lv].d = d;
   }
}

static void
tex_layout_init_spacing(struct tex_layout *layout)
{
   const struct pipe_resource *templ = layout->templ;

   if (layout->dev->gen >= ILO_GEN(7)) {
      /*
       * It is not explicitly states, but render targets are expected to be
       * UMS/CMS (samples non-interleaved) and depth/stencil buffers are
       * expected to be IMS (samples interleaved).
       *
       * See "Multisampled Surface Storage Format" field of SURFACE_STATE.
       */
      if (layout->has_depth || layout->has_stencil) {
         layout->interleaved = true;

         /*
          * From the Ivy Bridge PRM, volume 1 part 1, page 111:
          *
          *     "note that the depth buffer and stencil buffer have an implied
          *      value of ARYSPC_FULL"
          */
         layout->array_spacing_full = true;
      }
      else {
         layout->interleaved = false;

         /*
          * From the Ivy Bridge PRM, volume 4 part 1, page 66:
          *
          *     "If Multisampled Surface Storage Format is MSFMT_MSS and
          *      Number of Multisamples is not MULTISAMPLECOUNT_1, this field
          *      (Surface Array Spacing) must be set to ARYSPC_LOD0."
          *
          * As multisampled resources are not mipmapped, we never use
          * ARYSPC_FULL for them.
          */
         if (templ->nr_samples > 1)
            assert(templ->last_level == 0);
         layout->array_spacing_full = (templ->last_level > 0);
      }
   }
   else {
      /* GEN6 supports only interleaved samples */
      layout->interleaved = true;

      /*
       * From the Sandy Bridge PRM, volume 1 part 1, page 115:
       *
       *     "The separate stencil buffer does not support mip mapping, thus
       *      the storage for LODs other than LOD 0 is not needed. The
       *      following QPitch equation applies only to the separate stencil
       *      buffer:
       *
       *        QPitch = h_0"
       *
       * GEN6 does not support compact spacing otherwise.
       */
      layout->array_spacing_full = (layout->format != PIPE_FORMAT_S8_UINT);
   }
}

static void
tex_layout_init_tiling(struct tex_layout *layout)
{
   const struct pipe_resource *templ = layout->templ;
   const enum pipe_format format = layout->format;
   const unsigned tile_none = 1 << INTEL_TILING_NONE;
   const unsigned tile_x = 1 << INTEL_TILING_X;
   const unsigned tile_y = 1 << INTEL_TILING_Y;
   unsigned valid_tilings = tile_none | tile_x | tile_y;

   /*
    * From the Sandy Bridge PRM, volume 1 part 2, page 32:
    *
    *     "Display/Overlay   Y-Major not supported.
    *                        X-Major required for Async Flips"
    */
   if (unlikely(templ->bind & PIPE_BIND_SCANOUT))
      valid_tilings &= tile_x;

   /*
    * From the Sandy Bridge PRM, volume 3 part 2, page 158:
    *
    *     "The cursor surface address must be 4K byte aligned. The cursor must
    *      be in linear memory, it cannot be tiled."
    */
   if (unlikely(templ->bind & (PIPE_BIND_CURSOR | PIPE_BIND_LINEAR)))
      valid_tilings &= tile_none;

   /*
    * From the Ivy Bridge PRM, volume 4 part 1, page 76:
    *
    *     "The MCS surface must be stored as Tile Y."
    */
   if (templ->bind & ILO_BIND_MCS)
      valid_tilings &= tile_y;

   /*
    * From the Sandy Bridge PRM, volume 2 part 1, page 318:
    *
    *     "[DevSNB+]: This field (Tiled Surface) must be set to TRUE. Linear
    *      Depth Buffer is not supported."
    *
    *     "The Depth Buffer, if tiled, must use Y-Major tiling."
    *
    * From the Sandy Bridge PRM, volume 1 part 2, page 22:
    *
    *     "W-Major Tile Format is used for separate stencil."
    *
    * Since the HW does not support W-tiled fencing, we have to do it in the
    * driver.
    */
   if (templ->bind & PIPE_BIND_DEPTH_STENCIL) {
      switch (format) {
      case PIPE_FORMAT_S8_UINT:
         valid_tilings &= tile_none;
         break;
      default:
         valid_tilings &= tile_y;
         break;
      }
   }

   if (templ->bind & PIPE_BIND_RENDER_TARGET) {
      /*
       * From the Sandy Bridge PRM, volume 1 part 2, page 32:
       *
       *     "NOTE: 128BPE Format Color buffer ( render target ) MUST be
       *      either TileX or Linear."
       */
      if (layout->block_size == 16)
         valid_tilings &= ~tile_y;

      /*
       * From the Ivy Bridge PRM, volume 4 part 1, page 63:
       *
       *     "This field (Surface Vertical Aligment) must be set to VALIGN_4
       *      for all tiled Y Render Target surfaces."
       *
       *     "VALIGN_4 is not supported for surface format R32G32B32_FLOAT."
       */
      if (layout->dev->gen >= ILO_GEN(7) && layout->block_size == 12)
         valid_tilings &= ~tile_y;
   }

   /* no conflicting binding flags */
   assert(valid_tilings);

   layout->valid_tilings = valid_tilings;

   if (templ->bind & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW)) {
      /*
       * heuristically set a minimum width/height for enabling tiling
       */
      if (templ->width0 < 64 && (valid_tilings & ~tile_x))
         valid_tilings &= ~tile_x;

      if ((templ->width0 < 32 || templ->height0 < 16) &&
          (templ->width0 < 16 || templ->height0 < 32) &&
          (valid_tilings & ~tile_y))
         valid_tilings &= ~tile_y;
   }
   else {
      /* force linear if we are not sure where the texture is bound to */
      if (valid_tilings & tile_none)
         valid_tilings &= tile_none;
   }

   /* prefer tiled over linear */
   if (valid_tilings & tile_y)
      layout->tiling = INTEL_TILING_Y;
   else if (valid_tilings & tile_x)
      layout->tiling = INTEL_TILING_X;
   else
      layout->tiling = INTEL_TILING_NONE;
}

static void
tex_layout_init_format(struct tex_layout *layout)
{
   const struct pipe_resource *templ = layout->templ;
   enum pipe_format format;

   switch (templ->format) {
   case PIPE_FORMAT_ETC1_RGB8:
      format = PIPE_FORMAT_R8G8B8X8_UNORM;
      break;
   case PIPE_FORMAT_Z24_UNORM_S8_UINT:
      if (layout->separate_stencil)
         format = PIPE_FORMAT_Z24X8_UNORM;
      else
         format = templ->format;
      break;
   case PIPE_FORMAT_Z32_FLOAT_S8X24_UINT:
      if (layout->separate_stencil)
         format = PIPE_FORMAT_Z32_FLOAT;
      else
         format = templ->format;
      break;
   default:
      format = templ->format;
      break;
   }

   layout->format = format;

   layout->block_width = util_format_get_blockwidth(format);
   layout->block_height = util_format_get_blockheight(format);
   layout->block_size = util_format_get_blocksize(format);
   layout->compressed = util_format_is_compressed(format);
}

static void
tex_layout_init_hiz(struct tex_layout *layout)
{
   const struct pipe_resource *templ = layout->templ;
   const struct util_format_description *desc;

   desc = util_format_description(templ->format);
   layout->has_depth = util_format_has_depth(desc);
   layout->has_stencil = util_format_has_stencil(desc);

   if (!layout->has_depth)
      return;

   layout->hiz = true;

   /* no point in having HiZ */
   if (templ->usage == PIPE_USAGE_STAGING)
      layout->hiz = false;

   if (layout->dev->gen == ILO_GEN(6)) {
      /*
       * From the Sandy Bridge PRM, volume 2 part 1, page 312:
       *
       *     "The hierarchical depth buffer does not support the LOD field, it
       *      is assumed by hardware to be zero. A separate hierarachical
       *      depth buffer is required for each LOD used, and the
       *      corresponding buffer's state delivered to hardware each time a
       *      new depth buffer state with modified LOD is delivered."
       *
       * But we have a stronger requirement.  Because of layer offsetting
       * (check out the callers of ilo_texture_get_slice_offset()), we already
       * have to require the texture to be non-mipmapped and non-array.
       */
      if (templ->last_level > 0 || templ->array_size > 1 || templ->depth0 > 1)
         layout->hiz = false;
   }

   if (ilo_debug & ILO_DEBUG_NOHIZ)
      layout->hiz = false;

   if (layout->has_stencil) {
      /*
       * From the Sandy Bridge PRM, volume 2 part 1, page 317:
       *
       *     "This field (Separate Stencil Buffer Enable) must be set to the
       *      same value (enabled or disabled) as Hierarchical Depth Buffer
       *      Enable."
       *
       * GEN7+ requires separate stencil buffers.
       */
      if (layout->dev->gen >= ILO_GEN(7))
         layout->separate_stencil = true;
      else
         layout->separate_stencil = layout->hiz;

      if (layout->separate_stencil)
         layout->has_stencil = false;
   }
}

static bool
tex_layout_init(struct tex_layout *layout,
                struct pipe_screen *screen,
                const struct pipe_resource *templ,
                struct ilo_texture_slice **slices)
{
   struct ilo_screen *is = ilo_screen(screen);

   memset(layout, 0, sizeof(*layout));

   layout->dev = &is->dev;
   layout->templ = templ;

   /* note that there are dependencies between these functions */
   tex_layout_init_hiz(layout);
   tex_layout_init_format(layout);
   tex_layout_init_tiling(layout);
   tex_layout_init_spacing(layout);
   tex_layout_init_levels(layout);
   tex_layout_init_alignments(layout);
   tex_layout_init_qpitch(layout);

   if (templ->flags & PIPE_RESOURCE_FLAG_MAP_PERSISTENT) {
      /* require on-the-fly tiling/untiling or format conversion */
      if (layout->separate_stencil ||
          layout->format == PIPE_FORMAT_S8_UINT ||
          layout->format != templ->format)
         return false;
   }

   if (slices) {
      int lv;

      for (lv = 0; lv <= templ->last_level; lv++)
         layout->levels[lv].slices = slices[lv];
   }

   return true;
}

static void
tex_layout_align(struct tex_layout *layout)
{
   int align_w = 1, align_h = 1, pad_h = 0;

   /*
    * From the Sandy Bridge PRM, volume 1 part 1, page 118:
    *
    *     "To determine the necessary padding on the bottom and right side of
    *      the surface, refer to the table in Section 7.18.3.4 for the i and j
    *      parameters for the surface format in use. The surface must then be
    *      extended to the next multiple of the alignment unit size in each
    *      dimension, and all texels contained in this extended surface must
    *      have valid GTT entries."
    *
    *     "For cube surfaces, an additional two rows of padding are required
    *      at the bottom of the surface. This must be ensured regardless of
    *      whether the surface is stored tiled or linear.  This is due to the
    *      potential rotation of cache line orientation from memory to cache."
    *
    *     "For compressed textures (BC* and FXT1 surface formats), padding at
    *      the bottom of the surface is to an even compressed row, which is
    *      equal to a multiple of 8 uncompressed texel rows. Thus, for padding
    *      purposes, these surfaces behave as if j = 8 only for surface
    *      padding purposes. The value of 4 for j still applies for mip level
    *      alignment and QPitch calculation."
    */
   if (layout->templ->bind & PIPE_BIND_SAMPLER_VIEW) {
      align_w = MAX2(align_w, layout->align_i);
      align_h = MAX2(align_h, layout->align_j);

      if (layout->templ->target == PIPE_TEXTURE_CUBE)
         pad_h += 2;

      if (layout->compressed)
         align_h = MAX2(align_h, layout->align_j * 2);
   }

   /*
    * From the Sandy Bridge PRM, volume 1 part 1, page 118:
    *
    *     "If the surface contains an odd number of rows of data, a final row
    *      below the surface must be allocated."
    */
   if (layout->templ->bind & PIPE_BIND_RENDER_TARGET)
      align_h = MAX2(align_h, 2);

   /*
    * Depth Buffer Clear/Resolve works in 8x4 sample blocks.  In
    * ilo_texture_can_enable_hiz(), we always return true for the first slice.
    * To avoid out-of-bound access, we have to pad.
    */
   if (layout->hiz) {
      align_w = MAX2(align_w, 8);
      align_h = MAX2(align_h, 4);
   }

   layout->width = align(layout->width, align_w);
   layout->height = align(layout->height + pad_h, align_h);
}

/**
 * Layout a 2D texture.
 */
static void
tex_layout_2d(struct tex_layout *layout)
{
   const struct pipe_resource *templ = layout->templ;
   unsigned int level_x, level_y, num_slices;
   int lv;

   level_x = 0;
   level_y = 0;
   for (lv = 0; lv <= templ->last_level; lv++) {
      const unsigned int level_w = layout->levels[lv].w;
      const unsigned int level_h = layout->levels[lv].h;
      int slice;

      /* set slice offsets */
      if (layout->levels[lv].slices) {
         for (slice = 0; slice < templ->array_size; slice++) {
            layout->levels[lv].slices[slice].x = level_x;
            /* slices are qpitch apart in Y-direction */
            layout->levels[lv].slices[slice].y =
               level_y + layout->qpitch * slice;
         }
      }

      /* extend the size of the monolithic bo to cover this mip level */
      if (layout->width < level_x + level_w)
         layout->width = level_x + level_w;
      if (layout->height < level_y + level_h)
         layout->height = level_y + level_h;

      /* MIPLAYOUT_BELOW */
      if (lv == 1)
         level_x += align(level_w, layout->align_i);
      else
         level_y += align(level_h, layout->align_j);
   }

   num_slices = templ->array_size;
   /* samples of the same index are stored in a slice */
   if (templ->nr_samples > 1 && !layout->interleaved)
      num_slices *= templ->nr_samples;

   /* we did not take slices into consideration in the computation above */
   layout->height += layout->qpitch * (num_slices - 1);

   tex_layout_align(layout);
}

/**
 * Layout a 3D texture.
 */
static void
tex_layout_3d(struct tex_layout *layout)
{
   const struct pipe_resource *templ = layout->templ;
   unsigned int level_y;
   int lv;

   level_y = 0;
   for (lv = 0; lv <= templ->last_level; lv++) {
      const unsigned int level_w = layout->levels[lv].w;
      const unsigned int level_h = layout->levels[lv].h;
      const unsigned int level_d = layout->levels[lv].d;
      const unsigned int slice_pitch = align(level_w, layout->align_i);
      const unsigned int slice_qpitch = align(level_h, layout->align_j);
      const unsigned int num_slices_per_row = 1 << lv;
      int slice;

      for (slice = 0; slice < level_d; slice += num_slices_per_row) {
         int i;

         /* set slice offsets */
         if (layout->levels[lv].slices) {
            for (i = 0; i < num_slices_per_row && slice + i < level_d; i++) {
               layout->levels[lv].slices[slice + i].x = slice_pitch * i;
               layout->levels[lv].slices[slice + i].y = level_y;
            }
         }

         /* move on to the next slice row */
         level_y += slice_qpitch;
      }

      /* rightmost slice */
      slice = MIN2(num_slices_per_row, level_d) - 1;

      /* extend the size of the monolithic bo to cover this slice */
      if (layout->width < slice_pitch * slice + level_w)
         layout->width = slice_pitch * slice + level_w;
      if (lv == templ->last_level)
         layout->height = (level_y - slice_qpitch) + level_h;
   }

   tex_layout_align(layout);
}

/* note that this may force the texture to be linear */
static bool
tex_layout_calculate_bo_size(struct tex_layout *layout)
{
   assert(layout->width % layout->block_width == 0);
   assert(layout->height % layout->block_height == 0);
   assert(layout->qpitch % layout->block_height == 0);

   layout->bo_stride =
      (layout->width / layout->block_width) * layout->block_size;
   layout->bo_height = layout->height / layout->block_height;

   while (true) {
      int w = layout->bo_stride, h = layout->bo_height;
      int align_w, align_h;

      /*
       * From the Haswell PRM, volume 5, page 163:
       *
       *     "For linear surfaces, additional padding of 64 bytes is required
       *      at the bottom of the surface. This is in addition to the padding
       *      required above."
       */
      if (layout->dev->gen >= ILO_GEN(7.5) &&
          (layout->templ->bind & PIPE_BIND_SAMPLER_VIEW) &&
          layout->tiling == INTEL_TILING_NONE) {
         layout->bo_height +=
            (64 + layout->bo_stride - 1) / layout->bo_stride;
      }

      /*
       * From the Sandy Bridge PRM, volume 4 part 1, page 81:
       *
       *     "- For linear render target surfaces, the pitch must be a
       *        multiple of the element size for non-YUV surface formats.
       *        Pitch must be a multiple of 2 * element size for YUV surface
       *        formats.
       *      - For other linear surfaces, the pitch can be any multiple of
       *        bytes.
       *      - For tiled surfaces, the pitch must be a multiple of the tile
       *        width."
       *
       * Different requirements may exist when the bo is used in different
       * places, but our alignments here should be good enough that we do not
       * need to check layout->templ->bind.
       */
      switch (layout->tiling) {
      case INTEL_TILING_X:
         align_w = 512;
         align_h = 8;
         break;
      case INTEL_TILING_Y:
         align_w = 128;
         align_h = 32;
         break;
      default:
         if (layout->format == PIPE_FORMAT_S8_UINT) {
            /*
             * From the Sandy Bridge PRM, volume 1 part 2, page 22:
             *
             *     "A 4KB tile is subdivided into 8-high by 8-wide array of
             *      Blocks for W-Major Tiles (W Tiles). Each Block is 8 rows by 8
             *      bytes."
             *
             * Since we asked for INTEL_TILING_NONE instead of the non-existent
             * INTEL_TILING_W, we want to align to W tiles here.
             */
            align_w = 64;
            align_h = 64;
         }
         else {
            /* some good enough values */
            align_w = 64;
            align_h = 2;
         }
         break;
      }

      w = align(w, align_w);
      h = align(h, align_h);

      /* make sure the bo is mappable */
      if (layout->tiling != INTEL_TILING_NONE) {
         /*
          * Usually only the first 256MB of the GTT is mappable.
          *
          * See also how intel_context::max_gtt_map_object_size is calculated.
          */
         const size_t mappable_gtt_size = 256 * 1024 * 1024;

         /*
          * Be conservative.  We may be able to switch from VALIGN_4 to
          * VALIGN_2 if the layout was Y-tiled, but let's keep it simple.
          */
         if (mappable_gtt_size / w / 4 < h) {
            if (layout->valid_tilings & (1 << INTEL_TILING_NONE)) {
               layout->tiling = INTEL_TILING_NONE;
               continue;
            }
            else {
               ilo_warn("cannot force texture to be linear\n");
            }
         }
      }

      layout->bo_stride = w;
      layout->bo_height = h;
      break;
   }

   return (layout->bo_height <= max_resource_size / layout->bo_stride);
}

static void
tex_layout_calculate_hiz_size(struct tex_layout *layout)
{
   const struct pipe_resource *templ = layout->templ;
   const int hz_align_j = 8;
   int hz_width, hz_height;

   if (!layout->hiz)
      return;

   /*
    * See the Sandy Bridge PRM, volume 2 part 1, page 312, and the Ivy Bridge
    * PRM, volume 2 part 1, page 312-313.
    *
    * It seems HiZ buffer is aligned to 8x8, with every two rows packed into a
    * memory row.
    */

   hz_width = align(layout->levels[0].w, 16);

   if (templ->target == PIPE_TEXTURE_3D) {
      unsigned lv;

      hz_height = 0;

      for (lv = 0; lv <= templ->last_level; lv++) {
         const unsigned h = align(layout->levels[lv].h, hz_align_j);
         hz_height += h * layout->levels[lv].d;
      }

      hz_height /= 2;
   }
   else {
      const unsigned h0 = align(layout->levels[0].h, hz_align_j);
      unsigned hz_qpitch = h0;

      if (layout->array_spacing_full) {
         const unsigned h1 = align(layout->levels[1].h, hz_align_j);
         const unsigned htail =
            ((layout->dev->gen >= ILO_GEN(7)) ? 12 : 11) * hz_align_j;

         hz_qpitch += h1 + htail;
      }

      hz_height = hz_qpitch * templ->array_size / 2;

      if (layout->dev->gen >= ILO_GEN(7))
         hz_height = align(hz_height, 8);
   }

   /* align to Y-tile */
   layout->hiz_stride = align(hz_width, 128);
   layout->hiz_height = align(hz_height, 32);
}
