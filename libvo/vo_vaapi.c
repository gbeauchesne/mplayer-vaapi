/*
 * VA API output module
 *
 * Copyright (C) 2008-2009 Splitted-Desktop Systems
 *
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "mp_msg.h"
#include "help_mp.h"
#include "subopt-helper.h"
#include "video_out.h"
#include "video_out_internal.h"
#include "fastmemcpy.h"
#include "sub.h"
#include "x11_common.h"
#include "libavutil/common.h"
#include "libavcodec/vaapi.h"
#include "gui/interface.h"
#include "stats.h"
#include "libass/ass_mp.h"
#include <stdarg.h>

#if CONFIG_GL
#include "gl_common.h"
#include <GL/glu.h>
#include <GL/glx.h>
#endif

#include <assert.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <va/va_x11.h>
#if CONFIG_VAAPI_GLX
#include <va/va_glx.h>
#endif

static vo_info_t info = {
    "VA API with X11",
    "vaapi",
    "Gwenole Beauchesne <gbeauchesne@splitted-desktop.com>",
    ""
};

const LIBVO_EXTERN(vaapi)

/* Numbers of video surfaces */
#define MAX_OUTPUT_SURFACES       2 /* Maintain synchronisation points in flip_page() */
#define MAX_VIDEO_SURFACES       21 /* Maintain free surfaces in a queue (use least-recently-used) */
#define NUM_VIDEO_SURFACES_MPEG2  3 /* 1 decode frame, up to  2 references */
#define NUM_VIDEO_SURFACES_MPEG4  3 /* 1 decode frame, up to  2 references */
#define NUM_VIDEO_SURFACES_H264  21 /* 1 decode frame, up to 20 references */
#define NUM_VIDEO_SURFACES_VC1    3 /* 1 decode frame, up to  2 references */

typedef void (*draw_alpha_func)(int x0, int y0, int w, int h,
                                unsigned char *src, unsigned char *srca,
                                int stride);

typedef void (*eosd_draw_alpha_func)(unsigned char *src,
                                     int src_w, int src_h, int src_stride,
                                     int dst_x, int dst_y,
                                     uint32_t color);

struct vaapi_surface {
    VASurfaceID id;
    VAImage     image;
};

struct vaapi_equalizer {
    VADisplayAttribute brightness;
    VADisplayAttribute contrast;
    VADisplayAttribute hue;
    VADisplayAttribute saturation;
};

static int                      g_is_paused;
static uint32_t                 g_image_width;
static uint32_t                 g_image_height;
static uint32_t                 g_image_format;
static struct vo_rect           g_output_rect;
static struct vaapi_surface    *g_output_surfaces[MAX_OUTPUT_SURFACES];
static unsigned int             g_output_surface;
static int                      g_top_field_first;
static int                      g_deint;
static int                      g_deint_type;
static int                      g_colorspace;

#if CONFIG_GL
static MPGLContext              gl_context;
static int                      gl_enabled;
static int                      gl_binding;
static int                      gl_reflect;
static GLuint                   gl_texture;
static GLuint                   gl_font_base;
#endif

#if CONFIG_VAAPI_GLX
static int                      gl_visual_attr[] = {
    GLX_RGBA,
    GLX_RED_SIZE, 1,
    GLX_GREEN_SIZE, 1,
    GLX_BLUE_SIZE, 1,
    GLX_DOUBLEBUFFER,
    GL_NONE
};
static void                    *gl_surface;
#endif

static struct vaapi_context    *va_context;
static VAProfile               *va_profiles;
static int                      va_num_profiles;
static VAEntrypoint            *va_entrypoints;
static int                      va_num_entrypoints;
static VASurfaceID             *va_surface_ids;
static int                      va_num_surfaces;
static struct vaapi_surface   **va_free_surfaces;
static int                      va_free_surfaces_head_index;
static int                      va_free_surfaces_tail_index;
static VAImageFormat           *va_image_formats;
static int                      va_num_image_formats;
static VAImageFormat           *va_subpic_formats;
static unsigned int            *va_subpic_flags;
static int                      va_num_subpic_formats;
static VAImage                  va_osd_image;
static uint8_t                 *va_osd_image_data;
static struct vo_rect           va_osd_image_dirty_rect;
static VASubpictureID           va_osd_subpicture;
static int                      va_osd_associated;
static draw_alpha_func          va_osd_draw_alpha;
static uint8_t                 *va_osd_palette;
static struct vaapi_equalizer   va_equalizer;
static int                      va_eosd_used;
static VAImage                  va_eosd_image;
static uint8_t                 *va_eosd_image_data;
static VASubpictureID           va_eosd_subpicture;
static eosd_draw_alpha_func     va_eosd_draw_alpha;

///< Flag: direct surface mapping: use mpi->number to select free VA surface?
static int                      va_dm;

///< Flag: gather run-time statistics (CPU usage, frequency)
static int                      cpu_stats;
static unsigned int             cpu_frequency;
static float                    cpu_usage;

static int check_status(VAStatus status, const char *msg)
{
    if (status != VA_STATUS_SUCCESS) {
        mp_msg(MSGT_VO, MSGL_ERR, "[vo_vaapi] %s: %s\n", msg, vaErrorStr(status));
        return 0;
    }
    return 1;
}

static const char *string_of_VAImageFormat(VAImageFormat *imgfmt)
{
    static char str[5];
    str[0] = imgfmt->fourcc;
    str[1] = imgfmt->fourcc >> 8;
    str[2] = imgfmt->fourcc >> 16;
    str[3] = imgfmt->fourcc >> 24;
    str[4] = '\0';
    return str;
}

static const char *string_of_VAProfile(VAProfile profile)
{
    switch (profile) {
#define PROFILE(profile) \
        case VAProfile##profile: return "VAProfile" #profile
        PROFILE(MPEG2Simple);
        PROFILE(MPEG2Main);
        PROFILE(MPEG4Simple);
        PROFILE(MPEG4AdvancedSimple);
        PROFILE(MPEG4Main);
        PROFILE(H264Baseline);
        PROFILE(H264Main);
        PROFILE(H264High);
        PROFILE(VC1Simple);
        PROFILE(VC1Main);
        PROFILE(VC1Advanced);
#undef PROFILE
    }
    return "<unknown>";
}

static const char *string_of_VAEntrypoint(VAEntrypoint entrypoint)
{
    switch (entrypoint) {
#define ENTRYPOINT(entrypoint) \
        case VAEntrypoint##entrypoint: return "VAEntrypoint" #entrypoint
        ENTRYPOINT(VLD);
        ENTRYPOINT(IZZ);
        ENTRYPOINT(IDCT);
        ENTRYPOINT(MoComp);
        ENTRYPOINT(Deblocking);
#undef ENTRYPOINT
    }
    return "<unknown>";
}

static int has_profile(VAProfile profile)
{
    if (va_profiles && va_num_profiles > 0) {
        int i;
        for (i = 0; i < va_num_profiles; i++) {
            if (va_profiles[i] == profile)
                return 1;
        }
    }
    return 0;
}

static int VAProfile_from_imgfmt(uint32_t format)
{
    static const int mpeg2_profiles[] =
        { VAProfileMPEG2Main, VAProfileMPEG2Simple, -1 };
    static const int mpeg4_profiles[] =
        { VAProfileMPEG4Main, VAProfileMPEG4AdvancedSimple, VAProfileMPEG4Simple, -1 };
    static const int h264_profiles[] =
        { VAProfileH264High, VAProfileH264Main, VAProfileH264Baseline, -1 };
    static const int wmv3_profiles[] =
        { VAProfileVC1Main, VAProfileVC1Simple, -1 };
    static const int vc1_profiles[] =
        { VAProfileVC1Advanced, -1 };

    const int *profiles = NULL;
    switch (IMGFMT_VAAPI_CODEC(format)) {
    case IMGFMT_VAAPI_CODEC_MPEG2:
        profiles = mpeg2_profiles;
        break;
    case IMGFMT_VAAPI_CODEC_MPEG4:
        profiles = mpeg4_profiles;
        break;
    case IMGFMT_VAAPI_CODEC_H264:
        profiles = h264_profiles;
        break;
    case IMGFMT_VAAPI_CODEC_VC1:
        switch (format) {
        case IMGFMT_VAAPI_WMV3:
            profiles = wmv3_profiles;
            break;
        case IMGFMT_VAAPI_VC1:
            profiles = vc1_profiles;
            break;
        }
        break;
    }

    if (profiles) {
        for (int i = 0; profiles[i] != -1; i++) {
            if (has_profile(profiles[i]))
                return profiles[i];
        }
    }
    return -1;
}

static int has_entrypoint(VAEntrypoint entrypoint)
{
    if (va_entrypoints && va_num_entrypoints > 0) {
        int i;
        for (i = 0; i < va_num_entrypoints; i++) {
            if (va_entrypoints[i] == entrypoint)
                return 1;
        }
    }
    return 0;
}

