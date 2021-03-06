/*
    This file is part of darktable,
    Copyright (C) 2020 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "dtgtk/button.h"
#include "dtgtk/resetlabel.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "gui/color_picker_proxy.h"
#include "iop/iop_api.h"

#include <glib.h>
#include <math.h>
#include <stdlib.h>

#if defined(__GNUC__)
#pragma GCC optimize ("unroll-loops", "tree-loop-if-convert", \
                      "tree-loop-distribution", "no-strict-aliasing", \
                      "loop-interchange", "loop-nest-optimize", "tree-loop-im", \
                      "unswitch-loops", "tree-loop-ivcanon", "ira-loop-pressure", \
                      "split-ivs-in-unroller", "variable-expansion-in-unroller", \
                      "split-loops", "ivopts", "predictive-commoning",\
                      "tree-loop-linear", "loop-block", "loop-strip-mine", \
                      "finite-math-only", "fp-contract=fast", "fast-math")
#endif

/** DOCUMENTATION
 *
 * This module allows to invert scanned negatives and simulate their print on paper, based on Kodak Cineon
 * densitometry algorithm. It is better than the old invert module because it takes into account the Dmax of the film
 * and allows white balance adjustments, as well as paper grade (gamma) simulation. It also allows density correction
 * in log space, to account for the exposure settings of the scanner. Finally, it is applied after input colour profiling,
 * which means the inversion happens after the scanner or the camera got color-corrected, while the old invert module
 * invert the RAW, non-demosaiced, file before any colour correction.
 *
 * References :
 *
 *  - https://www.kodak.com/uploadedfiles/motion/US_plugins_acrobat_en_motion_education_sensitometry_workbook.pdf
 *  - http://www.digital-intermediate.co.uk/film/pdf/Cineon.pdf
 *  - https://lists.gnu.org/archive/html/openexr-devel/2005-03/msg00009.html
 **/

 #define THRESHOLD 2.3283064365386963e-10f // -32 EV


/* Declare external functions for vectorization. Doing so, OpenMP is supposed to inline them in loops. */
#if defined(_OPENMP) && !defined(_WIN32)

#pragma omp declare simd
extern float fmaxf(const float x, const float y);

#pragma omp declare simd
extern float exp2f(const float x);

#pragma omp declare simd
extern float powf(const float x, const float y);

#pragma omp declare simd
extern float log2f(const float x);

#pragma omp declare simd
extern float log10f(const float x);

#endif

/* Bring our own optimized maths functions because Clang makes dumb shit */

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float fast_exp10f(const float x)
{
  // we use the property : 10^x = exp(log(10) * x) = 2^(log(10) * x / log(2))
  // max relative error over x = [0; 4] is 1.5617955706227326e-15
  return exp2f(3.3219280948873626f * x);
}

// Since we are at it, write an optimized expf
#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float fast_expf(const float x)
{
  // we use the property : exp(x) = 2^(x / log(2))
  // max relative error over x = [0; 4] is 5.246203046472202e-16
  return exp2f(1.4426950408889634f * x);
}


static inline float v_maxf(const float vector[3])
{
  // Find the max over an RGB vector
  return fmaxf(fmaxf(vector[0], vector[1]), vector[2]);
}

static inline float v_minf(const float vector[3])
{
  // Find the min over an RGB vector
  return fminf(fminf(vector[0], vector[1]), vector[2]);
}


DT_MODULE_INTROSPECTION(1, dt_iop_negadoctor_params_t)


typedef enum dt_iop_negadoctor_filmstock_t
{
  // What kind of emulsion are we working on ?
  DT_FILMSTOCK_NB = 0,
  DT_FILMSTOCK_COLOR = 1,
  DT_FILMSTOCK_END = 2
} dt_iop_negadoctor_filmstock_t;


typedef enum dt_iop_negadoctor_pickcolor_type_t
{
  DT_NEGADOCTOR_NONE = 0,
  DT_NEGADOCTOR_DMIN = 1,
  DT_NEGADOCTOR_WB_high = 2,
  DT_NEGADOCTOR_OFFSET = 3,
  DT_NEGADOCTOR_DMAX = 4,
  DT_NEGADOCTOR_WB_low = 5,
  DT_NEGADOCTOR_EXPOSURE = 6,
  DT_NEGADOCTOR_BLACK = 7
} dt_iop_negadoctor_pickcolor_type_t;


typedef struct dt_iop_negadoctor_params_t
{
  dt_iop_negadoctor_filmstock_t film_stock;
  float DT_ALIGNED_PIXEL Dmin[4];         // color of film substrate
  float DT_ALIGNED_PIXEL wb_high[4];      // white balance RGB coeffs (illuminant)
  float DT_ALIGNED_PIXEL wb_low[4];       // white balance RGB offsets (base light)
  float D_max;                            // max density of film
  float offset;                           // inversion offset
  float black;                            // display black level
  float gamma;                            // display gamma
  float soft_clip;                        // highlights roll-off
  float exposure;                         // extra exposure
} dt_iop_negadoctor_params_t;


typedef struct dt_iop_negadoctor_data_t
{
  float DT_ALIGNED_PIXEL Dmin[4];         // color of film substrate
  float DT_ALIGNED_PIXEL wb_high[4];      // white balance RGB coeffs / Dmax
  float DT_ALIGNED_PIXEL offset[4];       // inversion offset
  float black;                            // display black level
  float gamma;                            // display gamma
  float soft_clip;                        // highlights roll-off
  float soft_clip_comp;                   // 1 - softclip, complement to 1
  float exposure;                         // extra exposure
} dt_iop_negadoctor_data_t;


typedef struct dt_iop_negadoctor_gui_data_t
{
  GtkNotebook *notebook;
  GtkWidget *film_stock;
  GtkWidget *Dmin_R, *Dmin_G, *Dmin_B;
  GtkWidget *wb_high_R, *wb_high_G, *wb_high_B;
  GtkWidget *wb_low_R, *wb_low_G, *wb_low_B;
  GtkWidget *D_max;
  GtkWidget *offset;
  GtkWidget *black, *gamma, *soft_clip, *exposure;
  GtkWidget *Dmin_picker, *Dmin_sampler;
  GtkWidget *WB_high_picker, *WB_high_sampler;
  GtkWidget *WB_low_picker, *WB_low_sampler;
  dt_iop_color_picker_t color_picker;
} dt_iop_negadoctor_gui_data_t;


typedef struct dt_iop_negadoctor_global_data_t
{
  int kernel_negadoctor;
} dt_iop_negadoctor_global_data_t;


