/* -*- mode: c; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/* vi: set ts=8 sw=8 sts=8: */
/*************************************************************************/ /*!
@File
@Codingstyle    LinuxKernel
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@License        Dual MIT/GPLv2

The contents of this file are subject to the MIT license as set out below.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

Alternatively, the contents of this file may be used under the terms of
the GNU General Public License Version 2 ("GPL") in which case the provisions
of GPL are applicable instead of those above.

If you wish to allow use of your version of this file only under the terms of
GPL, and not to allow others to use your version of this file under the terms
of the MIT license, indicate your decision by deleting the provisions above
and replace them with the notice and other provisions required by GPL as set
out in the file called "GPL-COPYING" included in this distribution. If you do
not delete the provisions above, a recipient may use your version of this file
under the terms of either the MIT license or GPL.

This License is also included in this distribution in the file called
"MIT-COPYING".

EXCEPT AS OTHERWISE STATED IN A NEGOTIATED AGREEMENT: (A) THE SOFTWARE IS
PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE AND NONINFRINGEMENT; AND (B) IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/ /**************************************************************************/

#include <linux/moduleparam.h>
#include <linux/version.h>

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 18, 0))
#include <drm/drm_gem.h>
#endif

#include "drm_pdp_drv.h"
#include "kernel_compatibility.h"

#define PDP_WIDTH_MIN			640
#define PDP_WIDTH_MAX			1280
#define PDP_HEIGHT_MIN			480
#define PDP_HEIGHT_MAX			1024

#define ODIN_PDP_WIDTH_MAX		1920
#define ODIN_PDP_HEIGHT_MAX		1080

#define PLATO_PDP_WIDTH_MAX		1920
#define PLATO_PDP_HEIGHT_MAX	1080

static bool async_flip_enable = true;

module_param(async_flip_enable,
	     bool,
	     S_IRUSR | S_IRGRP | S_IROTH);

MODULE_PARM_DESC(async_flip_enable,
		 "Enable support for 'faked' async flipping (default: Y)");


static void pdp_framebuffer_destroy(struct drm_framebuffer *fb)
{
	struct pdp_framebuffer *pdp_fb = to_pdp_framebuffer(fb);

	DRM_DEBUG_DRIVER("[FB:%d]\n", fb->base.id);

	drm_framebuffer_cleanup(fb);

	drm_gem_object_unreference_unlocked(pdp_fb->obj);

	kfree(pdp_fb);
}

static int pdp_framebuffer_create_handle(struct drm_framebuffer *fb,
					 struct drm_file *file,
					 unsigned int *handle)
{
	struct pdp_framebuffer *pdp_fb = to_pdp_framebuffer(fb);

	DRM_DEBUG_DRIVER("[FB:%d]\n", fb->base.id);

	return drm_gem_handle_create(file, pdp_fb->obj, handle);
}

static const struct drm_framebuffer_funcs pdp_framebuffer_funcs = {
	.destroy = pdp_framebuffer_destroy,
	.create_handle = pdp_framebuffer_create_handle,
	.dirty = NULL,
};

static int pdp_framebuffer_init(struct pdp_drm_private *dev_priv,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 5, 0)) || \
    (defined(CHROMIUMOS_KERNEL) && (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 18, 0)))
				const
#endif
				struct drm_mode_fb_cmd2 *mode_cmd,
				struct pdp_framebuffer *pdp_fb,
				struct drm_gem_object *obj)
{
	int err;

	switch (mode_cmd->pixel_format) {
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB8888:
		break;
	default:
		DRM_ERROR("pixel format not supported (format = %u)\n",
			  mode_cmd->pixel_format);
		return -EINVAL;
	}

	if (mode_cmd->flags & DRM_MODE_FB_INTERLACED) {
		DRM_ERROR("interlaced framebuffers not supported\n");
		return -EINVAL;
	}

	pdp_fb->base.dev = dev_priv->dev;
	drm_helper_mode_fill_fb_struct(dev_priv->dev, &pdp_fb->base, mode_cmd);
	pdp_fb->obj = obj;

	err = drm_framebuffer_init(dev_priv->dev,
				   &pdp_fb->base,
				   &pdp_framebuffer_funcs);
	if (err) {
		DRM_ERROR("failed to initialise framebuffer (err=%d)\n",
			  err);
		return err;
	}

	DRM_DEBUG_DRIVER("[FB:%d]\n", pdp_fb->base.base.id);

	return 0;
}


/*************************************************************************/ /*!
 DRM mode config callbacks
*/ /**************************************************************************/

static struct drm_framebuffer *
pdp_fb_create(struct drm_device *dev,
			struct drm_file *file,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 5, 0)) || \
    (defined(CHROMIUMOS_KERNEL) && (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 18, 0)))
			const
