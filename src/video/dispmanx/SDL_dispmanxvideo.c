/*
	SDL - Simple DirectMedia Layer
	Copyright (C) 1997-2012 Sam Lantinga

	This library is free software; you can redistribute it and/or
	modify it under the terms of the GNU Lesser General Public
	License as published by the Free Software Foundation; either
	version 2.1 of the License, or (at your option) any later version.

	This library is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
	Lesser General Public License for more details.

	You should have received a copy of the GNU Lesser General Public
	License along with this library; if not, write to the Free Software
	Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

	Sam Lantinga
	slouken@libsdl.org
*/

#include "SDL_config.h"

#include <stdio.h>

#include <string.h>

#include <bcm_host.h>

#include "SDL_video.h"
#include "SDL_mouse.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../fbcon/SDL_fbmouse_c.h"
#include "../fbcon/SDL_fbevents_c.h"

#define min(a,b) ((a)<(b)?(a):(b))
#define RGB565(r,g,b) (((r)>>3)<<11 | ((g)>>2)<<5 | (b)>>3)

/* Initialization/Query functions */
static int DISPMANX_VideoInit(_THIS, SDL_PixelFormat *vformat);
static SDL_Rect **DISPMANX_ListModes(_THIS, SDL_PixelFormat *format, Uint32 flags);
static SDL_Surface *DISPMANX_SetVideoMode(_THIS, SDL_Surface *current, int width, int height, int bpp, Uint32 flags);
static int DISPMANX_SetColors(_THIS, int firstcolor, int ncolors, SDL_Color *colors);
static void DISPMANX_VideoQuit(_THIS);

/* Hardware surface functions */
static void DISPMANX_WaitVBL(_THIS);
static void DISPMANX_WaitIdle(_THIS);
static void DISPMANX_DirectUpdate(_THIS, int numrects, SDL_Rect *rects);
static void DISPMANX_BlankBackground(void);
static void DISPMANX_FreeResources(void);
static void DISPMANX_FreeBackground (void);

typedef struct {
	DISPMANX_DISPLAY_HANDLE_T   display;
	DISPMANX_MODEINFO_T         amode;
	void                        *pixmem;
	DISPMANX_UPDATE_HANDLE_T    update;
	DISPMANX_RESOURCE_HANDLE_T  resources[2];
	DISPMANX_ELEMENT_HANDLE_T   element;
	VC_IMAGE_TYPE_T             pix_format;
	uint32_t                    vc_image_ptr;
	VC_DISPMANX_ALPHA_T         *alpha;
	VC_RECT_T                   src_rect;
	VC_RECT_T                   dst_rect;
	VC_RECT_T                   bmp_rect;
	int bits_per_pixel;
	int pitch;

	DISPMANX_RESOURCE_HANDLE_T  b_resource;
	DISPMANX_ELEMENT_HANDLE_T   b_element;
	DISPMANX_UPDATE_HANDLE_T    b_update;

	int ignore_ratio;

} __DISPMAN_VARIABLES_T;


static __DISPMAN_VARIABLES_T _DISPMAN_VARS;
static __DISPMAN_VARIABLES_T *dispvars = &_DISPMAN_VARS;

static int DISPMANX_Available(void)
{
	return (1);
}

static void DISPMANX_DeleteDevice(SDL_VideoDevice *device)
{
	SDL_free(device->hidden);
	SDL_free(device);
}

static SDL_VideoDevice *DISPMANX_CreateDevice(int devindex)
{
	SDL_VideoDevice *this;

	/* Initialize all variables that we clean on shutdown */
	this = (SDL_VideoDevice *)SDL_malloc(sizeof(SDL_VideoDevice));
	if ( this ) {
		SDL_memset(this, 0, (sizeof *this));
		this->hidden = (struct SDL_PrivateVideoData *)
				SDL_malloc((sizeof *this->hidden));
	}
	if ( (this == NULL) || (this->hidden == NULL) ) {
		SDL_OutOfMemory();
		if ( this ) {
			SDL_free(this);
		}
		return(0);
	}
	SDL_memset(this->hidden, 0, (sizeof *this->hidden));
	wait_vbl = DISPMANX_WaitVBL;
	wait_idle = DISPMANX_WaitIdle;
	mouse_fd = -1;
	keyboard_fd = -1;

	/* Set the function pointers */
	this->VideoInit = DISPMANX_VideoInit;
	this->ListModes = DISPMANX_ListModes;
	this->SetVideoMode = DISPMANX_SetVideoMode;
	this->SetColors = DISPMANX_SetColors;
	this->UpdateRects = DISPMANX_DirectUpdate;
	this->VideoQuit = DISPMANX_VideoQuit;
	this->CheckHWBlit = NULL;
	this->FillHWRect = NULL;
	this->SetHWColorKey = NULL;
	this->SetHWAlpha = NULL;
	this->SetCaption = NULL;
	this->SetIcon = NULL;
	this->IconifyWindow = NULL;
	this->GrabInput = NULL;
	this->GetWMInfo = NULL;
	this->InitOSKeymap = FB_InitOSKeymap;
	this->PumpEvents = FB_PumpEvents;
	this->CreateYUVOverlay = NULL;

	this->free = DISPMANX_DeleteDevice;

	return this;
}

