/*
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *
 * Laércio de Sousa <laerciosousa@sme-mogidascruzes.sp.gov.br>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>

#include <sys/ipc.h>
#include <sys/shm.h>

#ifdef NESTED_INPUT
#include <X11/XKBlib.h>
#endif

#include <xcb/xcb.h>
#include <xcb/xcb_aux.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_image.h>
#include <xcb/shm.h>
#include <xcb/randr.h>
#ifdef NESTED_INPUT
#include <xcb/xkb.h>
#endif

#include <xorg-server.h>
#include <xf86.h>

#include "client.h"

#ifdef NESTED_INPUT
#include "nested_input.h"
#endif

#define BUF_LEN 256

extern char *display;

struct NestedClientPrivate {
    /* Host X server data */
    char *displayName;
    char *xauthFile;
    int screenNumber;
    xcb_connection_t *conn;
    xcb_visualtype_t *visual;
    xcb_window_t rootWindow;
    xcb_gcontext_t gc;
    xcb_cursor_t emptyCursor;
    Bool usingShm;

    /* Nested X server window data */
    xcb_window_t window;
    int scrnIndex;
    int x;
    int y;
    unsigned int width;
    unsigned int height;
    Bool usingFullscreen;
    xcb_image_t *img;
    xcb_shm_segment_info_t shminfo;
    DeviceIntPtr dev; // The pointer to the input device.  Passed back to the
                      // input driver when posting input events.

    /* Common data */
    uint32_t attrs[2];
    uint32_t attr_mask;
};

static inline Bool
_NestedClientCheckExtension(xcb_connection_t *connection,
                            xcb_extension_t *extension)
{
    const xcb_query_extension_reply_t *rep =
        xcb_get_extension_data(connection, extension);
    return rep && rep->present;
}

static inline void
_NestedClientFree(NestedClientPrivatePtr pPriv)
{
    xcb_disconnect(pPriv->conn);
    free(pPriv);
}

static Bool
_NestedClientGetOutputGeometry(int scrnIndex,
                               xcb_connection_t *c,
                               xcb_window_t rootWindow,
                               char *output,
                               unsigned int *width,
                               unsigned int *height,
                               int *x,
                               int *y)
{
    Bool output_found = FALSE;
    int i, name_len = 0;
    char *name = NULL;
    xcb_generic_error_t *error;
    xcb_randr_query_version_cookie_t version_c;
    xcb_randr_query_version_reply_t *version_r;
    xcb_randr_get_screen_resources_cookie_t screen_resources_c;
    xcb_randr_get_screen_resources_reply_t *screen_resources_r;
    xcb_randr_output_t *randr_outputs;
    xcb_randr_get_output_info_cookie_t output_info_c;
    xcb_randr_get_output_info_reply_t *output_info_r;
    xcb_randr_get_crtc_info_cookie_t crtc_info_c;
    xcb_randr_get_crtc_info_reply_t *crtc_info_r;

    if (!_NestedClientCheckExtension(c, &xcb_randr_id))
    {
        xf86DrvMsg(scrnIndex,
                   X_ERROR,
                   "Host X server does not support RANDR extension (or it's disabled).\n");
        return FALSE;
    }

    /* Check RandR version */
    version_c = xcb_randr_query_version(c, 1, 2);
    version_r = xcb_randr_query_version_reply(c,
                                              version_c,
                                              &error);

    if (error != NULL || version_r == NULL)
    {
        xf86DrvMsg(scrnIndex,
                   X_ERROR,
                   "Failed to get RandR version supported by host X server.\n");
        return FALSE;
    }
    else if (version_r->major_version < 1 || version_r->minor_version < 2)
    {
        free(version_r);
        xf86DrvMsg(scrnIndex,
                   X_ERROR,
                   "Host X server doesn't support RandR 1.2, needed for -output usage.\n");
        return FALSE;
    }

    free(version_r);

    /* Get list of outputs from screen resources */
    screen_resources_c = xcb_randr_get_screen_resources(c,
                                                        rootWindow);
    screen_resources_r = xcb_randr_get_screen_resources_reply(c,
                                                              screen_resources_c,
                                                              NULL);
    randr_outputs = xcb_randr_get_screen_resources_outputs(screen_resources_r);

    for (i = 0; !output_found && i < screen_resources_r->num_outputs; i++)
    {
        /* Get info on the output */
        output_info_c = xcb_randr_get_output_info(c,
                                                  randr_outputs[i],
                                                  XCB_CURRENT_TIME);
        output_info_r = xcb_randr_get_output_info_reply(c,
                                                        output_info_c,
                                                        NULL);

        /* Get output name */
        name_len = xcb_randr_get_output_info_name_length(output_info_r);
        name = malloc(name_len + 1);
        strncpy(name, (char*)xcb_randr_get_output_info_name(output_info_r), name_len);
        name[name_len] = '\0';

        if (!strcmp(name, output))
        {
            output_found = TRUE;

            /* Check if output is connected */
            if (output_info_r->crtc == XCB_NONE)
            {
                free(name);
                free(output_info_r);
                free(screen_resources_r);
                xf86DrvMsg(scrnIndex,
                           X_ERROR,
                           "Output %s is currently disabled (or not connected).\n", output);
                return FALSE;
            }

            /* Get CRTC from output info */
            crtc_info_c = xcb_randr_get_crtc_info(c,
                                                  output_info_r->crtc,
                                                  XCB_CURRENT_TIME);
            crtc_info_r = xcb_randr_get_crtc_info_reply(c,
                                                        crtc_info_c,
                                                        NULL);

            /* Get CRTC geometry */
            if (x != NULL)
                *x = crtc_info_r->x;

            if (y != NULL)
                *y = crtc_info_r->y;

            if (width != NULL)
                *width = crtc_info_r->width;

            if (height != NULL)
                *height = crtc_info_r->height;

            free(crtc_info_r);
        }

        free(name);
        free(output_info_r);
    }

    free(screen_resources_r);

    if (!output_found)
        xf86DrvMsg(scrnIndex,
                   X_ERROR,
                   "Output %s not available in host X server.\n", output);

    return output_found;
}