static int VAEntrypoint_from_imgfmt(uint32_t format)
{
    int entrypoint = 0;
    switch (format) {
    case IMGFMT_VAAPI_MPEG2:
    case IMGFMT_VAAPI_MPEG4:
    case IMGFMT_VAAPI_H263:
    case IMGFMT_VAAPI_H264:
    case IMGFMT_VAAPI_WMV3:
    case IMGFMT_VAAPI_VC1:
        entrypoint = VAEntrypointVLD;
        break;
    case IMGFMT_VAAPI_MPEG2_IDCT:
        entrypoint = VAEntrypointIDCT;
        break;
    case IMGFMT_VAAPI_MPEG2_MOCO:
        entrypoint = VAEntrypointMoComp;
        break;
    }

    if (entrypoint)
        return has_entrypoint(entrypoint);

    return -1;
}

static VAImageFormat *find_image_format(uint32_t fourcc)
{
    if (va_image_formats && va_num_image_formats > 0) {
        int i;
        for (i = 0; i < va_num_image_formats; i++) {
            if (va_image_formats[i].fourcc == fourcc)
                return &va_image_formats[i];
        }
    }
    return NULL;
}

static VAImageFormat *VAImageFormat_from_imgfmt(uint32_t format)
{
    uint32_t fourcc = 0;

    switch (format) {
    case IMGFMT_NV12: fourcc = VA_FOURCC('N','V','1','2'); break;
    case IMGFMT_YV12: fourcc = VA_FOURCC('Y','V','1','2'); break;
    case IMGFMT_I420: fourcc = VA_FOURCC('I','4','2','0'); break;
    case IMGFMT_IYUV: fourcc = VA_FOURCC('I','Y','U','V'); break;
    }

    if (fourcc)
        return find_image_format(fourcc);

    return NULL;
}

static struct vaapi_surface *alloc_vaapi_surface(unsigned int width,
                                                 unsigned int height,
                                                 unsigned int format)
{
    struct vaapi_surface *surface = NULL;
    struct vaapi_surface **surfaces;
    VASurfaceID *surface_ids;
    VAStatus status;

    surface = malloc(sizeof(*surface));
    if (!surface)
        goto error;

    surfaces = realloc(va_free_surfaces,
                       (1 + va_num_surfaces) * sizeof(surfaces[0]));
    if (!surfaces)
        goto error;

    surface_ids = realloc(va_surface_ids,
                          (1 + va_num_surfaces) * sizeof(surface_ids[0]));
    if (!surface_ids)
        goto error;

    status = vaCreateSurfaces(va_context->display, width, height, format,
                              1, &surface->id);
    if (!check_status(status, "vaCreateSurfaces()"))
        goto error;

    va_surface_ids                    = surface_ids;
    va_surface_ids[va_num_surfaces]   = surface->id;
    va_free_surfaces                  = surfaces;
    va_free_surfaces[va_num_surfaces] = surface;
    surface->image.image_id           = VA_INVALID_ID;
    surface->image.buf                = VA_INVALID_ID;
    ++va_num_surfaces;
    return surface;
error:
    free(surface);
    return NULL;
}

static void resize(void)
{
    struct vo_rect src;

    calc_src_dst_rects(g_image_width, g_image_height,
                       &src, &g_output_rect, NULL, NULL);

    vo_x11_clearwindow(mDisplay, vo_window);

#if CONFIG_GL
#define FOVY     60.0f
#define ASPECT   1.0f
#define Z_NEAR   0.1f
#define Z_FAR    100.0f
#define Z_CAMERA 0.869f

    if (gl_enabled) {
        glViewport(0, 0, vo_dwidth, vo_dheight);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        gluPerspective(FOVY, ASPECT, Z_NEAR, Z_FAR);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        glTranslatef(-0.5f, -0.5f, -Z_CAMERA);
        glScalef(1.0f / (GLfloat)vo_dwidth,
                 -1.0f / (GLfloat)vo_dheight,
                 1.0f / (GLfloat)vo_dwidth);
        glTranslatef(0.0f, -1.0f * (GLfloat)vo_dheight, 0.0f);
    }
#endif

    flip_page();
}

#if CONFIG_GL
static int gl_build_font(void)
{
    XFontStruct *fi;

    gl_font_base = glGenLists(96);

    fi = XLoadQueryFont(mDisplay, "-adobe-helvetica-medium-r-normal--16-*-*-*-p-*-iso8859-1" );
    if (!fi) {
        fi = XLoadQueryFont(mDisplay, "fixed");
        if (!fi)
            return -1;
    }

    glXUseXFont(fi->fid, 32, 96, gl_font_base);
    XFreeFont(mDisplay, fi);
    return 0;
}

static void gl_printf(const char *format, ...)
{
    va_list args;
    char *text;
    int textlen;

    va_start(args, format);
    textlen = vsnprintf(NULL, 0, format, args);
    va_end(args);

    text = malloc(textlen + 1);
    if (!text)
        return;

    va_start(args, format);
    vsprintf(text, format, args);
    va_end(args);

    glPushAttrib(GL_LIST_BIT);
    glListBase(gl_font_base - 32);
    glCallLists(textlen, GL_UNSIGNED_BYTE, text);
    glPopAttrib();
    free(text);
}

static void gl_draw_rectangle(int x, int y, int w, int h, unsigned int rgba)
{
    glColor4f((GLfloat)((rgba >> 24) & 0xff) / 255.0,
              (GLfloat)((rgba >> 16) & 0xff) / 255.0,
              (GLfloat)((rgba >> 8) & 0xff) / 255.0,
              (GLfloat)(rgba & 0xff) / 255.0);

    glTranslatef((GLfloat)x, (GLfloat)y, 0.0f);
    glBegin(GL_QUADS);
    {
        glVertex2i(0, 0);
        glVertex2i(w, 0);
        glVertex2i(w, h);
        glVertex2i(0, h);
    }
    glEnd();
}
#endif

static inline unsigned char *get_osd_image_data(int x0, int y0)
{
    return (va_osd_image_data +
            va_osd_image.offsets[0] +
            va_osd_image.pitches[0] * y0 +
            x0 * ((va_osd_image.format.bits_per_pixel + 7) / 8));
}

static inline void set_osd_image_dirty_rect(int x, int y, int w, int h)
{
    struct vo_rect * const dirty_rect = &va_osd_image_dirty_rect;
    dirty_rect->left   = x + w;
    dirty_rect->top    = y + h;
    dirty_rect->right  = x;
    dirty_rect->bottom = y;
    dirty_rect->width  = w;
    dirty_rect->height = h;
}

static inline void update_osd_image_dirty_rect(int x, int y, int w, int h)
{
    struct vo_rect * const dirty_rect = &va_osd_image_dirty_rect;
    dirty_rect->left   = FFMIN(dirty_rect->left,   x);
    dirty_rect->top    = FFMIN(dirty_rect->top,    y);
    dirty_rect->right  = FFMAX(dirty_rect->right,  x + w);
    dirty_rect->bottom = FFMAX(dirty_rect->bottom, y + h);
    dirty_rect->width  = dirty_rect->right - dirty_rect->left;
    dirty_rect->height = dirty_rect->bottom - dirty_rect->top;
}

static void draw_alpha_rgb32(int x0, int y0, int w, int h,
                             unsigned char *src, unsigned char *srca,
                             int stride)
{
    int x, y;
    const unsigned int dststride = va_osd_image.pitches[0];
    unsigned char *dst = get_osd_image_data(x0, y0);

    update_osd_image_dirty_rect(x0, y0, w, h);

    for (y = 0; y < h; y++, dst += dststride, src += stride, srca += stride)
        for (x = 0; x < w; x++) {
            const unsigned char c = src[x];
            dst[4*x + 0] = c;
            dst[4*x + 1] = c;
            dst[4*x + 2] = c;
            dst[4*x + 3] = -srca[x];
        }
}

static void draw_alpha_IA44(int x0, int y0, int w, int h,
                            unsigned char *src, unsigned char *srca,
                            int stride)
{
    int x, y;
    const unsigned int dststride = va_osd_image.pitches[0];
    unsigned char *dst = get_osd_image_data(x0, y0);

    update_osd_image_dirty_rect(x0, y0, w, h);

    for (y = 0; y < h; y++, dst += dststride)
        for (x = 0; x < w; x++)
            dst[x] = (src[y*stride + x] & 0xf0) | (-srca[y*stride + x] >> 4);
}

static void draw_alpha_AI44(int x0, int y0, int w, int h,
                            unsigned char *src, unsigned char *srca,
                            int stride)
{
    int x, y;
    const unsigned int dststride = va_osd_image.pitches[0];
    unsigned char *dst = get_osd_image_data(x0, y0);

    update_osd_image_dirty_rect(x0, y0, w, h);

    for (y = 0; y < h; y++, dst += dststride)
        for (x = 0; x < w; x++)
            dst[x] = (src[y*stride + x] >> 4) | (-srca[y*stride + x] & 0xf0);
}