const char *name()
{
  return _("negadoctor");
}


int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE;
}


int default_group()
{
  return IOP_GROUP_BASIC;
}


int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}


void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_negadoctor_params_t *const p = (dt_iop_negadoctor_params_t *)p1;
  dt_iop_negadoctor_data_t *const d = (dt_iop_negadoctor_data_t *)piece->data;

  // keep WB_high even in B&W mode to apply sepia or warm tone look
  // but premultiply it aheard with Dmax to spare one div per pixel
  for(size_t c = 0; c < 4; c++) d->wb_high[c] = p->wb_high[c] / p->D_max;

  for(size_t c = 0; c < 4; c++) d->offset[c] = p->wb_high[c] * p->offset * p->wb_low[c];

  // ensure we use a monochrome Dmin for B&W film
  if(p->film_stock == DT_FILMSTOCK_COLOR)
    for(size_t c = 0; c < 4; c++) d->Dmin[c] = p->Dmin[c];
  else if(p->film_stock == DT_FILMSTOCK_NB)
    for(size_t c = 0; c < 4; c++) d->Dmin[c] = p->Dmin[0];

  // arithmetic trick allowing to rewrite the pixel inversion as FMA
  d->black = -p->exposure * (1.0f + p->black);

  // highlights soft clip
  d->soft_clip = p->soft_clip;
  d->soft_clip_comp = 1.0f - p->soft_clip;

  // copy
  d->exposure = p->exposure;
  d->gamma = p->gamma;
}


void process(struct dt_iop_module_t *const self, dt_dev_pixelpipe_iop_t *const piece,
             const void *const restrict ivoid, void *const restrict ovoid,
             const dt_iop_roi_t *const restrict roi_in, const dt_iop_roi_t *const restrict roi_out)
{
  const dt_iop_negadoctor_data_t *const d = piece->data;
  assert(piece->colors = 4);

  const float *const restrict in = (float *)ivoid;
  float *const restrict out = (float *)ovoid;


#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(d, in, out, roi_out) \
    aligned(in, out:64) collapse(2)
#endif
  for(size_t k = 0; k < roi_out->height * roi_out->width * 4; k += 4)
  {
    for(size_t c = 0; c < 4; c++)
    {
      // Unpack vectors one by one with extra pragmas to be sure the compiler understands they can be vectorized
      const float *const restrict pix_in = in + k;
      float *const restrict pix_out = out + k;
      const float *const restrict Dmin = __builtin_assume_aligned(d->Dmin, 16);
      const float *const restrict wb_high = __builtin_assume_aligned(d->wb_high, 16);
      const float *const restrict offset = __builtin_assume_aligned(d->offset, 16);

      // Convert transmission to density using Dmin as a fulcrum
      const float density = - log10f(Dmin[c] / fmaxf(pix_in[c], THRESHOLD)); // threshold to -32 EV

      // Correct density in log space
      const float corrected_de = wb_high[c] * density + offset[c];

      // Print density on paper : ((1 - 10^corrected_de + black) * exposure)^gamma rewritten for FMA
      const float print_linear = -(d->exposure * fast_exp10f(corrected_de) + d->black);
      const float print_gamma = powf(fmaxf(print_linear, 0.0f), d->gamma); // note : this is always > 0

      // Compress highlights. from https://lists.gnu.org/archive/html/openexr-devel/2005-03/msg00009.html
      pix_out[c] =  (print_gamma > d->soft_clip) ? d->soft_clip + (1.0f - fast_expf(-(print_gamma - d->soft_clip) / d->soft_clip_comp)) * d->soft_clip_comp
                                                 : print_gamma;
    }
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}


#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *const self, dt_dev_pixelpipe_iop_t *const piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const restrict roi_in, const dt_iop_roi_t *const restrict roi_out)
{
  const dt_iop_negadoctor_data_t *const d = (dt_iop_negadoctor_data_t *)piece->data;
  const dt_iop_negadoctor_global_data_t *const gd = (dt_iop_negadoctor_global_data_t *)self->global_data;

  cl_int err = -999;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

  dt_opencl_set_kernel_arg(devid, gd->kernel_negadoctor, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_negadoctor, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_negadoctor, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_negadoctor, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_negadoctor, 4, 4 * sizeof(float), (void *)&d->Dmin);
  dt_opencl_set_kernel_arg(devid, gd->kernel_negadoctor, 5, 4 * sizeof(float), (void *)&d->wb_high);
  dt_opencl_set_kernel_arg(devid, gd->kernel_negadoctor, 6, 4 * sizeof(float), (void *)&d->offset);
  dt_opencl_set_kernel_arg(devid, gd->kernel_negadoctor, 7, sizeof(float), (void *)&d->exposure);
  dt_opencl_set_kernel_arg(devid, gd->kernel_negadoctor, 8, sizeof(float), (void *)&d->black);
  dt_opencl_set_kernel_arg(devid, gd->kernel_negadoctor, 9, sizeof(float), (void *)&d->gamma);
  dt_opencl_set_kernel_arg(devid, gd->kernel_negadoctor, 10, sizeof(float), (void *)&d->soft_clip);
  dt_opencl_set_kernel_arg(devid, gd->kernel_negadoctor, 11, sizeof(float), (void *)&d->soft_clip_comp);

  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_negadoctor, sizes);
  if(err != CL_SUCCESS) goto error;
    return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_negadoctor] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif


void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_negadoctor_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_negadoctor_params_t));
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_negadoctor_params_t);
  module->gui_data = NULL;

  dt_iop_negadoctor_params_t tmp = (dt_iop_negadoctor_params_t){ .film_stock = DT_FILMSTOCK_COLOR,
                                                                 .Dmin = { 1.0f, 0.45f, 0.25f, 0.0f},
                                                                 .wb_high = { 1.0f, 1.0f, 1.0f, 0.0f },
                                                                 .wb_low = { 1.0f, 1.0f, 1.0f, 0.0f },
                                                                 .D_max = 2.046f,
                                                                 .offset = 0.0f,
                                                                 .gamma = 4.0f,
                                                                 .soft_clip = 0.9f,
                                                                 .exposure = 1.0f,
                                                                 .black = 0.0755f };

  memcpy(module->params, &tmp, sizeof(dt_iop_negadoctor_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_negadoctor_params_t));
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_negadoctor_params_t tmp = (dt_iop_negadoctor_params_t){ .film_stock = DT_FILMSTOCK_COLOR,
                                                                 .Dmin = { 1.13f, 0.49f, 0.27f, 0.0f},
                                                                 .wb_high = { 1.0f, 1.0f, 1.0f, 0.0f },
                                                                 .wb_low = { 1.0f, 1.0f, 1.0f, 0.0f },
                                                                 .D_max = 1.6f,
                                                                 .offset = 0.0f,
                                                                 .gamma = 4.0f,
                                                                 .soft_clip = 0.92f,
                                                                 .exposure = 1.0f,
                                                                 .black = 0.0755f };


  dt_gui_presets_add_generic(_("color film"), self->op, self->version(), &tmp, sizeof(tmp), 1);

  dt_iop_negadoctor_params_t tmq = (dt_iop_negadoctor_params_t){ .film_stock = DT_FILMSTOCK_NB,
                                                                 .Dmin = { 1.0f, 1.0f, 1.0f, 0.0f},
                                                                 .wb_high = { 1.0f, 1.0f, 1.0f, 0.0f },
                                                                 .wb_low = { 1.0f, 1.0f, 1.0f, 0.0f },
                                                                 .D_max = 2.2f,
                                                                 .offset = 0.2f,
                                                                 .gamma = 4.0f,
                                                                 .soft_clip = 0.92f,
                                                                 .exposure = 1.0f,
                                                                 .black = 0.1f };


  dt_gui_presets_add_generic(_("black and white film"), self->op, self->version(), &tmq, sizeof(tmq), 1);
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
  free(module->default_params);
  module->default_params = NULL;
}

void init_global(dt_iop_module_so_t *module)
{
  dt_iop_negadoctor_global_data_t *gd
      = (dt_iop_negadoctor_global_data_t *)malloc(sizeof(dt_iop_negadoctor_global_data_t));

  module->data = gd;
  const int program = 30; // negadoctor.cl, from programs.conf
  gd->kernel_negadoctor = dt_opencl_create_kernel(program, "negadoctor");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_negadoctor_global_data_t *gd = module->data;
  dt_opencl_free_kernel(gd->kernel_negadoctor);
  free(module->data);
  module->data = NULL;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = g_malloc0(sizeof(dt_iop_negadoctor_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  g_free(piece->data);
  piece->data = NULL;
}


/* Global GUI stuff */

static void setup_color_variables(dt_iop_negadoctor_gui_data_t *const g, const gint state)
{
  gtk_widget_set_visible(g->Dmin_G, state);
  gtk_widget_set_visible(g->Dmin_B, state);
}


static void toggle_stock_controls(dt_iop_module_t *const self)
{
  dt_iop_negadoctor_gui_data_t *const g = (dt_iop_negadoctor_gui_data_t *)self->gui_data;
  const dt_iop_negadoctor_params_t *const p = (dt_iop_negadoctor_params_t *)self->params;

  if(p->film_stock == DT_FILMSTOCK_NB)
  {
    // Hide color controls
    setup_color_variables(g, FALSE);
    dt_bauhaus_widget_set_label(g->Dmin_R, NULL, _("D min"));
  }
  else if(p->film_stock == DT_FILMSTOCK_COLOR)
  {
    // Show color controls
    setup_color_variables(g, TRUE);
    dt_bauhaus_widget_set_label(g->Dmin_R, NULL, _("D min red component"));
  }
  else
  {
    // We shouldn't be there
    fprintf(stderr, "negadoctor film stock: undefined behaviour\n");
  }
}


static void Dmin_picker_update(dt_iop_module_t *self)
{
  dt_iop_negadoctor_gui_data_t *const g = (dt_iop_negadoctor_gui_data_t *)self->gui_data;
  const dt_iop_negadoctor_params_t *const p = (dt_iop_negadoctor_params_t *)self->params;

  GdkRGBA color;
  color.alpha = 1.0f;

  if(p->film_stock == DT_FILMSTOCK_COLOR)
  {
    color.red = p->Dmin[0];
    color.green = p->Dmin[1];
    color.blue = p->Dmin[2];
  }
  else if(p->film_stock == DT_FILMSTOCK_NB)
  {
    color.red = color.green = color.blue = p->Dmin[0];
  }

  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->Dmin_picker), &color);
}

static void Dmin_picker_callback(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)self->gui_data;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  dt_iop_color_picker_reset(self, TRUE);

  GdkRGBA c;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &c);
  p->Dmin[0] = c.red;
  p->Dmin[1] = c.green;
  p->Dmin[2] = c.blue;

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->Dmin_R, p->Dmin[0] * 100.0f); // warning: GUI is in %
  dt_bauhaus_slider_set(g->Dmin_G, p->Dmin[1] * 100.0f); // warning: GUI is in %
  dt_bauhaus_slider_set(g->Dmin_B, p->Dmin[2] * 100.0f); // warning: GUI is in %
  darktable.gui->reset = reset;

  Dmin_picker_update(self);
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void WB_low_picker_update(dt_iop_module_t *self)
{
  dt_iop_negadoctor_gui_data_t *const g = (dt_iop_negadoctor_gui_data_t *)self->gui_data;
  const dt_iop_negadoctor_params_t *const p = (dt_iop_negadoctor_params_t *)self->params;

  GdkRGBA color;
  color.alpha = 1.0f;

  float WB_low_invert[3];
  for(size_t c = 0; c < 3; ++c) WB_low_invert[c] = 2.0f - p->wb_low[c];
  const float WB_low_max = v_maxf(WB_low_invert);
  for(size_t c = 0; c < 3; ++c) WB_low_invert[c] /= WB_low_max;

  color.red = WB_low_invert[0];
  color.green = WB_low_invert[1];
  color.blue = WB_low_invert[2];

  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->WB_low_picker), &color);
}

static void WB_low_picker_callback(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)self->gui_data;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  dt_iop_color_picker_reset(self, TRUE);

  GdkRGBA c;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &c);

  float RGB[3];
  RGB[0] = 2.0f - c.red;
  RGB[1] = 2.0f - c.green;
  RGB[2] = 2.0f - c.blue;

  float RGB_min = v_minf(RGB);
  for(size_t k = 0; k < 3; k++) p->wb_low[k] = RGB[k] / RGB_min;

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->wb_low_R, p->wb_low[0]);
  dt_bauhaus_slider_set(g->wb_low_G, p->wb_low[1]);
  dt_bauhaus_slider_set(g->wb_low_B, p->wb_low[2]);
  darktable.gui->reset = reset;

  WB_low_picker_update(self);
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void WB_high_picker_update(dt_iop_module_t *self)
{
  dt_iop_negadoctor_gui_data_t *const g = (dt_iop_negadoctor_gui_data_t *)self->gui_data;
  const dt_iop_negadoctor_params_t *const p = (dt_iop_negadoctor_params_t *)self->params;

  GdkRGBA color;
  color.alpha = 1.0f;

  float WB_high_invert[3];
  for(size_t c = 0; c < 3; ++c) WB_high_invert[c] = 2.0f - p->wb_high[c];
  const float WB_high_max = v_maxf(WB_high_invert);
  for(size_t c = 0; c < 3; ++c) WB_high_invert[c] /= WB_high_max;

  color.red = WB_high_invert[0];
  color.green = WB_high_invert[1];
  color.blue = WB_high_invert[2];

  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->WB_high_picker), &color);
}