static Bool
_NestedClientConnectionHasError(int scrnIndex, xcb_connection_t *c, char *displayName)
{
    switch (xcb_connection_has_error(c))
    {
    case XCB_CONN_ERROR:
        xf86DrvMsg(scrnIndex,
                   X_ERROR,
                   "Failed to connect to host X server at display %s.\n", displayName);
        return TRUE;
    case XCB_CONN_CLOSED_EXT_NOTSUPPORTED:
        xf86DrvMsg(scrnIndex,
                   X_ERROR,
                   "Connection to host X server closed: unsupported extension.\n");
        return TRUE;
    case XCB_CONN_CLOSED_MEM_INSUFFICIENT:
        xf86DrvMsg(scrnIndex,
                   X_ERROR,
                   "Connection to host X server closed: out of memory.\n");
        return TRUE;
    case XCB_CONN_CLOSED_REQ_LEN_EXCEED:
        xf86DrvMsg(scrnIndex,
                   X_ERROR,
                   "Connection to host X server closed: too many requests.\n");
        return TRUE;
    case XCB_CONN_CLOSED_PARSE_ERR:
        xf86DrvMsg(scrnIndex,
                   X_ERROR,
                   "Invalid display for host X server: %s\n", displayName);
        return TRUE;
    case XCB_CONN_CLOSED_INVALID_SCREEN:
        xf86DrvMsg(scrnIndex,
                   X_ERROR,
                   "Host X server does not have a screen matching display %s.\n", displayName);
        return TRUE;
    default:
        return FALSE;
    }
}

Bool
NestedClientCheckDisplay(int scrnIndex,
                         char *displayName,
                         char *xauthFile,
                         char *output,
                         unsigned int *width,
                         unsigned int *height,
                         int *x,
                         int *y)
{
    int n;
    xcb_connection_t *c;
    xcb_screen_t *s;

    /* Needed until we can pass authorization file
     *  directly to xcb_connect(). */
    if (xauthFile)
        setenv("XAUTHORITY", xauthFile, 1);

    c = xcb_connect(displayName, &n);

    if (_NestedClientConnectionHasError(scrnIndex, c, displayName))
        return FALSE;

    s = xcb_aux_get_screen(c, n);

    if (output != NULL)
    {
        if (!_NestedClientGetOutputGeometry(scrnIndex, c, s->root,
                                            output, width, height, x, y))
            return FALSE;
    }
    else
    {
        if (width != NULL)
            *width = s->width_in_pixels;

        if (height != NULL)
            *height = s->height_in_pixels;
    }

    xcb_disconnect(c);
    return TRUE;
}