static void draw_alpha_IA88(int x0, int y0, int w, int h,
                            unsigned char *src, unsigned char *srca,
                            int stride)
{
    int x, y;
    const unsigned int dststride = va_osd_image.pitches[0];
    unsigned char *dst = get_osd_image_data(x0, y0);

    update_osd_image_dirty_rect(x0, y0, w, h);

    for (y = 0; y < h; y++, dst += dststride)
        for (x = 0; x < w; x++) {
            dst[2*x + 0] =  src [y*stride + x];
            dst[2*x + 1] = -srca[y*stride + x];
        }
}

static void draw_alpha_AI88(int x0, int y0, int w, int h,
                            unsigned char *src, unsigned char *srca,
                            int stride)
{
    int x, y;
    const unsigned int dststride = va_osd_image.pitches[0];
    unsigned char *dst = get_osd_image_data(x0, y0);

    update_osd_image_dirty_rect(x0, y0, w, h);

    for (y = 0; y < h; y++, dst += dststride)
        for (x = 0; x < w; x++) {
            dst[2*x + 0] = -srca[y*stride + x];
            dst[2*x + 1] =  src [y*stride + x];
        }
}

///< List of subpicture formats in preferred order
static const struct {
    uint32_t format;
    draw_alpha_func draw_alpha;
}
va_osd_info[] = {
    { VA_FOURCC('I','A','4','4'), draw_alpha_IA44  },
    { VA_FOURCC('A','I','4','4'), draw_alpha_AI44  },
    { VA_FOURCC('I','A','8','8'), draw_alpha_IA88  },
    { VA_FOURCC('A','I','8','8'), draw_alpha_AI88  },
    { VA_FOURCC('B','G','R','A'), draw_alpha_rgb32 },
    { VA_FOURCC('R','G','B','A'), draw_alpha_rgb32 },
    { 0, NULL }
};

static uint8_t *gen_osd_palette(const VAImage *image)
{
    uint8_t *palette;
    int i, is_rgb;
    int r_idx = -1, g_idx = -1, b_idx = -1;
    int y_idx = -1, u_idx = -1, v_idx = -1;

    if (image->num_palette_entries < 1)
        return NULL;

    palette = malloc(image->num_palette_entries * image->entry_bytes);
    if (!palette)
        return NULL;

    for (i = 0; i < image->entry_bytes; i++) {
        switch (image->component_order[i]) {
        case 'R': r_idx = i; is_rgb = 1; break;
        case 'G': g_idx = i; is_rgb = 1; break;
        case 'B': b_idx = i; is_rgb = 1; break;
        case 'Y': y_idx = i; is_rgb = 0; break;
        case 'U': u_idx = i; is_rgb = 0; break;
        case 'V': v_idx = i; is_rgb = 0; break;
        }
    }

    if (r_idx != -1 && g_idx != -1 && b_idx != -1) {      /* RGB format */
        for (i = 0; i < image->num_palette_entries; i++) {
            const int n = i * image->entry_bytes;
            palette[n + r_idx] = i * 0xff / (image->num_palette_entries - 1);
            palette[n + g_idx] = i * 0xff / (image->num_palette_entries - 1);
            palette[n + b_idx] = i * 0xff / (image->num_palette_entries - 1);
        }
    }
    else if (y_idx != -1 && u_idx != -1 && v_idx != -1) { /* YUV format */
        for (i = 0; i < image->num_palette_entries; i++) {
            const int n = i * image->entry_bytes;
            palette[n + y_idx] = i * 0xff / (image->num_palette_entries - 1);
            palette[n + u_idx] = 0xff;
            palette[n + v_idx] = 0xff;
        }
    }
    else {
        mp_msg(MSGT_VO, MSGL_ERR, "[vo_vaapi] Could not set up subpicture palette\n");
        free(palette);
        palette = NULL;
    }
    return palette;
}

static void disable_osd(void)
{
    if (!va_osd_associated)
        return;

    vaDeassociateSubpicture(va_context->display,
                            va_osd_subpicture,
                            va_surface_ids, va_num_surfaces);

    va_osd_associated = 0;
}

static int enable_osd(const struct vo_rect *src_rect,
                      const struct vo_rect *dst_rect)
{
    VAStatus status;

    disable_osd();

    status = vaAssociateSubpicture(va_context->display,
                                   va_osd_subpicture,
                                   va_surface_ids, va_num_surfaces,
                                   src_rect->left,
                                   src_rect->top,
                                   src_rect->right - src_rect->left,
                                   src_rect->bottom - src_rect->top,
                                   dst_rect->left,
                                   dst_rect->top,
                                   dst_rect->right - dst_rect->left,
                                   dst_rect->bottom - dst_rect->top,
                                   0);
    if (!check_status(status, "vaAssociateSubpicture()"))
        return -1;

    va_osd_associated = 1;
    return 0;
}

static inline unsigned char *get_eosd_image_data(int x0, int y0)
{
    return (va_eosd_image_data +
            va_eosd_image.offsets[0] +
            va_eosd_image.pitches[0] * y0 +
            x0 * ((va_eosd_image.format.bits_per_pixel + 7) / 8));
}

static void eosd_draw_alpha_rgba(unsigned char *src,
                                 int src_w, int src_h, int src_stride,
                                 int dst_x, int dst_y,
                                 uint32_t color)
{
    int x, y;
    const unsigned int dst_stride = va_eosd_image.pitches[0];
    unsigned char *dst = get_eosd_image_data(dst_x, dst_y);
    const unsigned int r = (color >> 24) & 0xff;
    const unsigned int g = (color >> 16) & 0xff;
    const unsigned int b = (color >>  8) & 0xff;
    const unsigned int a = 0xff - (color & 0xff);

    // XXX: handle dirty rects
    for (y = 0; y < src_h; y++, dst += dst_stride, src += src_stride)
        for (x = 0; x < src_w; x++) {
            const unsigned int v = src[x];
            dst[4*x + 0] = (r * v + dst[4*x + 0] * (0xff - v)) / 255;
            dst[4*x + 1] = (g * v + dst[4*x + 1] * (0xff - v)) / 255;
            dst[4*x + 2] = (b * v + dst[4*x + 2] * (0xff - v)) / 255;
            dst[4*x + 3] = (a * v + dst[4*x + 3] * (0xff - v)) / 255;
        }
}

///< List of subpicture formats in preferred order
static const struct {
    uint32_t format;
    eosd_draw_alpha_func draw_alpha;
}
va_eosd_info[] = {
    { VA_FOURCC('R','G','B','A'), eosd_draw_alpha_rgba },
    { 0, NULL }
};

static int is_direct_mapping_init(void)
{
    VADisplayAttribute attr;
    VAStatus status;

    if (va_dm < 2)
        return va_dm;

    /* If the driver doesn't make a copy of the VA surface for
       display, then we have to retain it until it's no longer the
       visible surface. In other words, if the driver is using
       DirectSurface mode, we don't want to decode the new surface
       into the previous one that was used for display. */
    attr.type  = VADisplayAttribDirectSurface;
    attr.flags = VA_DISPLAY_ATTRIB_GETTABLE;

    status = vaGetDisplayAttributes(va_context->display, &attr, 1);
    if (status == VA_STATUS_SUCCESS)
        return !attr.value;
    return 0;
}

static inline int is_direct_mapping(void)
{
    static int dm = -1;
    if (dm < 0) {
        dm = is_direct_mapping_init();
        if (dm)
            mp_msg(MSGT_VO, MSGL_INFO,
                   "[vo_vaapi] Using 1:1 VA surface mapping\n");
    }
    return dm;
}

static int int_012(int *n)
{
    return *n >= 0 && *n <= 2;
}

static const opt_t subopts[] = {
    { "dm",          OPT_ARG_INT,  &va_dm,        (opt_test_f)int_012 },
    { "stats",       OPT_ARG_BOOL, &cpu_stats,    NULL },
    { "deint",       OPT_ARG_INT,  &g_deint,      (opt_test_f)int_012 },
    { "colorspace",  OPT_ARG_INT,  &g_colorspace, (opt_test_f)int_012 },
#if CONFIG_GL
    { "gl",          OPT_ARG_BOOL, &gl_enabled,   NULL },
    { "bind",        OPT_ARG_BOOL, &gl_binding,   NULL },
    { "reflect",     OPT_ARG_BOOL, &gl_reflect,   NULL },
#endif
    { NULL, }
};

