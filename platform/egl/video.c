/* Arcan-fe (OS/device platform), scriptable front-end engine
 *
 * Arcan-fe is the legal property of its developers, please refer
 * to the platform/LICENSE file distributed with this source distribution
 * for licensing terms.
 */

/*
 * A lot of toggleable options in this one.
 * Important ones:
 *  WITH_BCM      - special setup needed for certain broadcom GPUs
 *  WITH_GLES3    - default is GLES2, preferably we want 3 for PBOs
 *  WITH_OGL3     - when the 'nux graphics mess gets cleaned up,
 *                  this is the minimum version to support
 *  WITH_HEADLESS - allocates a GL context that lacks a framebuffer
 *                  only available on systems where we can use the
 *                  KHR_ method of context creation (dep, WITH_OGL3)
 *  WITH_RGB565   - Use RGB565 instead of other native formats,
 *                  this also requires GL_PIXEL_BPP to be set and a
 *                  shmif that has ARCAN_SHMPAGE_VCHANNELS set 3
 *  WITH_GLEW     - some setups might have problems with calls
 *                  (particularly if you want to use some fancy extension)
 *                  this library helps with that, but not needed everywhere
 *
 * Each different device / Windowing type etc. need to have this defined:
 * EGL_NATIVE_DISPLAY
 *
 * Should really split all the functions that are riddled with WITH_ defines
 * into a dispatch table in the beginning to cut down on macro clutter.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>

#include GL_HEADERS

#include "arcan_math.h"
#include "arcan_general.h"
#include "arcan_video.h"
#include "arcan_videoint.h"

#ifndef EGL_SUFFIX
#define EGL_SUFFIX platform
#endif

#define MERGE(X,Y) X ## Y
#define EVAL(X,Y) MERGE(X,Y)
#define PLATFORM_SYMBOL(fun) EVAL(EGL_SUFFIX, fun)

static struct {
	EGLDisplay disp;
	EGLContext ctx;
	EGLSurface surf;
	EGLConfig cfg;
	EGLNativeWindowType wnd;
} egl;

#ifdef WITH_GBMKMS

#include <gbm.h>
#include <drm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static struct {
	drmModeConnector* conn;
	drmModeEncoder* enc;
	drmModeModeInfo mode;
	drmModeCrtcPtr old_settings;

	struct gbm_surface* surf;
	struct gbm_device* dev;

	int fd;

	uint32_t fb_id;

} gbmkms = {
	.fd = -1
};

static char* egl_synchopts[] = {
	"default", "driver default buffer swap",
	NULL
};

enum {
	DEFAULT,
	ENDM
}	synchopt;

#define EGL_NATIVE_DISPLAY gbmkms.dev

/*
 * atexit handler, restore initial mode settings for output device,
 * also used for platform_prepare_external
 */
static void restore_gbmkms()
{
	if (!gbmkms.conn)
		return;

	drmModeSetCrtc(gbmkms.fd, gbmkms.old_settings->crtc_id,
		gbmkms.old_settings->buffer_id, gbmkms.old_settings->x,
		gbmkms.old_settings->y,
		&gbmkms.conn->connector_id, 1, &gbmkms.old_settings->mode
	);

	drmModeFreeCrtc(gbmkms.old_settings);
}