Bool
NestedClientValidDepth(int depth)
{
    /* XXX: implement! */
    return TRUE;
}

static void
_NestedClientTryXShm(NestedClientPrivatePtr pPriv)
{
    int shmMajor, shmMinor;
    Bool hasSharedPixmaps;

    /* Try to get share memory ximages for a little bit more speed */
    if (!_NestedClientCheckExtension(pPriv->conn, &xcb_shm_id))
        pPriv->usingShm = FALSE;
    else
    {
        xcb_generic_error_t *e;
        xcb_shm_query_version_cookie_t c;
        xcb_shm_query_version_reply_t *r;

        c = xcb_shm_query_version(pPriv->conn);
        r = xcb_shm_query_version_reply(pPriv->conn, c, &e);

        if (e)
        {
            pPriv->usingShm = FALSE;
            free(e);
        }
        else
        {
            shmMajor = r->major_version;
            shmMinor = r->minor_version;
            hasSharedPixmaps = r->shared_pixmaps;
            free(r);

            /* Really really check we have shm - better way ?*/
            xcb_shm_segment_info_t shminfo;
            xcb_void_cookie_t cookie;
            xcb_shm_seg_t shmseg;

            pPriv->usingShm = TRUE;

            shminfo.shmid = shmget(IPC_PRIVATE, 1, IPC_CREAT | 0777);
            shminfo.shmaddr = shmat(shminfo.shmid, 0, 0);

            shmseg = xcb_generate_id(pPriv->conn);
            cookie = xcb_shm_attach_checked(pPriv->conn,
                                            shmseg,
                                            shminfo.shmid,
                                            TRUE);
            e = xcb_request_check(pPriv->conn, cookie);

            if (e)
            {
                pPriv->usingShm = FALSE;
                free(e);
            }

            shmdt(shminfo.shmaddr);
            shmctl(shminfo.shmid, IPC_RMID, 0);
        }
    }

    if (!pPriv->usingShm)
        xf86DrvMsg(pPriv->scrnIndex,
                   X_INFO,
                   "XShm extension query failed. Dropping XShm support.\n");

    xf86DrvMsg(pPriv->scrnIndex,
               X_INFO,
               "XShm extension version %d.%d %s shared pixmaps\n",
               shmMajor, shmMinor, hasSharedPixmaps ? "with" : "without");
}

static void
_NestedClientCreateXImage(NestedClientPrivatePtr pPriv, int depth)
{
    if (pPriv->img != NULL)
    {
        /* Free up the image data if previously used
         * i.e. called by server reset */
        if (pPriv->usingShm)
        {
            xcb_shm_detach(pPriv->conn, pPriv->shminfo.shmseg);
            xcb_image_destroy(pPriv->img);
            shmdt(pPriv->shminfo.shmaddr);
            shmctl(pPriv->shminfo.shmid, IPC_RMID, 0);
        }
        else
        {
            free(pPriv->img->data);
            pPriv->img->data = NULL;
            xcb_image_destroy(pPriv->img);
        }
    }

    if (pPriv->usingShm)
    {
        pPriv->img = xcb_image_create_native(pPriv->conn,
                                             pPriv->width,
                                             pPriv->height,
                                             XCB_IMAGE_FORMAT_Z_PIXMAP,
                                             depth,
                                             NULL,
                                             ~0,
                                             NULL);

        /* XXX: change the 0777 mask? */
        pPriv->shminfo.shmid = shmget(IPC_PRIVATE,
                                      pPriv->img->stride * pPriv->height,
                                      IPC_CREAT | 0777);
        pPriv->img->data = shmat(pPriv->shminfo.shmid, 0, 0);
        pPriv->shminfo.shmaddr = pPriv->img->data;

        if (pPriv->img->data == (uint8_t *) -1)
        {
            xf86DrvMsg(pPriv->scrnIndex,
                       X_INFO,
                       "Can't attach SHM Segment, falling back to plain XImages.\n");
            pPriv->usingShm = FALSE;
            xcb_image_destroy(pPriv->img);
            shmctl(pPriv->shminfo.shmid, IPC_RMID, 0);
        }
        else
        {
            xf86DrvMsg(pPriv->scrnIndex,
                       X_INFO,
                       "SHM segment attached %p\n",
                       pPriv->shminfo.shmaddr);
            pPriv->shminfo.shmseg = xcb_generate_id(pPriv->conn);
            xcb_shm_attach(pPriv->conn,
                           pPriv->shminfo.shmseg,
                           pPriv->shminfo.shmid,
                           FALSE);
        }
    }

    if (!pPriv->usingShm)
    {
        xf86DrvMsg(pPriv->scrnIndex,
                   X_INFO,
                   "Creating image %dx%d for screen pPriv=%p\n",
                   pPriv->width, pPriv->height, pPriv);
        pPriv->img = xcb_image_create_native(pPriv->conn,
                                             pPriv->width,
                                             pPriv->height,
                                             XCB_IMAGE_FORMAT_Z_PIXMAP,
                                             depth,
                                             NULL,
                                             ~0,
                                             NULL);

        pPriv->img->data =
            malloc(pPriv->img->stride * pPriv->height);
    }
}