static void WB_high_picker_callback(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)self->gui_data;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  dt_iop_color_picker_reset(self, TRUE);

  GdkRGBA c;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &c);

  float RGB[3];
  RGB[0] = 2.0f - c.red;
  RGB[1] = 2.0f - c.green;
  RGB[2] = 2.0f - c.blue;

  float RGB_min = v_minf(RGB);
  for(size_t k = 0; k < 3; k++) p->wb_high[k] = RGB[k] / RGB_min;

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->wb_high_R, p->wb_high[0]);
  dt_bauhaus_slider_set(g->wb_high_G, p->wb_high[1]);
  dt_bauhaus_slider_set(g->wb_high_B, p->wb_high[2]);
  darktable.gui->reset = reset;

  WB_high_picker_update(self);
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


/* Color pickers auto-tuners */

// measure Dmin from the film edges first
static void apply_auto_Dmin(dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)self->gui_data;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  for(int k = 0; k < 4; k++) p->Dmin[k] = self->picked_color[k];

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->Dmin_R, p->Dmin[0] * 100.0f); // warning: GUI is in %
  dt_bauhaus_slider_set(g->Dmin_G, p->Dmin[1] * 100.0f); // warning: GUI is in %
  dt_bauhaus_slider_set(g->Dmin_B, p->Dmin[2] * 100.0f); // warning: GUI is in %
  darktable.gui->reset = reset;

  Dmin_picker_update(self);
  dt_control_queue_redraw_widget(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// from Dmin, find out the range of density values of the film and compute Dmax
static void apply_auto_Dmax(dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)self->gui_data;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  float RGB[3];
  for(int c = 0; c < 3; c++)
  {
    RGB[c] = log10f(p->Dmin[c] / fmaxf(self->picked_color_min[c], THRESHOLD));
  }

  // Take the max(RGB) for safety. Big values unclip whites
  p->D_max = v_maxf(RGB);

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->D_max, p->D_max);
  darktable.gui->reset = reset;

  dt_control_queue_redraw_widget(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// from Dmax, compute the offset so the range of density is rescaled between [0; 1]
static void apply_auto_offset(dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)self->gui_data;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  float RGB[3];
  for(int c = 0; c < 3; c++)
    RGB[c] = log10f(p->Dmin[c] / fmaxf(self->picked_color_max[c], THRESHOLD)) / p->D_max;

  // Take the min(RGB) for safety. Negative values unclip blacks
  p->offset = v_minf(RGB);

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->offset, p->offset); // warning: GUI is in %
  darktable.gui->reset = reset;

  dt_control_queue_redraw_widget(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// from Dmax and offset, compute the white balance correction as multipliers of the offset
// such that offset × wb[c] make black monochrome
static void apply_auto_WB_low(dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)self->gui_data;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  float RGB_min[3];
  for(int c = 0; c < 3; c++)
    RGB_min[c] = log10f(p->Dmin[c] / fmaxf(self->picked_color[c], THRESHOLD)) / p->D_max;

  const float RGB_v_min = v_minf(RGB_min); // warning: can be negative
  for(int c = 0; c < 3; c++) p->wb_low[c] =  RGB_v_min / RGB_min[c];

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->wb_low_R, p->wb_low[0]);
  dt_bauhaus_slider_set(g->wb_low_G, p->wb_low[1]);
  dt_bauhaus_slider_set(g->wb_low_B, p->wb_low[2]);
  darktable.gui->reset = reset;

  WB_low_picker_update(self);
  dt_control_queue_redraw_widget(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// from Dmax, offset and white balance multipliers, compute the white balance of the illuminant as multipliers of 1/Dmax
// such that WB[c] / Dmax make white monochrome
static void apply_auto_WB_high(dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)self->gui_data;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  float RGB_min[3];
  for(int c = 0; c < 3; c++)
    RGB_min[c] = fabsf(-1.0f / (p->offset * p->wb_low[c] - log10f(p->Dmin[c] / fmaxf(self->picked_color[c], THRESHOLD)) / p->D_max));

  const float RGB_v_min = v_minf(RGB_min); // warning : must be positive
  for(int c = 0; c < 3; c++) p->wb_high[c] = RGB_min[c] / RGB_v_min;

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->wb_high_R, p->wb_high[0]);
  dt_bauhaus_slider_set(g->wb_high_G, p->wb_high[1]);
  dt_bauhaus_slider_set(g->wb_high_B, p->wb_high[2]);
  darktable.gui->reset = reset;

  WB_high_picker_update(self);
  dt_control_queue_redraw_widget(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// from Dmax, offset and both white balances, compute the print black adjustment
// such that the printed values range from 0 to + infinity
static void apply_auto_black(dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)self->gui_data;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  float RGB[3];
  for(int c = 0; c < 3; c++)
  {
    RGB[c] = -log10f(p->Dmin[c] / fmaxf(self->picked_color_max[c], THRESHOLD));
    RGB[c] *= p->wb_high[c] / p->D_max;
    RGB[c] += p->wb_low[c] * p->offset * p->wb_high[c];
    RGB[c] = 0.1f - (1.0f - fast_exp10f(RGB[c])); // actually, remap between -3.32 EV and infinity for safety because gamma comes later
  }
  p->black = v_maxf(RGB);

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->black, p->black * 100.0f);
  darktable.gui->reset = reset;

  dt_control_queue_redraw_widget(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// from Dmax, offset, both white balances, and printblack, compute the print exposure adjustment as a scaling factor
// such that the printed values range from 0 to 1
static void apply_auto_exposure(dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)self->gui_data;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  float RGB[3];
  for(int c = 0; c < 3; c++)
  {
    RGB[c] = -log10f(p->Dmin[c] / fmaxf(self->picked_color_min[c], THRESHOLD));
    RGB[c] *= p->wb_high[c] / p->D_max;
    RGB[c] += p->wb_low[c] * p->offset;
    RGB[c] = 0.96f / (1.0f - fast_exp10f(RGB[c]) + p->black); // actually, remap in [0; 0.96] for safety
  }
  p->exposure = v_minf(RGB);

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->exposure, log2f(p->exposure));
  darktable.gui->reset = reset;

  dt_control_queue_redraw_widget(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static int _iop_color_picker_get_set(dt_iop_module_t *self, GtkWidget *button)
{
  dt_iop_negadoctor_gui_data_t *g =  (dt_iop_negadoctor_gui_data_t *)self->gui_data;

  const int current_picker = g->color_picker.current_picker;

  g->color_picker.current_picker = DT_NEGADOCTOR_NONE;

  if(button == g->Dmin_sampler)
    g->color_picker.current_picker = DT_NEGADOCTOR_DMIN;
  else if(button == g->WB_high_sampler)
    g->color_picker.current_picker = DT_NEGADOCTOR_WB_high;
  else if(button == g->offset)
    g->color_picker.current_picker = DT_NEGADOCTOR_OFFSET;
  else if(button == g->D_max)
    g->color_picker.current_picker = DT_NEGADOCTOR_DMAX;
  else if(button == g->WB_low_sampler)
    g->color_picker.current_picker = DT_NEGADOCTOR_WB_low;
  else if(button == g->exposure)
    g->color_picker.current_picker = DT_NEGADOCTOR_EXPOSURE;
  else if(button == g->black)
    g->color_picker.current_picker = DT_NEGADOCTOR_BLACK;

  if (current_picker == g->color_picker.current_picker)
    return DT_COLOR_PICKER_ALREADY_SELECTED;
  else
    return g->color_picker.current_picker;
}

static void _iop_color_picker_apply(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece)
{
  if(self->dt->gui->reset) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)self->gui_data;

  switch(g->color_picker.current_picker)
  {
    case DT_NEGADOCTOR_DMIN:
      apply_auto_Dmin(self);
      break;
    case DT_NEGADOCTOR_WB_high:
      apply_auto_WB_high(self);
      break;
    case DT_NEGADOCTOR_OFFSET:
      apply_auto_offset(self);
      break;
    case DT_NEGADOCTOR_DMAX:
      apply_auto_Dmax(self);
      break;
    case DT_NEGADOCTOR_WB_low:
      apply_auto_WB_low(self);
      break;
    case DT_NEGADOCTOR_EXPOSURE:
      apply_auto_exposure(self);
      break;
    case DT_NEGADOCTOR_BLACK:
      apply_auto_black(self);
      break;
    default:
      break;
  }
}

