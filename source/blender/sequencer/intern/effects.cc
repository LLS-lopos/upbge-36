/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 * SPDX-FileCopyrightText: 2003-2024 Blender Authors
 * SPDX-FileCopyrightText: 2005-2006 Peter Schlaile <peter [at] schlaile [dot] de>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "BLI_vector.hh"
#include "DNA_vec_types.h"
#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_map.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_path_utils.hh"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_task.hh"
#include "BLI_utildefines.h"

#include "DNA_packedFile_types.h"
#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"
#include "DNA_space_types.h"
#include "DNA_vfont_types.h"

#include "BKE_fcurve.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"
#include "IMB_interp.hh"
#include "IMB_metadata.hh"

#include "BLI_math_color_blend.h"

#include "RNA_prototypes.hh"

#include "RE_pipeline.h"

#include "SEQ_channels.hh"
#include "SEQ_effects.hh"
#include "SEQ_proxy.hh"
#include "SEQ_relations.hh"
#include "SEQ_render.hh"
#include "SEQ_time.hh"
#include "SEQ_utils.hh"

#include "BLF_api.hh"

#include "effects.hh"
#include "render.hh"

using namespace blender;

static SeqEffectHandle get_sequence_effect_impl(int seq_type);

/* -------------------------------------------------------------------- */
/* Sequencer font access.
 *
 * Text strips can access and use fonts from a background thread
 * (when depsgraph evaluation copies the scene, or when prefetch renders
 * frames with text strips in a background thread).
 *
 * To not interfere with what might be happening on the main thread, all
 * fonts used by the sequencer are made unique via #BLF_load_unique
 * #BLF_load_mem_unique, and there's a mutex to guard against
 * sequencer itself possibly using the fonts from several threads.
 */

struct SeqFontMap {
  /* File path -> font ID mapping for file-based fonts. */
  Map<std::string, int> path_to_file_font_id;
  /* Datablock name -> font ID mapping for memory (datablock) fonts. */
  Map<std::string, int> name_to_mem_font_id;

  /* Font access mutex. Recursive since it is locked from
   * text strip rendering, which can call into loading from within. */
  std::recursive_mutex mutex;
};

static SeqFontMap g_font_map;

void SEQ_fontmap_clear()
{
  for (const auto &item : g_font_map.path_to_file_font_id.items()) {
    BLF_unload_id(item.value);
  }
  g_font_map.path_to_file_font_id.clear();
  for (const auto &item : g_font_map.name_to_mem_font_id.items()) {
    BLF_unload_id(item.value);
  }
  g_font_map.name_to_mem_font_id.clear();
}

static int seq_load_font_file(const std::string &path)
{
  std::lock_guard lock(g_font_map.mutex);
  int fontid = g_font_map.path_to_file_font_id.add_or_modify(
      path,
      [&](int *fontid) {
        /* New path: load font. */
        *fontid = BLF_load_unique(path.c_str());
        return *fontid;
      },
      [&](int *fontid) {
        /* Path already in cache: add reference to already loaded font,
         * or load a new one in case that
         * font id was unloaded behind our backs. */
        if (*fontid >= 0) {
          if (BLF_is_loaded_id(*fontid)) {
            BLF_addref_id(*fontid);
          }
          else {
            *fontid = BLF_load_unique(path.c_str());
          }
        }
        return *fontid;
      });
  return fontid;
}

static int seq_load_font_mem(const std::string &name, const unsigned char *data, int data_size)
{
  std::lock_guard lock(g_font_map.mutex);
  int fontid = g_font_map.name_to_mem_font_id.add_or_modify(
      name,
      [&](int *fontid) {
        /* New name: load font. */
        *fontid = BLF_load_mem_unique(name.c_str(), data, data_size);
        return *fontid;
      },
      [&](int *fontid) {
        /* Name already in cache: add reference to already loaded font,
         * or (if we're on the main thread) load a new one in case that
         * font id was unloaded behind our backs. */
        if (*fontid >= 0) {
          if (BLF_is_loaded_id(*fontid)) {
            BLF_addref_id(*fontid);
          }
          else {
            *fontid = BLF_load_mem_unique(name.c_str(), data, data_size);
          }
        }
        return *fontid;
      });
  return fontid;
}

static void seq_unload_font(int fontid)
{
  std::lock_guard lock(g_font_map.mutex);
  bool unloaded = BLF_unload_id(fontid);
  /* If that was the last usage of the font and it got unloaded: remove
   * it from our maps. */
  if (unloaded) {
    g_font_map.path_to_file_font_id.remove_if([&](auto item) { return item.value == fontid; });
    g_font_map.name_to_mem_font_id.remove_if([&](auto item) { return item.value == fontid; });
  }
}

/* -------------------------------------------------------------------- */
/** \name Internal Utilities
 * \{ */

static void slice_get_byte_buffers(const SeqRenderData *context,
                                   const ImBuf *ibuf1,
                                   const ImBuf *ibuf2,
                                   const ImBuf *out,
                                   int start_line,
                                   uchar **rect1,
                                   uchar **rect2,
                                   uchar **rect_out)
{
  int offset = 4 * start_line * context->rectx;

  *rect1 = ibuf1->byte_buffer.data + offset;
  *rect_out = out->byte_buffer.data + offset;

  if (ibuf2) {
    *rect2 = ibuf2->byte_buffer.data + offset;
  }
}

static void slice_get_float_buffers(const SeqRenderData *context,
                                    const ImBuf *ibuf1,
                                    const ImBuf *ibuf2,
                                    const ImBuf *out,
                                    int start_line,
                                    float **rect1,
                                    float **rect2,
                                    float **rect_out)
{
  int offset = 4 * start_line * context->rectx;

  *rect1 = ibuf1->float_buffer.data + offset;
  *rect_out = out->float_buffer.data + offset;

  if (ibuf2) {
    *rect2 = ibuf2->float_buffer.data + offset;
  }
}

static float4 load_premul_pixel(const uchar *ptr)
{
  float4 res;
  straight_uchar_to_premul_float(res, ptr);
  return res;
}

static float4 load_premul_pixel(const float *ptr)
{
  return float4(ptr);
}

static void store_premul_pixel(const float4 &pix, uchar *dst)
{
  premul_float_to_straight_uchar(dst, pix);
}

static void store_premul_pixel(const float4 &pix, float *dst)
{
  *reinterpret_cast<float4 *>(dst) = pix;
}

static void store_opaque_black_pixel(uchar *dst)
{
  dst[0] = 0;
  dst[1] = 0;
  dst[2] = 0;
  dst[3] = 255;
}