static void
_NestedClientSetWindowTitle(NestedClientPrivatePtr pPriv,
                            const char *extra_text)
{
    char buf[BUF_LEN + 1];

    if (!pPriv)
        return;

    memset(buf, 0, BUF_LEN + 1);
    snprintf(buf, BUF_LEN, "Xorg at :%s.%d nested on %s%s%s",
             display,
             pPriv->scrnIndex,
             getenv("DISPLAY"),
             extra_text ? " " : "",
             extra_text ? extra_text : "");
    xcb_icccm_set_wm_name(pPriv->conn,
                          pPriv->window,
                          XCB_ATOM_STRING,
                          8,
                          strlen(buf),
                          buf);
    xcb_flush(pPriv->conn);
}

static void
_NestedClientSetWMClass(NestedClientPrivatePtr pPriv,
                        const char* wm_class)
{
    size_t class_len = strlen(wm_class) + 1;
    char *class_hint = malloc(class_len);

    if (class_hint)
    {
        strcpy(class_hint, wm_class);
        xcb_change_property(pPriv->conn,
                            XCB_PROP_MODE_REPLACE,
                            pPriv->window,
                            XCB_ATOM_WM_CLASS,
                            XCB_ATOM_STRING,
                            8,
                            class_len,
                            class_hint);
        free(class_hint);
    }
}

static void
_NestedClientEmptyCursorInit(NestedClientPrivatePtr pPriv)
{
    uint32_t pixel = 0;
    xcb_pixmap_t cursor_pxm;
    xcb_gcontext_t cursor_gc;
    xcb_rectangle_t rect = { 0, 0, 1, 1 };

    cursor_pxm = xcb_generate_id(pPriv->conn);
    xcb_create_pixmap(pPriv->conn, 1, cursor_pxm, pPriv->rootWindow, 1, 1);

    cursor_gc = xcb_generate_id(pPriv->conn);
    xcb_create_gc(pPriv->conn, cursor_gc, cursor_pxm,
                  XCB_GC_FOREGROUND, &pixel);
    xcb_poly_fill_rectangle(pPriv->conn, cursor_pxm, cursor_gc, 1, &rect);
    xcb_free_gc(pPriv->conn, cursor_gc);

    pPriv->emptyCursor = xcb_generate_id(pPriv->conn);
    xcb_create_cursor(pPriv->conn,
                      pPriv->emptyCursor,
                      cursor_pxm, cursor_pxm,
                      0, 0, 0,
                      0, 0, 0,
                      1, 1);

    xcb_free_pixmap(pPriv->conn, cursor_pxm);
}