static bool setup_gbmkms(uint16_t* w, uint16_t* h, bool switchres)
{
/* we don't got a command-line argument interface in place to set this up
 * in any other way (and don't want to go the .cfg route) */
	const char* dev = getenv("ARCAN_OUTPUT_DEVICE");
	if (!dev)
		dev = "/dev/dri/card0";

	gbmkms.fd = open(dev, O_RDWR);
	if (-1 == gbmkms.fd){
		arcan_warning("platform/egl: "
			"couldn't open display device node (%s), giving up.\n", dev);
		return false;
	}

	gbmkms.dev = gbm_create_device(gbmkms.fd);
	if (!gbmkms.dev){
		close(gbmkms.fd);
		arcan_warning("platform/egl: "
			"couldn't create GBM device connection, giving up.\n");
		return false;
	}

/* enumerate connectors among resources, find the first one that's connected */
	drmModeRes* reslist = drmModeGetResources(gbmkms.fd);
	for (int conn_ind = 0; conn_ind < reslist->count_connectors; conn_ind++){
		gbmkms.conn = drmModeGetConnector(gbmkms.fd, reslist->connectors[conn_ind]);
		if (!gbmkms.conn)
			continue;

		if (gbmkms.conn->connection == DRM_MODE_CONNECTED && gbmkms.conn->count_modes > 0)
			break;

		drmModeFreeConnector(gbmkms.conn);
		gbmkms.conn = NULL;
	}

	if (!gbmkms.conn){
		arcan_warning("platform/egl: "
			"no active connector found, cannot setup display.\n");
		close(gbmkms.fd);
/* any gdb_destroy_device(fd)? */
		return false;
	}

/* then find the first encoder that fits the selected connector */
	for (int enc_ind = 0; enc_ind < reslist->count_encoders; enc_ind++){
		gbmkms.enc = drmModeGetEncoder(gbmkms.fd, reslist->encoders[enc_ind]);
		if (!gbmkms.enc)
			continue;

		if (gbmkms.enc->encoder_id == gbmkms.conn->encoder_id)
			break;

		drmModeFreeEncoder(gbmkms.enc);
		gbmkms.enc = NULL;
	}

	if (!gbmkms.enc){
		arcan_warning("platform/egl: "
			"no suitable encoder found, cannot setup display.\n");
		close(gbmkms.fd);
		gbmkms.fd = -1;
		drmModeFreeConnector(gbmkms.conn);
		gbmkms.conn = NULL;
		return false;
	}

/* assumption: first display-mode is the most "suitable",
 * extending this would be sweeping for the user-preferred one */
	*w = gbmkms.conn->modes[0].hdisplay;
	*h = gbmkms.conn->modes[0].vdisplay;

	gbmkms.surf = gbm_surface_create(gbmkms.dev, *w, *h,
		GBM_BO_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	gbmkms.old_settings = drmModeGetCrtc(gbmkms.fd, gbmkms.enc->crtc_id);
	atexit(restore_gbmkms);

	egl.wnd = (EGLNativeWindowType) gbmkms.surf;

	return true;
}

#endif

#ifdef WITH_BCM

static char* egl_synchopts[] = {
	"default", "driver default buffer swap",
	NULL
};

enum {
	DEFAULT,
	ENDM
}	synchopt;

#include <bcm_host.h>
static bool alloc_bcm_wnd(uint16_t* w, uint16_t* h)
{
	DISPMANX_ELEMENT_HANDLE_T elem;
	DISPMANX_DISPLAY_HANDLE_T disp;
	DISPMANX_UPDATE_HANDLE_T upd;

  uint32_t dw;
  uint32_t dh;

  if (graphics_get_display_size(0, &dw, &dh) < 0){
		return false;
  }

	VC_RECT_T ddst = {
		.x = 0,
		.y = 0
	};

	VC_RECT_T dsrc = {
		.x = 0,
		.y = 0
	};

	if (*w == 0)
		*w = dw;
	else
		dw = *w;

	if (*h == 0)
		*h = dh;
	else
		dh = *h;

	ddst.width = dw;
	ddst.height = dh;

	dsrc.width = ddst.width << 16;
	dsrc.height = ddst.height << 16;

	VC_DISPMANX_ALPHA_T av = {
		.flags = DISPMANX_FLAGS_ALPHA_FIXED_ALL_PIXELS,
		.opacity = 255,
		.mask = 0
	};

	disp = vc_dispmanx_display_open(0);
	upd = vc_dispmanx_update_start(0);
	elem = vc_dispmanx_element_add(upd, disp,
		0, /* layer */
		&ddst,
		0,
		&dsrc,
		DISPMANX_PROTECTION_NONE,
		&av,
		0 /* clamp */,
		DISPMANX_NO_ROTATE);

	static EGL_DISPMANX_WINDOW_T wnd;
	wnd.element = elem;
	wnd.width = dw;
	wnd.height = dh;

	vc_dispmanx_update_submit_sync(upd);
	egl.wnd = &wnd;

	return true;
}
#endif

#ifndef EGL_NATIVE_DISPLAY
#define EGL_NATIVE_DISPLAY EGL_DEFAULT_DISPLAY
#endif

#ifdef WITH_OGL3
#ifdef WITH_HEADLESS
bool PLATFORM_SYMBOL(_video_init) (uint16_t w, uint16_t h,
	uint8_t bpp, bool fs, bool frames, const char* title)
{

}

#else
bool PLATFORM_SYMBOL(_video_init) (uint16_t w, uint16_t h,
	uint8_t bpp, bool fs, bool frames, const char* title)
{
	const EGLint attrlst[] =
	{
		EGL_SURFACE_TYPE,
		EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE,
		EGL_OPENGL_BIT,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 8,
		EGL_STENCIL_SIZE, 1,
		EGL_BUFFER_SIZE, 32,
		EGL_NONE
	};

#ifdef WITH_GBMKMS
	if (!setup_gbmkms(&w, &h, fs)){
		return false;
	}
#endif

	if (!eglBindAPI(EGL_OPENGL_API)){
		arcan_warning("Couldn't bind EGL/OpenGL API, "
			"likely that driver does not support the EGL/OGL combination."
			"check driver/GL libraries or try a different platform (e.g. GLES2+)\n");
		return false;
	}

	GLint major, minor;
	egl.disp = eglGetDisplay((EGLNativeDisplayType) EGL_NATIVE_DISPLAY);
	if (!eglInitialize(egl.disp, &major, &minor)){
		arcan_warning("Couldn't initialize EGL\n");
		return false;
	}

	GLint nc;
	if (!eglGetConfigs(egl.disp, NULL, 0, &nc)){
		arcan_warning("No configurations found\n");
		return false;
	}

	if (!eglChooseConfig(egl.disp, attrlst, &egl.cfg, 1, &nc)){
		arcan_warning("Couldn't activate/find a useful configuration\n");
		return false;
	}

#ifdef WITH_GLEW
	int err;
	if ( (err = glewInit()) != GLEW_OK){
		platform_video_shutdown();
		arcan_warning("Couldn't initialize GLEW\n");
		return false;
	}
#endif

	arcan_video_display.pbo_support = true;
	arcan_video_display.width = w;
	arcan_video_display.height = h;
	arcan_video_display.bpp = bpp;
	glViewport(0, 0, w, h);

	eglSwapInterval(egl.disp, 1);

	return true;
}
#endif

#else
bool PLATFORM_SYMBOL(_video_init) (uint16_t w, uint16_t h,
	uint8_t bpp, bool fs, bool frames, const char* caption)
{
	EGLint ca[] = {
		EGL_CONTEXT_CLIENT_VERSION,
		2, EGL_NONE
	};

/*
	EGLint attrlst[] = {
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 8,
		EGL_STENCIL_SIZE, 1,
		EGL_BUFFER_SIZE, 32,
		EGL_RENDERABLE_TYPE,
		EGL_OPENGL_ES2_BIT,
		EGL_NONE
	};
*/
	EGLint attrlst[] = {
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_NONE
	};

#ifdef WITH_BCM
	bcm_host_init();
#endif

#ifdef WITH_GBMKMS
	if (!setup_gbmkms(&w, &h, fs)){
		return false;
	}
#endif

	egl.disp = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (egl.disp == EGL_NO_DISPLAY){
		arcan_warning("Couldn't create display\n");
		return false;
	}

#if defined(WITH_GLES3)
	arcan_video_display.pbo_support = true;
#else
/* probe for PBO extensions before failing this path */
	arcan_video_display.pbo_support = false;
#endif

	EGLint major, minor;
	if (!eglInitialize(egl.disp, &major, &minor)){
		arcan_warning("(egl) Couldn't initialize EGL\n");
		return false;
	}
	else
		arcan_warning("EGL Version %d.%d Found\n", major, minor);

	EGLint ncfg;
	if (EGL_FALSE == eglChooseConfig(egl.disp, attrlst, &egl.cfg, 1, &ncfg)){
		arcan_warning("(cgl) couldn't activate/choose configuration\n");
		return false;
	}

	if (EGL_FALSE == eglBindAPI(EGL_OPENGL_ES_API)){
		arcan_warning("(egl) couldn't bind API\n");
		return false;
	}

	egl.ctx = eglCreateContext(egl.disp, egl.cfg, EGL_NO_CONTEXT, ca);
	if (egl.ctx == EGL_NO_CONTEXT){
		arcan_warning("(egl) Couldn't create EGL/GLES context\n");
		return false;
	}

#ifdef WITH_BCM
	alloc_bcm_wnd(&w, &h);
#endif

	egl.surf = eglCreateWindowSurface(egl.disp, egl.cfg, egl.wnd, NULL);
	if (egl.surf == EGL_NO_SURFACE){
		arcan_warning("Couldn't create window\n");
		return false;
	}

	if (!eglMakeCurrent(egl.disp, egl.surf, egl.surf, egl.ctx)){
		arcan_warning("Couldn't activate context\n");
		return false;
	}

/*
 * Interestingly enough, EGL swap allows dirty rect updates with
 * eglSwapBuffersREegionNOK. In animations, we can, each update,
 * take the full boundary volume or better yet, go quadtree
 * and do dirty regions that way. Not leveraged yet but should
 * definitely be a concern later on.
 */
	#define check() assert(glGetError() == 0)
	check();

	arcan_warning("EGL context active (%d x %d)\n", w, h);
	arcan_video_display.width = w;
	arcan_video_display.height = h;
	arcan_video_display.bpp = bpp;
	glViewport(0, 0, w, h);

	eglSwapInterval(egl.disp, 1);

	return true;
}
#endif

void PLATFORM_SYMBOL(_video_setsynch)(const char* arg)
{
	int ind = 0;

	while(egl_synchopts[ind]){
		if (strcmp(egl_synchopts[ind], arg) == 0){
			synchopt = (ind > 0 ? ind / 2 : ind);
			arcan_warning("synchronisation strategy set to (%s)\n",
				egl_synchopts[ind]);
			break;
		}

		ind += 2;
	}
}

void PLATFORM_SYMBOL(_video_synch)(uint64_t tick_count, float fract,
	video_synchevent pre, video_synchevent post)
{
	if (pre)
		pre();

	arcan_bench_register_cost( arcan_video_refresh(fract) );

	eglSwapBuffers(egl.disp, egl.surf);

#ifdef WITH_GBMKMS
	struct gbm_bo* bo = gbm_surface_lock_front_buffer(gbmkms.surf);

	unsigned handle = gbm_bo_get_handle(bo).u32;
	unsigned stride = gbm_bo_get_stride(bo);

	if (-1 == drmModeAddFB(gbmkms.fd,
		arcan_video_display.width, arcan_video_display.height,
		24, 32, stride, handle, &gbmkms.fb_id))
		arcan_fatal("platform/egl: couldn't obtain framebuffer handle\n");

	int fl;
	if (-1 ==
		drmModePageFlip(gbmkms.fd, gbmkms.enc->crtc_id,
			gbmkms.fb_id, DRM_MODE_PAGE_FLIP_EVENT, &fl))
		arcan_fatal("platform/egl: waiting for flip failure\n");

	int buf;
	read(gbmkms.fd, &buf, 1);
	gbm_surface_release_buffer(gbmkms.surf, bo);

// gbm_surface_has_free_buffers(surf)
/*	drmModeSetCrtc(fd, kms.encoder->crtc_id, kms.fb_id, 0, 0,
		&kms.connector->connector_id, 1, &kms.mode);*/
#endif

	if (post)
		post();
}

const char** PLATFORM_SYMBOL(_video_synchopts) ()
{
	return egl_synchopts;
}

void PLATFORM_SYMBOL(_video_prepare_external) () {}
void PLATFORM_SYMBOL(_video_restore_external) () {}
void PLATFORM_SYMBOL(_video_shutdown) ()
{
	eglDestroyContext(egl.disp, egl.ctx);
	eglDestroySurface(egl.disp, egl.surf);
  eglTerminate(egl.disp);
}

void PLATFORM_SYMBOL(_video_timing) (
	float* vsync, float* stddev, float* variance)
{
	*vsync = 16.667;
	*stddev = 0.01;
	*variance = 0.01;
}

void PLATFORM_SYMBOL(_video_minimize) () {}