static int preinit(const char *arg)
{
    VADisplayAttribute *display_attrs;
    VAStatus status;
    int va_major_version, va_minor_version;
    int i, max_image_formats, max_subpic_formats, max_profiles;
    int num_display_attrs, max_display_attrs;

    va_dm = 2;
    g_deint = 0;
    g_deint_type = 2;
    g_colorspace = 1;
    if (subopt_parse(arg, subopts) != 0) {
        mp_msg(MSGT_VO, MSGL_FATAL,
               "\n-vo vaapi command line help:\n"
               "Example: mplayer -vo vaapi:gl\n"
               "\nOptions:\n"
               "  dm\n"
               "    0: use least-recently-used VA surface\n"
               "    1: identify VA surface with MPI index\n"
               "    2: auto-detect use of direct surface mapping (default)\n"
               "  deint (all modes > 0 respect -field-dominance)\n"
               "    0: no deinterlacing (default)\n"
               "    1: only show first field\n"
               "    2: bob deinterlacing\n"
               "  colorspace\n"
               "    0: guess based on video resolution\n"
               "    1: ITU-R BT.601 (default)\n"
               "    2: ITU-R BT.709\n"
#if CONFIG_GL
               "  gl\n"
               "    Enable OpenGL rendering\n"
               "  bind\n"
               "    Use VA surface binding instead of copy\n"
               "  reflect\n"
               "    Enable OpenGL reflection effects\n"
#endif
               "\n" );
        return -1;
    }
    if (g_deint)
        g_deint_type = g_deint;
#if CONFIG_GL
    if (gl_enabled)
        mp_msg(MSGT_VO, MSGL_INFO, "[vo_vaapi] Using OpenGL rendering%s\n",
               gl_reflect ? ", with reflection effects" : "");
#endif

    stats_init();

#if CONFIG_GL
    if (gl_enabled && !init_mpglcontext(&gl_context, GLTYPE_X11))
        return -1;
    else
#endif
    if (!vo_init())
        return -1;

    va_context = calloc(1, sizeof(*va_context));
    if (!va_context)
        return -1;

#if CONFIG_VAAPI_GLX
    if (gl_enabled)
        va_context->display = vaGetDisplayGLX(mDisplay);
    else
#endif
        va_context->display = vaGetDisplay(mDisplay);
    if (!va_context->display)
        return -1;
    mp_msg(MSGT_VO, MSGL_DBG2, "[vo_vaapi] preinit(): VA display %p\n", va_context->display);

    status = vaInitialize(va_context->display, &va_major_version, &va_minor_version);
    if (!check_status(status, "vaInitialize()"))
        return -1;
    mp_msg(MSGT_VO, MSGL_DBG2, "[vo_vaapi] preinit(): VA API version %d.%d\n",
           va_major_version, va_minor_version);

    max_image_formats = vaMaxNumImageFormats(va_context->display);
    va_image_formats = calloc(max_image_formats, sizeof(*va_image_formats));
    if (!va_image_formats)
        return -1;
    status = vaQueryImageFormats(va_context->display, va_image_formats, &va_num_image_formats);
    if (!check_status(status, "vaQueryImageFormats()"))
        return -1;
    mp_msg(MSGT_VO, MSGL_DBG2, "[vo_vaapi] preinit(): %d image formats available\n",
           va_num_image_formats);
    for (i = 0; i < va_num_image_formats; i++)
        mp_msg(MSGT_VO, MSGL_DBG2, "  %s\n", string_of_VAImageFormat(&va_image_formats[i]));

    max_subpic_formats = vaMaxNumSubpictureFormats(va_context->display);
    va_subpic_formats = calloc(max_subpic_formats, sizeof(*va_subpic_formats));
    if (!va_subpic_formats)
        return -1;
    va_subpic_flags = calloc(max_subpic_formats, sizeof(*va_subpic_flags));
    if (!va_subpic_flags)
        return -1;
    status = vaQuerySubpictureFormats(va_context->display, va_subpic_formats, va_subpic_flags, &va_num_subpic_formats);
    if (!check_status(status, "vaQuerySubpictureFormats()"))
        va_num_subpic_formats = 0; /* XXX: don't error out for IEGD */
    mp_msg(MSGT_VO, MSGL_DBG2, "[vo_vaapi] preinit(): %d subpicture formats available\n",
           va_num_subpic_formats);
    for (i = 0; i < va_num_subpic_formats; i++)
        mp_msg(MSGT_VO, MSGL_DBG2, "  %s, flags 0x%x\n", string_of_VAImageFormat(&va_subpic_formats[i]), va_subpic_flags[i]);

    max_profiles = vaMaxNumProfiles(va_context->display);
    va_profiles = calloc(max_profiles, sizeof(*va_profiles));
    if (!va_profiles)
        return -1;
    status = vaQueryConfigProfiles(va_context->display, va_profiles, &va_num_profiles);
    if (!check_status(status, "vaQueryConfigProfiles()"))
        return -1;
    mp_msg(MSGT_VO, MSGL_DBG2, "[vo_vaapi] preinit(): %d profiles available\n",
           va_num_profiles);
    for (i = 0; i < va_num_profiles; i++)
        mp_msg(MSGT_VO, MSGL_DBG2, "  %s\n", string_of_VAProfile(va_profiles[i]));

    va_osd_subpicture      = VA_INVALID_ID;
    va_osd_image.image_id  = VA_INVALID_ID;
    va_eosd_subpicture     = VA_INVALID_ID;
    va_eosd_image.image_id = VA_INVALID_ID;

    max_display_attrs = vaMaxNumDisplayAttributes(va_context->display);
    display_attrs = calloc(max_display_attrs, sizeof(*display_attrs));
    if (display_attrs) {
        num_display_attrs = 0;
        status = vaQueryDisplayAttributes(va_context->display,
                                          display_attrs, &num_display_attrs);
        if (check_status(status, "vaQueryDisplayAttributes()")) {
            for (i = 0; i < num_display_attrs; i++) {
                VADisplayAttribute *attr;
                switch (display_attrs[i].type) {
                case VADisplayAttribBrightness:
                    attr = &va_equalizer.brightness;
                    break;
                case VADisplayAttribContrast:
                    attr = &va_equalizer.contrast;
                    break;
                case VADisplayAttribHue:
                    attr = &va_equalizer.hue;
                    break;
                case VADisplayAttribSaturation:
                    attr = &va_equalizer.saturation;
                    break;
                default:
                    attr = NULL;
                    break;
                }
                if (attr)
                    *attr = display_attrs[i];
            }
        }
        free(display_attrs);
    }
    return 0;
}

static void free_video_specific(void)
{
    int i;

#if CONFIG_VAAPI_GLX
    if (gl_surface) {
        VAStatus status;
        status = vaDestroySurfaceGLX(va_context->display, gl_surface);
        check_status(status, "vaDestroySurfaceGLX()");
        gl_surface = NULL;
    }
#endif

    if (va_context && va_context->context_id) {
        vaDestroyContext(va_context->display, va_context->context_id);
        va_context->context_id = 0;
    }

    if (va_free_surfaces) {
        for (i = 0; i < va_num_surfaces; i++) {
            if (!va_free_surfaces[i])
                continue;
            if (va_free_surfaces[i]->image.image_id != VA_INVALID_ID) {
                vaDestroyImage(va_context->display,
                               va_free_surfaces[i]->image.image_id);
                va_free_surfaces[i]->image.image_id = VA_INVALID_ID;
            }
            free(va_free_surfaces[i]);
            va_free_surfaces[i] = NULL;
        }
        free(va_free_surfaces);
        va_free_surfaces = NULL;
        va_free_surfaces_head_index = 0;
        va_free_surfaces_tail_index = 0;
    }

    g_output_surface = 0;
    memset(g_output_surfaces, 0, sizeof(g_output_surfaces));

    if (va_osd_palette) {
        free(va_osd_palette);
        va_osd_palette = NULL;
    }

    disable_osd();

    if (va_eosd_subpicture != VA_INVALID_ID) {
        vaDestroySubpicture(va_context->display, va_eosd_subpicture);
        va_eosd_subpicture = VA_INVALID_ID;
    }

    if (va_eosd_image.image_id != VA_INVALID_ID) {
        vaDestroyImage(va_context->display, va_eosd_image.image_id);
        va_eosd_image.image_id = VA_INVALID_ID;
    }

    if (va_osd_subpicture != VA_INVALID_ID) {
        vaDestroySubpicture(va_context->display, va_osd_subpicture);
        va_osd_subpicture = VA_INVALID_ID;
    }

    if (va_osd_image.image_id != VA_INVALID_ID) {
        vaDestroyImage(va_context->display, va_osd_image.image_id);
        va_osd_image.image_id = VA_INVALID_ID;
    }

    if (va_surface_ids) {
        vaDestroySurfaces(va_context->display, va_surface_ids, va_num_surfaces);
        free(va_surface_ids);
        va_surface_ids = NULL;
        va_num_surfaces = 0;
    }

    if (va_context && va_context->config_id) {
        vaDestroyConfig(va_context->display, va_context->config_id);
        va_context->config_id = 0;
    }

    if (va_entrypoints) {
        free(va_entrypoints);
        va_entrypoints = NULL;
    }

#if CONFIG_GL
    if (gl_texture) {
        glDeleteTextures(1, &gl_texture);
        gl_texture = GL_NONE;
    }
#endif
}