static Bool
_NestedClientHostXInit(NestedClientPrivatePtr pPriv)
{
    uint16_t red, green, blue;
    uint32_t pixel;
    xcb_screen_t *screen;

    pPriv->attrs[0] =
#ifdef NESTED_INPUT
        XCB_EVENT_MASK_BUTTON_PRESS   |
        XCB_EVENT_MASK_BUTTON_RELEASE |
        XCB_EVENT_MASK_POINTER_MOTION |
        XCB_EVENT_MASK_KEY_PRESS      |
        XCB_EVENT_MASK_KEY_RELEASE    |
#endif
        XCB_EVENT_MASK_EXPOSURE;
    pPriv->attr_mask = XCB_CW_EVENT_MASK;

    /* Needed until we can pass xauthFile directly to xcb_connect(). */
    if (pPriv->xauthFile)
        setenv("XAUTHORITY", pPriv->xauthFile, 1);

    pPriv->conn = xcb_connect(pPriv->displayName, &pPriv->screenNumber);

    if (_NestedClientConnectionHasError(pPriv->scrnIndex, pPriv->conn, pPriv->displayName))
        return FALSE;

    screen = xcb_aux_get_screen(pPriv->conn, pPriv->screenNumber);
    pPriv->rootWindow = screen->root;
    pPriv->gc = xcb_generate_id(pPriv->conn);
    pPriv->visual = xcb_aux_find_visual_by_id(screen,
                                              screen->root_visual);

    xcb_create_gc(pPriv->conn, pPriv->gc, pPriv->rootWindow, 0, NULL);

    if (!xcb_aux_parse_color("red", &red, &green, &blue))
    {
        xcb_lookup_color_cookie_t c =
            xcb_lookup_color(pPriv->conn, screen->default_colormap, 3, "red");
        xcb_lookup_color_reply_t *r =
            xcb_lookup_color_reply(pPriv->conn, c, NULL);
        red = r->exact_red;
        green = r->exact_green;
        blue = r->exact_blue;
        free(r);
    }

    {
        xcb_alloc_color_cookie_t c = xcb_alloc_color(pPriv->conn,
                                                     screen->default_colormap,
                                                     red, green, blue);
        xcb_alloc_color_reply_t *r = xcb_alloc_color_reply(pPriv->conn, c, NULL);
        pixel = r->pixel;
        free(r);
    }

    xcb_change_gc(pPriv->conn, pPriv->gc, XCB_GC_FOREGROUND, &pixel);

    _NestedClientEmptyCursorInit(pPriv);

    xcb_flush(pPriv->conn);
}

static void
_NestedClientSetFullscreenHint(NestedClientPrivatePtr pPriv)
{
    xcb_intern_atom_cookie_t cookie_WINDOW_STATE,
                             cookie_WINDOW_STATE_FULLSCREEN;
    xcb_atom_t atom_WINDOW_STATE, atom_WINDOW_STATE_FULLSCREEN;
    int index;
    xcb_intern_atom_reply_t *reply;

    cookie_WINDOW_STATE = xcb_intern_atom(pPriv->conn, FALSE,
                                          strlen("_NET_WM_STATE"),
                                          "_NET_WM_STATE");
    cookie_WINDOW_STATE_FULLSCREEN =
        xcb_intern_atom(pPriv->conn, FALSE,
                        strlen("_NET_WM_STATE_FULLSCREEN"),
                        "_NET_WM_STATE_FULLSCREEN");

    reply = xcb_intern_atom_reply(pPriv->conn, cookie_WINDOW_STATE, NULL);
    atom_WINDOW_STATE = reply->atom;
    free(reply);

    reply = xcb_intern_atom_reply(pPriv->conn, cookie_WINDOW_STATE_FULLSCREEN,
                                  NULL);
    atom_WINDOW_STATE_FULLSCREEN = reply->atom;
    free(reply);

    xcb_change_property(pPriv->conn,
                        PropModeReplace,
                        pPriv->window,
                        atom_WINDOW_STATE,
                        XCB_ATOM_ATOM,
                        32,
                        1,
                        &atom_WINDOW_STATE_FULLSCREEN);

}