#endif
			struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct pdp_drm_private *dev_priv = dev->dev_private;
	struct pdp_framebuffer *pdp_fb;
	struct drm_gem_object *bo;
	int err;

	bo = drm_gem_object_lookup(file, mode_cmd->handles[0]);
	if (!bo) {
		DRM_ERROR("failed to find buffer with handle %u\n",
			  mode_cmd->handles[0]);
		return ERR_PTR(-ENOENT);
	}

	pdp_fb = kzalloc(sizeof(*pdp_fb), GFP_KERNEL);
	if (!pdp_fb) {
		drm_gem_object_unreference_unlocked(bo);
		return ERR_PTR(-ENOMEM);
	}

	err = pdp_framebuffer_init(dev_priv, mode_cmd, pdp_fb, bo);
	if (err) {
		kfree(pdp_fb);
		drm_gem_object_unreference_unlocked(bo);
		return ERR_PTR(err);
	}

	DRM_DEBUG_DRIVER("[FB:%d]\n", pdp_fb->base.base.id);

	return &pdp_fb->base;
}

static const struct drm_mode_config_funcs pdp_mode_config_funcs = {
	.fb_create = pdp_fb_create,
	.output_poll_changed = NULL,
};


int pdp_modeset_init(struct pdp_drm_private *dev_priv)
{
	struct drm_device *dev = dev_priv->dev;
	int err;

	drm_mode_config_init(dev);

	dev->mode_config.funcs = &pdp_mode_config_funcs;
	dev->mode_config.min_width = PDP_WIDTH_MIN;
	dev->mode_config.min_height = PDP_HEIGHT_MIN;

	switch (dev_priv->version) {
	case PDP_VERSION_APOLLO:
		dev->mode_config.max_width = PDP_WIDTH_MAX;
		dev->mode_config.max_height = PDP_HEIGHT_MAX;
		break;
	case PDP_VERSION_ODIN:
		dev->mode_config.max_width = ODIN_PDP_WIDTH_MAX;
		dev->mode_config.max_height = ODIN_PDP_HEIGHT_MAX;
		break;
	case PDP_VERSION_PLATO:
		dev->mode_config.max_width = PLATO_PDP_WIDTH_MAX;
		dev->mode_config.max_height = PLATO_PDP_HEIGHT_MAX;
		break;
	default:
		BUG();
	}

	DRM_INFO("max_width is %d\n",
		dev->mode_config.max_width);
	DRM_INFO("max_height is %d\n",
		dev->mode_config.max_height);

	dev->mode_config.fb_base = 0;
	dev->mode_config.async_page_flip = async_flip_enable;

	DRM_INFO("%s async flip support is %s\n",
		 dev->driver->name, async_flip_enable ? "enabled" : "disabled");

	dev_priv->crtc = pdp_crtc_create(dev, 0);
	if (IS_ERR(dev_priv->crtc)) {
		DRM_ERROR("failed to create a CRTC\n");
		err = PTR_ERR(dev_priv->crtc);
		goto err_config_cleanup;
	}

	switch (dev_priv->version) {
	case PDP_VERSION_APOLLO:
	case PDP_VERSION_ODIN:
		dev_priv->connector = pdp_dvi_connector_create(dev);
		if (IS_ERR(dev_priv->connector)) {
			DRM_ERROR("failed to create a connector\n");
			err = PTR_ERR(dev_priv->connector);
			goto err_config_cleanup;
		}

		dev_priv->encoder = pdp_tmds_encoder_create(dev);
		if (IS_ERR(dev_priv->encoder)) {
			DRM_ERROR("failed to create an encoder\n");
			err = PTR_ERR(dev_priv->encoder);
			goto err_config_cleanup;
		}

		err = drm_mode_connector_attach_encoder(dev_priv->connector,
							dev_priv->encoder);
		if (err) {
			DRM_ERROR("failed to attach [ENCODER:%d:%s] to "
				  "[CONNECTOR:%d:%s] (err=%d)\n",
				  dev_priv->encoder->base.id,
				  dev_priv->encoder->name,
				  dev_priv->connector->base.id,
				  dev_priv->connector->name,
				  err);
			goto err_config_cleanup;
		}

		err = drm_connector_register(dev_priv->connector);
		if (err) {
			DRM_ERROR("[CONNECTOR:%d:%s] failed to register (err=%d)\n",
				  dev_priv->connector->base.id,
				  dev_priv->connector->name,
				  err);
			goto err_config_cleanup;
		}
		break;
	case PDP_VERSION_PLATO:
		/* dev_priv->connector = pdp_hdmi_connector_create(dev);
		if (IS_ERR(dev_priv->connector)) {
			DRM_ERROR("failed to create a connector\n");
			err = PTR_ERR(dev_priv->connector);
			goto err_config_cleanup;
		} */
		break;
	default:
		BUG();
	}

	DRM_DEBUG_DRIVER("initialised\n");

	return 0;

err_config_cleanup:
	drm_mode_config_cleanup(dev);

	return err;
}

void pdp_modeset_cleanup(struct pdp_drm_private *dev_priv)
{
	drm_mode_config_cleanup(dev_priv->dev);

	DRM_DEBUG_DRIVER("cleaned up\n");
}
