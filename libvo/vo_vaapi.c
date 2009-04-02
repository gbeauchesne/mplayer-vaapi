/*
 * VA API output module
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include <va/va_x11.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "config.h"
#include "mp_msg.h"
#include "help_mp.h"
#include "video_out.h"
#include "video_out_internal.h"
#include "x11_common.h"
#include "aspect.h"
#include "libavutil/common.h"
#include "libavcodec/vaapi.h"

static vo_info_t info = {
    "VA API with X11",
    "vaapi",
    "Gwenole Beauchesne <gbeauchesne@splitted-desktop.com>",
    ""
};

const LIBVO_EXTERN(vaapi)

/* Numbers of video surfaces */
#define NUM_VIDEO_SURFACES_MPEG2  3 /* 1 decode frame, up to 2 reference */
#define NUM_VIDEO_SURFACES_MPEG4  3 /* 1 decode frame, up to 2 reference */
#define NUM_VIDEO_SURFACES_H264  17 /* 1 decode frame, up to 16 references */
#define NUM_VIDEO_SURFACES_VC1    3 /* 1 decode frame, up to 2 references */

static int g_is_paused;
static uint32_t g_image_width;
static uint32_t g_image_height;
static uint32_t g_image_format;
static uint32_t g_drawable_xoffset; // XXX: use vo_x instead?
static uint32_t g_drawable_yoffset; // XXX: use vo_y instead?

static struct vaapi_context *va_context;
static VAProfile *va_profiles;
static int va_num_profiles;
static VAEntrypoint *va_entrypoints;
static int va_num_entrypoints;
static VASurfaceID *va_surface_ids;
static int va_num_surfaces;
static VAImageFormat *va_image_formats;
static int va_num_image_formats;
static VASurfaceID g_output_surface;

#if DEBUG
#define VA_CHECK_STATUS(status) do {                                    \
        if ((status) != VA_STATUS_SUCCESS) {                            \
            mp_msg(MSGT_VO, MSGL_ERR, "vo_vaapi:(%s:%d): status (%d) != VA_STATUS_SUCCESS\n", \
                   __FILE__, __LINE__, status);                         \
            /*assert((status) == VA_STATUS_SUCCESS);*/                  \
        }                                                               \
    } while (0)
#else
#define VA_CHECK_STATUS(status)
#endif