static void
_NestedClientCreateWindow(NestedClientPrivatePtr pPriv)
{
    xcb_size_hints_t sizeHints;

    sizeHints.flags = XCB_ICCCM_SIZE_HINT_P_POSITION
                      | XCB_ICCCM_SIZE_HINT_P_SIZE
                      | XCB_ICCCM_SIZE_HINT_P_MIN_SIZE
                      | XCB_ICCCM_SIZE_HINT_P_MAX_SIZE;
    sizeHints.min_width = pPriv->width;
    sizeHints.max_width = pPriv->width;
    sizeHints.min_height = pPriv->height;
    sizeHints.max_height = pPriv->height;

    pPriv->window = xcb_generate_id(pPriv->conn);
    pPriv->img = NULL;

    xcb_create_window(pPriv->conn,
                      XCB_COPY_FROM_PARENT,
                      pPriv->window,
                      pPriv->rootWindow,
                      0, 0, 100, 100, /* will resize */
                      0,
                      XCB_WINDOW_CLASS_COPY_FROM_PARENT,
                      pPriv->visual->visual_id,
                      pPriv->attr_mask,
                      pPriv->attrs);

    xcb_icccm_set_wm_normal_hints(pPriv->conn,
                                  pPriv->window,
                                  &sizeHints);

    if (pPriv->usingFullscreen)
        _NestedClientSetFullscreenHint(pPriv);

    _NestedClientSetWindowTitle(pPriv, "");
    _NestedClientSetWMClass(pPriv, "Xorg");

    {
        uint32_t mask = XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
        uint32_t values[2] = { pPriv->width, pPriv->height };
        xcb_configure_window(pPriv->conn, pPriv->window, mask, values);
    }

    xcb_map_window(pPriv->conn, pPriv->window);

    {
        uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
        uint32_t values[2] = { pPriv->x, pPriv->y };
        xcb_configure_window(pPriv->conn, pPriv->window, mask, values);
    }
}

NestedClientPrivatePtr
NestedClientCreateScreen(int scrnIndex,
                         char *displayName,
                         char *xauthFile,
                         Bool wantFullscreenHint,
                         int width,
                         int height,
                         int originX,
                         int originY,
                         int depth,
                         int bitsPerPixel,
                         Pixel *retRedMask,
                         Pixel *retGreenMask,
                         Pixel *retBlueMask)
{
    NestedClientPrivatePtr pPriv = malloc(sizeof(struct NestedClientPrivate));

    if (!pPriv)
        return NULL;

    pPriv->displayName = displayName;
    pPriv->xauthFile = xauthFile;
    pPriv->scrnIndex = scrnIndex;
    pPriv->usingFullscreen = wantFullscreenHint;
    pPriv->width = width;
    pPriv->height = height;
    pPriv->x = originX;
    pPriv->y = originY;
    pPriv->dev = NULL;

    _NestedClientHostXInit(pPriv);
    _NestedClientCreateWindow(pPriv);
    _NestedClientTryXShm(pPriv);
    _NestedClientCreateXImage(pPriv, depth);
    NestedClientHideCursor(pPriv);

#if 0
    xf86DrvMsg(pPriv->scrnIndex, X_INFO, "width: %d\n", pPriv->img->width);
    xf86DrvMsg(pPriv->scrnIndex, X_INFO, "height: %d\n", pPriv->img->height);
    xf86DrvMsg(pPriv->scrnIndex, X_INFO, "depth: %d\n", pPriv->img->depth);
    xf86DrvMsg(pPriv->scrnIndex, X_INFO, "bpp: %d\n", pPriv->img->bpp);
    xf86DrvMsg(pPriv->scrnIndex, X_INFO, "red_mask: 0x%x\n", pPriv->visual->red_mask);
    xf86DrvMsg(pPriv->scrnIndex, X_INFO, "gre_mask: 0x%x\n", pPriv->visual->green_mask);
    xf86DrvMsg(pPriv->scrnIndex, X_INFO, "blu_mask: 0x%x\n", pPriv->visual->blue_mask);
#endif

    *retRedMask = pPriv->visual->red_mask;
    *retGreenMask = pPriv->visual->green_mask;
    *retBlueMask = pPriv->visual->blue_mask;

    return pPriv;
}

void
NestedClientHideCursor(NestedClientPrivatePtr pPriv)
{
    xcb_change_window_attributes(pPriv->conn,
                                 pPriv->window,
                                 XCB_CW_CURSOR,
                                 &pPriv->emptyCursor);
}

char *
NestedClientGetFrameBuffer(NestedClientPrivatePtr pPriv)
{
    return pPriv->img->data;
}