static void _iop_color_picker_update(dt_iop_module_t *self)
{
  dt_iop_negadoctor_gui_data_t *const g =  (dt_iop_negadoctor_gui_data_t *)self->gui_data;
  const int which_colorpicker = g->color_picker.current_picker;
  dt_bauhaus_widget_set_quad_active(g->offset, which_colorpicker == DT_NEGADOCTOR_OFFSET);
  dt_bauhaus_widget_set_quad_active(g->D_max, which_colorpicker == DT_NEGADOCTOR_DMAX);
}


/** gui callbacks, these are needed. */

static void film_stock_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;
  p->film_stock = dt_bauhaus_combobox_get(w);
  toggle_stock_controls(self);
  Dmin_picker_update(self);
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void Dmin_R_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  if(p->film_stock == DT_FILMSTOCK_COLOR)
    p->Dmin[0] = dt_bauhaus_slider_get(w) / 100.0f;                             // warning: GUI is in %
  else if(p->film_stock == DT_FILMSTOCK_NB)
    p->Dmin[0] = p->Dmin[1] = p->Dmin[2] = dt_bauhaus_slider_get(w) / 100.0f;   // warning: GUI is in %

  Dmin_picker_update(self);
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void Dmin_G_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  p->Dmin[1] = dt_bauhaus_slider_get(w) / 100.0f;                               // warning: GUI is in %

  Dmin_picker_update(self);
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void Dmin_B_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  p->Dmin[2] = dt_bauhaus_slider_get(w) / 100.0f;                               // warning: GUI is in %

  Dmin_picker_update(self);
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void wb_high_R_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  p->wb_high[0] = dt_bauhaus_slider_get(w);

  WB_high_picker_update(self);
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void wb_high_G_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  p->wb_high[1] = dt_bauhaus_slider_get(w);

  WB_high_picker_update(self);
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void wb_high_B_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  p->wb_high[2] = dt_bauhaus_slider_get(w);

  WB_high_picker_update(self);
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void wb_low_R_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  p->wb_low[0] = dt_bauhaus_slider_get(w);

  WB_low_picker_update(self);
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void wb_low_G_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  p->wb_low[1] = dt_bauhaus_slider_get(w);

  WB_low_picker_update(self);
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void wb_low_B_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  p->wb_low[2] = dt_bauhaus_slider_get(w);

  WB_low_picker_update(self);
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void D_max_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;
  p->D_max = dt_bauhaus_slider_get(w);
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void offset_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;
  p->offset = dt_bauhaus_slider_get(w);
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void gamma_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;
  p->gamma = dt_bauhaus_slider_get(w);
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void soft_clip_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;
  p->soft_clip = dt_bauhaus_slider_get(w) / 100.0f;                             // warning: GUI is in %
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void exposure_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;
  p->exposure = powf(2.0f, dt_bauhaus_slider_get(w));                           // warning: GUI is in EV
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void black_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;
  p->black = dt_bauhaus_slider_get(w) / 100.0f;                                 // warning: GUI is in %
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