static void uninit(void)
{
    if (!vo_config_count)
        return;

    free_video_specific();

    if (va_profiles) {
        free(va_profiles);
        va_profiles = NULL;
    }

    if (va_subpic_flags) {
        free(va_subpic_flags);
        va_subpic_flags = NULL;
    }

    if (va_subpic_formats) {
        free(va_subpic_formats);
        va_subpic_formats = NULL;
    }

    if (va_image_formats) {
        free(va_image_formats);
        va_image_formats = NULL;
    }

    if (va_context && va_context->display) {
        vaTerminate(va_context->display);
        va_context->display = NULL;
    }

    if (va_context) {
        free(va_context);
        va_context = NULL;
    }

#ifdef CONFIG_XF86VM
    vo_vm_close();
#endif
#if CONFIG_GL
    if (gl_enabled)
        uninit_mpglcontext(&gl_context);
    else
#endif
    vo_x11_uninit();

    stats_exit();
}

static int config_x11(uint32_t width, uint32_t height,
                      uint32_t display_width, uint32_t display_height,
                      uint32_t flags, char *title)
{
    Colormap cmap;
    XVisualInfo visualInfo;
    XVisualInfo *vi;
    XSetWindowAttributes xswa;
    unsigned long xswa_mask;
    XWindowAttributes wattr;
    int depth;

#ifdef CONFIG_GUI
    if (use_gui)
        guiGetEvent(guiSetShVideo, 0);  // the GUI will set up / resize our window
    else
#endif
    {
#ifdef CONFIG_XF86VM
        if (flags & VOFLAG_MODESWITCHING)
            vo_vm_switch();
#endif
        XGetWindowAttributes(mDisplay, DefaultRootWindow(mDisplay), &wattr);
        depth = wattr.depth;
        if (depth != 15 && depth != 16 && depth != 24 && depth != 32)
            depth = 24;
        XMatchVisualInfo(mDisplay, mScreen, depth, TrueColor, &visualInfo);

#if CONFIG_VAAPI_GLX
        if (gl_enabled) {
            vi = glXChooseVisual(mDisplay, mScreen, gl_visual_attr);
            if (!vi)
                return -1;
            cmap = XCreateColormap(mDisplay, mRootWin, vi->visual, AllocNone);
            if (cmap == None)
                return -1;
        }
        else
#endif
        {
            vi = &visualInfo;
            XMatchVisualInfo(mDisplay, mScreen, depth, TrueColor, vi);
            cmap = CopyFromParent;
        }

        vo_x11_create_vo_window(vi,
                                vo_dx, vo_dy, display_width, display_height,
                                flags, cmap, "vaapi", title);

        if (vi != &visualInfo)
            XFree(vi);

        xswa_mask             = CWBorderPixel | CWBackPixel;
        xswa.border_pixel     = 0;
        xswa.background_pixel = 0;
        XChangeWindowAttributes(mDisplay, vo_window, xswa_mask, &xswa);

#ifdef CONFIG_XF86VM
        if (flags & VOFLAG_MODESWITCHING) {
            /* Grab the mouse pointer in our window */
            if (vo_grabpointer)
                XGrabPointer(mDisplay, vo_window, True, 0,
                             GrabModeAsync, GrabModeAsync,
                             vo_window, None, CurrentTime);
            XSetInputFocus(mDisplay, vo_window, RevertToNone, CurrentTime);
        }
#endif
    }

    if ((flags & VOFLAG_FULLSCREEN) && WinID <= 0)
        vo_fs = VO_TRUE;
    return 0;
}

#if CONFIG_VAAPI_GLX
static int config_glx(unsigned int width, unsigned int height)
{
    if (gl_context.setGlWindow(&gl_context) == SET_WINDOW_FAILED)
        return -1;

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_TEXTURE_2D);
    glDrawBuffer(vo_doublebuffering ? GL_BACK : GL_FRONT);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Create OpenGL texture */
    /* XXX: assume GL_ARB_texture_non_power_of_two is available */
    glEnable(GL_TEXTURE_2D);
    glGenTextures(1, &gl_texture);
    BindTexture(GL_TEXTURE_2D, gl_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    BindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);

    glClearColor(0.0, 0.0, 0.0, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);

    if (gl_build_font() < 0)
        return -1;
    return 0;
}
#endif

static int config_vaapi(uint32_t width, uint32_t height, uint32_t format)
{
    VAConfigAttrib attrib;
    VAStatus status;
    int i, j, profile, entrypoint, max_entrypoints, num_surfaces;

    /* Create video surfaces */
    if (!IMGFMT_IS_VAAPI(format))
        num_surfaces = MAX_OUTPUT_SURFACES;
    else {
        switch (IMGFMT_VAAPI_CODEC(format)) {
        case IMGFMT_VAAPI_CODEC_MPEG2:
            num_surfaces = NUM_VIDEO_SURFACES_MPEG2;
            break;
        case IMGFMT_VAAPI_CODEC_MPEG4:
            num_surfaces = NUM_VIDEO_SURFACES_MPEG4;
            break;
        case IMGFMT_VAAPI_CODEC_H264:
            num_surfaces = NUM_VIDEO_SURFACES_H264;
            break;
        case IMGFMT_VAAPI_CODEC_VC1:
            num_surfaces = NUM_VIDEO_SURFACES_VC1;
            break;
        default:
            num_surfaces = 0;
            break;
        }
        if (num_surfaces == 0)
            return -1;
        if (!is_direct_mapping())
            num_surfaces = FFMIN(2 * num_surfaces, MAX_VIDEO_SURFACES);
    }
    for (i = 0; i < num_surfaces; i++) {
        struct vaapi_surface *surface;
        surface = alloc_vaapi_surface(width, height, VA_RT_FORMAT_YUV420);
        if (!surface)
            return -1;
    }
    assert(va_num_surfaces == num_surfaces);

#if CONFIG_VAAPI_GLX
    /* Create GLX surfaces */
    if (gl_enabled) {
        status = vaCreateSurfaceGLX(va_context->display,
                                    GL_TEXTURE_2D, gl_texture,
                                    &gl_surface);
        if (!check_status(status, "vaCreateSurfaceGLX()"))
            return -1;
    }
#endif

    /* Create OSD data */
    va_osd_draw_alpha     = NULL;
    va_osd_image.image_id = VA_INVALID_ID;
    va_osd_image.buf      = VA_INVALID_ID;
    va_osd_subpicture     = VA_INVALID_ID;
    for (i = 0; va_osd_info[i].format; i++) {
        for (j = 0; j < va_num_subpic_formats; j++)
            if (va_subpic_formats[j].fourcc == va_osd_info[i].format)
                break;
        if (j < va_num_subpic_formats &&
            vaCreateImage(va_context->display, &va_subpic_formats[j],
                          width, height, &va_osd_image) == VA_STATUS_SUCCESS) {
            va_osd_palette = gen_osd_palette(&va_osd_image);
            if (((!va_osd_image.num_palette_entries) ^ (!va_osd_palette)) == 0)
                break;
        }
    }
    if (va_osd_info[i].format &&
        vaCreateSubpicture(va_context->display, va_osd_image.image_id,
                           &va_osd_subpicture) == VA_STATUS_SUCCESS) {
        va_osd_draw_alpha = va_osd_info[i].draw_alpha;
        if (va_osd_palette) {
            status = vaSetImagePalette(va_context->display,
                                       va_osd_image.image_id, va_osd_palette);
            check_status(status, "vaSetImagePalette()");
        }
        mp_msg(MSGT_VO, MSGL_DBG2, "[vo_vaapi] Using %s surface for OSD\n",
               string_of_VAImageFormat(&va_osd_image.format));
    }

    /* Create EOSD data */
    va_eosd_draw_alpha     = NULL;
    va_eosd_image.image_id = VA_INVALID_ID;
    va_eosd_image.buf      = VA_INVALID_ID;
    va_eosd_subpicture     = VA_INVALID_ID;
    for (i = 0; va_eosd_info[i].format; i++) {
        for (j = 0; j < va_num_subpic_formats; j++)
            if (va_subpic_formats[j].fourcc == va_eosd_info[i].format)
                break;
        if (j < va_num_subpic_formats &&
            vaCreateImage(va_context->display, &va_subpic_formats[j],
                          width, height, &va_eosd_image) == VA_STATUS_SUCCESS)
            break;
    }
    if (va_eosd_info[i].format &&
        vaCreateSubpicture(va_context->display, va_eosd_image.image_id,
                           &va_eosd_subpicture) == VA_STATUS_SUCCESS) {
        va_eosd_draw_alpha = va_eosd_info[i].draw_alpha;
        mp_msg(MSGT_VO, MSGL_DBG2, "[vo_vaapi] Using %s surface for EOSD\n",
               string_of_VAImageFormat(&va_eosd_image.format));
    }

    /* Allocate VA images */
    if (!IMGFMT_IS_VAAPI(format)) {
        VAImageFormat *image_format = VAImageFormat_from_imgfmt(format);
        if (!image_format)
            return -1;
        for (i = 0; i < va_num_surfaces; i++) {
            status = vaCreateImage(va_context->display, image_format,
                                   width, height, &va_free_surfaces[i]->image);
            if (!check_status(status, "vaCreateImage()"))
                return -1;
        }
        return 0;
    }

    /* Check profile */
    profile = VAProfile_from_imgfmt(format);
    if (profile < 0)
        return -1;

    /* Check entry-point (only VLD for now) */
    max_entrypoints = vaMaxNumEntrypoints(va_context->display);
    va_entrypoints = calloc(max_entrypoints, sizeof(*va_entrypoints));
    if (!va_entrypoints)
        return -1;

    status = vaQueryConfigEntrypoints(va_context->display, profile,
                                      va_entrypoints, &va_num_entrypoints);
    if (!check_status(status, "vaQueryConfigEntrypoints()"))
        return -1;

    mp_msg(MSGT_VO, MSGL_DBG2, "[vo_vaapi] config_vaapi(%s): %d entrypoints available\n",
           string_of_VAProfile(profile), va_num_entrypoints);
    for (i = 0; i < va_num_entrypoints; i++)
        mp_msg(MSGT_VO, MSGL_DBG2, "  %s\n", string_of_VAEntrypoint(va_entrypoints[i]));

    entrypoint = VAEntrypoint_from_imgfmt(format);
    if (entrypoint != VAEntrypointVLD)
        return -1;

    /* Check chroma format (only 4:2:0 for now) */
    attrib.type = VAConfigAttribRTFormat;
    status = vaGetConfigAttributes(va_context->display, profile, entrypoint, &attrib, 1);
    if (!check_status(status, "vaGetConfigAttributes()"))
        return -1;
    if ((attrib.value & VA_RT_FORMAT_YUV420) == 0)
        return -1;

    /* Create a configuration for the decode pipeline */
    status = vaCreateConfig(va_context->display, profile, entrypoint, &attrib, 1, &va_context->config_id);
    if (!check_status(status, "vaCreateConfig()"))
        return -1;

    /* Create a context for the decode pipeline */
    status = vaCreateContext(va_context->display, va_context->config_id,
                             width, height, VA_PROGRESSIVE,
                             va_surface_ids, va_num_surfaces,
                             &va_context->context_id);
    if (!check_status(status, "vaCreateContext()"))
        return -1;
    return 0;
}