void
NestedClientUpdateScreen(NestedClientPrivatePtr pPriv,
                         int16_t x1, int16_t y1,
                         int16_t x2, int16_t y2)
{
    if (pPriv->usingShm)
        xcb_image_shm_put(pPriv->conn, pPriv->window,
                          pPriv->gc, pPriv->img,
                          pPriv->shminfo,
                          x1, y1, x1, y1, x2 - x1, y2 - y1, FALSE);
    else
        xcb_image_put(pPriv->conn, pPriv->window, pPriv->gc,
                      pPriv->img, x1, y1, 0);

    xcb_aux_sync(pPriv->conn);
}

static inline void
_NestedClientProcessExpose(NestedClientPrivatePtr pPriv,
                           xcb_generic_event_t *ev)
{
    xcb_expose_event_t *xev = (xcb_expose_event_t *)ev;
    NestedClientUpdateScreen(pPriv,
                             xev->x,
                             xev->y,
                             xev->x + xev->width,
                             xev->y + xev->height);
}

#ifdef NESTED_INPUT
static inline Bool
_NestedClientEventCheckInputDevice(NestedClientPrivatePtr pPriv)
{
    if (!pPriv->dev)
    {
        xf86DrvMsg(pPriv->scrnIndex,
                   X_INFO,
                   "Input device is not yet initialized, ignoring input.\n");

        return FALSE;
    }

    return TRUE;
}

static inline void
_NestedClientProcessMotionNotify(NestedClientPrivatePtr pPriv,
                                 xcb_generic_event_t *ev)
{
    if (_NestedClientEventCheckInputDevice(pPriv))
    {
        xcb_motion_notify_event_t *mev = (xcb_motion_notify_event_t *)ev;
        NestedInputPostMouseMotionEvent(pPriv->dev,
                                        mev->event_x,
                                        mev->event_y);
    }
}

static inline void
_NestedClientProcessKeyPress(NestedClientPrivatePtr pPriv,
                             xcb_generic_event_t *ev)
{
    if (_NestedClientEventCheckInputDevice(pPriv))
    {
        xcb_key_press_event_t *kev = (xcb_key_press_event_t *)ev;
        NestedInputPostKeyboardEvent(pPriv->dev, kev->detail, TRUE);
    }
}

static inline void
_NestedClientProcessKeyRelease(NestedClientPrivatePtr pPriv,
                               xcb_generic_event_t *ev)
{
    if (_NestedClientEventCheckInputDevice(pPriv))
    {
        xcb_key_release_event_t *kev = (xcb_key_release_event_t *)ev;
        NestedInputPostKeyboardEvent(pPriv->dev, kev->detail, FALSE);
    }
}

static inline void
_NestedClientProcessButtonPress(NestedClientPrivatePtr pPriv,
                                xcb_generic_event_t *ev)
{
    if (_NestedClientEventCheckInputDevice(pPriv))
    {
        xcb_button_press_event_t *bev = (xcb_button_press_event_t *)ev;
        NestedInputPostButtonEvent(pPriv->dev, bev->detail, TRUE);
    }
}

static inline void
_NestedClientProcessButtonRelease(NestedClientPrivatePtr pPriv,
                                  xcb_generic_event_t *ev)
{
    if (_NestedClientEventCheckInputDevice(pPriv))
    {
        xcb_button_release_event_t *bev = (xcb_button_release_event_t *)ev;
        NestedInputPostButtonEvent(pPriv->dev, bev->detail, FALSE);
    }
}
#endif

void
NestedClientCheckEvents(NestedClientPrivatePtr pPriv)
{
    xcb_generic_event_t *ev;

    while (TRUE)
    {
        ev = xcb_poll_for_event(pPriv->conn);

        if (!ev)
        {
            if (xcb_connection_has_error(pPriv->conn))
                exit(1);

            break;
        }

        switch (ev->response_type & ~0x80)
        {
        case XCB_EXPOSE:
            _NestedClientProcessExpose(pPriv, ev);
            break;
#ifdef NESTED_INPUT
        case XCB_MOTION_NOTIFY:
            _NestedClientProcessMotionNotify(pPriv, ev);
            break;
        case XCB_KEY_PRESS:
            _NestedClientProcessKeyPress(pPriv, ev);
            break;
        case XCB_KEY_RELEASE:
            _NestedClientProcessKeyRelease(pPriv, ev);
            break;
        case XCB_BUTTON_PRESS:
            _NestedClientProcessButtonPress(pPriv, ev);
            break;
        case XCB_BUTTON_RELEASE:
            _NestedClientProcessButtonRelease(pPriv, ev);
            break;
#endif
        }

        free(ev);
    }
}