void gui_init(dt_iop_module_t *self)
{
  // init the slider (more sophisticated layouts are possible with gtk tables and boxes):
  self->gui_data = malloc(sizeof(dt_iop_negadoctor_gui_data_t));
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)self->gui_data;
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  // Film emulsion

  g->film_stock = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->film_stock, NULL, _("film stock"));
  dt_bauhaus_combobox_add(g->film_stock, _("black and white"));
  dt_bauhaus_combobox_add(g->film_stock, _("color"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->film_stock), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->film_stock), "value-changed", G_CALLBACK(film_stock_callback), self);
  gtk_widget_set_tooltip_text(g->film_stock, _("toogle on or off the color controls"));

  g->notebook = GTK_NOTEBOOK(gtk_notebook_new());
  GtkWidget *page1 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  GtkWidget *page2 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  GtkWidget *page3 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));

  gtk_notebook_append_page(GTK_NOTEBOOK(g->notebook), page1, gtk_label_new(_("film properties")));
  gtk_notebook_append_page(GTK_NOTEBOOK(g->notebook), page2, gtk_label_new(_("corrections")));
  gtk_notebook_append_page(GTK_NOTEBOOK(g->notebook), page3, gtk_label_new(_("print properties")));

  gtk_widget_show_all(GTK_WIDGET(gtk_notebook_get_nth_page(g->notebook, 0)));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->notebook), FALSE, FALSE, 0);

  dtgtk_justify_notebook_tabs(g->notebook);

  // Dmin

  gtk_box_pack_start(GTK_BOX(page1), dt_ui_section_label_new(_("color of the film base")), FALSE, FALSE, 0);

  GtkWidget *row1 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));

  GdkRGBA color1 = (GdkRGBA){.red = p->Dmin[0], .green = p->Dmin[0], .blue = p->Dmin[0], .alpha = 1.0 };

  g->Dmin_picker = gtk_color_button_new_with_rgba(&color1);
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->Dmin_picker), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->Dmin_picker), _("select color of film material from a swatch"));
  gtk_box_pack_start(GTK_BOX(row1), GTK_WIDGET(g->Dmin_picker), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->Dmin_picker), "color-set", G_CALLBACK(Dmin_picker_callback), self);

  g->Dmin_sampler = dtgtk_button_new(dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT, NULL);
  gtk_widget_set_tooltip_text(g->Dmin_sampler , _("pick color of film material from image"));
  gtk_box_pack_start(GTK_BOX(row1), GTK_WIDGET(g->Dmin_sampler), FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->Dmin_sampler ), "clicked", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);

  gtk_box_pack_start(GTK_BOX(page1), GTK_WIDGET(row1), FALSE, FALSE, 0);

  g->Dmin_R = dt_bauhaus_slider_new_with_range(self, 0.001, 150., 0.25, p->Dmin[0] * 100, 2);
  dt_bauhaus_slider_set_format(g->Dmin_R, "%.2f %%");
  dt_bauhaus_widget_set_label(g->Dmin_R, NULL, _("D min red component"));
  gtk_widget_set_tooltip_text(g->Dmin_R, _("adjust the color and shade of the film transparent base.\n"
                                           "this value depends on the film material, \n"
                                           "the chemical fog produced while developing the film,\n"
                                           "and the scanner white balance."));
  gtk_box_pack_start(GTK_BOX(page1), GTK_WIDGET(g->Dmin_R), FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->Dmin_R), "value-changed", G_CALLBACK(Dmin_R_callback), self);

  g->Dmin_G = dt_bauhaus_slider_new_with_range(self, 0.001, 150., 0.25, p->Dmin[1] * 100, 2);
  dt_bauhaus_slider_set_format(g->Dmin_G, "%.2f %%");
  dt_bauhaus_widget_set_label(g->Dmin_G, NULL, _("D min green component"));
  gtk_widget_set_tooltip_text(g->Dmin_G, _("adjust the color and shade of the film transparent base.\n"
                                           "this value depends on the film material, \n"
                                           "the chemical fog produced while developing the film,\n"
                                           "and the scanner white balance."));
  gtk_box_pack_start(GTK_BOX(page1), GTK_WIDGET(g->Dmin_G), FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->Dmin_G), "value-changed", G_CALLBACK(Dmin_G_callback), self);

  g->Dmin_B = dt_bauhaus_slider_new_with_range(self, 0.001, 150., 0.25, p->Dmin[2] * 100, 2);
  dt_bauhaus_slider_set_format(g->Dmin_B, "%.2f %%");
  dt_bauhaus_widget_set_label(g->Dmin_B, NULL, _("D min blue component"));
  gtk_widget_set_tooltip_text(g->Dmin_B, _("adjust the color and shade of the film transparent base.\n"
                                           "this value depends on the film material, \n"
                                           "the chemical fog produced while developing the film,\n"
                                           "and the scanner white balance."));
  gtk_box_pack_start(GTK_BOX(page1), GTK_WIDGET(g->Dmin_B), FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->Dmin_B), "value-changed", G_CALLBACK(Dmin_B_callback), self);

  // D max and scanner bias

  gtk_box_pack_start(GTK_BOX(page1), dt_ui_section_label_new(_("dynamic range of the film")), FALSE, FALSE, 0);

  g->D_max = dt_bauhaus_slider_new_with_range(self, 0.1, 6.0, 0.01, p->D_max, 2);
  dt_bauhaus_slider_set_format(g->D_max, "%.2f dB");
  dt_bauhaus_widget_set_label(g->D_max, NULL, _("D max"));
  dt_bauhaus_widget_set_quad_paint(g->D_max, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->D_max, TRUE);
  g_signal_connect(G_OBJECT(g->D_max), "quad-pressed", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);
  gtk_widget_set_tooltip_text(g->D_max, _("maximum density of the film, corresponding to white after inversion.\n"
                                          "this value depends on the film specifications, the developing process,\n"
                                          "the dynamic range of the scene and the scanner exposure settings."));
  gtk_box_pack_start(GTK_BOX(page1), GTK_WIDGET(g->D_max), FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->D_max), "value-changed", G_CALLBACK(D_max_callback), self);

  gtk_box_pack_start(GTK_BOX(page1), dt_ui_section_label_new(_("scanner exposure settings")), FALSE, FALSE, 0);

  g->offset = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.01, p->offset, 2);
  dt_bauhaus_slider_set_format(g->offset, "%+.2f dB");
  dt_bauhaus_widget_set_label(g->offset, NULL, _("scan exposure bias"));
  dt_bauhaus_widget_set_quad_paint(g->offset, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->offset, TRUE);
  g_signal_connect(G_OBJECT(g->offset), "quad-pressed", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);
  gtk_widget_set_tooltip_text(g->offset, _("correct the exposure of the scanner, for all RGB channels,\n"
                                           "before the inversion, so blacks are neither clipped or too pale."));
  gtk_box_pack_start(GTK_BOX(page1), GTK_WIDGET(g->offset), FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->offset), "value-changed", G_CALLBACK(offset_callback), self);

  // WB shadows
  gtk_box_pack_start(GTK_BOX(page2), dt_ui_section_label_new(_("shadows color cast")), FALSE, FALSE, 0);

  GtkWidget *row3 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));

  const float max_wb_low = fmaxf(fmaxf(p->wb_low[0], p->wb_low[1]), p->wb_low[2]);
  GdkRGBA color3 = (GdkRGBA){.red = p->wb_low[0] / max_wb_low, .green = p->wb_low[0] / max_wb_low, .blue = p->wb_high[0] / max_wb_low, .alpha = 1.0 };

  g->WB_low_picker = gtk_color_button_new_with_rgba(&color3);
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->WB_low_picker), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->WB_low_picker), _("select color of shadows from a swatch"));
  gtk_box_pack_start(GTK_BOX(row3), GTK_WIDGET(g->WB_low_picker), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->WB_low_picker), "color-set", G_CALLBACK(WB_low_picker_callback), self);

  g->WB_low_sampler = dtgtk_button_new(dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT, NULL);
  gtk_widget_set_tooltip_text(g->WB_low_sampler , _("pick shadows color from image"));
  gtk_box_pack_start(GTK_BOX(row3), GTK_WIDGET(g->WB_low_sampler), FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->WB_low_sampler), "clicked", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);

  gtk_box_pack_start(GTK_BOX(page2), GTK_WIDGET(row3), FALSE, FALSE, 0);

  g->wb_low_R = dt_bauhaus_slider_new_with_range(self, 0.25, 2., 0.01, p->wb_low[0], 2);
  dt_bauhaus_widget_set_label(g->wb_low_R, NULL, _("shadows red offset"));
  gtk_widget_set_tooltip_text(g->wb_low_R, _("correct the color cast in shadows so blacks are\n"
                                             "truly achromatic. Setting this value before\n"
                                             "the highlights illuminant white balance will help\n"
                                             "recovering the global white balance in difficult cases."));
  gtk_box_pack_start(GTK_BOX(page2), GTK_WIDGET(g->wb_low_R), FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->wb_low_R), "value-changed", G_CALLBACK(wb_low_R_callback), self);

  g->wb_low_G = dt_bauhaus_slider_new_with_range(self, 0.25, 2., 0.01, p->wb_low[1], 2);
  dt_bauhaus_widget_set_label(g->wb_low_G, NULL, _("shadows green offset"));
  gtk_widget_set_tooltip_text(g->wb_low_G, _("correct the color cast in shadows so blacks are\n"
                                             "truly achromatic. Setting this value before\n"
                                             "the highlights illuminant white balance will help\n"
                                             "recovering the global white balance in difficult cases."));
  gtk_box_pack_start(GTK_BOX(page2), GTK_WIDGET(g->wb_low_G), FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->wb_low_G), "value-changed", G_CALLBACK(wb_low_G_callback), self);

  g->wb_low_B = dt_bauhaus_slider_new_with_range(self, 0.25, 2., 0.01, p->wb_low[2], 2);
  dt_bauhaus_widget_set_label(g->wb_low_B, NULL, _("shadows blue offset"));
  gtk_widget_set_tooltip_text(g->wb_low_B, _("correct the color cast in shadows so blacks are\n"
                                             "truly achromatic. Setting this value before\n"
                                             "the highlights illuminant white balance will help\n"
                                             "recovering the global white balance in difficult cases."));
  gtk_box_pack_start(GTK_BOX(page2), GTK_WIDGET(g->wb_low_B), FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->wb_low_B), "value-changed", G_CALLBACK(wb_low_B_callback), self);

  // WB highlights
  gtk_box_pack_start(GTK_BOX(page2), dt_ui_section_label_new(_("highlights white balance")), FALSE, FALSE, 0);

  GtkWidget *row2 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));

  const float max_wb_high = fmaxf(fmaxf(p->wb_high[0], p->wb_high[1]), p->wb_high[2]);
  GdkRGBA color2 = (GdkRGBA){.red = p->wb_high[0] / max_wb_high, .green = p->wb_high[0] / max_wb_high, .blue = p->wb_high[0] / max_wb_high, .alpha = 1.0 };

  g->WB_high_picker = gtk_color_button_new_with_rgba(&color2);
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->WB_high_picker), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->WB_high_picker), _("select color of illuminant from a swatch"));
  gtk_box_pack_start(GTK_BOX(row2), GTK_WIDGET(g->WB_high_picker), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->WB_high_picker), "color-set", G_CALLBACK(WB_high_picker_callback), self);

  g->WB_high_sampler = dtgtk_button_new(dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT, NULL);
  gtk_widget_set_tooltip_text(g->WB_high_sampler , _("pick illuminant color from image"));
  gtk_box_pack_start(GTK_BOX(row2), GTK_WIDGET(g->WB_high_sampler), FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->WB_high_sampler), "clicked", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);

  gtk_box_pack_start(GTK_BOX(page2), GTK_WIDGET(row2), FALSE, FALSE, 0);

  g->wb_high_R = dt_bauhaus_slider_new_with_range(self, 0.25, 2., 0.01, p->wb_high[0], 2);
  dt_bauhaus_widget_set_label(g->wb_high_R, NULL, _("illuminant red gain"));
  gtk_widget_set_tooltip_text(g->wb_high_R, _("correct the color of the illuminant so whites are\n"
                                              "truly achromatic. Setting this value after\n"
                                              "the shadows color cast will help\n"
                                              "recovering the global white balance in difficult cases."));
  gtk_box_pack_start(GTK_BOX(page2), GTK_WIDGET(g->wb_high_R), FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->wb_high_R), "value-changed", G_CALLBACK(wb_high_R_callback), self);

  g->wb_high_G = dt_bauhaus_slider_new_with_range(self, 0.25, 2., 0.01, p->wb_high[1], 2);
  dt_bauhaus_widget_set_label(g->wb_high_G, NULL, _("illuminant green gain"));
  gtk_widget_set_tooltip_text(g->wb_high_G, _("correct the color of the illuminant so whites are\n"
                                              "truly achromatic. Setting this value after\n"
                                              "the shadows color cast will help\n"
                                              "recovering the global white balance in difficult cases."));
  gtk_box_pack_start(GTK_BOX(page2), GTK_WIDGET(g->wb_high_G), FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->wb_high_G), "value-changed", G_CALLBACK(wb_high_G_callback), self);

  g->wb_high_B = dt_bauhaus_slider_new_with_range(self, 0.25, 2., 0.01, p->wb_high[2], 2);
  dt_bauhaus_widget_set_label(g->wb_high_B, NULL, _("illuminant blue gain"));
  gtk_widget_set_tooltip_text(g->wb_high_B, _("correct the color of the illuminant so whites are\n"
                                              "truly achromatic. Setting this value after\n"
                                              "the shadows color cast will help\n"
                                              "recovering the global white balance in difficult cases."));
  gtk_box_pack_start(GTK_BOX(page2), GTK_WIDGET(g->wb_high_B), FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->wb_high_B), "value-changed", G_CALLBACK(wb_high_B_callback), self);

  // print corrections
  gtk_box_pack_start(GTK_BOX(page3), dt_ui_section_label_new(_("virtual paper properties")), FALSE, FALSE, 0);

  g->black = dt_bauhaus_slider_new_with_range(self, -50., 50., 0.05, p->black * 100, 2);
  dt_bauhaus_slider_set_format(g->black, "%+.2f %%");
  dt_bauhaus_widget_set_label(g->black, NULL, _("paper black (density correction)"));
  dt_bauhaus_widget_set_quad_paint(g->black, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->black, TRUE);
  g_signal_connect(G_OBJECT(g->black), "quad-pressed", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);
  gtk_widget_set_tooltip_text(g->black, _("correct the density of black after the inversion,\n"
                                          "to adjust the global contrast while avoiding clipping shadows."));
  gtk_box_pack_start(GTK_BOX(page3), GTK_WIDGET(g->black), FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->black), "value-changed", G_CALLBACK(black_callback), self);

  g->gamma = dt_bauhaus_slider_new_with_range(self, 1., 8.0, 0.05, p->gamma, 2);
  dt_bauhaus_widget_set_label(g->gamma, NULL, _("paper grade (gamma)"));
  gtk_box_pack_start(GTK_BOX(page3), GTK_WIDGET(g->gamma), FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(g->gamma, _("select the grade of the virtual paper, which is actually\n"
                                          "equivalent to applying a gamma. it compensates the film D max\n"
                                          "and recovers the contrast. use a high grade for high D max."));
  g_signal_connect(G_OBJECT(g->gamma), "value-changed", G_CALLBACK(gamma_callback), self);

  g->soft_clip = dt_bauhaus_slider_new_with_range(self, 0.01, 100.0, 1., p->soft_clip * 100, 2);
  dt_bauhaus_slider_set_format(g->soft_clip, "%.2f %%");
  dt_bauhaus_widget_set_label(g->soft_clip, NULL, _("paper gloss (specular highlights)"));
  gtk_box_pack_start(GTK_BOX(page3), GTK_WIDGET(g->soft_clip), FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(g->soft_clip, _("gradually compress specular highlights past this value\n"
                                              "to avoid clipping while pushing the exposure for midtones.\n"
                                              "this somewhat reproduces the behaviour of matte paper."));
  g_signal_connect(G_OBJECT(g->soft_clip), "value-changed", G_CALLBACK(soft_clip_callback), self);

  gtk_box_pack_start(GTK_BOX(page3), dt_ui_section_label_new(_("virtual print emulation")), FALSE, FALSE, 0);

  g->exposure = dt_bauhaus_slider_new_with_range(self, -1., +1., 0.01, log2f(p->exposure), 2);
  dt_bauhaus_slider_set_format(g->exposure, "%+.2f EV");
  dt_bauhaus_widget_set_label(g->exposure, NULL, _("print exposure adjustment"));
  dt_bauhaus_widget_set_quad_paint(g->exposure, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->exposure, TRUE);
  g_signal_connect(G_OBJECT(g->exposure), "quad-pressed", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);
  gtk_widget_set_tooltip_text(g->exposure, _("correct the printing exposure after inversion to adjust\n"
                                             "the global contrast and avoid clipping highlights."));
  gtk_box_pack_start(GTK_BOX(page3), GTK_WIDGET(g->exposure), FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->exposure), "value-changed", G_CALLBACK(exposure_callback), self);

  toggle_stock_controls(self);

  dt_iop_init_picker(&g->color_picker,
                      self,
                      DT_COLOR_PICKER_AREA,
                      _iop_color_picker_get_set,
                      _iop_color_picker_apply,
                      _iop_color_picker_update);
}