static int config(uint32_t width, uint32_t height,
                  uint32_t display_width, uint32_t display_height,
                  uint32_t flags, char *title, uint32_t format)
{
    mp_msg(MSGT_VO, MSGL_DBG2, "[vo_vaapi] config(): size %dx%d, display size %dx%d, flags %x, title '%s', format %x (%s)\n",
           width, height, display_width, display_height, flags, title, format, vo_format_name(format));

    free_video_specific();

    if (config_x11(width, height, display_width, display_height, flags, title) < 0)
        return -1;

#if CONFIG_VAAPI_GLX
    if (gl_enabled && config_glx(width, height) < 0)
        return -1;
#endif

    if (config_vaapi(width, height, format) < 0)
        return -1;

    g_is_paused    = 0;
    g_image_width  = width;
    g_image_height = height;
    g_image_format = format;
    resize();
    return 0;
}

static int query_format(uint32_t format)
{
    const int default_caps = (VFCAP_CSP_SUPPORTED |
                              VFCAP_CSP_SUPPORTED_BY_HW |
                              VFCAP_HWSCALE_UP |
                              VFCAP_HWSCALE_DOWN |
                              VFCAP_OSD |
                              VFCAP_EOSD);

    mp_msg(MSGT_VO, MSGL_DBG2, "[vo_vaapi] query_format(): format %x (%s)\n",
           format, vo_format_name(format));

    switch (format) {
    case IMGFMT_VAAPI_MPEG2:
    case IMGFMT_VAAPI_MPEG4:
    case IMGFMT_VAAPI_H263:
    case IMGFMT_VAAPI_H264:
    case IMGFMT_VAAPI_WMV3:
    case IMGFMT_VAAPI_VC1:
        return default_caps | VOCAP_NOSLICES;
    case IMGFMT_NV12:
    case IMGFMT_YV12:
    case IMGFMT_I420:
    case IMGFMT_IYUV:
        if (VAImageFormat_from_imgfmt(format))
            return default_caps;
        break;
    }
    return 0;
}

static inline int get_field_flags(int i)
{
    return (g_deint ? 
            ((g_top_field_first == i) ^ (g_deint > 1) ?
             VA_BOTTOM_FIELD : VA_TOP_FIELD) : VA_FRAME_PICTURE);
}

static inline int get_colorspace_flags(void)
{
    int csp;
    switch (g_colorspace) {
    case 0:
        csp = ((g_image_width >= 1280 || g_image_height > 576) ?
               VA_SRC_BT709 : VA_SRC_BT601);
        break;
    case 1:
        csp = VA_SRC_BT601;
        break;
    case 2:
        csp = VA_SRC_BT709;
        break;
    default:
        assert(0);
        break;
    }
    return csp;
}

static void put_surface_x11(struct vaapi_surface *surface)
{
    VAStatus status;
    int i;

    for (i = 0; i <= !!(g_deint > 1); i++) {
        status = vaPutSurface(va_context->display,
                              surface->id,
                              vo_window,
                              0, 0, g_image_width, g_image_height,
                              g_output_rect.left,
                              g_output_rect.top,
                              g_output_rect.width,
                              g_output_rect.height,
                              NULL, 0,
                              get_field_flags(i) | get_colorspace_flags());
        if (!check_status(status, "vaPutSurface()"))
            return;
    }
}

#if CONFIG_VAAPI_GLX
static void put_surface_glx(struct vaapi_surface *surface)
{
    VAStatus status;
    int i;

    if (gl_binding) {
        for (i = 0; i <= !!(g_deint > 1); i++) {
            status = vaAssociateSurfaceGLX(va_context->display,
                                           gl_surface,
                                           surface->id,
                                           get_field_flags(i) | get_colorspace_flags());
            if (!check_status(status, "vaAssociateSurfaceGLX()"))
                return;
        }
    }
    else {
        for (i = 0; i <= !!(g_deint > 1); i++) {
            status = vaCopySurfaceGLX(va_context->display,
                                      gl_surface,
                                      surface->id,
                                      get_field_flags(i) | get_colorspace_flags());

            if (status == VA_STATUS_ERROR_UNIMPLEMENTED) {
                mp_msg(MSGT_VO, MSGL_WARN,
                       "[vo_vaapi] vaCopySurfaceGLX() is not implemented\n");
                gl_binding = 1;
            }
            else {
                if (!check_status(status, "vaCopySurfaceGLX()"))
                    return;
            }
        }
    }
    g_output_surfaces[g_output_surface] = surface;
}

static int glx_bind_texture(void)
{
    VAStatus status;

    glEnable(GL_TEXTURE_2D);
    BindTexture(GL_TEXTURE_2D, gl_texture);

    if (gl_binding) {
        status = vaBeginRenderSurfaceGLX(va_context->display, gl_surface);
        if (!check_status(status, "vaBeginRenderSurfaceGLX()"))
            return -1;
    }
    return 0;
}

static int glx_unbind_texture(void)
{
    VAStatus status;

    if (gl_binding) {
        status = vaEndRenderSurfaceGLX(va_context->display, gl_surface);
        if (!check_status(status, "vaEndRenderSurfaceGLX()"))
            return -1;
    }

    BindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
    return 0;
}

static void render_background(void)
{
    /* Original code from Mirco Muller (MacSlow):
       <http://cgit.freedesktop.org/~macslow/gl-gst-player/> */
    GLfloat fStartX = 0.0f;
    GLfloat fStartY = 0.0f;
    GLfloat fWidth  = (GLfloat)vo_dwidth;
    GLfloat fHeight = (GLfloat)vo_dheight;

    glBegin(GL_QUADS);
    {
        /* top third, darker grey to white */
        glColor3f(0.85f, 0.85f, 0.85f);
        glVertex3f(fStartX, fStartY, 0.0f);
        glColor3f(0.85f, 0.85f, 0.85f);
        glVertex3f(fStartX + fWidth, fStartY, 0.0f);
        glColor3f(1.0f, 1.0f, 1.0f);
        glVertex3f(fStartX + fWidth, fStartY + fHeight / 3.0f, 0.0f);
        glColor3f(1.0f, 1.0f, 1.0f);
        glVertex3f(fStartX, fStartY + fHeight / 3.0f, 0.0f);

        /* middle third, just plain white */
        glColor3f(1.0f, 1.0f, 1.0f);
        glVertex3f(fStartX, fStartY + fHeight / 3.0f, 0.0f);
        glVertex3f(fStartX + fWidth, fStartY + fHeight / 3.0f, 0.0f);
        glVertex3f(fStartX + fWidth, fStartY + 2.0f * fHeight / 3.0f, 0.0f);
        glVertex3f(fStartX, fStartY + 2.0f * fHeight / 3.0f, 0.0f);

        /* bottom third, white to lighter grey */
        glColor3f(1.0f, 1.0f, 1.0f);
        glVertex3f(fStartX, fStartY + 2.0f * fHeight / 3.0f, 0.0f);
        glColor3f(1.0f, 1.0f, 1.0f);
        glVertex3f(fStartX + fWidth, fStartY + 2.0f * fHeight / 3.0f, 0.0f);
        glColor3f(0.62f, 0.66f, 0.69f);
        glVertex3f(fStartX + fWidth, fStartY + fHeight, 0.0f);
        glColor3f(0.62f, 0.66f, 0.69f);
        glVertex3f(fStartX, fStartY + fHeight, 0.0f);
    }
    glEnd();
}