static void store_opaque_black_pixel(float *dst)
{
  dst[0] = 0.0f;
  dst[1] = 0.0f;
  dst[2] = 0.0f;
  dst[3] = 1.0f;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Glow Effect
 * \{ */

static ImBuf *prepare_effect_imbufs(const SeqRenderData *context,
                                    ImBuf *ibuf1,
                                    ImBuf *ibuf2,
                                    bool uninitialized_pixels = true)
{
  ImBuf *out;
  Scene *scene = context->scene;
  int x = context->rectx;
  int y = context->recty;
  int base_flags = uninitialized_pixels ? IB_uninitialized_pixels : 0;

  if (!ibuf1 && !ibuf2) {
    /* hmmm, global float option ? */
    out = IMB_allocImBuf(x, y, 32, IB_rect | base_flags);
  }
  else if ((ibuf1 && ibuf1->float_buffer.data) || (ibuf2 && ibuf2->float_buffer.data)) {
    /* if any inputs are float, output is float too */
    out = IMB_allocImBuf(x, y, 32, IB_rectfloat | base_flags);
  }
  else {
    out = IMB_allocImBuf(x, y, 32, IB_rect | base_flags);
  }

  if (out->float_buffer.data) {
    if (ibuf1 && !ibuf1->float_buffer.data) {
      seq_imbuf_to_sequencer_space(scene, ibuf1, true);
    }

    if (ibuf2 && !ibuf2->float_buffer.data) {
      seq_imbuf_to_sequencer_space(scene, ibuf2, true);
    }

    IMB_colormanagement_assign_float_colorspace(out, scene->sequencer_colorspace_settings.name);
  }
  else {
    if (ibuf1 && !ibuf1->byte_buffer.data) {
      IMB_rect_from_float(ibuf1);
    }

    if (ibuf2 && !ibuf2->byte_buffer.data) {
      IMB_rect_from_float(ibuf2);
    }
  }

  /* If effect only affecting a single channel, forward input's metadata to the output. */
  if (ibuf1 != nullptr && ibuf1 == ibuf2) {
    IMB_metadata_copy(out, ibuf1);
  }

  return out;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Alpha Over Effect
 * \{ */

static void init_alpha_over_or_under(Sequence *seq)
{
  Sequence *seq1 = seq->seq1;
  Sequence *seq2 = seq->seq2;

  seq->seq2 = seq1;
  seq->seq1 = seq2;
}

static bool alpha_opaque(uchar alpha)
{
  return alpha == 255;
}

static bool alpha_opaque(float alpha)
{
  return alpha >= 1.0f;
}

/* dst = src1 over src2 (alpha from src1) */
template<typename T>
static void do_alphaover_effect(
    float fac, int width, int height, const T *src1, const T *src2, T *dst)
{
  if (fac <= 0.0f) {
    memcpy(dst, src2, sizeof(T) * 4 * width * height);
    return;
  }

  for (int pixel_idx = 0; pixel_idx < width * height; pixel_idx++) {
    if (src1[3] <= 0.0f) {
      /* Alpha of zero. No color addition will happen as the colors are pre-multiplied. */
      memcpy(dst, src2, sizeof(T) * 4);
    }
    else if (fac == 1.0f && alpha_opaque(src1[3])) {
      /* No change to `src1` as `fac == 1` and fully opaque. */
      memcpy(dst, src1, sizeof(T) * 4);
    }
    else {
      float4 col1 = load_premul_pixel(src1);
      float mfac = 1.0f - fac * col1.w;
      float4 col2 = load_premul_pixel(src2);
      float4 col = fac * col1 + mfac * col2;
      store_premul_pixel(col, dst);
    }
    src1 += 4;
    src2 += 4;
    dst += 4;
  }
}

static void do_alphaover_effect(const SeqRenderData *context,
                                Sequence * /*seq*/,
                                float /*timeline_frame*/,
                                float fac,
                                const ImBuf *ibuf1,
                                const ImBuf *ibuf2,
                                int start_line,
                                int total_lines,
                                ImBuf *out)
{
  if (out->float_buffer.data) {
    float *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_float_buffers(context, ibuf1, ibuf2, out, start_line, &rect1, &rect2, &rect_out);

    do_alphaover_effect(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
  else {
    uchar *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_byte_buffers(context, ibuf1, ibuf2, out, start_line, &rect1, &rect2, &rect_out);

    do_alphaover_effect(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Alpha Under Effect
 * \{ */

/* dst = src1 under src2 (alpha from src2) */
template<typename T>
static void do_alphaunder_effect(
    float fac, int width, int height, const T *src1, const T *src2, T *dst)
{
  if (fac <= 0.0f) {
    memcpy(dst, src2, sizeof(T) * 4 * width * height);
    return;
  }

  for (int pixel_idx = 0; pixel_idx < width * height; pixel_idx++) {
    if (src2[3] <= 0.0f && fac >= 1.0f) {
      memcpy(dst, src1, sizeof(T) * 4);
    }
    else if (alpha_opaque(src2[3])) {
      memcpy(dst, src2, sizeof(T) * 4);
    }
    else {
      float4 col2 = load_premul_pixel(src2);
      float mfac = fac * (1.0f - col2.w);
      float4 col1 = load_premul_pixel(src1);
      float4 col = mfac * col1 + col2;
      store_premul_pixel(col, dst);
    }
    src1 += 4;
    src2 += 4;
    dst += 4;
  }
}

static void do_alphaunder_effect(const SeqRenderData *context,
                                 Sequence * /*seq*/,
                                 float /*timeline_frame*/,
                                 float fac,
                                 const ImBuf *ibuf1,
                                 const ImBuf *ibuf2,
                                 int start_line,
                                 int total_lines,
                                 ImBuf *out)
{
  if (out->float_buffer.data) {
    float *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_float_buffers(context, ibuf1, ibuf2, out, start_line, &rect1, &rect2, &rect_out);

    do_alphaunder_effect(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
  else {
    uchar *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_byte_buffers(context, ibuf1, ibuf2, out, start_line, &rect1, &rect2, &rect_out);

    do_alphaunder_effect(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cross Effect
 * \{ */

static void do_cross_effect_byte(float fac, int x, int y, uchar *rect1, uchar *rect2, uchar *out)
{
  uchar *rt1 = rect1;
  uchar *rt2 = rect2;
  uchar *rt = out;

  int temp_fac = int(256.0f * fac);
  int temp_mfac = 256 - temp_fac;

  for (int i = 0; i < y; i++) {
    for (int j = 0; j < x; j++) {
      rt[0] = (temp_mfac * rt1[0] + temp_fac * rt2[0]) >> 8;
      rt[1] = (temp_mfac * rt1[1] + temp_fac * rt2[1]) >> 8;
      rt[2] = (temp_mfac * rt1[2] + temp_fac * rt2[2]) >> 8;
      rt[3] = (temp_mfac * rt1[3] + temp_fac * rt2[3]) >> 8;

      rt1 += 4;
      rt2 += 4;
      rt += 4;
    }
  }
}

static void do_cross_effect_float(float fac, int x, int y, float *rect1, float *rect2, float *out)
{
  float *rt1 = rect1;
  float *rt2 = rect2;
  float *rt = out;

  float mfac = 1.0f - fac;

  for (int i = 0; i < y; i++) {
    for (int j = 0; j < x; j++) {
      rt[0] = mfac * rt1[0] + fac * rt2[0];
      rt[1] = mfac * rt1[1] + fac * rt2[1];
      rt[2] = mfac * rt1[2] + fac * rt2[2];
      rt[3] = mfac * rt1[3] + fac * rt2[3];

      rt1 += 4;
      rt2 += 4;
      rt += 4;
    }
  }
}

static void do_cross_effect(const SeqRenderData *context,
                            Sequence * /*seq*/,
                            float /*timeline_frame*/,
                            float fac,
                            const ImBuf *ibuf1,
                            const ImBuf *ibuf2,
                            int start_line,
                            int total_lines,
                            ImBuf *out)
{
  if (out->float_buffer.data) {
    float *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_float_buffers(context, ibuf1, ibuf2, out, start_line, &rect1, &rect2, &rect_out);

    do_cross_effect_float(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
  else {
    uchar *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_byte_buffers(context, ibuf1, ibuf2, out, start_line, &rect1, &rect2, &rect_out);

    do_cross_effect_byte(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Gamma Cross
 * \{ */

/* One could argue that gamma cross should not be hardcoded to 2.0 gamma,
 * but instead either do proper input->linear conversion (often sRGB). Or
 * maybe not even that, but do interpolation in some perceptual color space
 * like OKLAB. But currently it is fixed to just 2.0 gamma. */

static float gammaCorrect(float c)
{
  if (UNLIKELY(c < 0)) {
    return -(c * c);
  }
  return c * c;
}

static float invGammaCorrect(float c)
{
  return sqrtf_signed(c);
}

template<typename T>
static void do_gammacross_effect(
    float fac, int width, int height, const T *src1, const T *src2, T *dst)
{
  float mfac = 1.0f - fac;

  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      float4 col1 = load_premul_pixel(src1);
      float4 col2 = load_premul_pixel(src2);
      float4 col;
      for (int c = 0; c < 4; ++c) {
        col[c] = gammaCorrect(mfac * invGammaCorrect(col1[c]) + fac * invGammaCorrect(col2[c]));
      }
      store_premul_pixel(col, dst);
      src1 += 4;
      src2 += 4;
      dst += 4;
    }
  }
}

static void do_gammacross_effect(const SeqRenderData *context,
                                 Sequence * /*seq*/,
                                 float /*timeline_frame*/,
                                 float fac,
                                 const ImBuf *ibuf1,
                                 const ImBuf *ibuf2,
                                 int start_line,
                                 int total_lines,
                                 ImBuf *out)
{
  if (out->float_buffer.data) {
    float *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_float_buffers(context, ibuf1, ibuf2, out, start_line, &rect1, &rect2, &rect_out);

    do_gammacross_effect(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
  else {
    uchar *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_byte_buffers(context, ibuf1, ibuf2, out, start_line, &rect1, &rect2, &rect_out);

    do_gammacross_effect(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Color Add Effect
 * \{ */

static void do_add_effect_byte(float fac, int x, int y, uchar *rect1, uchar *rect2, uchar *out)
{
  uchar *cp1 = rect1;
  uchar *cp2 = rect2;
  uchar *rt = out;

  int temp_fac = int(256.0f * fac);

  for (int i = 0; i < y; i++) {
    for (int j = 0; j < x; j++) {
      const int temp_fac2 = temp_fac * int(cp2[3]);
      rt[0] = min_ii(cp1[0] + ((temp_fac2 * cp2[0]) >> 16), 255);
      rt[1] = min_ii(cp1[1] + ((temp_fac2 * cp2[1]) >> 16), 255);
      rt[2] = min_ii(cp1[2] + ((temp_fac2 * cp2[2]) >> 16), 255);
      rt[3] = cp1[3];

      cp1 += 4;
      cp2 += 4;
      rt += 4;
    }
  }
}

static void do_add_effect_float(float fac, int x, int y, float *rect1, float *rect2, float *out)
{
  float *rt1 = rect1;
  float *rt2 = rect2;
  float *rt = out;

  for (int i = 0; i < y; i++) {
    for (int j = 0; j < x; j++) {
      const float temp_fac = (1.0f - (rt1[3] * (1.0f - fac))) * rt2[3];
      rt[0] = rt1[0] + temp_fac * rt2[0];
      rt[1] = rt1[1] + temp_fac * rt2[1];
      rt[2] = rt1[2] + temp_fac * rt2[2];
      rt[3] = rt1[3];

      rt1 += 4;
      rt2 += 4;
      rt += 4;
    }
  }
}

static void do_add_effect(const SeqRenderData *context,
                          Sequence * /*seq*/,
                          float /*timeline_frame*/,
                          float fac,
                          const ImBuf *ibuf1,
                          const ImBuf *ibuf2,
                          int start_line,
                          int total_lines,
                          ImBuf *out)
{
  if (out->float_buffer.data) {
    float *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_float_buffers(context, ibuf1, ibuf2, out, start_line, &rect1, &rect2, &rect_out);

    do_add_effect_float(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
  else {
    uchar *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_byte_buffers(context, ibuf1, ibuf2, out, start_line, &rect1, &rect2, &rect_out);

    do_add_effect_byte(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Color Subtract Effect
 * \{ */

static void do_sub_effect_byte(float fac, int x, int y, uchar *rect1, uchar *rect2, uchar *out)
{
  uchar *cp1 = rect1;
  uchar *cp2 = rect2;
  uchar *rt = out;

  int temp_fac = int(256.0f * fac);

  for (int i = 0; i < y; i++) {
    for (int j = 0; j < x; j++) {
      const int temp_fac2 = temp_fac * int(cp2[3]);
      rt[0] = max_ii(cp1[0] - ((temp_fac2 * cp2[0]) >> 16), 0);
      rt[1] = max_ii(cp1[1] - ((temp_fac2 * cp2[1]) >> 16), 0);
      rt[2] = max_ii(cp1[2] - ((temp_fac2 * cp2[2]) >> 16), 0);
      rt[3] = cp1[3];

      cp1 += 4;
      cp2 += 4;
      rt += 4;
    }
  }
}

static void do_sub_effect_float(float fac, int x, int y, float *rect1, float *rect2, float *out)
{
  float *rt1 = rect1;
  float *rt2 = rect2;
  float *rt = out;

  float mfac = 1.0f - fac;

  for (int i = 0; i < y; i++) {
    for (int j = 0; j < x; j++) {
      const float temp_fac = (1.0f - (rt1[3] * mfac)) * rt2[3];
      rt[0] = max_ff(rt1[0] - temp_fac * rt2[0], 0.0f);
      rt[1] = max_ff(rt1[1] - temp_fac * rt2[1], 0.0f);
      rt[2] = max_ff(rt1[2] - temp_fac * rt2[2], 0.0f);
      rt[3] = rt1[3];

      rt1 += 4;
      rt2 += 4;
      rt += 4;
    }
  }
}

static void do_sub_effect(const SeqRenderData *context,
                          Sequence * /*seq*/,
                          float /*timeline_frame*/,
                          float fac,
                          const ImBuf *ibuf1,
                          const ImBuf *ibuf2,
                          int start_line,
                          int total_lines,
                          ImBuf *out)
{
  if (out->float_buffer.data) {
    float *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_float_buffers(context, ibuf1, ibuf2, out, start_line, &rect1, &rect2, &rect_out);

    do_sub_effect_float(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
  else {
    uchar *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_byte_buffers(context, ibuf1, ibuf2, out, start_line, &rect1, &rect2, &rect_out);

    do_sub_effect_byte(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drop Effect
 * \{ */

/* Must be > 0 or add pre-copy, etc to the function. */
#define XOFF 8
#define YOFF 8

static void do_drop_effect_byte(float fac, int x, int y, uchar *rect2i, uchar *rect1i, uchar *outi)
{
  const int xoff = min_ii(XOFF, x);
  const int yoff = min_ii(YOFF, y);

  int temp_fac = int(70.0f * fac);

  uchar *rt2 = rect2i + yoff * 4 * x;
  uchar *rt1 = rect1i;
  uchar *out = outi;
  for (int i = 0; i < y - yoff; i++) {
    memcpy(out, rt1, sizeof(*out) * xoff * 4);
    rt1 += xoff * 4;
    out += xoff * 4;

    for (int j = xoff; j < x; j++) {
      int temp_fac2 = ((temp_fac * rt2[3]) >> 8);

      *(out++) = std::max(0, *rt1 - temp_fac2);
      rt1++;
      *(out++) = std::max(0, *rt1 - temp_fac2);
      rt1++;
      *(out++) = std::max(0, *rt1 - temp_fac2);
      rt1++;
      *(out++) = std::max(0, *rt1 - temp_fac2);
      rt1++;
      rt2 += 4;
    }
    rt2 += xoff * 4;
  }
  memcpy(out, rt1, sizeof(*out) * yoff * 4 * x);
}

static void do_drop_effect_float(
    float fac, int x, int y, float *rect2i, float *rect1i, float *outi)
{
  const int xoff = min_ii(XOFF, x);
  const int yoff = min_ii(YOFF, y);

  float temp_fac = 70.0f * fac;

  float *rt2 = rect2i + yoff * 4 * x;
  float *rt1 = rect1i;
  float *out = outi;
  for (int i = 0; i < y - yoff; i++) {
    memcpy(out, rt1, sizeof(*out) * xoff * 4);
    rt1 += xoff * 4;
    out += xoff * 4;

    for (int j = xoff; j < x; j++) {
      float temp_fac2 = temp_fac * rt2[3];

      *(out++) = std::max(0.0f, *rt1 - temp_fac2);
      rt1++;
      *(out++) = std::max(0.0f, *rt1 - temp_fac2);
      rt1++;
      *(out++) = std::max(0.0f, *rt1 - temp_fac2);
      rt1++;
      *(out++) = std::max(0.0f, *rt1 - temp_fac2);
      rt1++;
      rt2 += 4;
    }
    rt2 += xoff * 4;
  }
  memcpy(out, rt1, sizeof(*out) * yoff * 4 * x);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Multiply Effect
 * \{ */

static void do_mul_effect_byte(float fac, int x, int y, uchar *rect1, uchar *rect2, uchar *out)
{
  uchar *rt1 = rect1;
  uchar *rt2 = rect2;
  uchar *rt = out;

  int temp_fac = int(256.0f * fac);

  /* Formula:
   * `fac * (a * b) + (1 - fac) * a => fac * a * (b - 1) + axaux = c * px + py * s;` // + centx
   * `yaux = -s * px + c * py;` // + centy */

  for (int i = 0; i < y; i++) {
    for (int j = 0; j < x; j++) {
      rt[0] = rt1[0] + ((temp_fac * rt1[0] * (rt2[0] - 255)) >> 16);
      rt[1] = rt1[1] + ((temp_fac * rt1[1] * (rt2[1] - 255)) >> 16);
      rt[2] = rt1[2] + ((temp_fac * rt1[2] * (rt2[2] - 255)) >> 16);
      rt[3] = rt1[3] + ((temp_fac * rt1[3] * (rt2[3] - 255)) >> 16);

      rt1 += 4;
      rt2 += 4;
      rt += 4;
    }
  }
}

static void do_mul_effect_float(float fac, int x, int y, float *rect1, float *rect2, float *out)
{
  float *rt1 = rect1;
  float *rt2 = rect2;
  float *rt = out;

  /* Formula:
   * `fac * (a * b) + (1 - fac) * a => fac * a * (b - 1) + a`. */

  for (int i = 0; i < y; i++) {
    for (int j = 0; j < x; j++) {
      rt[0] = rt1[0] + fac * rt1[0] * (rt2[0] - 1.0f);
      rt[1] = rt1[1] + fac * rt1[1] * (rt2[1] - 1.0f);
      rt[2] = rt1[2] + fac * rt1[2] * (rt2[2] - 1.0f);
      rt[3] = rt1[3] + fac * rt1[3] * (rt2[3] - 1.0f);

      rt1 += 4;
      rt2 += 4;
      rt += 4;
    }
  }
}

static void do_mul_effect(const SeqRenderData *context,
                          Sequence * /*seq*/,
                          float /*timeline_frame*/,
                          float fac,
                          const ImBuf *ibuf1,
                          const ImBuf *ibuf2,
                          int start_line,
                          int total_lines,
                          ImBuf *out)
{
  if (out->float_buffer.data) {
    float *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_float_buffers(context, ibuf1, ibuf2, out, start_line, &rect1, &rect2, &rect_out);

    do_mul_effect_float(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
  else {
    uchar *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_byte_buffers(context, ibuf1, ibuf2, out, start_line, &rect1, &rect2, &rect_out);

    do_mul_effect_byte(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blend Mode Effect
 * \{ */

/* blend_function has to be: void (T* dst, const T *src1, const T *src2) */
template<typename T, typename Func>
static void apply_blend_function(
    float fac, int width, int height, const T *src1, T *src2, T *dst, Func blend_function)
{
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      T achannel = src2[3];
      src2[3] = T(achannel * fac);
      blend_function(dst, src1, src2);
      src2[3] = achannel;
      dst[3] = src1[3];
      src1 += 4;
      src2 += 4;
      dst += 4;
    }
  }
}

static void do_blend_effect_float(
    float fac, int x, int y, const float *rect1, float *rect2, int btype, float *out)
{
  switch (btype) {
    case SEQ_TYPE_ADD:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_add_float);
      break;
    case SEQ_TYPE_SUB:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_sub_float);
      break;
    case SEQ_TYPE_MUL:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_mul_float);
      break;
    case SEQ_TYPE_DARKEN:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_darken_float);
      break;
    case SEQ_TYPE_COLOR_BURN:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_burn_float);
      break;
    case SEQ_TYPE_LINEAR_BURN:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_linearburn_float);
      break;
    case SEQ_TYPE_SCREEN:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_screen_float);
      break;
    case SEQ_TYPE_LIGHTEN:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_lighten_float);
      break;
    case SEQ_TYPE_DODGE:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_dodge_float);
      break;
    case SEQ_TYPE_OVERLAY:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_overlay_float);
      break;
    case SEQ_TYPE_SOFT_LIGHT:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_softlight_float);
      break;
    case SEQ_TYPE_HARD_LIGHT:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_hardlight_float);
      break;
    case SEQ_TYPE_PIN_LIGHT:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_pinlight_float);
      break;
    case SEQ_TYPE_LIN_LIGHT:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_linearlight_float);
      break;
    case SEQ_TYPE_VIVID_LIGHT:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_vividlight_float);
      break;
    case SEQ_TYPE_BLEND_COLOR:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_color_float);
      break;
    case SEQ_TYPE_HUE:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_hue_float);
      break;
    case SEQ_TYPE_SATURATION:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_saturation_float);
      break;
    case SEQ_TYPE_VALUE:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_luminosity_float);
      break;
    case SEQ_TYPE_DIFFERENCE:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_difference_float);
      break;
    case SEQ_TYPE_EXCLUSION:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_exclusion_float);
      break;
    default:
      break;
  }
}

static void do_blend_effect_byte(
    float fac, int x, int y, const uchar *rect1, uchar *rect2, int btype, uchar *out)
{
  switch (btype) {
    case SEQ_TYPE_ADD:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_add_byte);
      break;
    case SEQ_TYPE_SUB:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_sub_byte);
      break;
    case SEQ_TYPE_MUL:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_mul_byte);
      break;
    case SEQ_TYPE_DARKEN:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_darken_byte);
      break;
    case SEQ_TYPE_COLOR_BURN:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_burn_byte);
      break;
    case SEQ_TYPE_LINEAR_BURN:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_linearburn_byte);
      break;
    case SEQ_TYPE_SCREEN:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_screen_byte);
      break;
    case SEQ_TYPE_LIGHTEN:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_lighten_byte);
      break;
    case SEQ_TYPE_DODGE:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_dodge_byte);
      break;
    case SEQ_TYPE_OVERLAY:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_overlay_byte);
      break;
    case SEQ_TYPE_SOFT_LIGHT:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_softlight_byte);
      break;
    case SEQ_TYPE_HARD_LIGHT:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_hardlight_byte);
      break;
    case SEQ_TYPE_PIN_LIGHT:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_pinlight_byte);
      break;
    case SEQ_TYPE_LIN_LIGHT:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_linearlight_byte);
      break;
    case SEQ_TYPE_VIVID_LIGHT:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_vividlight_byte);
      break;
    case SEQ_TYPE_BLEND_COLOR:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_color_byte);
      break;
    case SEQ_TYPE_HUE:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_hue_byte);
      break;
    case SEQ_TYPE_SATURATION:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_saturation_byte);
      break;
    case SEQ_TYPE_VALUE:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_luminosity_byte);
      break;
    case SEQ_TYPE_DIFFERENCE:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_difference_byte);
      break;
    case SEQ_TYPE_EXCLUSION:
      apply_blend_function(fac, x, y, rect1, rect2, out, blend_color_exclusion_byte);
      break;
    default:
      break;
  }
}

static void do_blend_mode_effect(const SeqRenderData *context,
                                 Sequence *seq,
                                 float /*timeline_frame*/,
                                 float fac,
                                 const ImBuf *ibuf1,
                                 const ImBuf *ibuf2,
                                 int start_line,
                                 int total_lines,
                                 ImBuf *out)
{
  if (out->float_buffer.data) {
    float *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;
    slice_get_float_buffers(context, ibuf1, ibuf2, out, start_line, &rect1, &rect2, &rect_out);
    do_blend_effect_float(
        fac, context->rectx, total_lines, rect1, rect2, seq->blend_mode, rect_out);
  }
  else {
    uchar *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;
    slice_get_byte_buffers(context, ibuf1, ibuf2, out, start_line, &rect1, &rect2, &rect_out);
    do_blend_effect_byte(
        fac, context->rectx, total_lines, rect1, rect2, seq->blend_mode, rect_out);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Color Mix Effect
 * \{ */

static void init_colormix_effect(Sequence *seq)
{
  if (seq->effectdata) {
    MEM_freeN(seq->effectdata);
  }
  seq->effectdata = MEM_callocN(sizeof(ColorMixVars), "colormixvars");
  ColorMixVars *data = (ColorMixVars *)seq->effectdata;
  data->blend_effect = SEQ_TYPE_OVERLAY;
  data->factor = 1.0f;
}

static void do_colormix_effect(const SeqRenderData *context,
                               Sequence *seq,
                               float /*timeline_frame*/,
                               float /*fac*/,
                               const ImBuf *ibuf1,
                               const ImBuf *ibuf2,
                               int start_line,
                               int total_lines,
                               ImBuf *out)
{
  float fac;

  ColorMixVars *data = static_cast<ColorMixVars *>(seq->effectdata);
  fac = data->factor;

  if (out->float_buffer.data) {
    float *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;
    slice_get_float_buffers(context, ibuf1, ibuf2, out, start_line, &rect1, &rect2, &rect_out);
    do_blend_effect_float(
        fac, context->rectx, total_lines, rect1, rect2, data->blend_effect, rect_out);
  }
  else {
    uchar *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;
    slice_get_byte_buffers(context, ibuf1, ibuf2, out, start_line, &rect1, &rect2, &rect_out);
    do_blend_effect_byte(
        fac, context->rectx, total_lines, rect1, rect2, data->blend_effect, rect_out);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Wipe Effect
 * \{ */

struct WipeZone {
  float angle;
  int flip;
  int xo, yo;
  int width;
  float pythangle;
  float clockWidth;
  int type;
  bool forward;
};

static WipeZone precalc_wipe_zone(const WipeVars *wipe, int xo, int yo)
{
  WipeZone zone;
  zone.flip = (wipe->angle < 0.0f);
  zone.angle = tanf(fabsf(wipe->angle));
  zone.xo = xo;
  zone.yo = yo;
  zone.width = int(wipe->edgeWidth * ((xo + yo) / 2.0f));
  zone.pythangle = 1.0f / sqrtf(zone.angle * zone.angle + 1.0f);
  zone.clockWidth = wipe->edgeWidth * float(M_PI);
  zone.type = wipe->wipetype;
  zone.forward = wipe->forward != 0;
  return zone;
}

/**
 * This function calculates the blur band for the wipe effects.
 */
static float in_band(float width, float dist, int side, int dir)
{
  float alpha;

  if (width == 0) {
    return float(side);
  }

  if (width < dist) {
    return float(side);
  }

  if (side == 1) {
    alpha = (dist + 0.5f * width) / (width);
  }
  else {
    alpha = (0.5f * width - dist) / (width);
  }

  if (dir == 0) {
    alpha = 1 - alpha;
  }

  return alpha;
}

static float check_zone(const WipeZone *wipezone, int x, int y, float fac)
{
  float posx, posy, hyp, hyp2, angle, hwidth, b1, b2, b3, pointdist;
  float temp1, temp2, temp3, temp4; /* some placeholder variables */
  int xo = wipezone->xo;
  int yo = wipezone->yo;
  float halfx = xo * 0.5f;
  float halfy = yo * 0.5f;
  float widthf, output = 0;
  int width;

  if (wipezone->flip) {
    x = xo - x;
  }
  angle = wipezone->angle;

  if (wipezone->forward) {
    posx = fac * xo;
    posy = fac * yo;
  }
  else {
    posx = xo - fac * xo;
    posy = yo - fac * yo;
  }

  switch (wipezone->type) {
    case DO_SINGLE_WIPE:
      width = min_ii(wipezone->width, fac * yo);
      width = min_ii(width, yo - fac * yo);

      if (angle == 0.0f) {
        b1 = posy;
        b2 = y;
        hyp = fabsf(y - posy);
      }
      else {
        b1 = posy - (-angle) * posx;
        b2 = y - (-angle) * x;
        hyp = fabsf(angle * x + y + (-posy - angle * posx)) * wipezone->pythangle;
      }

      if (angle < 0) {
        temp1 = b1;
        b1 = b2;
        b2 = temp1;
      }

      if (wipezone->forward) {
        if (b1 < b2) {
          output = in_band(width, hyp, 1, 1);
        }
        else {
          output = in_band(width, hyp, 0, 1);
        }
      }
      else {
        if (b1 < b2) {
          output = in_band(width, hyp, 0, 1);
        }
        else {
          output = in_band(width, hyp, 1, 1);
        }
      }
      break;

    case DO_DOUBLE_WIPE:
      if (!wipezone->forward) {
        fac = 1.0f - fac; /* Go the other direction */
      }

      width = wipezone->width; /* calculate the blur width */
      hwidth = width * 0.5f;
      if (angle == 0) {
        b1 = posy * 0.5f;
        b3 = yo - posy * 0.5f;
        b2 = y;

        hyp = fabsf(y - posy * 0.5f);
        hyp2 = fabsf(y - (yo - posy * 0.5f));
      }
      else {
        b1 = posy * 0.5f - (-angle) * posx * 0.5f;
        b3 = (yo - posy * 0.5f) - (-angle) * (xo - posx * 0.5f);
        b2 = y - (-angle) * x;

        hyp = fabsf(angle * x + y + (-posy * 0.5f - angle * posx * 0.5f)) * wipezone->pythangle;
        hyp2 = fabsf(angle * x + y + (-(yo - posy * 0.5f) - angle * (xo - posx * 0.5f))) *
               wipezone->pythangle;
      }

      hwidth = min_ff(hwidth, fabsf(b3 - b1) / 2.0f);

      if (b2 < b1 && b2 < b3) {
        output = in_band(hwidth, hyp, 0, 1);
      }
      else if (b2 > b1 && b2 > b3) {
        output = in_band(hwidth, hyp2, 0, 1);
      }
      else {
        if (hyp < hwidth && hyp2 > hwidth) {
          output = in_band(hwidth, hyp, 1, 1);
        }
        else if (hyp > hwidth && hyp2 < hwidth) {
          output = in_band(hwidth, hyp2, 1, 1);
        }
        else {
          output = in_band(hwidth, hyp2, 1, 1) * in_band(hwidth, hyp, 1, 1);
        }
      }
      if (!wipezone->forward) {
        output = 1 - output;
      }
      break;
    case DO_CLOCK_WIPE:
      /*
       * temp1: angle of effect center in rads
       * temp2: angle of line through (halfx, halfy) and (x, y) in rads
       * temp3: angle of low side of blur
       * temp4: angle of high side of blur
       */
      output = 1.0f - fac;
      widthf = wipezone->clockWidth;
      temp1 = 2.0f * float(M_PI) * fac;

      if (wipezone->forward) {
        temp1 = 2.0f * float(M_PI) - temp1;
      }

      x = x - halfx;
      y = y - halfy;

      temp2 = atan2f(y, x);
      if (temp2 < 0.0f) {
        temp2 += 2.0f * float(M_PI);
      }

      if (wipezone->forward) {
        temp3 = temp1 - widthf * fac;
        temp4 = temp1 + widthf * (1 - fac);
      }
      else {
        temp3 = temp1 - widthf * (1 - fac);
        temp4 = temp1 + widthf * fac;
      }
      if (temp3 < 0) {
        temp3 = 0;
      }
      if (temp4 > 2.0f * float(M_PI)) {
        temp4 = 2.0f * float(M_PI);
      }

      if (temp2 < temp3) {
        output = 0;
      }
      else if (temp2 > temp4) {
        output = 1;
      }
      else {
        output = (temp2 - temp3) / (temp4 - temp3);
      }
      if (x == 0 && y == 0) {
        output = 1;
      }
      if (output != output) {
        output = 1;
      }
      if (wipezone->forward) {
        output = 1 - output;
      }
      break;
    case DO_IRIS_WIPE:
      if (xo > yo) {
        yo = xo;
      }
      else {
        xo = yo;
      }

      if (!wipezone->forward) {
        fac = 1 - fac;
      }

      width = wipezone->width;
      hwidth = width * 0.5f;

      temp1 = (halfx - (halfx)*fac);
      pointdist = hypotf(temp1, temp1);

      temp2 = hypotf(halfx - x, halfy - y);
      if (temp2 > pointdist) {
        output = in_band(hwidth, fabsf(temp2 - pointdist), 0, 1);
      }
      else {
        output = in_band(hwidth, fabsf(temp2 - pointdist), 1, 1);
      }

      if (!wipezone->forward) {
        output = 1 - output;
      }

      break;
  }
  if (output < 0) {
    output = 0;
  }
  else if (output > 1) {
    output = 1;
  }
  return output;
}

static void init_wipe_effect(Sequence *seq)
{
  if (seq->effectdata) {
    MEM_freeN(seq->effectdata);
  }

  seq->effectdata = MEM_callocN(sizeof(WipeVars), "wipevars");
}

static int num_inputs_wipe()
{
  return 2;
}

static void free_wipe_effect(Sequence *seq, const bool /*do_id_user*/)
{
  MEM_SAFE_FREE(seq->effectdata);
}

static void copy_wipe_effect(Sequence *dst, const Sequence *src, const int /*flag*/)
{
  dst->effectdata = MEM_dupallocN(src->effectdata);
}

template<typename T>
static void do_wipe_effect(
    const Sequence *seq, float fac, int width, int height, const T *rect1, const T *rect2, T *out)
{
  using namespace blender;
  const WipeVars *wipe = (const WipeVars *)seq->effectdata;
  const WipeZone wipezone = precalc_wipe_zone(wipe, width, height);

  threading::parallel_for(IndexRange(height), 64, [&](const IndexRange y_range) {
    const T *cp1 = rect1 ? rect1 + y_range.first() * width * 4 : nullptr;
    const T *cp2 = rect2 ? rect2 + y_range.first() * width * 4 : nullptr;
    T *rt = out + y_range.first() * width * 4;
    for (const int y : y_range) {
      for (int x = 0; x < width; x++) {
        float check = check_zone(&wipezone, x, y, fac);
        if (check) {
          if (cp1) {
            float4 col1 = load_premul_pixel(cp1);
            float4 col2 = load_premul_pixel(cp2);
            float4 col = col1 * check + col2 * (1.0f - check);
            store_premul_pixel(col, rt);
          }
          else {
            store_opaque_black_pixel(rt);
          }
        }
        else {
          if (cp2) {
            memcpy(rt, cp2, sizeof(T) * 4);
          }
          else {
            store_opaque_black_pixel(rt);
          }
        }

        rt += 4;
        if (cp1 != nullptr) {
          cp1 += 4;
        }
        if (cp2 != nullptr) {
          cp2 += 4;
        }
      }
    }
  });
}

static ImBuf *do_wipe_effect(const SeqRenderData *context,
                             Sequence *seq,
                             float /*timeline_frame*/,
                             float fac,
                             ImBuf *ibuf1,
                             ImBuf *ibuf2)
{
  ImBuf *out = prepare_effect_imbufs(context, ibuf1, ibuf2);

  if (out->float_buffer.data) {
    do_wipe_effect(seq,
                   fac,
                   context->rectx,
                   context->recty,
                   ibuf1->float_buffer.data,
                   ibuf2->float_buffer.data,
                   out->float_buffer.data);
  }
  else {
    do_wipe_effect(seq,
                   fac,
                   context->rectx,
                   context->recty,
                   ibuf1->byte_buffer.data,
                   ibuf2->byte_buffer.data,
                   out->byte_buffer.data);
  }

  return out;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Transform Effect
 * \{ */

static void init_transform_effect(Sequence *seq)
{
  if (seq->effectdata) {
    MEM_freeN(seq->effectdata);
  }

  seq->effectdata = MEM_callocN(sizeof(TransformVars), "transformvars");

  TransformVars *transform = (TransformVars *)seq->effectdata;

  transform->ScalexIni = 1.0f;
  transform->ScaleyIni = 1.0f;

  transform->xIni = 0.0f;
  transform->yIni = 0.0f;

  transform->rotIni = 0.0f;

  transform->interpolation = 1;
  transform->percent = 1;
  transform->uniform_scale = 0;
}

static int num_inputs_transform()
{
  return 1;
}

static void free_transform_effect(Sequence *seq, const bool /*do_id_user*/)
{
  MEM_SAFE_FREE(seq->effectdata);
}

static void copy_transform_effect(Sequence *dst, const Sequence *src, const int /*flag*/)
{
  dst->effectdata = MEM_dupallocN(src->effectdata);
}

static void transform_image(int x,
                            int y,
                            int start_line,
                            int total_lines,
                            const ImBuf *ibuf,
                            ImBuf *out,
                            float scale_x,
                            float scale_y,
                            float translate_x,
                            float translate_y,
                            float rotate,
                            int interpolation)
{
  /* Rotate */
  float s = sinf(rotate);
  float c = cosf(rotate);

  float4 *dst_fl = reinterpret_cast<float4 *>(out->float_buffer.data);
  uchar4 *dst_ch = reinterpret_cast<uchar4 *>(out->byte_buffer.data);

  size_t offset = size_t(x) * start_line;
  for (int yi = start_line; yi < start_line + total_lines; yi++) {
    for (int xi = 0; xi < x; xi++) {
      /* Translate point. */
      float xt = xi - translate_x;
      float yt = yi - translate_y;

      /* Rotate point with center ref. */
      float xr = c * xt + s * yt;
      float yr = -s * xt + c * yt;

      /* Scale point with center ref. */
      xt = xr / scale_x;
      yt = yr / scale_y;

      /* Undo reference center point. */
      xt += (x / 2.0f);
      yt += (y / 2.0f);

      /* interpolate */
      switch (interpolation) {
        case 0:
          if (dst_fl) {
            dst_fl[offset] = imbuf::interpolate_nearest_border_fl(ibuf, xt, yt);
          }
          else {
            dst_ch[offset] = imbuf::interpolate_nearest_border_byte(ibuf, xt, yt);
          }
          break;
        case 1:
          if (dst_fl) {
            dst_fl[offset] = imbuf::interpolate_bilinear_border_fl(ibuf, xt, yt);
          }
          else {
            dst_ch[offset] = imbuf::interpolate_bilinear_border_byte(ibuf, xt, yt);
          }
          break;
        case 2:
          if (dst_fl) {
            dst_fl[offset] = imbuf::interpolate_cubic_bspline_fl(ibuf, xt, yt);
          }
          else {
            dst_ch[offset] = imbuf::interpolate_cubic_bspline_byte(ibuf, xt, yt);
          }
          break;
      }
      offset++;
    }
  }
}

static void do_transform_effect(const SeqRenderData *context,
                                Sequence *seq,
                                float /*timeline_frame*/,
                                float /*fac*/,
                                const ImBuf *ibuf1,
                                const ImBuf * /*ibuf2*/,
                                int start_line,
                                int total_lines,
                                ImBuf *out)
{
  TransformVars *transform = (TransformVars *)seq->effectdata;
  float scale_x, scale_y, translate_x, translate_y, rotate_radians;

  /* Scale */
  if (transform->uniform_scale) {
    scale_x = scale_y = transform->ScalexIni;
  }
  else {
    scale_x = transform->ScalexIni;
    scale_y = transform->ScaleyIni;
  }

  int x = context->rectx;
  int y = context->recty;

  /* Translate */
  if (!transform->percent) {
    /* Compensate text size for preview render size. */
    double proxy_size_comp = context->scene->r.size / 100.0;
    if (context->preview_render_size != SEQ_RENDER_SIZE_SCENE) {
      proxy_size_comp = SEQ_rendersize_to_scale_factor(context->preview_render_size);
    }

    translate_x = transform->xIni * proxy_size_comp + (x / 2.0f);
    translate_y = transform->yIni * proxy_size_comp + (y / 2.0f);
  }
  else {
    translate_x = x * (transform->xIni / 100.0f) + (x / 2.0f);
    translate_y = y * (transform->yIni / 100.0f) + (y / 2.0f);
  }

  /* Rotate */
  rotate_radians = DEG2RADF(transform->rotIni);

  transform_image(x,
                  y,
                  start_line,
                  total_lines,
                  ibuf1,
                  out,
                  scale_x,
                  scale_y,
                  translate_x,
                  translate_y,
                  rotate_radians,
                  transform->interpolation);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Glow Effect
 * \{ */

static void glow_blur_bitmap(
    const float4 *src, float4 *map, int width, int height, float blur, int quality)
{
  using namespace blender;

  /* If we're not really blurring, bail out */
  if (blur <= 0) {
    return;
  }

  /* If result would be no blurring, early out. */
  const int halfWidth = ((quality + 1) * blur);
  if (halfWidth == 0) {
    return;
  }

  Array<float4> temp(width * height);

  /* Initialize the gaussian filter.
   * TODO: use code from #RE_filter_value. */
  Array<float> filter(halfWidth * 2);
  const float k = -1.0f / (2.0f * float(M_PI) * blur * blur);
  float weight = 0;
  for (int ix = 0; ix < halfWidth; ix++) {
    weight = float(exp(k * (ix * ix)));
    filter[halfWidth - ix] = weight;
    filter[halfWidth + ix] = weight;
  }
  filter[0] = weight;
  /* Normalize the array */
  float fval = 0;
  for (int ix = 0; ix < halfWidth * 2; ix++) {
    fval += filter[ix];
  }
  for (int ix = 0; ix < halfWidth * 2; ix++) {
    filter[ix] /= fval;
  }

  /* Blur the rows: read map, write temp */
  threading::parallel_for(IndexRange(height), 32, [&](const IndexRange y_range) {
    for (const int y : y_range) {
      for (int x = 0; x < width; x++) {
        float4 curColor = float4(0.0f);
        int xmin = math::max(x - halfWidth, 0);
        int xmax = math::min(x + halfWidth, width);
        for (int nx = xmin, index = (xmin - x) + halfWidth; nx < xmax; nx++, index++) {
          curColor += map[nx + y * width] * filter[index];
        }
        temp[x + y * width] = curColor;
      }
    }
  });

  /* Blur the columns: read temp, write map */
  threading::parallel_for(IndexRange(width), 32, [&](const IndexRange x_range) {
    const float4 one = float4(1.0f);
    for (const int x : x_range) {
      for (int y = 0; y < height; y++) {
        float4 curColor = float4(0.0f);
        int ymin = math::max(y - halfWidth, 0);
        int ymax = math::min(y + halfWidth, height);
        for (int ny = ymin, index = (ymin - y) + halfWidth; ny < ymax; ny++, index++) {
          curColor += temp[x + ny * width] * filter[index];
        }
        if (src != nullptr) {
          curColor = math::min(one, src[x + y * width] + curColor);
        }
        map[x + y * width] = curColor;
      }
    }
  });
}

static void blur_isolate_highlights(const float4 *in,
                                    float4 *out,
                                    int width,
                                    int height,
                                    float threshold,
                                    float boost,
                                    float clamp)
{
  using namespace blender;
  threading::parallel_for(IndexRange(height), 64, [&](const IndexRange y_range) {
    const float4 clampv = float4(clamp);
    for (const int y : y_range) {
      int index = y * width;
      for (int x = 0; x < width; x++, index++) {

        /* Isolate the intensity */
        float intensity = (in[index].x + in[index].y + in[index].z - threshold);
        float4 val;
        if (intensity > 0) {
          val = math::min(clampv, in[index] * (boost * intensity));
        }
        else {
          val = float4(0.0f);
        }
        out[index] = val;
      }
    }
  });
}

static void init_glow_effect(Sequence *seq)
{
  if (seq->effectdata) {
    MEM_freeN(seq->effectdata);
  }

  seq->effectdata = MEM_callocN(sizeof(GlowVars), "glowvars");

  GlowVars *glow = (GlowVars *)seq->effectdata;
  glow->fMini = 0.25;
  glow->fClamp = 1.0;
  glow->fBoost = 0.5;
  glow->dDist = 3.0;
  glow->dQuality = 3;
  glow->bNoComp = 0;
}

static int num_inputs_glow()
{
  return 1;
}

static void free_glow_effect(Sequence *seq, const bool /*do_id_user*/)
{
  MEM_SAFE_FREE(seq->effectdata);
}

static void copy_glow_effect(Sequence *dst, const Sequence *src, const int /*flag*/)
{
  dst->effectdata = MEM_dupallocN(src->effectdata);
}

static void do_glow_effect_byte(Sequence *seq,
                                int render_size,
                                float fac,
                                int x,
                                int y,
                                uchar *rect1,
                                uchar * /*rect2*/,
                                uchar *out)
{
  using namespace blender;
  GlowVars *glow = (GlowVars *)seq->effectdata;

  Array<float4> inbuf(x * y);
  Array<float4> outbuf(x * y);

  using namespace blender;
  IMB_colormanagement_transform_from_byte_threaded(*inbuf.data(), rect1, x, y, 4, "sRGB", "sRGB");

  blur_isolate_highlights(
      inbuf.data(), outbuf.data(), x, y, glow->fMini * 3.0f, glow->fBoost * fac, glow->fClamp);
  glow_blur_bitmap(glow->bNoComp ? nullptr : inbuf.data(),
                   outbuf.data(),
                   x,
                   y,
                   glow->dDist * (render_size / 100.0f),
                   glow->dQuality);

  threading::parallel_for(IndexRange(y), 64, [&](const IndexRange y_range) {
    size_t offset = y_range.first() * x;
    IMB_buffer_byte_from_float(out + offset * 4,
                               *(outbuf.data() + offset),
                               4,
                               0.0f,
                               IB_PROFILE_SRGB,
                               IB_PROFILE_SRGB,
                               true,
                               x,
                               y_range.size(),
                               x,
                               x);
  });
}

static void do_glow_effect_float(Sequence *seq,
                                 int render_size,
                                 float fac,
                                 int x,
                                 int y,
                                 float *rect1,
                                 float * /*rect2*/,
                                 float *out)
{
  using namespace blender;
  float4 *outbuf = reinterpret_cast<float4 *>(out);
  float4 *inbuf = reinterpret_cast<float4 *>(rect1);
  GlowVars *glow = (GlowVars *)seq->effectdata;

  blur_isolate_highlights(
      inbuf, outbuf, x, y, glow->fMini * 3.0f, glow->fBoost * fac, glow->fClamp);
  glow_blur_bitmap(glow->bNoComp ? nullptr : inbuf,
                   outbuf,
                   x,
                   y,
                   glow->dDist * (render_size / 100.0f),
                   glow->dQuality);
}

static ImBuf *do_glow_effect(const SeqRenderData *context,
                             Sequence *seq,
                             float /*timeline_frame*/,
                             float fac,
                             ImBuf *ibuf1,
                             ImBuf *ibuf2)
{
  ImBuf *out = prepare_effect_imbufs(context, ibuf1, ibuf2);

  int render_size = 100 * context->rectx / context->scene->r.xsch;

  if (out->float_buffer.data) {
    do_glow_effect_float(seq,
                         render_size,
                         fac,
                         context->rectx,
                         context->recty,
                         ibuf1->float_buffer.data,
                         nullptr,
                         out->float_buffer.data);
  }
  else {
    do_glow_effect_byte(seq,
                        render_size,
                        fac,
                        context->rectx,
                        context->recty,
                        ibuf1->byte_buffer.data,
                        nullptr,
                        out->byte_buffer.data);
  }

  return out;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Solid Color Effect
 * \{ */

static void init_solid_color(Sequence *seq)
{
  if (seq->effectdata) {
    MEM_freeN(seq->effectdata);
  }

  seq->effectdata = MEM_callocN(sizeof(SolidColorVars), "solidcolor");

  SolidColorVars *cv = (SolidColorVars *)seq->effectdata;
  cv->col[0] = cv->col[1] = cv->col[2] = 0.5;
}

static int num_inputs_color()
{
  return 0;
}

static void free_solid_color(Sequence *seq, const bool /*do_id_user*/)
{
  MEM_SAFE_FREE(seq->effectdata);
}

static void copy_solid_color(Sequence *dst, const Sequence *src, const int /*flag*/)
{
  dst->effectdata = MEM_dupallocN(src->effectdata);
}

static StripEarlyOut early_out_color(const Sequence * /*seq*/, float /*fac*/)
{
  return StripEarlyOut::NoInput;
}

static ImBuf *do_solid_color(const SeqRenderData *context,
                             Sequence *seq,
                             float /*timeline_frame*/,
                             float /*fac*/,
                             ImBuf *ibuf1,
                             ImBuf *ibuf2)
{
  using namespace blender;
  ImBuf *out = prepare_effect_imbufs(context, ibuf1, ibuf2);

  SolidColorVars *cv = (SolidColorVars *)seq->effectdata;

  threading::parallel_for(IndexRange(out->y), 64, [&](const IndexRange y_range) {
    if (out->byte_buffer.data) {
      /* Byte image. */
      uchar color[4];
      rgb_float_to_uchar(color, cv->col);
      color[3] = 255;

      uchar *dst = out->byte_buffer.data + y_range.first() * out->x * 4;
      uchar *dst_end = dst + y_range.size() * out->x * 4;
      while (dst < dst_end) {
        memcpy(dst, color, sizeof(color));
        dst += 4;
      }
    }
    else {
      /* Float image. */
      float color[4];
      color[0] = cv->col[0];
      color[1] = cv->col[1];
      color[2] = cv->col[2];
      color[3] = 1.0f;

      float *dst = out->float_buffer.data + y_range.first() * out->x * 4;
      float *dst_end = dst + y_range.size() * out->x * 4;
      while (dst < dst_end) {
        memcpy(dst, color, sizeof(color));
        dst += 4;
      }
    }
  });

  out->planes = R_IMF_PLANES_RGB;

  return out;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Multi-Camera Effect
 * \{ */

/** No effect inputs for multi-camera, we use #give_ibuf_seq. */
static int num_inputs_multicam()
{
  return 0;
}

static StripEarlyOut early_out_multicam(const Sequence * /*seq*/, float /*fac*/)
{
  return StripEarlyOut::NoInput;
}

static ImBuf *do_multicam(const SeqRenderData *context,
                          Sequence *seq,
                          float timeline_frame,
                          float /*fac*/,
                          ImBuf * /*ibuf1*/,
                          ImBuf * /*ibuf2*/)
{
  ImBuf *out;
  Editing *ed;

  if (seq->multicam_source == 0 || seq->multicam_source >= seq->machine) {
    return nullptr;
  }

  ed = context->scene->ed;
  if (!ed) {
    return nullptr;
  }
  ListBase *seqbasep = SEQ_get_seqbase_by_seq(context->scene, seq);
  ListBase *channels = SEQ_get_channels_by_seq(&ed->seqbase, &ed->channels, seq);
  if (!seqbasep) {
    return nullptr;
  }

  out = seq_render_give_ibuf_seqbase(
      context, timeline_frame, seq->multicam_source, channels, seqbasep);

  return out;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Adjustment Effect
 * \{ */

/** No effect inputs for adjustment, we use #give_ibuf_seq. */
static int num_inputs_adjustment()
{
  return 0;
}

static StripEarlyOut early_out_adjustment(const Sequence * /*seq*/, float /*fac*/)
{
  return StripEarlyOut::NoInput;
}

static ImBuf *do_adjustment_impl(const SeqRenderData *context, Sequence *seq, float timeline_frame)
{
  Editing *ed;
  ImBuf *i = nullptr;

  ed = context->scene->ed;

  ListBase *seqbasep = SEQ_get_seqbase_by_seq(context->scene, seq);
  ListBase *channels = SEQ_get_channels_by_seq(&ed->seqbase, &ed->channels, seq);

  /* Clamp timeline_frame to strip range so it behaves as if it had "still frame" offset (last
   * frame is static after end of strip). This is how most strips behave. This way transition
   * effects that doesn't overlap or speed effect can't fail rendering outside of strip range. */
  timeline_frame = clamp_i(timeline_frame,
                           SEQ_time_left_handle_frame_get(context->scene, seq),
                           SEQ_time_right_handle_frame_get(context->scene, seq) - 1);

  if (seq->machine > 1) {
    i = seq_render_give_ibuf_seqbase(
        context, timeline_frame, seq->machine - 1, channels, seqbasep);
  }

  /* Found nothing? so let's work the way up the meta-strip stack, so
   * that it is possible to group a bunch of adjustment strips into
   * a meta-strip and have that work on everything below the meta-strip. */

  if (!i) {
    Sequence *meta;

    meta = SEQ_find_metastrip_by_sequence(&ed->seqbase, nullptr, seq);

    if (meta) {
      i = do_adjustment_impl(context, meta, timeline_frame);
    }
  }

  return i;
}

static ImBuf *do_adjustment(const SeqRenderData *context,
                            Sequence *seq,
                            float timeline_frame,
                            float /*fac*/,
                            ImBuf * /*ibuf1*/,
                            ImBuf * /*ibuf2*/)
{
  ImBuf *out;
  Editing *ed;

  ed = context->scene->ed;

  if (!ed) {
    return nullptr;
  }

  out = do_adjustment_impl(context, seq, timeline_frame);

  return out;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Speed Effect
 * \{ */

static void init_speed_effect(Sequence *seq)
{
  if (seq->effectdata) {
    MEM_freeN(seq->effectdata);
  }

  seq->effectdata = MEM_callocN(sizeof(SpeedControlVars), "speedcontrolvars");

  SpeedControlVars *v = (SpeedControlVars *)seq->effectdata;
  v->speed_control_type = SEQ_SPEED_STRETCH;
  v->speed_fader = 1.0f;
  v->speed_fader_length = 0.0f;
  v->speed_fader_frame_number = 0.0f;
}

static void load_speed_effect(Sequence *seq)
{
  SpeedControlVars *v = (SpeedControlVars *)seq->effectdata;
  v->frameMap = nullptr;
}

static int num_inputs_speed()
{
  return 1;
}

static void free_speed_effect(Sequence *seq, const bool /*do_id_user*/)
{
  SpeedControlVars *v = (SpeedControlVars *)seq->effectdata;
  if (v->frameMap) {
    MEM_freeN(v->frameMap);
  }
  MEM_SAFE_FREE(seq->effectdata);
}

static void copy_speed_effect(Sequence *dst, const Sequence *src, const int /*flag*/)
{
  SpeedControlVars *v;
  dst->effectdata = MEM_dupallocN(src->effectdata);
  v = (SpeedControlVars *)dst->effectdata;
  v->frameMap = nullptr;
}

static StripEarlyOut early_out_speed(const Sequence * /*seq*/, float /*fac*/)
{
  return StripEarlyOut::DoEffect;
}

static FCurve *seq_effect_speed_speed_factor_curve_get(Scene *scene, Sequence *seq)
{
  return id_data_find_fcurve(&scene->id, seq, &RNA_Sequence, "speed_factor", 0, nullptr);
}

void seq_effect_speed_rebuild_map(Scene *scene, Sequence *seq)
{
  const int effect_strip_length = SEQ_time_right_handle_frame_get(scene, seq) -
                                  SEQ_time_left_handle_frame_get(scene, seq);

  if ((seq->seq1 == nullptr) || (effect_strip_length < 1)) {
    return; /* Make COVERITY happy and check for (CID 598) input strip. */
  }

  const FCurve *fcu = seq_effect_speed_speed_factor_curve_get(scene, seq);
  if (fcu == nullptr) {
    return;
  }

  SpeedControlVars *v = (SpeedControlVars *)seq->effectdata;
  if (v->frameMap) {
    MEM_freeN(v->frameMap);
  }

  v->frameMap = static_cast<float *>(MEM_mallocN(sizeof(float) * effect_strip_length, __func__));
  v->frameMap[0] = 0.0f;

  float target_frame = 0;
  for (int frame_index = 1; frame_index < effect_strip_length; frame_index++) {
    target_frame += evaluate_fcurve(fcu, SEQ_time_left_handle_frame_get(scene, seq) + frame_index);
    const int target_frame_max = SEQ_time_strip_length_get(scene, seq->seq1);
    CLAMP(target_frame, 0, target_frame_max);
    v->frameMap[frame_index] = target_frame;
  }
}

static void seq_effect_speed_frame_map_ensure(Scene *scene, Sequence *seq)
{
  const SpeedControlVars *v = (SpeedControlVars *)seq->effectdata;
  if (v->frameMap != nullptr) {
    return;
  }

  seq_effect_speed_rebuild_map(scene, seq);
}

float seq_speed_effect_target_frame_get(Scene *scene,
                                        Sequence *seq_speed,
                                        float timeline_frame,
                                        int input)
{
  if (seq_speed->seq1 == nullptr) {
    return 0.0f;
  }

  SEQ_effect_handle_get(seq_speed); /* Ensure, that data are initialized. */
  int frame_index = round_fl_to_int(SEQ_give_frame_index(scene, seq_speed, timeline_frame));
  SpeedControlVars *s = (SpeedControlVars *)seq_speed->effectdata;
  const Sequence *source = seq_speed->seq1;

  float target_frame = 0.0f;
  switch (s->speed_control_type) {
    case SEQ_SPEED_STRETCH: {
      /* Only right handle controls effect speed! */
      const float target_content_length = SEQ_time_strip_length_get(scene, source) -
                                          source->startofs;
      const float speed_effetct_length = SEQ_time_right_handle_frame_get(scene, seq_speed) -
                                         SEQ_time_left_handle_frame_get(scene, seq_speed);
      const float ratio = frame_index / speed_effetct_length;
      target_frame = target_content_length * ratio;
      break;
    }
    case SEQ_SPEED_MULTIPLY: {
      const FCurve *fcu = seq_effect_speed_speed_factor_curve_get(scene, seq_speed);
      if (fcu != nullptr) {
        seq_effect_speed_frame_map_ensure(scene, seq_speed);
        target_frame = s->frameMap[frame_index];
      }
      else {
        target_frame = frame_index * s->speed_fader;
      }
      break;
    }
    case SEQ_SPEED_LENGTH:
      target_frame = SEQ_time_strip_length_get(scene, source) * (s->speed_fader_length / 100.0f);
      break;
    case SEQ_SPEED_FRAME_NUMBER:
      target_frame = s->speed_fader_frame_number;
      break;
  }

  CLAMP(target_frame, 0, SEQ_time_strip_length_get(scene, source));
  target_frame += seq_speed->start;

  /* No interpolation. */
  if ((s->flags & SEQ_SPEED_USE_INTERPOLATION) == 0) {
    return target_frame;
  }

  /* Interpolation is used, switch between current and next frame based on which input is
   * requested. */
  return input == 0 ? target_frame : ceil(target_frame);
}

static float speed_effect_interpolation_ratio_get(Scene *scene,
                                                  Sequence *seq_speed,
                                                  float timeline_frame)
{
  const float target_frame = seq_speed_effect_target_frame_get(
      scene, seq_speed, timeline_frame, 0);
  return target_frame - floor(target_frame);
}

static ImBuf *do_speed_effect(const SeqRenderData *context,
                              Sequence *seq,
                              float timeline_frame,
                              float fac,
                              ImBuf *ibuf1,
                              ImBuf *ibuf2)
{
  const SpeedControlVars *s = (SpeedControlVars *)seq->effectdata;
  SeqEffectHandle cross_effect = get_sequence_effect_impl(SEQ_TYPE_CROSS);
  ImBuf *out;

  if (s->flags & SEQ_SPEED_USE_INTERPOLATION) {
    fac = speed_effect_interpolation_ratio_get(context->scene, seq, timeline_frame);
    /* Current frame is ibuf1, next frame is ibuf2. */
    out = seq_render_effect_execute_threaded(
        &cross_effect, context, nullptr, timeline_frame, fac, ibuf1, ibuf2);
    return out;
  }

  /* No interpolation. */
  return IMB_dupImBuf(ibuf1);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Over-Drop Effect
 * \{ */

static void do_overdrop_effect(const SeqRenderData *context,
                               Sequence * /*seq*/,
                               float /*timeline_frame*/,
                               float fac,
                               const ImBuf *ibuf1,
                               const ImBuf *ibuf2,
                               int start_line,
                               int total_lines,
                               ImBuf *out)
{
  int x = context->rectx;
  int y = total_lines;

  if (out->float_buffer.data) {
    float *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_float_buffers(context, ibuf1, ibuf2, out, start_line, &rect1, &rect2, &rect_out);

    do_drop_effect_float(fac, x, y, rect1, rect2, rect_out);
    do_alphaover_effect(fac, x, y, rect1, rect2, rect_out);
  }
  else {
    uchar *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_byte_buffers(context, ibuf1, ibuf2, out, start_line, &rect1, &rect2, &rect_out);

    do_drop_effect_byte(fac, x, y, rect1, rect2, rect_out);
    do_alphaover_effect(fac, x, y, rect1, rect2, rect_out);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Gaussian Blur
 * \{ */

static void init_gaussian_blur_effect(Sequence *seq)
{
  if (seq->effectdata) {
    MEM_freeN(seq->effectdata);
  }

  seq->effectdata = MEM_callocN(sizeof(GaussianBlurVars), "gaussianblurvars");
}

static int num_inputs_gaussian_blur()
{
  return 1;
}

static void free_gaussian_blur_effect(Sequence *seq, const bool /*do_id_user*/)
{
  MEM_SAFE_FREE(seq->effectdata);
}

static void copy_gaussian_blur_effect(Sequence *dst, const Sequence *src, const int /*flag*/)
{
  dst->effectdata = MEM_dupallocN(src->effectdata);
}

static StripEarlyOut early_out_gaussian_blur(const Sequence *seq, float /*fac*/)
{
  GaussianBlurVars *data = static_cast<GaussianBlurVars *>(seq->effectdata);
  if (data->size_x == 0.0f && data->size_y == 0) {
    return StripEarlyOut::UseInput1;
  }
  return StripEarlyOut::DoEffect;
}

static Array<float> make_gaussian_blur_kernel(float rad, int size)
{
  int n = 2 * size + 1;
  Array<float> gaussian(n);

  float sum = 0.0f;
  float fac = (rad > 0.0f ? 1.0f / rad : 0.0f);
  for (int i = -size; i <= size; i++) {
    float val = RE_filter_value(R_FILTER_GAUSS, float(i) * fac);
    sum += val;
    gaussian[i + size] = val;
  }

  float inv_sum = 1.0f / sum;
  for (int i = 0; i < n; i++) {
    gaussian[i] *= inv_sum;
  }

  return gaussian;
}

template<typename T>
static void gaussian_blur_x(const Span<float> gaussian,
                            int half_size,
                            int start_line,
                            int width,
                            int height,
                            int /*frame_height*/,
                            const T *rect,
                            T *dst)
{
  dst += int64_t(start_line) * width * 4;
  for (int y = start_line; y < start_line + height; y++) {
    for (int x = 0; x < width; x++) {
      float4 accum(0.0f);
      float accum_weight = 0.0f;

      int xmin = math::max(x - half_size, 0);
      int xmax = math::min(x + half_size, width - 1);
      for (int nx = xmin, index = (xmin - x) + half_size; nx <= xmax; nx++, index++) {
        float weight = gaussian[index];
        int offset = (y * width + nx) * 4;
        accum += float4(rect + offset) * weight;
        accum_weight += weight;
      }
      accum *= (1.0f / accum_weight);
      if constexpr (math::is_math_float_type<T>) {
        dst[0] = accum[0];
        dst[1] = accum[1];
        dst[2] = accum[2];
        dst[3] = accum[3];
      }
      else {
        dst[0] = accum[0] + 0.5f;
        dst[1] = accum[1] + 0.5f;
        dst[2] = accum[2] + 0.5f;
        dst[3] = accum[3] + 0.5f;
      }
      dst += 4;
    }
  }
}

template<typename T>
static void gaussian_blur_y(const Span<float> gaussian,
                            int half_size,
                            int start_line,
                            int width,
                            int height,
                            int frame_height,
                            const T *rect,
                            T *dst)
{
  dst += int64_t(start_line) * width * 4;
  for (int y = start_line; y < start_line + height; y++) {
    for (int x = 0; x < width; x++) {
      float4 accum(0.0f);
      float accum_weight = 0.0f;
      int ymin = math::max(y - half_size, 0);
      int ymax = math::min(y + half_size, frame_height - 1);
      for (int ny = ymin, index = (ymin - y) + half_size; ny <= ymax; ny++, index++) {
        float weight = gaussian[index];
        int offset = (ny * width + x) * 4;
        accum += float4(rect + offset) * weight;
        accum_weight += weight;
      }
      accum *= (1.0f / accum_weight);
      if constexpr (math::is_math_float_type<T>) {
        dst[0] = accum[0];
        dst[1] = accum[1];
        dst[2] = accum[2];
        dst[3] = accum[3];
      }
      else {
        dst[0] = accum[0] + 0.5f;
        dst[1] = accum[1] + 0.5f;
        dst[2] = accum[2] + 0.5f;
        dst[3] = accum[3] + 0.5f;
      }
      dst += 4;
    }
  }
}

static ImBuf *do_gaussian_blur_effect(const SeqRenderData *context,
                                      Sequence *seq,
                                      float /*timeline_frame*/,
                                      float /*fac*/,
                                      ImBuf *ibuf1,
                                      ImBuf * /*ibuf2*/)
{
  using namespace blender;

  /* Create blur kernel weights. */
  const GaussianBlurVars *data = static_cast<const GaussianBlurVars *>(seq->effectdata);
  const int half_size_x = int(data->size_x + 0.5f);
  const int half_size_y = int(data->size_y + 0.5f);
  Array<float> gaussian_x = make_gaussian_blur_kernel(data->size_x, half_size_x);
  Array<float> gaussian_y = make_gaussian_blur_kernel(data->size_y, half_size_y);

  const int width = context->rectx;
  const int height = context->recty;
  const bool is_float = ibuf1->float_buffer.data;

  /* Horizontal blur: create output, blur ibuf1 into it. */
  ImBuf *out = prepare_effect_imbufs(context, ibuf1, nullptr);
  threading::parallel_for(IndexRange(context->recty), 32, [&](const IndexRange y_range) {
    const int y_first = y_range.first();
    const int y_size = y_range.size();
    if (is_float) {
      gaussian_blur_x(gaussian_x,
                      half_size_x,
                      y_first,
                      width,
                      y_size,
                      height,
                      ibuf1->float_buffer.data,
                      out->float_buffer.data);
    }
    else {
      gaussian_blur_x(gaussian_x,
                      half_size_x,
                      y_first,
                      width,
                      y_size,
                      height,
                      ibuf1->byte_buffer.data,
                      out->byte_buffer.data);
    }
  });

  /* Vertical blur: create output, blur previous output into it. */
  ibuf1 = out;
  out = prepare_effect_imbufs(context, ibuf1, nullptr);
  threading::parallel_for(IndexRange(context->recty), 32, [&](const IndexRange y_range) {
    const int y_first = y_range.first();
    const int y_size = y_range.size();
    if (is_float) {
      gaussian_blur_y(gaussian_y,
                      half_size_y,
                      y_first,
                      width,
                      y_size,
                      height,
                      ibuf1->float_buffer.data,
                      out->float_buffer.data);
    }
    else {
      gaussian_blur_y(gaussian_y,
                      half_size_y,
                      y_first,
                      width,
                      y_size,
                      height,
                      ibuf1->byte_buffer.data,
                      out->byte_buffer.data);
    }
  });

  /* Free the first output. */
  IMB_freeImBuf(ibuf1);

  return out;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Text Effect
 * \{ */

/* `data->text[0] == 0` is ignored on purpose in order to make it possible to edit  */
bool SEQ_effects_can_render_text(const Sequence *seq)
{
  TextVars *data = static_cast<TextVars *>(seq->effectdata);
  if (data->text_size < 1.0f ||
      ((data->color[3] == 0.0f) &&
       (data->shadow_color[3] == 0.0f || (data->flag & SEQ_TEXT_SHADOW) == 0) &&
       (data->outline_color[3] == 0.0f || data->outline_width <= 0.0f ||
        (data->flag & SEQ_TEXT_OUTLINE) == 0)))
  {
    return false;
  }
  return true;
}

static void init_text_effect(Sequence *seq)
{
  if (seq->effectdata) {
    MEM_freeN(seq->effectdata);
  }

  TextVars *data = static_cast<TextVars *>(
      seq->effectdata = MEM_callocN(sizeof(TextVars), "textvars"));
  data->text_font = nullptr;
  data->text_blf_id = -1;
  data->text_size = 60.0f;

  copy_v4_fl(data->color, 1.0f);
  data->shadow_color[3] = 0.7f;
  data->shadow_angle = DEG2RADF(65.0f);
  data->shadow_offset = 0.04f;
  data->shadow_blur = 0.0f;
  data->box_color[0] = 0.2f;
  data->box_color[1] = 0.2f;
  data->box_color[2] = 0.2f;
  data->box_color[3] = 0.7f;
  data->box_margin = 0.01f;
  data->box_roundness = 0.0f;
  data->outline_color[3] = 0.7f;
  data->outline_width = 0.05f;

  STRNCPY(data->text, "Text");

  data->loc[0] = 0.5f;
  data->loc[1] = 0.5f;
  data->anchor_x = SEQ_TEXT_ALIGN_X_CENTER;
  data->anchor_y = SEQ_TEXT_ALIGN_Y_CENTER;
  data->align = SEQ_TEXT_ALIGN_X_CENTER;
  data->wrap_width = 1.0f;
}

void SEQ_effect_text_font_unload(TextVars *data, const bool do_id_user)
{
  if (data == nullptr) {
    return;
  }

  /* Unlink the VFont */
  if (do_id_user && data->text_font != nullptr) {
    id_us_min(&data->text_font->id);
    data->text_font = nullptr;
  }

  /* Unload the font. */
  if (data->text_blf_id >= 0) {
    seq_unload_font(data->text_blf_id);
    data->text_blf_id = -1;
  }
}

void SEQ_effect_text_font_load(TextVars *data, const bool do_id_user)
{
  VFont *vfont = data->text_font;
  if (vfont == nullptr) {
    return;
  }

  if (do_id_user) {
    id_us_plus(&vfont->id);
  }

  if (vfont->packedfile != nullptr) {
    PackedFile *pf = vfont->packedfile;
    /* Create a name that's unique between library data-blocks to avoid loading
     * a font per strip which will load fonts many times.
     *
     * WARNING: this isn't fool proof!
     * The #VFont may be renamed which will cause this to load multiple times,
     * in practice this isn't so likely though. */
    char name[MAX_ID_FULL_NAME];
    BKE_id_full_name_get(name, &vfont->id, 0);

    data->text_blf_id = seq_load_font_mem(name, static_cast<const uchar *>(pf->data), pf->size);
  }
  else {
    char filepath[FILE_MAX];
    STRNCPY(filepath, vfont->filepath);

    BLI_path_abs(filepath, ID_BLEND_PATH_FROM_GLOBAL(&vfont->id));
    data->text_blf_id = seq_load_font_file(filepath);
  }
}

static void free_text_effect(Sequence *seq, const bool do_id_user)
{
  TextVars *data = static_cast<TextVars *>(seq->effectdata);
  SEQ_effect_text_font_unload(data, do_id_user);

  if (data) {
    MEM_delete(data->runtime);
    MEM_freeN(data);
    seq->effectdata = nullptr;
  }
}

static void load_text_effect(Sequence *seq)
{
  TextVars *data = static_cast<TextVars *>(seq->effectdata);
  SEQ_effect_text_font_load(data, false);
}

static void copy_text_effect(Sequence *dst, const Sequence *src, const int flag)
{
  dst->effectdata = MEM_dupallocN(src->effectdata);
  TextVars *data = static_cast<TextVars *>(dst->effectdata);

  data->runtime = nullptr;
  data->text_blf_id = -1;
  SEQ_effect_text_font_load(data, (flag & LIB_ID_CREATE_NO_USER_REFCOUNT) == 0);
}

static int num_inputs_text()
{
  return 0;
}

static StripEarlyOut early_out_text(const Sequence *seq, float /*fac*/)
{
  if (!SEQ_effects_can_render_text(seq)) {
    return StripEarlyOut::UseInput1;
  }
  return StripEarlyOut::NoInput;
}

/* Simplified version of gaussian blur specifically for text shadow blurring:
 * - Data is only the alpha channel,
 * - Skips blur outside of shadow rectangle. */
static void text_gaussian_blur_x(const Span<float> gaussian,
                                 int half_size,
                                 int start_line,
                                 int width,
                                 int height,
                                 const uchar *rect,
                                 uchar *dst,
                                 const rcti &shadow_rect)
{
  dst += int64_t(start_line) * width;
  for (int y = start_line; y < start_line + height; y++) {
    for (int x = 0; x < width; x++) {
      float accum(0.0f);
      if (x >= shadow_rect.xmin && x <= shadow_rect.xmax) {
        float accum_weight = 0.0f;
        int xmin = math::max(x - half_size, shadow_rect.xmin);
        int xmax = math::min(x + half_size, shadow_rect.xmax);
        for (int nx = xmin, index = (xmin - x) + half_size; nx <= xmax; nx++, index++) {
          float weight = gaussian[index];
          int offset = y * width + nx;
          accum += rect[offset] * weight;
          accum_weight += weight;
        }
        accum *= (1.0f / accum_weight);
      }

      *dst = accum;
      dst++;
    }
  }
}

static void text_gaussian_blur_y(const Span<float> gaussian,
                                 int half_size,
                                 int start_line,
                                 int width,
                                 int height,
                                 const uchar *rect,
                                 uchar *dst,
                                 const rcti &shadow_rect)
{
  dst += int64_t(start_line) * width;
  for (int y = start_line; y < start_line + height; y++) {
    for (int x = 0; x < width; x++) {
      float accum(0.0f);
      if (x >= shadow_rect.xmin && x <= shadow_rect.xmax) {
        float accum_weight = 0.0f;
        int ymin = math::max(y - half_size, shadow_rect.ymin);
        int ymax = math::min(y + half_size, shadow_rect.ymax);
        for (int ny = ymin, index = (ymin - y) + half_size; ny <= ymax; ny++, index++) {
          float weight = gaussian[index];
          int offset = ny * width + x;
          accum += rect[offset] * weight;
          accum_weight += weight;
        }
        accum *= (1.0f / accum_weight);
      }
      *dst = accum;
      dst++;
    }
  }
}

static void clamp_rect(int width, int height, rcti &r_rect)
{
  r_rect.xmin = math::clamp(r_rect.xmin, 0, width - 1);
  r_rect.xmax = math::clamp(r_rect.xmax, 0, width - 1);
  r_rect.ymin = math::clamp(r_rect.ymin, 0, height - 1);
  r_rect.ymax = math::clamp(r_rect.ymax, 0, height - 1);
}

static void initialize_shadow_alpha(int width,
                                    int height,
                                    int2 offset,
                                    const rcti &shadow_rect,
                                    const uchar *input,
                                    Array<uchar> &r_shadow_mask)
{
  const IndexRange shadow_y_range(shadow_rect.ymin, shadow_rect.ymax - shadow_rect.ymin + 1);
  threading::parallel_for(shadow_y_range, 8, [&](const IndexRange y_range) {
    for (const int64_t y : y_range) {
      const int64_t src_y = math::clamp<int64_t>(y + offset.y, 0, height - 1);
      for (int x = shadow_rect.xmin; x <= shadow_rect.xmax; x++) {
        int src_x = math::clamp(x - offset.x, 0, width - 1);
        size_t src_offset = width * src_y + src_x;
        size_t dst_offset = width * y + x;
        r_shadow_mask[dst_offset] = input[src_offset * 4 + 3];
      }
    }
  });
}

static void composite_shadow(int width,
                             const rcti &shadow_rect,
                             const float4 &shadow_color,
                             const Array<uchar> &shadow_mask,
                             uchar *output)
{
  const IndexRange shadow_y_range(shadow_rect.ymin, shadow_rect.ymax - shadow_rect.ymin + 1);
  threading::parallel_for(shadow_y_range, 8, [&](const IndexRange y_range) {
    for (const int64_t y : y_range) {
      size_t offset = y * width + shadow_rect.xmin;
      uchar *dst = output + offset * 4;
      for (int x = shadow_rect.xmin; x <= shadow_rect.xmax; x++, offset++, dst += 4) {
        uchar a = shadow_mask[offset];
        if (a == 0) {
          /* Fully transparent, leave output pixel as is. */
          continue;
        }
        float4 col1 = load_premul_pixel(dst);
        float4 col2 = shadow_color * (a * (1.0f / 255.0f));
        /* Blend under the output. */
        float fac = 1.0f - col1.w;
        float4 col = col1 + fac * col2;
        store_premul_pixel(col, dst);
      }
    }
  });
}

static void draw_text_shadow(const SeqRenderData *context,
                             const TextVars *data,
                             int line_height,
                             const rcti &rect,
                             ImBuf *out)
{
  const int width = context->rectx;
  const int height = context->recty;
  /* Blur value of 1.0 applies blur kernel that is half of text line height. */
  const float blur_amount = line_height * 0.5f * data->shadow_blur;
  bool do_blur = blur_amount >= 1.0f;

  Array<uchar> shadow_mask(size_t(width) * height, 0);

  const int2 offset = int2(cosf(data->shadow_angle) * line_height * data->shadow_offset,
                           sinf(data->shadow_angle) * line_height * data->shadow_offset);

  rcti shadow_rect = rect;
  BLI_rcti_translate(&shadow_rect, offset.x, -offset.y);
  BLI_rcti_pad(&shadow_rect, 1, 1);
  clamp_rect(width, height, shadow_rect);

  /* Initialize shadow by copying existing text/outline alpha. */
  initialize_shadow_alpha(width, height, offset, shadow_rect, out->byte_buffer.data, shadow_mask);

  if (do_blur) {
    /* Create blur kernel weights. */
    const int half_size = int(blur_amount + 0.5f);
    Array<float> gaussian = make_gaussian_blur_kernel(blur_amount, half_size);

    BLI_rcti_pad(&shadow_rect, half_size + 1, half_size + 1);
    clamp_rect(width, height, shadow_rect);

    /* Horizontal blur: blur shadow_mask into blur_buffer. */
    Array<uchar> blur_buffer(size_t(width) * height, NoInitialization());
    IndexRange blur_y_range(shadow_rect.ymin, shadow_rect.ymax - shadow_rect.ymin + 1);
    threading::parallel_for(blur_y_range, 8, [&](const IndexRange y_range) {
      const int y_first = y_range.first();
      const int y_size = y_range.size();
      text_gaussian_blur_x(gaussian,
                           half_size,
                           y_first,
                           width,
                           y_size,
                           shadow_mask.data(),
                           blur_buffer.data(),
                           shadow_rect);
    });

    /* Vertical blur: blur blur_buffer into shadow_mask. */
    threading::parallel_for(blur_y_range, 8, [&](const IndexRange y_range) {
      const int y_first = y_range.first();
      const int y_size = y_range.size();
      text_gaussian_blur_y(gaussian,
                           half_size,
                           y_first,
                           width,
                           y_size,
                           blur_buffer.data(),
                           shadow_mask.data(),
                           shadow_rect);
    });
  }

  /* Composite shadow under regular output. */
  float4 color = data->shadow_color;
  color.x *= color.w;
  color.y *= color.w;
  color.z *= color.w;
  composite_shadow(width, shadow_rect, color, shadow_mask, out->byte_buffer.data);
}

/* Text outline calculation is done by Jump Flooding Algorithm (JFA).
 * This is similar to inpaint/jump_flooding in Compositor, also to
 * "The Quest for Very Wide Outlines", Ben Golus 2020
 * https://bgolus.medium.com/the-quest-for-very-wide-outlines-ba82ed442cd9 */

constexpr uint16_t JFA_INVALID = 0xFFFF;

struct JFACoord {
  uint16_t x;
  uint16_t y;
};

static void jump_flooding_pass(Span<JFACoord> input,
                               MutableSpan<JFACoord> output,
                               int2 size,
                               IndexRange x_range,
                               IndexRange y_range,
                               int step_size)
{
  threading::parallel_for(y_range, 8, [&](const IndexRange sub_y_range) {
    for (const int64_t y : sub_y_range) {
      size_t index = y * size.x;
      for (const int64_t x : x_range) {
        float2 coord = float2(x, y);

        /* For each pixel, sample 9 pixels at +/- step size pattern,
         * and output coordinate of closest to the boundary. */
        JFACoord closest_texel{JFA_INVALID, JFA_INVALID};
        float minimum_squared_distance = std::numeric_limits<float>::max();
        for (int dy = -step_size; dy <= step_size; dy += step_size) {
          int yy = y + dy;
          if (yy < 0 || yy >= size.y) {
            continue;
          }
          for (int dx = -step_size; dx <= step_size; dx += step_size) {
            int xx = x + dx;
            if (xx < 0 || xx >= size.x) {
              continue;
            }
            JFACoord val = input[size_t(yy) * size.x + xx];
            if (val.x == JFA_INVALID) {
              continue;
            }

            float squared_distance = math::distance_squared(float2(val.x, val.y), coord);
            if (squared_distance < minimum_squared_distance) {
              minimum_squared_distance = squared_distance;
              closest_texel = val;
            }
          }
        }

        output[index + x] = closest_texel;
      }
    }
  });
}
namespace blender::seq {

static void text_draw(const TextVarsRuntime *runtime, float color[4])
{
  for (const LineInfo &line : runtime->lines) {
    for (const CharInfo &character : line.characters) {
      BLF_position(runtime->font, character.position.x, character.position.y, 0.0f);
      BLF_buffer_col(runtime->font, color);
      BLF_draw_buffer(runtime->font, character.str_ptr, character.byte_length);
    }
  }
}

static rcti draw_text_outline(const SeqRenderData *context,
                              const TextVars *data,
                              const TextVarsRuntime *runtime,
                              ColorManagedDisplay *display,
                              ImBuf *out)
{
  /* Outline width of 1.0 maps to half of text line height. */
  const int outline_width = int(runtime->line_height * 0.5f * data->outline_width);
  if (outline_width < 1 || data->outline_color[3] <= 0.0f ||
      ((data->flag & SEQ_TEXT_OUTLINE) == 0))
  {
    return runtime->text_boundbox;
  }

  const int2 size = int2(context->rectx, context->recty);

  /* Draw white text into temporary buffer. */
  const size_t pixel_count = size_t(size.x) * size.y;
  Array<uchar4> tmp_buf(pixel_count, uchar4(0));
  BLF_buffer(runtime->font, nullptr, (uchar *)tmp_buf.data(), size.x, size.y, display);

  text_draw(runtime, float4(1.0f));

  rcti outline_rect = runtime->text_boundbox;
  BLI_rcti_pad(&outline_rect, outline_width + 1, outline_width + 1);
  outline_rect.xmin = clamp_i(outline_rect.xmin, 0, size.x - 1);
  outline_rect.xmax = clamp_i(outline_rect.xmax, 0, size.x - 1);
  outline_rect.ymin = clamp_i(outline_rect.ymin, 0, size.y - 1);
  outline_rect.ymax = clamp_i(outline_rect.ymax, 0, size.y - 1);
  const IndexRange rect_x_range(outline_rect.xmin, outline_rect.xmax - outline_rect.xmin + 1);
  const IndexRange rect_y_range(outline_rect.ymin, outline_rect.ymax - outline_rect.ymin + 1);

  /* Initialize JFA: invalid values for empty regions, pixel coordinates
   * for opaque regions. */
  Array<JFACoord> boundary(pixel_count, NoInitialization());
  threading::parallel_for(IndexRange(size.y), 16, [&](const IndexRange y_range) {
    for (const int y : y_range) {
      size_t index = size_t(y) * size.x;
      for (int x = 0; x < size.x; x++, index++) {
        bool is_opaque = tmp_buf[index].w >= 128;
        JFACoord coord;
        coord.x = is_opaque ? x : JFA_INVALID;
        coord.y = is_opaque ? y : JFA_INVALID;
        boundary[index] = coord;
      }
    }
  });

  /* Do jump flooding calculations. */
  JFACoord invalid_coord{JFA_INVALID, JFA_INVALID};
  Array<JFACoord> initial_flooded_result(pixel_count, invalid_coord);
  jump_flooding_pass(boundary, initial_flooded_result, size, rect_x_range, rect_y_range, 1);

  Array<JFACoord> *result_to_flood = &initial_flooded_result;
  Array<JFACoord> intermediate_result(pixel_count, invalid_coord);
  Array<JFACoord> *result_after_flooding = &intermediate_result;

  int step_size = power_of_2_max_i(outline_width) / 2;

  while (step_size != 0) {
    jump_flooding_pass(
        *result_to_flood, *result_after_flooding, size, rect_x_range, rect_y_range, step_size);
    std::swap(result_to_flood, result_after_flooding);
    step_size /= 2;
  }

  /* Premultiplied outline color. */
  float4 color = data->outline_color;
  color.x *= color.w;
  color.y *= color.w;
  color.z *= color.w;

  const float text_color_alpha = data->color[3];

  /* We have distances to the closest opaque parts of the image now. Composite the
   * outline into the output image. */

  threading::parallel_for(rect_y_range, 8, [&](const IndexRange y_range) {
    for (const int y : y_range) {
      size_t index = size_t(y) * size.x + rect_x_range.start();
      uchar *dst = out->byte_buffer.data + index * 4;
      for (int x = rect_x_range.start(); x < rect_x_range.one_after_last(); x++, index++, dst += 4)
      {
        JFACoord closest_texel = (*result_to_flood)[index];
        if (closest_texel.x == JFA_INVALID) {
          /* Outside of outline, leave output pixel as is. */
          continue;
        }

        /* Fade out / anti-alias the outline over one pixel towards outline distance. */
        float distance = math::distance(float2(x, y), float2(closest_texel.x, closest_texel.y));
        float alpha = math::clamp(outline_width - distance + 1.0f, 0.0f, 1.0f);

        /* Do not put outline inside the text shape:
         * - When overall text color is fully opaque, we want to make
         *   outline fully transparent only where text is fully opaque.
         *   This ensures that combined anti-aliased pixels at text boundary
         *   are properly fully opaque.
         * - However when text color is fully transparent, we want to
         *   Use opposite alpha of text, to anti-alias the inner edge of
         *   the outline.
         * In between those two, interpolate the alpha modulation factor. */
        float text_alpha = tmp_buf[index].w * (1.0f / 255.0f);
        float mul_opaque_text = text_alpha >= 1.0f ? 0.0f : 1.0f;
        float mul_transparent_text = 1.0f - text_alpha;
        float mul = math::interpolate(mul_transparent_text, mul_opaque_text, text_color_alpha);
        alpha *= mul;

        float4 col1 = color;
        col1 *= alpha;

        /* Blend over the output. */
        float mfac = 1.0f - col1.w;
        float4 col2 = load_premul_pixel(dst);
        float4 col = col1 + mfac * col2;
        store_premul_pixel(col, dst);
      }
    }
  });
  BLF_buffer(runtime->font, nullptr, out->byte_buffer.data, size.x, size.y, display);

  return outline_rect;
}

/* Similar to #IMB_rectfill_area but blends the given color under the
 * existing image. Also can do rounded corners. Only works on byte buffers. */
static void fill_rect_alpha_under(
    const ImBuf *ibuf, const float col[4], int x1, int y1, int x2, int y2, float corner_radius)
{
  const int width = ibuf->x;
  const int height = ibuf->y;
  x1 = math::clamp(x1, 0, width);
  x2 = math::clamp(x2, 0, width);
  y1 = math::clamp(y1, 0, height);
  y2 = math::clamp(y2, 0, height);
  if (x1 > x2) {
    std::swap(x1, x2);
  }
  if (y1 > y2) {
    std::swap(y1, y2);
  }
  if (x1 == x2 || y1 == y2) {
    return;
  }

  corner_radius = math::clamp(corner_radius, 0.0f, math::min(x2 - x1, y2 - y1) / 2.0f);

  float4 premul_col_base;
  straight_to_premul_v4_v4(premul_col_base, col);

  threading::parallel_for(IndexRange::from_begin_end(y1, y2), 16, [&](const IndexRange y_range) {
    for (const int y : y_range) {
      uchar *dst = ibuf->byte_buffer.data + (size_t(width) * y + x1) * 4;
      float origin_x = 0.0f, origin_y = 0.0f;
      for (int x = x1; x < x2; x++) {
        float4 pix = load_premul_pixel(dst);
        float fac = 1.0f - pix.w;

        float4 premul_col = premul_col_base;
        bool is_corner = false;
        if (x < x1 + corner_radius && y < y1 + corner_radius) {
          is_corner = true;
          origin_x = x1 + corner_radius - 1;
          origin_y = y1 + corner_radius - 1;
        }
        else if (x >= x2 - corner_radius && y < y1 + corner_radius) {
          is_corner = true;
          origin_x = x2 - corner_radius;
          origin_y = y1 + corner_radius - 1;
        }
        else if (x < x1 + corner_radius && y >= y2 - corner_radius) {
          is_corner = true;
          origin_x = x1 + corner_radius - 1;
          origin_y = y2 - corner_radius;
        }
        else if (x >= x2 - corner_radius && y >= y2 - corner_radius) {
          is_corner = true;
          origin_x = x2 - corner_radius;
          origin_y = y2 - corner_radius;
        }
        if (is_corner) {
          /* If we are inside rounded corner, evaluate a superellipse and
           * modulate color with that. Superellipse instead of just a circle
           * since the curvature between flat and rounded area looks a bit
           * nicer. */
          constexpr float curve_pow = 2.1f;
          float r = powf(powf(abs(x - origin_x), curve_pow) + powf(abs(y - origin_y), curve_pow),
                         1.0f / curve_pow);
          float alpha = math::clamp(corner_radius - r, 0.0f, 1.0f);
          premul_col *= alpha;
        }

        float4 dst_fl = fac * premul_col + pix;
        store_premul_pixel(dst_fl, dst);
        dst += 4;
      }
    }
  });
}

static int text_effect_line_size_get(const SeqRenderData *context, const Sequence *seq)
{
  TextVars *data = static_cast<TextVars *>(seq->effectdata);
  /* Compensate text size for preview render size. */
  double proxy_size_comp = context->scene->r.size / 100.0;
  if (context->preview_render_size != SEQ_RENDER_SIZE_SCENE) {
    proxy_size_comp = SEQ_rendersize_to_scale_factor(context->preview_render_size);
  }

  return proxy_size_comp * data->text_size;
}

static int text_effect_font_init(const SeqRenderData *context, const Sequence *seq, int font_flags)
{
  TextVars *data = static_cast<TextVars *>(seq->effectdata);
  int font = blf_mono_font_render;

  /* In case font got unloaded behind our backs: mark it as needing a load. */
  if (data->text_blf_id >= 0 && !BLF_is_loaded_id(data->text_blf_id)) {
    data->text_blf_id = SEQ_FONT_NOT_LOADED;
  }

  if (data->text_blf_id == SEQ_FONT_NOT_LOADED) {
    data->text_blf_id = -1;

    SEQ_effect_text_font_load(data, false);
  }

  if (data->text_blf_id >= 0) {
    font = data->text_blf_id;
  }

  BLF_size(font, text_effect_line_size_get(context, seq));
  BLF_enable(font, font_flags);
  return font;
}

static blender::Vector<CharInfo> build_character_info(const TextVars *data, int font)
{
  blender::Vector<CharInfo> characters;
  const size_t len_max = BLI_strnlen(data->text, sizeof(data->text));
  int byte_offset = 0;
  int char_index = 0;
  while (byte_offset <= len_max) {
    const char *str = data->text + byte_offset;
    const int char_length = BLI_str_utf8_size_safe(str);

    CharInfo char_info;
    char_info.index = char_index;
    char_info.str_ptr = str;
    char_info.byte_length = char_length;
    char_info.advance_x = BLF_glyph_advance(font, str);
    characters.append(char_info);

    byte_offset += char_length;
    char_index++;
  }
  return characters;
}

static int wrap_width_get(const TextVars *data, const int2 image_size)
{
  if (data->wrap_width == 0.0f) {
    return std::numeric_limits<int>::max();
  }
  return data->wrap_width * image_size.x;
}

/* Lines must contain CharInfo for newlines and \0, as UI must know where they begin. */
static void apply_word_wrapping(const TextVars *data,
                                TextVarsRuntime *runtime,
                                const int2 image_size,
                                blender::Vector<CharInfo> &characters)
{
  const int wrap_width = wrap_width_get(data, image_size);

  float2 char_position{0.0f, 0.0f};
  CharInfo *last_space = nullptr;

  /* First pass: Find characters where line has to be broken. */
  for (CharInfo &character : characters) {
    if (character.str_ptr[0] == ' ') {
      character.position = char_position;
      last_space = &character;
    }
    if (character.str_ptr[0] == '\n') {
      char_position.x = 0;
      last_space = nullptr;
    }
    if (character.str_ptr[0] != '\0' && char_position.x > wrap_width && last_space != nullptr) {
      last_space->do_wrap = true;
      char_position -= last_space->position + last_space->advance_x;
    }
    char_position.x += character.advance_x;
  }

  /* Second pass: Fill lines with characters. */
  char_position = {0.0f, 0.0f};
  runtime->lines.append(LineInfo());
  for (CharInfo &character : characters) {
    character.position = char_position;
    runtime->lines.last().characters.append(character);
    runtime->lines.last().width = char_position.x;

    char_position.x += character.advance_x;

    if (character.do_wrap || character.str_ptr[0] == '\n') {
      runtime->lines.append(LineInfo());
      char_position.x = 0;
      char_position.y -= runtime->line_height;
    }
  }
}

static int text_box_width_get(const blender::Vector<LineInfo> &lines)
{
  int width_max = 0;

  for (const LineInfo &line : lines) {
    width_max = std::max(width_max, line.width);
  }
  return width_max;
}

static float2 horizontal_alignment_offset_get(const TextVars *data,
                                              float line_width,
                                              int width_max)
{
  const float line_offset = (width_max - line_width);

  if (data->align == SEQ_TEXT_ALIGN_X_RIGHT) {
    return {line_offset, 0.0f};
  }
  else if (data->align == SEQ_TEXT_ALIGN_X_CENTER) {
    return {line_offset / 2.0f, 0.0f};
  }

  return {0.0f, 0.0f};
}

static float2 anchor_offset_get(const TextVars *data, int width_max, int text_height)
{
  float2 anchor_offset;

  switch (data->anchor_x) {
    case SEQ_TEXT_ALIGN_X_LEFT:
      anchor_offset.x = 0;
      break;
    case SEQ_TEXT_ALIGN_X_CENTER:
      anchor_offset.x = -width_max / 2.0f;
      break;
    case SEQ_TEXT_ALIGN_X_RIGHT:
      anchor_offset.x = -width_max;
      break;
  }
  switch (data->anchor_y) {
    case SEQ_TEXT_ALIGN_Y_TOP:
      anchor_offset.y = 0;
      break;
    case SEQ_TEXT_ALIGN_Y_CENTER:
      anchor_offset.y = text_height / 2.0f;
      break;
    case SEQ_TEXT_ALIGN_Y_BOTTOM:
      anchor_offset.y = text_height;
      break;
  }

  return anchor_offset;
}

static void calc_boundbox(const TextVars *data, TextVarsRuntime *runtime, const int2 image_size)
{
  const int text_height = runtime->lines.size() * runtime->line_height;

  int width_max = text_box_width_get(runtime->lines);

  /* Add width to empty text, so there is something to draw or select. */
  if (width_max == 0) {
    width_max = text_height * 2;
  }

  const float2 image_center{data->loc[0] * image_size.x, data->loc[1] * image_size.y};
  const float2 anchor = anchor_offset_get(data, width_max, text_height);

  runtime->text_boundbox.xmin = anchor.x + image_center.x;
  runtime->text_boundbox.xmax = anchor.x + image_center.x + width_max;
  runtime->text_boundbox.ymin = anchor.y + image_center.y - text_height;
  runtime->text_boundbox.ymax = runtime->text_boundbox.ymin + text_height;
}

static void apply_text_alignment(const TextVars *data,
                                 TextVarsRuntime *runtime,
                                 const int2 image_size)
{
  const int width_max = text_box_width_get(runtime->lines);
  const int text_height = runtime->lines.size() * runtime->line_height;

  const float2 image_center{data->loc[0] * image_size.x, data->loc[1] * image_size.y};
  const float2 line_height_offset{0.0f,
                                  float(-runtime->line_height - BLF_descender(runtime->font))};
  const float2 anchor = anchor_offset_get(data, width_max, text_height);

  for (LineInfo &line : runtime->lines) {
    const float2 alignment_x = horizontal_alignment_offset_get(data, line.width, width_max);
    const float2 alignment = math::round(image_center + line_height_offset + alignment_x + anchor);

    for (CharInfo &character : line.characters) {
      character.position += alignment;
    }
  }
}

static void calc_text_runtime(const Sequence *seq, int font, const int2 image_size)
{
  TextVars *data = static_cast<TextVars *>(seq->effectdata);

  if (data->runtime != nullptr) {
    MEM_delete(data->runtime);
  }

  data->runtime = MEM_new<TextVarsRuntime>(__func__);
  TextVarsRuntime *runtime = data->runtime;
  runtime->font = font;
  runtime->line_height = BLF_height_max(font);
  runtime->font_descender = BLF_descender(font);
  runtime->character_count = BLI_strlen_utf8(data->text);

  blender::Vector<CharInfo> characters_temp = build_character_info(data, font);
  apply_word_wrapping(data, runtime, image_size, characters_temp);
  apply_text_alignment(data, runtime, image_size);
  calc_boundbox(data, runtime, image_size);
}

static ImBuf *do_text_effect(const SeqRenderData *context,
                             Sequence *seq,
                             float /*timeline_frame*/,
                             float /*fac*/,
                             ImBuf * /*ibuf1*/,
                             ImBuf * /*ibuf2*/)
{
  /* NOTE: text rasterization only fills in part of output image,
   * need to clear it. */
  ImBuf *out = prepare_effect_imbufs(context, nullptr, nullptr, false);
  TextVars *data = static_cast<TextVars *>(seq->effectdata);

  const char *display_device = context->scene->display_settings.display_device;
  ColorManagedDisplay *display = IMB_colormanagement_display_get_named(display_device);
  const int font_flags = ((data->flag & SEQ_TEXT_BOLD) ? BLF_BOLD : 0) |
                         ((data->flag & SEQ_TEXT_ITALIC) ? BLF_ITALIC : 0);

  /* Guard against parallel accesses to the fonts map. */
  std::lock_guard lock(g_font_map.mutex);

  const int font = text_effect_font_init(context, seq, font_flags);

  calc_text_runtime(seq, font, {out->x, out->y});
  TextVarsRuntime *runtime = data->runtime;

  rcti outline_rect = draw_text_outline(context, data, runtime, display, out);
  BLF_buffer(font, nullptr, out->byte_buffer.data, out->x, out->y, display);
  text_draw(runtime, data->color);
  BLF_buffer(font, nullptr, nullptr, 0, 0, nullptr);
  BLF_disable(font, font_flags);

  /* Draw shadow. */
  if (data->flag & SEQ_TEXT_SHADOW) {
    draw_text_shadow(context, data, runtime->line_height, outline_rect, out);
  }

  /* Draw box under text. */
  if (data->flag & SEQ_TEXT_BOX) {
    if (out->byte_buffer.data) {
      const int margin = data->box_margin * out->x;
      const int minx = runtime->text_boundbox.xmin - margin;
      const int maxx = runtime->text_boundbox.xmax + margin;
      const int miny = runtime->text_boundbox.ymin - margin;
      const int maxy = runtime->text_boundbox.ymax + margin;
      float corner_radius = data->box_roundness * (maxy - miny) / 2.0f;
      fill_rect_alpha_under(out, data->box_color, minx, miny, maxx, maxy, corner_radius);
    }
  }

  return out;
}
}  // namespace blender::seq

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sequence Effect Factory
 * \{ */

static void init_noop(Sequence * /*seq*/) {}

static void load_noop(Sequence * /*seq*/) {}

static void free_noop(Sequence * /*seq*/, const bool /*do_id_user*/) {}

static int num_inputs_default()
{
  return 2;
}

static void copy_effect_default(Sequence *dst, const Sequence *src, const int /*flag*/)
{
  dst->effectdata = MEM_dupallocN(src->effectdata);
}

static void free_effect_default(Sequence *seq, const bool /*do_id_user*/)
{
  MEM_SAFE_FREE(seq->effectdata);
}

static StripEarlyOut early_out_noop(const Sequence * /*seq*/, float /*fac*/)
{
  return StripEarlyOut::DoEffect;
}

static StripEarlyOut early_out_fade(const Sequence * /*seq*/, float fac)
{
  if (fac == 0.0f) {
    return StripEarlyOut::UseInput1;
  }
  if (fac == 1.0f) {
    return StripEarlyOut::UseInput2;
  }
  return StripEarlyOut::DoEffect;
}

static StripEarlyOut early_out_mul_input2(const Sequence * /*seq*/, float fac)
{
  if (fac == 0.0f) {
    return StripEarlyOut::UseInput1;
  }
  return StripEarlyOut::DoEffect;
}

static StripEarlyOut early_out_mul_input1(const Sequence * /*seq*/, float fac)
{
  if (fac == 0.0f) {
    return StripEarlyOut::UseInput2;
  }
  return StripEarlyOut::DoEffect;
}

static void get_default_fac_noop(const Scene * /*scene*/,
                                 const Sequence * /*seq*/,
                                 float /*timeline_frame*/,
                                 float *fac)
{
  *fac = 1.0f;
}

static void get_default_fac_fade(const Scene *scene,
                                 const Sequence *seq,
                                 float timeline_frame,
                                 float *fac)
{
  *fac = float(timeline_frame - SEQ_time_left_handle_frame_get(scene, seq));
  *fac /= SEQ_time_strip_length_get(scene, seq);
  *fac = math::clamp(*fac, 0.0f, 1.0f);
}

static ImBuf *init_execution(const SeqRenderData *context, ImBuf *ibuf1, ImBuf *ibuf2)
{
  ImBuf *out = prepare_effect_imbufs(context, ibuf1, ibuf2);
  return out;
}

static SeqEffectHandle get_sequence_effect_impl(int seq_type)
{
  SeqEffectHandle rval;
  int sequence_type = seq_type;

  rval.multithreaded = false;
  rval.supports_mask = false;
  rval.init = init_noop;
  rval.num_inputs = num_inputs_default;
  rval.load = load_noop;
  rval.free = free_noop;
  rval.early_out = early_out_noop;
  rval.get_default_fac = get_default_fac_noop;
  rval.execute = nullptr;
  rval.init_execution = init_execution;
  rval.execute_slice = nullptr;
  rval.copy = nullptr;

  switch (sequence_type) {
    case SEQ_TYPE_CROSS:
      rval.multithreaded = true;
      rval.execute_slice = do_cross_effect;
      rval.early_out = early_out_fade;
      rval.get_default_fac = get_default_fac_fade;
      break;
    case SEQ_TYPE_GAMCROSS:
      rval.multithreaded = true;
      rval.early_out = early_out_fade;
      rval.get_default_fac = get_default_fac_fade;
      rval.execute_slice = do_gammacross_effect;
      break;
    case SEQ_TYPE_ADD:
      rval.multithreaded = true;
      rval.execute_slice = do_add_effect;
      rval.early_out = early_out_mul_input2;
      break;
    case SEQ_TYPE_SUB:
      rval.multithreaded = true;
      rval.execute_slice = do_sub_effect;
      rval.early_out = early_out_mul_input2;
      break;
    case SEQ_TYPE_MUL:
      rval.multithreaded = true;
      rval.execute_slice = do_mul_effect;
      rval.early_out = early_out_mul_input2;
      break;
    case SEQ_TYPE_SCREEN:
    case SEQ_TYPE_OVERLAY:
    case SEQ_TYPE_COLOR_BURN:
    case SEQ_TYPE_LINEAR_BURN:
    case SEQ_TYPE_DARKEN:
    case SEQ_TYPE_LIGHTEN:
    case SEQ_TYPE_DODGE:
    case SEQ_TYPE_SOFT_LIGHT:
    case SEQ_TYPE_HARD_LIGHT:
    case SEQ_TYPE_PIN_LIGHT:
    case SEQ_TYPE_LIN_LIGHT:
    case SEQ_TYPE_VIVID_LIGHT:
    case SEQ_TYPE_BLEND_COLOR:
    case SEQ_TYPE_HUE:
    case SEQ_TYPE_SATURATION:
    case SEQ_TYPE_VALUE:
    case SEQ_TYPE_DIFFERENCE:
    case SEQ_TYPE_EXCLUSION:
      rval.multithreaded = true;
      rval.execute_slice = do_blend_mode_effect;
      rval.early_out = early_out_mul_input2;
      break;
    case SEQ_TYPE_COLORMIX:
      rval.multithreaded = true;
      rval.init = init_colormix_effect;
      rval.free = free_effect_default;
      rval.copy = copy_effect_default;
      rval.execute_slice = do_colormix_effect;
      rval.early_out = early_out_mul_input2;
      break;
    case SEQ_TYPE_ALPHAOVER:
      rval.multithreaded = true;
      rval.init = init_alpha_over_or_under;
      rval.execute_slice = do_alphaover_effect;
      rval.early_out = early_out_mul_input1;
      break;
    case SEQ_TYPE_OVERDROP:
      rval.multithreaded = true;
      rval.execute_slice = do_overdrop_effect;
      break;
    case SEQ_TYPE_ALPHAUNDER:
      rval.multithreaded = true;
      rval.init = init_alpha_over_or_under;
      rval.execute_slice = do_alphaunder_effect;
      break;
    case SEQ_TYPE_WIPE:
      rval.init = init_wipe_effect;
      rval.num_inputs = num_inputs_wipe;
      rval.free = free_wipe_effect;
      rval.copy = copy_wipe_effect;
      rval.early_out = early_out_fade;
      rval.get_default_fac = get_default_fac_fade;
      rval.execute = do_wipe_effect;
      break;
    case SEQ_TYPE_GLOW:
      rval.init = init_glow_effect;
      rval.num_inputs = num_inputs_glow;
      rval.free = free_glow_effect;
      rval.copy = copy_glow_effect;
      rval.execute = do_glow_effect;
      break;
    case SEQ_TYPE_TRANSFORM:
      rval.multithreaded = true;
      rval.init = init_transform_effect;
      rval.num_inputs = num_inputs_transform;
      rval.free = free_transform_effect;
      rval.copy = copy_transform_effect;
      rval.execute_slice = do_transform_effect;
      break;
    case SEQ_TYPE_SPEED:
      rval.init = init_speed_effect;
      rval.num_inputs = num_inputs_speed;
      rval.load = load_speed_effect;
      rval.free = free_speed_effect;
      rval.copy = copy_speed_effect;
      rval.execute = do_speed_effect;
      rval.early_out = early_out_speed;
      break;
    case SEQ_TYPE_COLOR:
      rval.init = init_solid_color;
      rval.num_inputs = num_inputs_color;
      rval.early_out = early_out_color;
      rval.free = free_solid_color;
      rval.copy = copy_solid_color;
      rval.execute = do_solid_color;
      break;
    case SEQ_TYPE_MULTICAM:
      rval.num_inputs = num_inputs_multicam;
      rval.early_out = early_out_multicam;
      rval.execute = do_multicam;
      break;
    case SEQ_TYPE_ADJUSTMENT:
      rval.supports_mask = true;
      rval.num_inputs = num_inputs_adjustment;
      rval.early_out = early_out_adjustment;
      rval.execute = do_adjustment;
      break;
    case SEQ_TYPE_GAUSSIAN_BLUR:
      rval.init = init_gaussian_blur_effect;
      rval.num_inputs = num_inputs_gaussian_blur;
      rval.free = free_gaussian_blur_effect;
      rval.copy = copy_gaussian_blur_effect;
      rval.early_out = early_out_gaussian_blur;
      rval.execute = do_gaussian_blur_effect;
      break;
    case SEQ_TYPE_TEXT:
      rval.num_inputs = num_inputs_text;
      rval.init = init_text_effect;
      rval.free = free_text_effect;
      rval.load = load_text_effect;
      rval.copy = copy_text_effect;
      rval.early_out = early_out_text;
      rval.execute = blender::seq::do_text_effect;
      break;
  }

  return rval;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public Sequencer Effect API
 * \{ */

SeqEffectHandle SEQ_effect_handle_get(Sequence *seq)
{
  SeqEffectHandle rval = {false, false, nullptr};

  if (seq->type & SEQ_TYPE_EFFECT) {
    rval = get_sequence_effect_impl(seq->type);
    if ((seq->flag & SEQ_EFFECT_NOT_LOADED) != 0) {
      rval.load(seq);
      seq->flag &= ~SEQ_EFFECT_NOT_LOADED;
    }
  }

  return rval;
}

SeqEffectHandle seq_effect_get_sequence_blend(Sequence *seq)
{
  SeqEffectHandle rval = {false, false, nullptr};

  if (seq->blend_mode != 0) {
    if ((seq->flag & SEQ_EFFECT_NOT_LOADED) != 0) {
      /* load the effect first */
      rval = get_sequence_effect_impl(seq->type);
      rval.load(seq);
    }

    rval = get_sequence_effect_impl(seq->blend_mode);
    if ((seq->flag & SEQ_EFFECT_NOT_LOADED) != 0) {
      /* now load the blend and unset unloaded flag */
      rval.load(seq);
      seq->flag &= ~SEQ_EFFECT_NOT_LOADED;
    }
  }

  return rval;
}

int SEQ_effect_get_num_inputs(int seq_type)
{
  SeqEffectHandle rval = get_sequence_effect_impl(seq_type);

  int count = rval.num_inputs();
  if (rval.execute || (rval.execute_slice && rval.init_execution)) {
    return count;
  }
  return 0;
}

/** \} */
