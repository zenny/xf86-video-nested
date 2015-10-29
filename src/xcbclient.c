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

#include <X11/XKBlib.h>

#include <xcb/xcb.h>
#include <xcb/xcb_aux.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_image.h>
#include <xcb/shm.h>
#include <xcb/randr.h>
#include <xcb/xkb.h>

#include <xorg-server.h>
#include <xf86.h>

#include "client.h"

#include "nested_input.h"

#define BUF_LEN 256

#define MAX(a, b) (((a) <= (b)) ? (b) : (a))

extern Bool enableNestedInput;
extern char *display;

static xcb_atom_t atom_WM_DELETE_WINDOW;

typedef struct _Output {
    const char *name;
    int x;
    int y;
    unsigned int width;
    unsigned int height;
} Output;

struct NestedClientPrivate {
    /* Host X server data */
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

static Bool
_NestedClientConnectionHasError(int scrnIndex,
                                xcb_connection_t *conn)
{
    const char *displayName = getenv("DISPLAY");

    switch (xcb_connection_has_error(conn))
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
                   "Connection to host X server closed: exceeding request length that server accepts.\n");
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
_NestedClientCheckRandRVersion(int scrnIndex,
                               xcb_connection_t *conn,
                               int major, int minor)
{
    xcb_randr_query_version_cookie_t cookie;
    xcb_randr_query_version_reply_t *reply;
    xcb_generic_error_t *error;

    if (!_NestedClientCheckExtension(conn, &xcb_randr_id))
    {
        xf86DrvMsg(scrnIndex,
                   X_ERROR,
                   "Host X server does not support RANDR extension (or it's disabled).\n");
        return FALSE;
    }

    /* Check RandR version */
    cookie = xcb_randr_query_version(conn, major, minor);
    reply = xcb_randr_query_version_reply(conn, cookie, &error);

    if (!reply)
    {
        xf86DrvMsg(scrnIndex,
                   X_ERROR,
                   "Failed to get RandR version supported by host X server. Error code = %d.\n",
                   error->error_code);
        free(error);
        return FALSE;
    }
    else if (reply->major_version < major || reply->minor_version < minor)
    {
        xf86DrvMsg(scrnIndex,
                   X_ERROR,
                   "Host X server doesn't support RandR %d.%d, needed for Option \"Output\" usage.\n",
                   major, minor);
        free(reply);
        return FALSE;
    }

    free(reply);
    return TRUE;
}

static Bool
_NestedClientOutputInit(int scrnIndex,
                        xcb_connection_t *conn,
                        int screenNumber,
                        Output *output,
                        Bool enable,
                        Output *relativeTo,
                        char relation)
{
    xcb_generic_error_t *error;
    xcb_screen_t *screen;
    xcb_randr_get_screen_resources_cookie_t screen_resources_c;
    xcb_randr_get_screen_resources_reply_t *screen_resources_r;
    xcb_randr_output_t *outputs;
    xcb_randr_mode_info_t *available_modes;
    int available_modes_len, i, j;

    if (!_NestedClientCheckRandRVersion(scrnIndex, conn, 1, 2))
        return FALSE;

    screen = xcb_aux_get_screen(conn, screenNumber);

    /* Get list of outputs from screen resources */
    screen_resources_c = xcb_randr_get_screen_resources(conn,
                                                        screen->root);
    screen_resources_r = xcb_randr_get_screen_resources_reply(conn,
                                                              screen_resources_c,
                                                              &error);

    if (!screen_resources_r)
    {
        xf86DrvMsg(scrnIndex,
                   X_ERROR,
                   "Failed to get host X server screen resources. Error code = %d.\n",
                   error->error_code);
        free(error);
        return FALSE;
    }

    outputs = xcb_randr_get_screen_resources_outputs(screen_resources_r);
    available_modes = xcb_randr_get_screen_resources_modes(screen_resources_r);
    available_modes_len = xcb_randr_get_screen_resources_modes_length(screen_resources_r);

    for (i = 0; i < screen_resources_r->num_outputs; i++)
    {
        char *name;
        int name_len;
        xcb_randr_get_output_info_cookie_t output_info_c;
        xcb_randr_get_output_info_reply_t *output_info_r;

        /* Get info on the output */
        output_info_c = xcb_randr_get_output_info(conn,
                                                  outputs[i],
                                                  XCB_TIME_CURRENT_TIME);
        output_info_r = xcb_randr_get_output_info_reply(conn,
                                                        output_info_c,
                                                        &error);

        if (!output_info_r)
        {
            xf86DrvMsg(scrnIndex,
                       X_ERROR,
                       "Failed to get info for output %d. Error code = %d.\n",
                       outputs[i], error->error_code);
            free(error);
            continue;
        }

        /* Get output name */
        name_len = xcb_randr_get_output_info_name_length(output_info_r);
        name = malloc(name_len + 1);
        strncpy(name, (char *)xcb_randr_get_output_info_name(output_info_r), name_len);
        name[name_len] = '\0';

        if (!strcmp(name, output->name))
        {
            /* Output found! */
            if (output_info_r->crtc != XCB_NONE)
            {
                /* Output is enabled! Get its CRTC geometry */
                xcb_randr_get_crtc_info_cookie_t crtc_info_c;
                xcb_randr_get_crtc_info_reply_t *crtc_info_r;

                crtc_info_c = xcb_randr_get_crtc_info(conn,
                                                      output_info_r->crtc,
                                                      XCB_TIME_CURRENT_TIME);
                crtc_info_r = xcb_randr_get_crtc_info_reply(conn,
                                                            crtc_info_c,
                                                            &error);

                if (!crtc_info_r)
                {
                    xf86DrvMsg(scrnIndex,
                               X_ERROR,
                               "Failed to get CRTC info for output %s. Error code = %d.\n",
                               name, error->error_code);
                    free(error);
                    free(output_info_r);
                    free(screen_resources_r);
                    return FALSE;
                }
                else
                {
                    output->width = crtc_info_r->width;
                    output->height = crtc_info_r->height;
                    output->x = crtc_info_r->x;
                    output->y = crtc_info_r->y;
                    free(crtc_info_r);
                }
            }
            else if (enable)
            {
                /* Output is disabled. Try to enable it. */
                unsigned int new_screen_width, new_screen_height,
                             new_screen_width_mm, new_screen_height_mm;
                xcb_randr_mode_t *modes;
                xcb_randr_crtc_t *crtcs;
                xcb_randr_set_crtc_config_cookie_t crtc_config_c;
                xcb_randr_set_crtc_config_reply_t *crtc_config_r;

                modes = xcb_randr_get_output_info_modes(output_info_r);
                crtcs = xcb_randr_get_output_info_crtcs(output_info_r);

                for (j = 0; j < available_modes_len; j++)
                {
                    if (available_modes[j].id == modes[0])
                    {
                        output->width = available_modes[j].width;
                        output->height = available_modes[j].height;
                        break;
                    }
                }

                if (relativeTo != NULL)
                {
                    switch (relation)
                    {
                    case 'L':
#if 0
                        output->x = relativeTo->x - output->width;
                        output->y = relativeTo->y;
                        new_screen_width = relativeTo->width + output->width;
                        new_screen_height = MAX(relativeTo->height, output->height);
                        break;
#else
                        xf86DrvMsg(scrnIndex,
                                   X_WARNING,
                                   "Option \"LeftOf\" for output %s is not currently supported. Falling back to \"RightOf\".\n",
                                   output->name);
#endif
                    case 'R':
                        output->x = relativeTo->x + relativeTo->width;
                        output->y = relativeTo->y;
                        new_screen_width = relativeTo->width + output->width;
                        new_screen_height = MAX(relativeTo->height, output->height);
                        break;
                    case 'A':
#if 0
                        output->x = relativeTo->x;
                        output->y = relativeTo->y - output->height;
                        new_screen_width = MAX(relativeTo->width, output->width);
                        new_screen_height = relativeTo->height + output->height;
                        break;
#else
                        xf86DrvMsg(scrnIndex,
                                   X_WARNING,
                                   "Option \"Above\" for output %s is not currently supported. Falling back to \"Below\".\n",
                                   output->name);
#endif
                    case 'B':
                        output->x = relativeTo->x;
                        output->y = relativeTo->y + relativeTo->height;
                        new_screen_width = MAX(relativeTo->width, output->width);
                        new_screen_height = relativeTo->height + output->height;
                        break;
                    }

                    new_screen_width_mm = (new_screen_width / screen->width_in_pixels) * screen->width_in_millimeters;
                    new_screen_height_mm = (new_screen_height / screen->height_in_pixels) * screen->height_in_millimeters;
                }

                xf86DrvMsg(scrnIndex,
                           X_INFO,
                           "New screen size to allocate output %s: %dx%d px, %dx%d mm.\n",
                           output->name,
                           new_screen_width,
                           new_screen_height,
                           new_screen_width_mm,
                           new_screen_height_mm);

                xcb_randr_set_screen_size(conn,
                                          screen->root,
                                          new_screen_width,
                                          new_screen_height,
                                          new_screen_width_mm,
                                          new_screen_height_mm);

                crtc_config_c = xcb_randr_set_crtc_config(conn,
                                                          crtcs[0],
                                                          XCB_TIME_CURRENT_TIME,
                                                          XCB_TIME_CURRENT_TIME,
                                                          output->x,
                                                          output->y,
                                                          modes[0],
                                                          XCB_RANDR_ROTATION_ROTATE_0,
                                                          1,
                                                          &outputs[i]);
                crtc_config_r = xcb_randr_set_crtc_config_reply(conn, crtc_config_c, &error);

                if (!crtc_config_r)
                {
                    xf86DrvMsg(scrnIndex,
                               X_ERROR,
                               "Failed to enable output %s. Error code = %d.\n",
                               output->name, error->error_code);
                    free(error);
                    free(name);
                    free(output_info_r);
                    free(screen_resources_r);
                    return FALSE;
                }

                free(crtc_config_r);
            }
            else
            {
                xf86DrvMsg(scrnIndex,
                           X_ERROR,
                           "Output %s is currently disabled or disconnected.\n",
                           output->name);
                free(error);
                free(name);
                free(output_info_r);
                free(screen_resources_r);
                return FALSE;
            }

            free(name);
            free(output_info_r);
            free(screen_resources_r);
            return TRUE;
        }

        free(output_info_r);
    }

    free(screen_resources_r);
    return FALSE;
}

Bool
NestedClientCheckDisplay(int scrnIndex,
                         const char *output,
                         Bool enable,
                         const char *parentOutput,
                         char relation,
                         unsigned int *width,
                         unsigned int *height,
                         int *x,
                         int *y)
{
    int n;
    xcb_connection_t *conn;
    Output thisOutput;

    conn = xcb_connect(NULL, &n);

    if (_NestedClientConnectionHasError(scrnIndex, conn))
        return FALSE;

    if (output != NULL)
    {
        thisOutput.name = output;
        thisOutput.width = 0;
        thisOutput.height = 0;
        thisOutput.x = 0;
        thisOutput.y = 0;

        if (parentOutput != NULL)
        {
            Output relativeTo;

            relativeTo.name = parentOutput;
            relativeTo.width = 0;
            relativeTo.height = 0;
            relativeTo.x = 0;
            relativeTo.y = 0;

            if (!_NestedClientOutputInit(scrnIndex, conn, n, &relativeTo, FALSE, NULL, '\0'))
                return FALSE;

            if (!_NestedClientOutputInit(scrnIndex, conn, n, &thisOutput, enable, &relativeTo, relation))
                return FALSE;
        }
        else
        {
            if (!_NestedClientOutputInit(scrnIndex, conn, n, &thisOutput, enable, NULL, '\0'))
                return FALSE;
        }

        xf86DrvMsg(scrnIndex,
                   X_INFO,
                   "Got CRTC geometry from output %s: %dx%d+%d+%d\n",
                   thisOutput.name,
                   thisOutput.width,
                   thisOutput.height,
                   thisOutput.x,
                   thisOutput.y);

        if (width != NULL)
            *width = thisOutput.width;

        if (height != NULL)
            *height = thisOutput.height;

        if (x != NULL)
            *x = thisOutput.x;

        if (y != NULL)
            *y = thisOutput.y;
    }
    else
    {
        xcb_screen_t *s = xcb_aux_get_screen(conn, n);

        if (width != NULL)
            *width = s->width_in_pixels;

        if (height != NULL)
            *height = s->height_in_pixels;
    }

    xcb_disconnect(conn);
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
    int shmMajor = 0, shmMinor = 0;
    Bool hasSharedPixmaps = FALSE;

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

    pPriv->attrs[0] = XCB_EVENT_MASK_EXPOSURE;
    
    if (enableNestedInput)
        pPriv->attrs[0] |= XCB_EVENT_MASK_BUTTON_PRESS   |
                           XCB_EVENT_MASK_BUTTON_RELEASE |
                           XCB_EVENT_MASK_POINTER_MOTION |
                           XCB_EVENT_MASK_KEY_PRESS      |
                           XCB_EVENT_MASK_KEY_RELEASE;

    pPriv->attr_mask = XCB_CW_EVENT_MASK;

    pPriv->conn = xcb_connect(NULL, &pPriv->screenNumber);

    if (_NestedClientConnectionHasError(pPriv->scrnIndex, pPriv->conn))
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

    return TRUE;
}

static void
_NestedClientSetFullscreenHint(NestedClientPrivatePtr pPriv)
{
    xcb_intern_atom_cookie_t cookie_WINDOW_STATE,
                             cookie_WINDOW_STATE_FULLSCREEN;
    xcb_atom_t atom_WINDOW_STATE, atom_WINDOW_STATE_FULLSCREEN;
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
                        XCB_PROP_MODE_REPLACE,
                        pPriv->window,
                        atom_WINDOW_STATE,
                        XCB_ATOM_ATOM,
                        32,
                        1,
                        &atom_WINDOW_STATE_FULLSCREEN);
}