void gui_update(dt_iop_module_t *const self)
{
  // let gui slider match current parameters:
  dt_iop_negadoctor_gui_data_t *const g = (dt_iop_negadoctor_gui_data_t *)self->gui_data;
  const dt_iop_negadoctor_params_t *const p = (dt_iop_negadoctor_params_t *)self->params;

  dt_iop_color_picker_reset(self, TRUE);

  self->color_picker_box[0] = self->color_picker_box[1] = .10f;
  self->color_picker_box[2] = self->color_picker_box[3] = .50f;
  self->color_picker_point[0] = self->color_picker_point[1] = 0.5f;

  dt_bauhaus_combobox_set(g->film_stock, p->film_stock);

  // Dmin - warning: GUI is in %
  dt_bauhaus_slider_set(g->Dmin_R, p->Dmin[0] * 100.0f);
  dt_bauhaus_slider_set(g->Dmin_G, p->Dmin[1] * 100.0f);
  dt_bauhaus_slider_set(g->Dmin_B, p->Dmin[2] * 100.0f);

  // Dmax
  dt_bauhaus_slider_set(g->D_max, p->D_max);

  // Scanner exposure offset
  dt_bauhaus_slider_set(g->offset, p->offset);

  // WB_high
  dt_bauhaus_slider_set(g->wb_high_R, p->wb_high[0]);
  dt_bauhaus_slider_set(g->wb_high_G, p->wb_high[1]);
  dt_bauhaus_slider_set(g->wb_high_B, p->wb_high[2]);

  // WB_high
  dt_bauhaus_slider_set(g->wb_low_R, p->wb_low[0]);
  dt_bauhaus_slider_set(g->wb_low_G, p->wb_low[1]);
  dt_bauhaus_slider_set(g->wb_low_B, p->wb_low[2]);

  // Print
  dt_bauhaus_slider_set(g->exposure, log2f(p->exposure));     // warning: GUI is in EV
  dt_bauhaus_slider_set(g->black, p->black * 100.0f);         // warning: GUI is in %
  dt_bauhaus_slider_set(g->gamma, p->gamma);
  dt_bauhaus_slider_set(g->soft_clip, p->soft_clip * 100.0f); // warning: GUI is in %

  // Update custom stuff
  toggle_stock_controls(self);
  Dmin_picker_update(self);
  WB_high_picker_update(self);
  WB_low_picker_update(self);
}


void gui_cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}


void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  if(!in) dt_iop_color_picker_reset(self, TRUE);
}


void gui_reset(dt_iop_module_t *self)
{
  dt_iop_color_picker_reset(self, TRUE);
}