static void render_frame(void)
{
    struct vo_rect * const r = &g_output_rect;

    if (glx_bind_texture() < 0)
        return;
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    {
        glTexCoord2f(0.0f, 0.0f); glVertex2i(r->left, r->top);
        glTexCoord2f(0.0f, 1.0f); glVertex2i(r->left, r->bottom);
        glTexCoord2f(1.0f, 1.0f); glVertex2i(r->right, r->bottom);
        glTexCoord2f(1.0f, 0.0f); glVertex2i(r->right, r->top);
    }
    glEnd();
    if (glx_unbind_texture() < 0)
        return;
}

static void render_reflection(void)
{
    struct vo_rect * const r = &g_output_rect;
    const unsigned int rh  = g_output_rect.height / 5;
    GLfloat ry = 1.0f - (GLfloat)rh / (GLfloat)r->height;

    if (glx_bind_texture() < 0)
        return;
    glBegin(GL_QUADS);
    {
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glTexCoord2f(0.0f, 1.0f); glVertex2i(r->left, r->top);
        glTexCoord2f(1.0f, 1.0f); glVertex2i(r->right, r->top);

        glColor4f(1.0f, 1.0f, 1.0f, 0.0f);
        glTexCoord2f(1.0f, ry); glVertex2i(r->right, r->top + rh);
        glTexCoord2f(0.0f, ry); glVertex2i(r->left, r->top + rh);
    }
    glEnd();
    if (glx_unbind_texture() < 0)
        return;
}

static void flip_page_glx(void)
{
    glClear(GL_COLOR_BUFFER_BIT);

    if (gl_reflect) {
        render_background();

        glPushMatrix();
        glRotatef(20.0f, 0.0f, 1.0f, 0.0f);
        glTranslatef(50.0f, 0.0f, 0.0f);
    }

    render_frame();

    if (gl_reflect) {
        glPushMatrix();
        glTranslatef(0.0, (GLfloat)g_output_rect.height + 5.0f, 0.0f);
        render_reflection();
        glPopMatrix();
        glPopMatrix();
    }

    if (cpu_stats) {
        gl_draw_rectangle(0, 0, vo_dwidth, 32, 0x000000ff);
        glColor3f(1.0f, 1.0f, 1.0f);
        glRasterPos2i(16, 20);
        gl_printf("MPlayer: %.1f%% of CPU @ %u MHz", cpu_usage, cpu_frequency);
    }

    gl_context.swapGlBuffers(&gl_context);

    if (vo_fs) /* avoid flickering borders in fullscreen mode */
        glClear(GL_COLOR_BUFFER_BIT);
}
#endif

static void put_surface(struct vaapi_surface *surface)
{
    if (!surface || surface->id == VA_INVALID_SURFACE)
        return;

    if (va_eosd_used)
        vaAssociateSubpicture(va_context->display,
                              va_eosd_subpicture,
                              &surface->id, 1,
                              0, 0, g_image_width, g_image_height,
                              0, 0, g_image_width, g_image_height,
                              0);

#if CONFIG_VAAPI_GLX
    if (gl_enabled)
        put_surface_glx(surface);
    else
#endif
        put_surface_x11(surface);

    if (va_eosd_used)
        vaDeassociateSubpicture(va_context->display, va_eosd_subpicture,
                                &surface->id, 1);
}

static int draw_slice(uint8_t * image[], int stride[],
                      int w, int h, int x, int y)
{
    struct vaapi_surface * const surface = va_free_surfaces[g_output_surface];
    VAImage * const va_image = &surface->image;
    VAStatus status;
    uint8_t *image_data = NULL;
    uint8_t *dst[3] = { 0, };
    unsigned int dst_stride[3];

    mp_msg(MSGT_VO, MSGL_DBG2, "[vo_vaapi] draw_slice(): location (%d,%d), size %dx%d\n", x, y, w, h);

    status = vaMapBuffer(va_context->display, va_image->buf, &image_data);
    if (!check_status(status, "vaMapBuffer()"))
        return VO_FALSE;

    dst_stride[0] = va_image->pitches[0];
    dst[0] = image_data + va_image->offsets[0] + y * dst_stride[0] + x;

    memcpy_pic(dst[0], image[0], w, h, dst_stride[0], stride[0]);

    x /= 2;
    y /= 2;
    w /= 2;
    h /= 2;

    if (g_image_format == IMGFMT_YV12) {
        /* MPlayer's YV12 is actually I420, so swap U/V components */
        dst_stride[1] = va_image->pitches[2];
        dst[1] = image_data + va_image->offsets[2] + y * dst_stride[1] + x;
        dst_stride[2] = va_image->pitches[1];
        dst[2] = image_data + va_image->offsets[1] + y * dst_stride[2] + x;
    }
    else {
        if (image[1]) {
            dst_stride[1] = va_image->pitches[1];
            dst[1] = image_data + va_image->offsets[1] + y * dst_stride[1] + x;
        }
        if (image[2]) {
            dst_stride[2] = va_image->pitches[2];
            dst[2] = image_data + va_image->offsets[2] + y * dst_stride[2] + x;
        }
    }

    if (image[1]) /* RGBA only has a single plane */
        memcpy_pic(dst[1], image[1], w, h, dst_stride[1], stride[1]);

    if (image[2]) /* NV12 only has two planes */
        memcpy_pic(dst[2], image[2], w, h, dst_stride[2], stride[2]);

    status = vaUnmapBuffer(va_context->display, surface->image.buf);
    if (!check_status(status, "vaUnmapBuffer()"))
        return VO_FALSE;

    return VO_TRUE;
}

static int draw_frame(uint8_t * src[])
{
    mp_msg(MSGT_VO,MSGL_INFO, MSGTR_LIBVO_X11_DrawFrameCalled);

    return -1;
}

static void draw_osd(void)
{
    VAStatus status;

    if (!va_osd_draw_alpha)
        return;

    if (!vo_update_osd(g_image_width, g_image_height))
        return;

    if (!vo_osd_check_range_update(0, 0, g_image_width, g_image_height)) {
        disable_osd();
        return;
    }

    status = vaMapBuffer(va_context->display, va_osd_image.buf,
                         &va_osd_image_data);
    if (!check_status(status, "vaMapBuffer()"))
        return;

    memset(va_osd_image_data, 0, va_osd_image.data_size);

    set_osd_image_dirty_rect(0, 0, g_image_width, g_image_height);
    vo_draw_text(g_image_width, g_image_height, va_osd_draw_alpha);

    status = vaUnmapBuffer(va_context->display, va_osd_image.buf);
    if (!check_status(status, "vaUnmapBuffer()"))
        return;
    va_osd_image_data = NULL;

    enable_osd(&va_osd_image_dirty_rect, &va_osd_image_dirty_rect);
}

static void draw_eosd(mp_eosd_images_t *imgs)
{
    ass_image_t *img = imgs->imgs;
    ass_image_t *i;
    VAStatus status;

    if (!va_eosd_draw_alpha)
        return;

    // Nothing changed, no need to redraw
    if (imgs->changed == 0)
        return;

    // There's nothing to render!
    if (!img) {
        va_eosd_used = 0;
        return;
    }

    if (imgs->changed == 1)
        goto eosd_skip_upload;

    status = vaMapBuffer(va_context->display, va_eosd_image.buf,
                         &va_eosd_image_data);
    if (!check_status(status, "vaMapBuffer()"))
        return;

    memset(va_eosd_image_data, 0, va_eosd_image.data_size);

    for (i = img; i; i = i->next)
        va_eosd_draw_alpha(i->bitmap, i->w, i->h, i->stride,
                           i->dst_x, i->dst_y, i->color);

    status = vaUnmapBuffer(va_context->display, va_eosd_image.buf);
    if (!check_status(status, "vaUnmapBuffer()"))
        return;
    va_eosd_image_data = NULL;

eosd_skip_upload:
    va_eosd_used = 1;
}