static void
_NestedClientSetDeleteWindowHint(NestedClientPrivatePtr pPriv)
{
    xcb_intern_atom_cookie_t cookie_WM_PROTOCOLS,
                             cookie_WM_DELETE_WINDOW;
    xcb_atom_t atom_WM_PROTOCOLS;
    xcb_intern_atom_reply_t *reply;

    cookie_WM_PROTOCOLS = xcb_intern_atom(pPriv->conn, FALSE,
                                          strlen("WM_PROTOCOLS"),
                                          "WM_PROTOCOLS");
    cookie_WM_DELETE_WINDOW =
        xcb_intern_atom(pPriv->conn, FALSE,
                        strlen("WM_DELETE_WINDOW"),
                        "WM_DELETE_WINDOW");

    reply = xcb_intern_atom_reply(pPriv->conn, cookie_WM_PROTOCOLS, NULL);
    atom_WM_PROTOCOLS = reply->atom;
    free(reply);

    reply = xcb_intern_atom_reply(pPriv->conn, cookie_WM_DELETE_WINDOW,
                                  NULL);
    atom_WM_DELETE_WINDOW = reply->atom;
    free(reply);

    xcb_change_property(pPriv->conn,
                        XCB_PROP_MODE_REPLACE,
                        pPriv->window,
                        atom_WM_PROTOCOLS,
                        XCB_ATOM_ATOM,
                        32,
                        1,
                        &atom_WM_DELETE_WINDOW);
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

    _NestedClientSetDeleteWindowHint(pPriv);
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
                         Bool wantFullscreenHint,
                         unsigned int width,
                         unsigned int height,
                         int originX,
                         int originY,
                         unsigned int depth,
                         unsigned int bitsPerPixel,
                         Pixel *retRedMask,
                         Pixel *retGreenMask,
                         Pixel *retBlueMask)
{
    NestedClientPrivatePtr pPriv = malloc(sizeof(struct NestedClientPrivate));

    if (!pPriv)
        return NULL;

    pPriv->scrnIndex = scrnIndex;
    pPriv->usingFullscreen = wantFullscreenHint;
    pPriv->width = width;
    pPriv->height = height;
    pPriv->x = originX;
    pPriv->y = originY;
    pPriv->dev = NULL;

    if (!_NestedClientHostXInit(pPriv))
    {
        _NestedClientFree(pPriv);
        return NULL;
    }

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
    return (char *)pPriv->img->data;
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

static inline void
_NestedClientProcessClientMessage(NestedClientPrivatePtr pPriv,
                                  xcb_generic_event_t *ev)
{
    xcb_client_message_event_t *cmev = (xcb_client_message_event_t *)ev;

    if (cmev->data.data32[0] == atom_WM_DELETE_WINDOW)
    {
        /* XXX: Is there a better way to do this? */
        xf86DrvMsg(pPriv->scrnIndex,
                   X_INFO,
                   "Nested client window closed.\n");
        free(ev);
        exit(0);
    }
}

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

void
NestedClientCheckEvents(NestedClientPrivatePtr pPriv)
{
    xcb_generic_event_t *ev;

    while (TRUE)
    {
        ev = xcb_poll_for_event(pPriv->conn);

        if (!ev)
        {
            if (_NestedClientConnectionHasError(pPriv->scrnIndex,
                                                pPriv->conn))
            {
                /* XXX: Is there a better way to do this? */
                xf86DrvMsg(pPriv->scrnIndex,
                           X_ERROR,
                           "Connection with host X server lost.\n");
                free(ev);
                NestedClientCloseScreen(pPriv);
                exit(1);
            }

            break;
        }

        switch (ev->response_type & ~0x80)
        {
        case XCB_EXPOSE:
            _NestedClientProcessExpose(pPriv, ev);
            break;
        case XCB_CLIENT_MESSAGE:
            _NestedClientProcessClientMessage(pPriv, ev);
            break;
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
        }

        free(ev);
        xcb_flush(pPriv->conn);
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

Bool NestedClientGetKeyboardMappings(NestedClientPrivatePtr pPriv,
                                     KeySymsPtr keySyms,
                                     CARD8 *modmap,
                                     XkbControlsPtr ctrls)
{
    int mapWidth;
    int min_keycode, max_keycode;
    int i, j;
    int keymap_len;
    xcb_keysym_t *keymap;
    xcb_keycode_t *modifiermap;
    xcb_get_keyboard_mapping_cookie_t mapping_c;
    xcb_get_keyboard_mapping_reply_t *mapping_r;
    xcb_get_modifier_mapping_cookie_t modifier_c;
    xcb_get_modifier_mapping_reply_t *modifier_r;
    xcb_xkb_use_extension_cookie_t use_c;
    xcb_xkb_use_extension_reply_t *use_r;
    xcb_xkb_get_controls_cookie_t controls_c;
    xcb_xkb_get_controls_reply_t *controls_r;
    
    use_c = xcb_xkb_use_extension(pPriv->conn,
                                  XCB_XKB_MAJOR_VERSION,
                                  XCB_XKB_MINOR_VERSION);
    use_r = xcb_xkb_use_extension_reply(pPriv->conn, use_c, NULL);
    
    if (!use_r) {
        xf86DrvMsg(pPriv->scrnIndex,
                   X_ERROR,
                   "Couldn't use XKB extension.\n");
        return FALSE;
    } else if (!use_r->supported) {
        xf86DrvMsg(pPriv->scrnIndex,
                   X_ERROR,
                   "XKB extension is not supported in X server.\n");
        free(use_r);
        return FALSE;
    } else {
        free(use_r);
        
        controls_c = xcb_xkb_get_controls(pPriv->conn,
                                          XCB_XKB_ID_USE_CORE_KBD);
        controls_r = xcb_xkb_get_controls_reply(pPriv->conn,
                                                controls_c,
                                                NULL);

        if (!controls_r) {
            xf86DrvMsg(pPriv->scrnIndex,
                       X_ERROR,
                       "Couldn't get XKB keyboard controls.");
            return FALSE;
        }

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

        ctrls->enabled_ctrls = controls_r->enabledControls;

        for (i = 0; i < XkbPerKeyBitArraySize; i++)
            ctrls->per_key_repeat[i] = controls_r->perKeyRepeat[i];

        free(controls_r);
    }

    return TRUE;
}