VideoBootStrap DISPMANX_bootstrap = {
	"dispmanx", "Dispmanx Raspberry Pi VC",
	DISPMANX_Available, DISPMANX_CreateDevice
};

static int DISPMANX_VideoInit(_THIS, SDL_PixelFormat *vformat)
{
#if !SDL_THREADS_DISABLED
	/* Create the hardware surface lock mutex */
	hw_lock = SDL_CreateMutex();
	if ( hw_lock == NULL ) {
		SDL_SetError("Unable to create lock mutex");
		DISPMANX_VideoQuit(this);
		return(-1);
	}
#endif

	/* Enable mouse and keyboard support */
	if ( FB_OpenKeyboard(this) < 0 ) {
		DISPMANX_VideoQuit(this);
		return(-1);
	}
	if ( FB_OpenMouse(this) < 0 ) {
		const char *sdl_nomouse;

		sdl_nomouse = SDL_getenv("SDL_NOMOUSE");
		if ( ! sdl_nomouse ) {
			SDL_SetError("Unable to open mouse");
			DISPMANX_VideoQuit(this);
			return(-1);
		}
	}

	vformat->BitsPerPixel = 16;
	vformat->Rmask = 0;
	vformat->Gmask = 0;
	vformat->Bmask = 0;

	/* We're done! */
	return(0);
}

static SDL_Surface *DISPMANX_SetVideoMode(_THIS, SDL_Surface *current, int width, int height, int bpp, Uint32 flags)
{
	if ((width == 0) | (height == 0)) goto go_video_console;

	uint32_t screen = 0;

	bcm_host_init();

	dispvars->display = vc_dispmanx_display_open( screen );

	vc_dispmanx_display_get_info( dispvars->display, &(dispvars->amode));
	printf( "Dispmanx: Physical video mode is %d x %d\n",
	dispvars->amode.width, dispvars->amode.height );

	DISPMANX_BlankBackground();

	Uint32 Rmask;
	Uint32 Gmask;
	Uint32 Bmask;

	dispvars->bits_per_pixel = bpp;
	dispvars->pitch = ( ALIGN_UP( width, 16 ) * (bpp/8) );

	height = ALIGN_UP( height, 16);

	switch (bpp) {
		case 8:
			dispvars->pix_format = VC_IMAGE_8BPP;
			break;
		case 16:
			dispvars->pix_format = VC_IMAGE_RGB565;
			break;
		case 32:
			dispvars->pix_format = VC_IMAGE_XRGB8888;
			break;
		default:
			printf ("Dispmanx: [ERROR] - wrong bpp: %d\n",bpp);
			return (NULL);
	}

	printf ("Dispmanx: Using internal program mode: %d x %d %d bpp\n",
		width, height, dispvars->bits_per_pixel);

	printf ("Dispmanx: Using physical mode: %d x %d %d bpp\n",
		dispvars->amode.width, dispvars->amode.height,
		dispvars->bits_per_pixel);

	dispvars->ignore_ratio = (int) SDL_getenv("SDL_DISPMANX_IGNORE_RATIO");

	if (dispvars->ignore_ratio)
		vc_dispmanx_rect_set( &(dispvars->dst_rect), 0, 0, dispvars->amode.width , dispvars->amode.height );
	else {
		float width_scale, height_scale;
		width_scale = (float) dispvars->amode.width / width;
		height_scale = (float) dispvars->amode.height / height;
		float scale = min(width_scale, height_scale);
		int dst_width = width * scale;
		int dst_height = height * scale;

		int dst_xpos  = (dispvars->amode.width - dst_width) / 2;
		int dst_ypos  = (dispvars->amode.height - dst_height) / 2;

		printf ("Dispmanx: Scaling to %d x %d\n", dst_width, dst_height);

		vc_dispmanx_rect_set( &(dispvars->dst_rect), dst_xpos, dst_ypos,
		dst_width , dst_height );
	}

	vc_dispmanx_rect_set (&(dispvars->bmp_rect), 0, 0, width, height);

	vc_dispmanx_rect_set (&(dispvars->src_rect), 0, 0, width << 16, height << 16);

	VC_DISPMANX_ALPHA_T layerAlpha;

	layerAlpha.flags = DISPMANX_FLAGS_ALPHA_FIXED_ALL_PIXELS;
	layerAlpha.opacity = 255;
	layerAlpha.mask	   = 0;
	dispvars->alpha = &layerAlpha;

	dispvars->resources[0] = vc_dispmanx_resource_create( dispvars->pix_format, width, height, &(dispvars->vc_image_ptr) );
	dispvars->resources[1] = vc_dispmanx_resource_create( dispvars->pix_format, width, height, &(dispvars->vc_image_ptr) );

	dispvars->pixmem = calloc( 1, dispvars->pitch * height);

	Rmask = 0;
	Gmask = 0;
	Bmask = 0;
	if ( ! SDL_ReallocFormat(current, bpp, Rmask, Gmask, Bmask, 0) ) {
		return(NULL);
	}

	current->w = width;
	current->h = height;

	current->pitch  = dispvars->pitch;
	current->pixels = dispvars->pixmem;

	dispvars->update = vc_dispmanx_update_start( 0 );

	dispvars->element = vc_dispmanx_element_add( dispvars->update,
		dispvars->display, 0 /*layer*/, &(dispvars->dst_rect),
		dispvars->resources[flip_page], &(dispvars->src_rect),
		DISPMANX_PROTECTION_NONE, dispvars->alpha, 0 /*clamp*/,
		/*VC_IMAGE_ROT0*/ 0 );

	vc_dispmanx_update_submit_sync( dispvars->update );

	go_video_console:
	if ( FB_EnterGraphicsMode(this) < 0 )
		return(NULL);

	return(current);
}