static void flip_page(void)
{
    struct vaapi_surface *surface;

    mp_msg(MSGT_VO, MSGL_DBG2, "[vo_vaapi] flip_page()\n");

    surface = g_output_surfaces[g_output_surface];
    if (surface)
        put_surface(surface);
    g_output_surface = (g_output_surface + 1) % MAX_OUTPUT_SURFACES;

#if CONFIG_VAAPI_GLX
    if (gl_enabled && surface)
        flip_page_glx();
#endif
}

static struct vaapi_surface *get_surface(mp_image_t *mpi)
{
    struct vaapi_surface *surface;

    if (mpi->type == MP_IMGTYPE_NUMBERED && is_direct_mapping()) {
        assert(mpi->number < va_num_surfaces);
        surface = va_free_surfaces[mpi->number];
        return surface;
    }

    /* Push current surface to a free slot */
    if (mpi->priv) {
        assert(!va_free_surfaces[va_free_surfaces_tail_index]);
        va_free_surfaces[va_free_surfaces_tail_index] = mpi->priv;
        va_free_surfaces_tail_index = (va_free_surfaces_tail_index + 1) % va_num_surfaces;
    }

    /* Pop the least recently used free surface */
    assert(va_free_surfaces[va_free_surfaces_head_index]);
    surface = va_free_surfaces[va_free_surfaces_head_index];
    va_free_surfaces[va_free_surfaces_head_index] = NULL;
    va_free_surfaces_head_index = (va_free_surfaces_head_index + 1) % va_num_surfaces;
    return surface;
}

static uint32_t get_image(mp_image_t *mpi)
{
    struct vaapi_surface *surface;

    if (mpi->type != MP_IMGTYPE_NUMBERED)
        return VO_FALSE;

    if (!IMGFMT_IS_VAAPI(g_image_format))
        return VO_FALSE;

    surface = get_surface(mpi);
    if (!surface)
        return VO_FALSE;

    mpi->flags |= MP_IMGFLAG_DIRECT;
    mpi->stride[0] = mpi->stride[1] = mpi->stride[2] = mpi->stride[3] = 0;
    mpi->planes[0] = mpi->planes[1] = mpi->planes[2] = mpi->planes[3] = NULL;
    mpi->planes[0] = (char *)surface;
    mpi->planes[3] = (char *)(uintptr_t)surface->id;
    mpi->num_planes = 1;
    mpi->priv = surface;

    mp_msg(MSGT_VO, MSGL_DBG2, "[vo_vaapi] get_image(): surface 0x%08x\n", surface->id);

    return VO_TRUE;
}

static int put_image(mp_image_t *mpi, struct vaapi_surface *surface)
{
    VAStatus status;
 
    if ((mpi->flags & (MP_IMGFLAG_PLANAR|MP_IMGFLAG_YUV)) != (MP_IMGFLAG_PLANAR|MP_IMGFLAG_YUV))
        return VO_FALSE;

    if (!(mpi->flags & MP_IMGFLAG_DRAW_CALLBACK)) {
        if (!draw_slice(mpi->planes, mpi->stride, mpi->w, mpi->h, 0, 0))
            return VO_FALSE;
    }

    status = vaPutImage(va_context->display,
                        surface->id,
                        surface->image.image_id,
                        mpi->x, mpi->y, mpi->w, mpi->h,
                        mpi->x, mpi->y, mpi->w, mpi->h);
    if (!check_status(status, "vaPutImage()"))
        return VO_FALSE;

    return VO_TRUE;
}

static uint32_t draw_image(mp_image_t *mpi)
{
    struct vaapi_surface *surface = (struct vaapi_surface *)mpi->priv;

    if (!IMGFMT_IS_VAAPI(mpi->imgfmt)) {
        /* XXX: no direct rendering in non-accelerated mode */
        surface = va_free_surfaces[g_output_surface];
        if (!put_image(mpi, surface))
            return VO_FALSE;
    }

    mp_msg(MSGT_VO, MSGL_DBG2, "[vo_vaapi] draw_image(): surface 0x%08x\n", surface->id);

    g_output_surfaces[g_output_surface] = surface;

    if (mpi->fields & MP_IMGFIELD_ORDERED)
        g_top_field_first = !!(mpi->fields & MP_IMGFIELD_TOP_FIRST);
    else
        g_top_field_first = 1;

    if (cpu_stats) {
        static uint64_t ticks;
        if ((ticks++ % 30) == 0) {
            cpu_frequency = get_cpu_frequency();
            cpu_usage = get_cpu_usage(CPU_USAGE_QUANTUM);
        }
    }
    return VO_TRUE;
}

static void check_events(void)
{
    int events = vo_x11_check_events(mDisplay);

    if (events & VO_EVENT_RESIZE)
        resize();

    if ((events & (VO_EVENT_EXPOSE|VO_EVENT_RESIZE)) && g_is_paused) {
        struct vaapi_surface *surface = g_output_surfaces[g_output_surface];
        if (surface)
            put_surface(surface);
    }
}

static VADisplayAttribute *get_display_attribute(const char *name)
{
    VADisplayAttribute *attr;
    if (!strcasecmp(name, "brightness"))
        attr = &va_equalizer.brightness;
    else if (!strcasecmp(name, "contrast"))
        attr = &va_equalizer.contrast;
    else if (!strcasecmp(name, "saturation"))
        attr = &va_equalizer.saturation;
    else if (!strcasecmp(name, "hue"))
        attr = &va_equalizer.hue;
    else
        attr = NULL;
    return attr;
}

static int get_equalizer(const char *name, int *value)
{
    VADisplayAttribute * const attr = get_display_attribute(name);
    int r;

    if (!attr || !(attr->flags & VA_DISPLAY_ATTRIB_GETTABLE))
        return VO_NOTIMPL;

    /* normalize to -100 .. 100 range */
    r = attr->max_value - attr->min_value;
    if (r == 0)
        return VO_NOTIMPL;
    *value = ((attr->value - attr->min_value) * 200) / r - 100;
    return VO_TRUE;
}

static int set_equalizer(const char *name, int value)
{
    VADisplayAttribute * const attr = get_display_attribute(name);
    VAStatus status;
    int r;

    if (!attr || !(attr->flags & VA_DISPLAY_ATTRIB_SETTABLE))
        return VO_NOTIMPL;

    /* normalize to attribute value range */
    r = attr->max_value - attr->min_value;
    if (r == 0)
        return VO_NOTIMPL;
    attr->value = ((value + 100) * r) / 200 + attr->min_value;

    status = vaSetDisplayAttributes(va_context->display, attr, 1);
    if (!check_status(status, "vaSetDisplayAttributes()"))
        return VO_FALSE;
    return VO_TRUE;
}

static int control(uint32_t request, void *data, ...)
{
    switch (request) {
    case VOCTRL_GET_DEINTERLACE:
        *(int*)data = g_deint;
        return VO_TRUE;
    case VOCTRL_SET_DEINTERLACE:
        g_deint = *(int*)data;
        if (g_deint)
            g_deint = g_deint_type;
        return VO_TRUE;
    case VOCTRL_PAUSE:
        return (g_is_paused = 1);
    case VOCTRL_RESUME:
        return (g_is_paused = 0);
    case VOCTRL_QUERY_FORMAT:
        return query_format(*((uint32_t *)data));
    case VOCTRL_GET_IMAGE:
        return get_image(data);
    case VOCTRL_DRAW_IMAGE:
        return draw_image(data);
    case VOCTRL_GUISUPPORT:
        return VO_TRUE;
    case VOCTRL_BORDER:
        vo_x11_border();
        resize();
        return VO_TRUE;
    case VOCTRL_FULLSCREEN:
        vo_x11_fullscreen();
        resize();
        return VO_TRUE;
    case VOCTRL_SET_EQUALIZER: {
        va_list ap;
        int value;

        va_start(ap, data);
        value = va_arg(ap, int);

        va_end(ap);
        return set_equalizer(data, value);
    }
    case VOCTRL_GET_EQUALIZER: {
        va_list ap;
        int *value;

        va_start(ap, data);
        value = va_arg(ap, int *);

        va_end(ap);
        return get_equalizer(data, value);
    }
    case VOCTRL_ONTOP:
        vo_x11_ontop();
        return VO_TRUE;
    case VOCTRL_GET_PANSCAN:
        return VO_TRUE;
    case VOCTRL_SET_PANSCAN:
        resize();
        return VO_TRUE;
    case VOCTRL_GET_HWACCEL_CONTEXT:
        *((void **)data) = va_context;
        return VO_TRUE;
    case VOCTRL_DRAW_EOSD:
        if (!data)
            return VO_FALSE;
        draw_eosd(data);
        return VO_TRUE;
    case VOCTRL_GET_EOSD_RES: {
        mp_eosd_res_t *r = data;
        r->mt = r->mb = r->ml = r->mr = 0;
        r->srcw = g_image_width;
        r->srch = g_image_height;
        r->w    = g_image_width;
        r->h    = g_image_height;
        return VO_TRUE;
    }
    }
    return VO_NOTIMPL;
}