static const char *string_of_VAImageFormat(VAImageFormat *imgfmt)
{
    static char str[5]; /* XXX: not MT-safe */
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
    assert(va_profiles && va_num_profiles > 0);
    for (int i = 0; i < va_num_profiles; i++) {
        if (va_profiles[i] == profile)
            return 1;
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
    assert(va_entrypoints && va_num_entrypoints > 0);
    for (int i = 0; i < va_num_entrypoints; i++) {
        if (va_entrypoints[i] == entrypoint)
            return 1;
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

static int init_entrypoints(VAProfile profile)
{
    VAStatus status;
    int i, max_entrypoints;

    if (va_entrypoints && va_num_entrypoints > 0)
        return 0;

    if (va_entrypoints)
        free(va_entrypoints);

    max_entrypoints = vaMaxNumEntrypoints(va_context->display);
    va_entrypoints = calloc(max_entrypoints, sizeof(*va_entrypoints));
    if (va_entrypoints == NULL)
        return -1;

    status = vaQueryConfigEntrypoints(va_context->display, profile,
                                      va_entrypoints, &va_num_entrypoints);
    VA_CHECK_STATUS(status);
    if (status != VA_STATUS_SUCCESS)
        return -1;

    mp_msg(MSGT_VO, MSGL_DBG2, "vo_vaapi::init_entrypoints(%s): %d entrypoints available\n",
           string_of_VAProfile(profile), va_num_entrypoints);
    for (i = 0; i < va_num_entrypoints; i++)
        mp_msg(MSGT_VO, MSGL_DBG2, " %s\n", string_of_VAEntrypoint(va_entrypoints[i]));
    return 0;
}

static inline VASurfaceID get_surface(int number)
{
    if (number > va_num_surfaces)
        return 0;
    return va_surface_ids[number];
}

static void calc_drwXY(uint32_t *drwX, uint32_t *drwY)
{
    *drwX = *drwY = 0;
    if (vo_fs) {
        aspect(&vo_dwidth, &vo_dheight, A_ZOOM);
        vo_dwidth = FFMIN(vo_dwidth, vo_screenwidth);
        vo_dheight = FFMIN(vo_dheight, vo_screenheight);
        *drwX = (vo_screenwidth - vo_dwidth) / 2;
        *drwY = (vo_screenheight - vo_dheight) / 2;
        mp_msg(MSGT_VO, MSGL_V, "[vaapi-fs] dx: %d dy: %d dw: %d dh: %d\n",
               *drwX, *drwY, vo_dwidth, vo_dheight);
    }
    else if (WinID == 0) {
        *drwX = vo_dx;
        *drwY = vo_dy;
    }
}

static int preinit(const char *arg)
{
    VAStatus va_status;
    int va_major_version, va_minor_version;
    int i, max_image_formats, max_profiles;

    if (arg) {
        mp_msg(MSGT_VO, MSGL_ERR, "vo_vaapi: unknown subdevice: %s\n", arg);
        return ENOSYS;
    }

    if (!vo_init())
        return -1;

    va_context = calloc(1, sizeof(*va_context));
    if (va_context == NULL)
        return -1;

    va_context->display = vaGetDisplay(mDisplay);
    if (va_context->display == NULL)
        return -1;
    mp_msg(MSGT_VO, MSGL_DBG2, "vo_vaapi::preinit(): VA display %p\n", va_context->display);

    va_status = vaInitialize(va_context->display, &va_major_version, &va_minor_version);
    VA_CHECK_STATUS(va_status);
    if (va_status != VA_STATUS_SUCCESS)
        return -1;
    mp_msg(MSGT_VO, MSGL_DBG2, "vo_vaapi::preinit(): VA API version %d.%d\n",
           va_major_version, va_minor_version);

    max_image_formats = vaMaxNumImageFormats(va_context->display);
    va_image_formats = calloc(max_image_formats, sizeof(*va_image_formats));
    if (va_image_formats == NULL)
        return -1;
    va_status = vaQueryImageFormats(va_context->display, va_image_formats, &va_num_image_formats);
    VA_CHECK_STATUS(va_status);
    if (va_status != VA_STATUS_SUCCESS)
        return -1;
    mp_msg(MSGT_VO, MSGL_DBG2, "vo_vaapi::preinit(): %d image formats available\n",
           va_num_image_formats);
    for (i = 0; i < va_num_image_formats; i++)
        mp_msg(MSGT_VO, MSGL_DBG2, " %s\n", string_of_VAImageFormat(&va_image_formats[i]));

    max_profiles = vaMaxNumProfiles(va_context->display);
    va_profiles = calloc(max_profiles, sizeof(*va_profiles));
    if (va_profiles == NULL)
        return -1;
    va_status = vaQueryConfigProfiles(va_context->display, va_profiles, &va_num_profiles);
    VA_CHECK_STATUS(va_status);
    if (va_status != VA_STATUS_SUCCESS)
        return -1;
    mp_msg(MSGT_VO, MSGL_DBG2, "vo_vaapi::preinit(): %d profiles available\n",
           va_num_profiles);
    for (i = 0; i < va_num_profiles; i++)
        mp_msg(MSGT_VO, MSGL_DBG2, " %s\n", string_of_VAProfile(va_profiles[i]));

    return 0;
}

static void free_video_specific(void)
{
    if (va_context && va_context->context_id) {
        vaDestroyContext(va_context->display, va_context->context_id);
        va_context->context_id = 0;
    }

    if (va_surface_ids) {
        vaDestroySurfaces(va_context->display, va_surface_ids, va_num_surfaces);
        free(va_surface_ids);
        va_surface_ids = NULL;
    }

    if (va_context && va_context->config_id) {
        vaDestroyConfig(va_context->display, va_context->config_id);
        va_context->config_id = 0;
    }
}

static void uninit(void)
{
    free_video_specific();

    if (va_entrypoints) {
        free(va_entrypoints);
        va_entrypoints = NULL;
    }

    if (va_profiles) {
        free(va_profiles);
        va_profiles = NULL;
    }

    if (va_context && va_context->display) {
        vaTerminate(va_context->display);
        va_context->display = NULL;
    }

    if (va_context) {
        free(va_context);
        va_context = NULL;
    }
}

static int config_x11(uint32_t width, uint32_t height,
                      uint32_t display_width, uint32_t display_height,
                      uint32_t flags, char *title)
{
    XVisualInfo visualInfo;
    XSetWindowAttributes xswa;
    unsigned long xswa_mask;
    XWindowAttributes wattr;
    int depth;

    /* XXX: merge the GUI support stuff */
    XGetWindowAttributes(mDisplay, DefaultRootWindow(mDisplay), &wattr);
    depth = wattr.depth;
    if (depth != 15 && depth != 16 && depth != 24 && depth != 32)
        depth = 24;
    XMatchVisualInfo(mDisplay, mScreen, depth, TrueColor, &visualInfo);

    vo_x11_create_vo_window(&visualInfo,
                            vo_dx, vo_dy, display_width, display_height,
                            flags, CopyFromParent, "va_x11", title);

    xswa_mask = CWBorderPixel|CWBackPixel;
    xswa.border_pixel = 0;
    xswa.background_pixel = 0;
    XChangeWindowAttributes(mDisplay, vo_window, xswa_mask, &xswa);

    if (vo_gc != None)
        XFreeGC(mDisplay, vo_gc);
    vo_gc = XCreateGC(mDisplay, vo_window, 0L, NULL);
    XSync(mDisplay, False);

    if ((flags & VOFLAG_FULLSCREEN) && WinID <= 0)
        vo_fs = VO_TRUE;
    calc_drwXY(&g_drawable_xoffset, &g_drawable_yoffset);
    return 0;
}

static int config(uint32_t width, uint32_t height,
                  uint32_t display_width, uint32_t display_height,
                  uint32_t flags, char *title, uint32_t format)
{
    VAConfigAttrib attrib;
    VAStatus status;
    int profile, entrypoint;

    mp_msg(MSGT_VO, MSGL_DBG2, "vo_vaapi::config(): size %dx%d, display size %dx%d, flags %x, title '%s', format %x (%s)\n",
           width, height, display_width, display_height, flags, title, format, vo_format_name(format));

    if (config_x11(width, height, display_width, display_height, flags, title) < 0)
        return -1;

    /* Check we have not already called config() before */
    if (g_image_format == format &&
        g_image_width == width &&
        g_image_height == height &&
        va_num_surfaces &&
        va_context->config_id > 0 &&
        va_context->context_id > 0 &&
        vo_window > 0) {
        mp_msg(MSGT_VO, MSGL_WARN, "vo_vaapi::config(): check why we are reconfiguring again the VO!\n");
        return 0;
    }

    /* Check format -- query_format() should have checked that for us */
    if (!IMGFMT_IS_VAAPI(format)) {
        assert(IMGFMT_IS_VAAPI(format));
        return -1;
    }

    /* Check profile -- query_format() should have checked that for us */
    profile = VAProfile_from_imgfmt(format);
    if (profile < 0) {
        assert(profile >= 0);
        return -1;
    }

    /* Check entry-point -- query_format() should have checked that for us */
    init_entrypoints(profile);
    entrypoint = VAEntrypoint_from_imgfmt(format);
    if (entrypoint < 0) {
        assert(entrypoint >= 0);
        return -1;
    }

    /* Check chroma format -- query_format() should have checked that for us */
    attrib.type = VAConfigAttribRTFormat;
    status = vaGetConfigAttributes(va_context->display, profile, entrypoint, &attrib, 1);
    VA_CHECK_STATUS(status);
    if (status != VA_STATUS_SUCCESS)
        return -1;
    if ((attrib.value & VA_RT_FORMAT_YUV420) == 0) {
        assert((attrib.value & VA_RT_FORMAT_YUV420) != 0);
        return -1;
    }

    free_video_specific();

    /* Create a configuration for the decode pipeline */
    status = vaCreateConfig(va_context->display, profile, entrypoint, &attrib, 1, &va_context->config_id);
    VA_CHECK_STATUS(status);
    if (status != VA_STATUS_SUCCESS)
        return -1;

    /* Create video surfaces */
    switch (IMGFMT_VAAPI_CODEC(format)) {
    case IMGFMT_VAAPI_CODEC_MPEG2:
        va_num_surfaces = NUM_VIDEO_SURFACES_MPEG2;
        break;
    case IMGFMT_VAAPI_CODEC_MPEG4:
        va_num_surfaces = NUM_VIDEO_SURFACES_MPEG4;
        break;
    case IMGFMT_VAAPI_CODEC_H264:
        va_num_surfaces = NUM_VIDEO_SURFACES_H264;
        break;
    case IMGFMT_VAAPI_CODEC_VC1:
        va_num_surfaces = NUM_VIDEO_SURFACES_VC1;
        break;
    default:
        assert(0);
        return -1;
    }
    va_surface_ids = calloc(va_num_surfaces, sizeof(*va_surface_ids));
    if (va_surface_ids == NULL)
        return -1;
    status = vaCreateSurfaces(va_context->display, width, height, VA_RT_FORMAT_YUV420,
                              va_num_surfaces, va_surface_ids);
    VA_CHECK_STATUS(status);
    if (status != VA_STATUS_SUCCESS)
        return -1;

    /* Create a context for the decode pipeline */
    status = vaCreateContext(va_context->display, va_context->config_id,
                             width, height, VA_PROGRESSIVE,
                             va_surface_ids, va_num_surfaces,
                             &va_context->context_id);
    VA_CHECK_STATUS(status);
    if (status != VA_STATUS_SUCCESS)
        return -1;

    g_is_paused    = 0;
    g_image_width  = width;
    g_image_height = height;
    g_image_format = format;
    return 0;
}

static int has_hw_codec(uint32_t format)
{
    VAConfigAttrib attrib;
    VAStatus status;
    int profile, entrypoint;

    if (!IMGFMT_IS_VAAPI(format))
        return 0;

    /* check for codec */
    profile = VAProfile_from_imgfmt(format);
    if (profile < 0)
        return 0;

    /* check for entry-point */
    /* XXX: only VLD is supported at this time */
    init_entrypoints(profile);
    entrypoint = VAEntrypoint_from_imgfmt(format);
    assert(entrypoint == VAEntrypointVLD);
    if (entrypoint != VAEntrypointVLD)
        return 0;

    /* check chroma format (only 4:2:0 for now) */
    attrib.type = VAConfigAttribRTFormat;
    status = vaGetConfigAttributes(va_context->display, profile, entrypoint, &attrib, 1);
    VA_CHECK_STATUS(status);
    if (status != VA_STATUS_SUCCESS)
        return 0;
    if ((attrib.value & VA_RT_FORMAT_YUV420) == 0)
        return 0;

    mp_msg(MSGT_VO, MSGL_DBG2, "vo_vaapi::has_hw_codec(): HW decoder available\n");
    return 1;
}

static int query_format(uint32_t format)
{
    mp_msg(MSGT_VO, MSGL_DBG2, "vo_vaapi::query_format(): format %x (%s)\n",
           format, vo_format_name(format));

    if (has_hw_codec(format))
        return (VFCAP_CSP_SUPPORTED |
                VFCAP_CSP_SUPPORTED_BY_HW |
                VFCAP_HWSCALE_UP |
                VFCAP_HWSCALE_DOWN);

    return 0;
}

static int put_surface(VASurfaceID surface)
{
    VAStatus status;

    status = vaSyncSurface(va_context->display, va_context->context_id,
                           surface);

    VA_CHECK_STATUS(status);
    if (status != VA_STATUS_SUCCESS)
        return VO_ERROR;

    status = vaPutSurface(va_context->display,
                          surface,
                          vo_window,
                          0, 0, g_image_width, g_image_height,
                          g_drawable_xoffset, g_drawable_yoffset,
                          vo_dwidth, vo_dheight,
                          NULL, 0,
                          VA_FRAME_PICTURE);

    return VO_TRUE;
}

static int draw_slice(uint8_t * image[], int stride[],
                      int w, int h, int x, int y)
{
    mp_msg(MSGT_VO, MSGL_DBG2, "vo_vaapi::draw_slice(): location (%d,%d), size %dx%d\n", x, y, w, h);

    return VO_TRUE;
}

static int draw_frame(uint8_t * src[])
{
    mp_msg(MSGT_VO,MSGL_INFO, MSGTR_LIBVO_X11_DrawFrameCalled);

    return -1;
}

static void draw_osd(void)
{
    // XXX: not implemented
}

static void flip_page(void)
{
    mp_msg(MSGT_VO, MSGL_DBG2, "vo_vaapi::flip_page()\n");

    if (g_output_surface == 0)
        return;
    put_surface(g_output_surface);
    g_output_surface = 0;
}

static uint32_t get_image(mp_image_t *mpi)
{
    VASurfaceID surface;

    mp_msg(MSGT_VO, MSGL_DBG2, "vo_vaapi::get_image()\n");

    if (mpi->type != MP_IMGTYPE_NUMBERED)
        return VO_FALSE;

    if (!IMGFMT_IS_VAAPI(g_image_format))
        return VO_FALSE;

    surface = get_surface(mpi->number);
    assert(surface != 0);

    mpi->flags |= MP_IMGFLAG_DIRECT;
    mpi->stride[0] = mpi->stride[1] = mpi->stride[2] = mpi->stride[3] = 0;
    mpi->planes[0] = mpi->planes[1] = mpi->planes[2] = mpi->planes[3] = NULL;
    mpi->planes[0] = mpi->planes[3] = (char *)(uintptr_t)surface;
    mpi->num_planes = 1;

    mp_msg(MSGT_VO, MSGL_DBG2, "vo_vaapi::get_image(): surface 0x%08x\n", surface);

    return VO_TRUE;
}

static uint32_t draw_image(mp_image_t *mpi)
{
    VASurfaceID surface = (uintptr_t)mpi->planes[3];

    mp_msg(MSGT_VO, MSGL_DBG2, "vo_vaapi::draw_image(): surface 0x%08x\n", surface);

    g_output_surface = surface;
    return VO_TRUE;
}

static void check_events(void)
{
    int events = vo_x11_check_events(mDisplay);

    if (events & VO_EVENT_RESIZE) {
        vo_x11_clearwindow(mDisplay, vo_window);
        calc_drwXY(&g_drawable_xoffset, &g_drawable_yoffset);
    }

    if (events & (VO_EVENT_EXPOSE|VO_EVENT_RESIZE)) {
    }
}

static int control(uint32_t request, void *data, ...)
{
    switch (request) {
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
    case VOCTRL_FULLSCREEN:
        vo_x11_fullscreen();
        vo_x11_clearwindow(mDisplay, vo_window);
        calc_drwXY(&g_drawable_xoffset, &g_drawable_yoffset);
        return VO_TRUE;
    case VOCTRL_ONTOP:
        vo_x11_ontop();
        return VO_TRUE;
    case VOCTRL_GET_HWACCEL_CONTEXT:
        *((void **)data) = va_context;
        return VO_TRUE;
    }
    return VO_NOTIMPL;
}