static void DISPMANX_BlankBackground(void)
{
	VC_IMAGE_TYPE_T type = VC_IMAGE_RGB565;
	uint32_t vc_image_ptr;
	uint16_t image = 0x0000; // black

	VC_RECT_T dst_rect, src_rect;

	dispvars->b_resource = vc_dispmanx_resource_create( type, 1 /*width*/, 1 /*height*/, &vc_image_ptr );

	vc_dispmanx_rect_set( &dst_rect, 0, 0, 1, 1);

	vc_dispmanx_resource_write_data( dispvars->b_resource, type, sizeof(image), &image, &dst_rect );

	vc_dispmanx_rect_set( &src_rect, 0, 0, 1<<16, 1<<16);
	vc_dispmanx_rect_set( &dst_rect, 0, 0, 0, 0);

	dispvars->b_update = vc_dispmanx_update_start(0);

	dispvars->b_element = vc_dispmanx_element_add(dispvars->b_update, dispvars->display, -1 /*layer*/, &dst_rect,
		dispvars->b_resource, &src_rect, DISPMANX_PROTECTION_NONE, NULL, NULL, (DISPMANX_TRANSFORM_T)0 );

	vc_dispmanx_update_submit_sync( dispvars->b_update );
}

static void DISPMANX_WaitVBL(_THIS)
{
	return;
}

static void DISPMANX_WaitIdle(_THIS)
{
	return;
}

static void DISPMANX_DirectUpdate(_THIS, int numrects, SDL_Rect *rects)
{
	vc_dispmanx_resource_write_data( dispvars->resources[flip_page],
		dispvars->pix_format, dispvars->pitch, dispvars->pixmem,
		&(dispvars->bmp_rect) );

	dispvars->update = vc_dispmanx_update_start( 0 );

	vc_dispmanx_element_change_source(dispvars->update, dispvars->element, dispvars->resources[flip_page]);

	vc_dispmanx_update_submit_sync( dispvars->update );

	flip_page = !flip_page;

	return;
}

static int DISPMANX_SetColors(_THIS, int firstcolor, int ncolors, SDL_Color *colors)
{
	int i;
	static unsigned short pal[256];

	//Set up the colormap
	for (i = 0; i < ncolors; i++) {
		pal[i] = RGB565 ((colors[i]).r, (colors[i]).g, (colors[i]).b);
	}
	vc_dispmanx_resource_set_palette(  dispvars->resources[flip_page], pal, 0, sizeof pal );
	vc_dispmanx_resource_set_palette(  dispvars->resources[!flip_page], pal, 0, sizeof pal );

	return(1);
}

static SDL_Rect **DISPMANX_ListModes(_THIS, SDL_PixelFormat *format, Uint32 flags)
{
	return((SDL_Rect **)-1);
}

static void DISPMANX_FreeResources(void){
	dispvars->update = vc_dispmanx_update_start( 0 );
	vc_dispmanx_element_remove(dispvars->update, dispvars->element);
	vc_dispmanx_update_submit_sync( dispvars->update );

	vc_dispmanx_resource_delete( dispvars->resources[0] );
	vc_dispmanx_resource_delete( dispvars->resources[1] );

	vc_dispmanx_display_close( dispvars->display );
}

static void DISPMANX_FreeBackground (void) {
	dispvars->b_update = vc_dispmanx_update_start( 0 );

	vc_dispmanx_resource_delete( dispvars->b_resource );
	vc_dispmanx_element_remove ( dispvars->b_update, dispvars->b_element);

	vc_dispmanx_update_submit_sync( dispvars->b_update );
}

static void DISPMANX_VideoQuit(_THIS)
{
	/* Clear the lock mutex */
	if ( hw_lock ) {
		SDL_DestroyMutex(hw_lock);
		hw_lock = NULL;
	}

	if (dispvars->pixmem != NULL){
		DISPMANX_FreeBackground();
		DISPMANX_FreeResources();
	}

	FB_CloseMouse(this);
	FB_CloseKeyboard(this);
}