void
NestedClientCloseScreen(NestedClientPrivatePtr pPriv)
{
    if (pPriv->usingShm)
    {
        xcb_shm_detach(pPriv->conn, pPriv->shminfo.shmseg);
        shmdt(pPriv->shminfo.shmaddr);
    }

    xcb_image_destroy(pPriv->img);
    _NestedClientFree(pPriv);
}

void
NestedClientSetDevicePtr(NestedClientPrivatePtr pPriv, DeviceIntPtr dev)
{
    pPriv->dev = dev;
}

int
NestedClientGetFileDescriptor(NestedClientPrivatePtr pPriv)
{
    return xcb_get_file_descriptor(pPriv->conn);
}

#ifdef NESTED_INPUT
Bool NestedClientGetKeyboardMappings(NestedClientPrivatePtr pPriv,
                                     KeySymsPtr keySyms,
                                     CARD8 *modmap,
                                     XkbControlsPtr ctrls)
{
    /* XXX: REVIEW THIS CODE AFTER XCB-XKB BECOMES STABLE */
    int mapWidth;
    int min_keycode, max_keycode;
    int i, j;
    int keymap_len;
    xcb_generic_error_t *e;
    xcb_keysym_t *keymap;
    xcb_keycode_t *modifiermap;
    xcb_get_keyboard_mapping_cookie_t mapping_c;
    xcb_get_keyboard_mapping_reply_t *mapping_r;
    xcb_get_modifier_mapping_cookie_t modifier_c;
    xcb_get_modifier_mapping_reply_t *modifier_r;
    xcb_xkb_get_controls_cookie_t controls_c;
    xcb_xkb_get_controls_reply_t *controls_r;

    min_keycode = xcb_get_setup(pPriv->conn)->min_keycode;
    max_keycode = xcb_get_setup(pPriv->conn)->max_keycode;

    mapping_c = xcb_get_keyboard_mapping(pPriv->conn,
                                         min_keycode,
                                         max_keycode - min_keycode + 1);
    mapping_r = xcb_get_keyboard_mapping_reply(pPriv->conn,
                                               mapping_c,
                                               NULL);
    mapWidth = mapping_r->keysyms_per_keycode;
    keymap = xcb_get_keyboard_mapping_keysyms(mapping_r);
    keymap_len = xcb_get_keyboard_mapping_keysyms_length(mapping_r);

    modifier_c = xcb_get_modifier_mapping(pPriv->conn);
    modifier_r = xcb_get_modifier_mapping_reply(pPriv->conn,
                                                modifier_c,
                                                NULL);
    modifiermap = xcb_get_modifier_mapping_keycodes(modifier_r);
    memset(modmap, 0, sizeof(CARD8) * MAP_LENGTH);

    for (j = 0; j < 8; j++)
        for (i = 0; i < modifier_r->keycodes_per_modifier; i++) {
            CARD8 keycode;

            if ((keycode = modifiermap[j * modifier_r->keycodes_per_modifier + i]))
                modmap[keycode] |= 1 << j;
    }

    free(modifier_r);

    keySyms->minKeyCode = min_keycode;
    keySyms->maxKeyCode = max_keycode;
    keySyms->mapWidth = mapWidth;
    keySyms->map = calloc(keymap_len, sizeof(KeySym));

    for (i = 0; i < keymap_len; i++)
        keySyms->map[i] = keymap[i];

    free(mapping_r);

    controls_c = xcb_xkb_get_controls(pPriv->conn,
                                      XCB_XKB_ID_USE_CORE_KBD);
    controls_r = xcb_xkb_get_controls_reply(pPriv->conn,
                                            controls_c,
                                            &e);

    if (e)
    {
        free(e);
        return FALSE;
    }

    ctrls->enabled_ctrls = controls_r->enabledControls;

    for (i = 0; i < XkbPerKeyBitArraySize; i++)
        ctrls->per_key_repeat[i] = controls_r->perKeyRepeat[i];

    free(controls_r);

    return TRUE;
}
#endif
