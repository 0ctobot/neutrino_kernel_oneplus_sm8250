
/*
 * drm_sysfs.c - Modifications to drm_sysfs_class.c to support
 *               extra sysfs attribute from DRM. Normal drm_sysfs_class
 *               does not allow adding attributes.
 *
 * Copyright (c) 2004 Jon Smirl <jonsmirl@gmail.com>
 * Copyright (c) 2003-2004 Greg Kroah-Hartman <greg@kroah.com>
 * Copyright (c) 2003-2004 IBM Corp.
 *
 * This file is released under the GPLv2
 *
 */

#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/gfp.h>
#include <linux/err.h>
#include <linux/export.h>

#include <drm/drm_sysfs.h>
#include <drm/drmP.h>
#include "drm_internal.h"
#include <linux/list.h>
#include <linux/of.h>
#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/module.h>
#include <linux/init.h>
#include <drm/drm_mipi_dsi.h>
#include <linux/input.h>
#include <linux/proc_fs.h>

#define to_drm_minor(d) dev_get_drvdata(d)
#define to_drm_connector(d) dev_get_drvdata(d)

#define DSI_PANEL_SAMSUNG_S6E3HC2 0
#define DSI_PANEL_SAMSUNG_S6E3FC2X01 1
#define DSI_PANEL_SAMSUNG_SOFEF03F_M 2
#define DSI_PANEL_SAMSUNG_ANA6705 3
#define DSI_PANEL_SAMSUNG_ANA6706 4
#define DSI_PANEL_SAMSUNG_AMB655XL 5

int dsi_cmd_log_enable;
EXPORT_SYMBOL(dsi_cmd_log_enable);

/**
 * DOC: overview
 *
 * DRM provides very little additional support to drivers for sysfs
 * interactions, beyond just all the standard stuff. Drivers who want to expose
 * additional sysfs properties and property groups can attach them at either
 * &drm_device.dev or &drm_connector.kdev.
 *
 * Registration is automatically handled when calling drm_dev_register(), or
 * drm_connector_register() in case of hot-plugged connectors. Unregistration is
 * also automatically handled by drm_dev_unregister() and
 * drm_connector_unregister().
 */

static struct device_type drm_sysfs_device_minor = {
	.name = "drm_minor"
};

struct class *drm_class;

static char *drm_devnode(struct device *dev, umode_t *mode)
{
	return kasprintf(GFP_KERNEL, "dri/%s", dev_name(dev));
}

static CLASS_ATTR_STRING(version, S_IRUGO, "drm 1.1.0 20060810");

/**
 * drm_sysfs_init - initialize sysfs helpers
 *
 * This is used to create the DRM class, which is the implicit parent of any
 * other top-level DRM sysfs objects.
 *
 * You must call drm_sysfs_destroy() to release the allocated resources.
 *
 * Return: 0 on success, negative error code on failure.
 */
int drm_sysfs_init(void)
{
	int err;

	drm_class = class_create(THIS_MODULE, "drm");
	if (IS_ERR(drm_class))
		return PTR_ERR(drm_class);

	err = class_create_file(drm_class, &class_attr_version.attr);
	if (err) {
		class_destroy(drm_class);
		drm_class = NULL;
		return err;
	}

	drm_class->devnode = drm_devnode;
	return 0;
}

/**
 * drm_sysfs_destroy - destroys DRM class
 *
 * Destroy the DRM device class.
 */
void drm_sysfs_destroy(void)
{
	if (IS_ERR_OR_NULL(drm_class))
		return;
	class_remove_file(drm_class, &class_attr_version.attr);
	class_destroy(drm_class);
	drm_class = NULL;
}

/*
 * Connector properties
 */
static ssize_t status_store(struct device *device,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct drm_connector *connector = to_drm_connector(device);
	struct drm_device *dev = connector->dev;
	enum drm_connector_force old_force;
	int ret;

	ret = mutex_lock_interruptible(&dev->mode_config.mutex);
	if (ret)
		return ret;

	old_force = connector->force;

	if (sysfs_streq(buf, "detect"))
		connector->force = 0;
	else if (sysfs_streq(buf, "on"))
		connector->force = DRM_FORCE_ON;
	else if (sysfs_streq(buf, "on-digital"))
		connector->force = DRM_FORCE_ON_DIGITAL;
	else if (sysfs_streq(buf, "off"))
		connector->force = DRM_FORCE_OFF;
	else
		ret = -EINVAL;

	if (old_force != connector->force || !connector->force) {
		DRM_DEBUG_KMS("[CONNECTOR:%d:%s] force updated from %d to %d or reprobing\n",
			      connector->base.id,
			      connector->name,
			      old_force, connector->force);

		connector->funcs->fill_modes(connector,
					     dev->mode_config.max_width,
					     dev->mode_config.max_height);
	}

	mutex_unlock(&dev->mode_config.mutex);

	return ret ? ret : count;
}

static ssize_t status_show(struct device *device,
			   struct device_attribute *attr,
			   char *buf)
{
	struct drm_connector *connector = to_drm_connector(device);
	enum drm_connector_status status;

	status = READ_ONCE(connector->status);

	return snprintf(buf, PAGE_SIZE, "%s\n",
			drm_get_connector_status_name(status));
}

static ssize_t dpms_show(struct device *device,
			   struct device_attribute *attr,
			   char *buf)
{
	struct drm_connector *connector = to_drm_connector(device);
	int dpms;

	dpms = READ_ONCE(connector->dpms);

	return snprintf(buf, PAGE_SIZE, "%s\n",
			drm_get_dpms_name(dpms));
}

static ssize_t enabled_show(struct device *device,
			    struct device_attribute *attr,
			   char *buf)
{
	struct drm_connector *connector = to_drm_connector(device);
	bool enabled;

	enabled = READ_ONCE(connector->encoder);

	return snprintf(buf, PAGE_SIZE, enabled ? "enabled\n" : "disabled\n");
}

static ssize_t edid_show(struct file *filp, struct kobject *kobj,
			 struct bin_attribute *attr, char *buf, loff_t off,
			 size_t count)
{
	struct device *connector_dev = kobj_to_dev(kobj);
	struct drm_connector *connector = to_drm_connector(connector_dev);
	unsigned char *edid;
	size_t size;
	ssize_t ret = 0;

	mutex_lock(&connector->dev->mode_config.mutex);
	if (!connector->edid_blob_ptr)
		goto unlock;

	edid = connector->edid_blob_ptr->data;
	size = connector->edid_blob_ptr->length;
	if (!edid)
		goto unlock;

	if (off >= size)
		goto unlock;

	if (off + count > size)
		count = size - off;
	memcpy(buf, edid + off, count);

	ret = count;
unlock:
	mutex_unlock(&connector->dev->mode_config.mutex);

	return ret;
}

static ssize_t modes_show(struct device *device,
			   struct device_attribute *attr,
			   char *buf)
{
	struct drm_connector *connector = to_drm_connector(device);
	struct drm_display_mode *mode;
	int written = 0;

	mutex_lock(&connector->dev->mode_config.mutex);
	list_for_each_entry(mode, &connector->modes, head) {
		written += snprintf(buf + written, PAGE_SIZE - written, "%s\n",
				    mode->name);
	}
	mutex_unlock(&connector->dev->mode_config.mutex);

	return written;
}
static ssize_t acl_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int acl_mode = 0;

	acl_mode = dsi_display_get_acl_mode(connector);

	ret = scnprintf(buf, PAGE_SIZE, "acl mode = %d\n"
									"0--acl mode(off)\n"
									"1--acl mode(5)\n"
									"2--acl mode(10)\n"
									"3--acl mode(15)\n",
									acl_mode);
	return ret;
}

static ssize_t acl_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int acl_mode = 0;

	ret = kstrtoint(buf, 10, &acl_mode);
	if (ret) {
		pr_err("kstrtoint failed. ret=%d\n", ret);
		return ret;
	}

	ret = dsi_display_set_acl_mode(connector, acl_mode);
	if (ret)
		pr_err("set acl mode(%d) fail\n", acl_mode);

	return count;
}
static ssize_t hbm_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int hbm_mode = 0;

	hbm_mode = dsi_display_get_hbm_mode(connector);

	ret = scnprintf(buf, PAGE_SIZE, "hbm mode = %d\n"
											"0--hbm mode(off)\n"
											"1--hbm mode(XX)\n"
											"2--hbm mode(XX)\n"
											"3--hbm mode(XX)\n"
											"4--hbm mode(XX)\n"
											"5--hbm mode(670)\n",
											hbm_mode);
	return ret;
}

static ssize_t hbm_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int hbm_mode = 0;
	int panel_stage_info = 0;

	ret = kstrtoint(buf, 10, &hbm_mode);
	if (ret) {
		pr_err("kstrtoint failed. ret=%d\n", ret);
		return ret;
	}

	if (dsi_panel_name == DSI_PANEL_SAMSUNG_ANA6705) {
		panel_stage_info = dsi_display_get_stage_info(connector);
		if (((panel_stage_info == 0x02) || (panel_stage_info == 0x03)
			|| (panel_stage_info == 0x04)) && (hbm_mode == 4)) {
			hbm_mode = hbm_mode - 1;
		} else {
			pr_err("19821 panel stage version is T0/DVT2/PVT&MP");
		}
	}
	ret = dsi_display_set_hbm_mode(connector, hbm_mode);
	if (ret)
		pr_err("set hbm mode(%d) fail\n", hbm_mode);

	return count;
}

static ssize_t seed_lp_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int seed_lp_mode = 0;

	ret = kstrtoint(buf, 10, &seed_lp_mode);
	if (ret) {
		pr_err("kstrtoint failed. ret=%d\n", ret);
		return ret;
	}

	if ((dsi_panel_name == DSI_PANEL_SAMSUNG_ANA6706) ||
		(dsi_panel_name == DSI_PANEL_SAMSUNG_ANA6705)) {
		ret = dsi_display_set_seed_lp_mode(connector, seed_lp_mode);
		if (ret)
			pr_err("set seed lp (%d) fail\n", seed_lp_mode);
	}

	return count;
}
static ssize_t seed_lp_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int seed_lp_mode = 0;

	if ((dsi_panel_name == DSI_PANEL_SAMSUNG_ANA6706) ||
		(dsi_panel_name == DSI_PANEL_SAMSUNG_ANA6705)) {
		seed_lp_mode = dsi_display_get_seed_lp_mode(connector);
	}
	ret = scnprintf(buf, PAGE_SIZE, "seed lp mode = %d\n"
									"4--seed lp mode(off)\n"
									"0--seed lp mode(mode0)\n"
									"1--seed lp mode(mode1)\n"
									"2--seed lp mode(mode2)\n",
									seed_lp_mode);
	return ret;
}
static ssize_t hbm_brightness_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int hbm_brightness = 0;

	hbm_brightness = dsi_display_get_hbm_brightness(connector);

	ret = scnprintf(buf, PAGE_SIZE, "%d\n", hbm_brightness);
	return ret;
}

static ssize_t hbm_brightness_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int hbm_brightness = 0;

	ret = kstrtoint(buf, 10, &hbm_brightness);
	if (ret) {
		pr_err("kstrtoint failed. ret=%d\n", ret);
		return ret;
	}

	ret = dsi_display_set_hbm_brightness(connector, hbm_brightness);
	if (ret)
		pr_err("set hbm brightness (%d) failed\n", hbm_brightness);
	return count;
}

static ssize_t op_friginer_print_hbm_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int op_hbm_mode = 0;

	op_hbm_mode = dsi_display_get_fp_hbm_mode(connector);

	ret = scnprintf(buf, PAGE_SIZE, "OP_FP mode = %d\n"
									"0--finger-hbm mode(off)\n"
									"1--finger-hbm mode(600)\n",
									op_hbm_mode);
	return ret;
}

static ssize_t op_friginer_print_hbm_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int op_hbm_mode = 0;

	ret = kstrtoint(buf, 10, &op_hbm_mode);
	if (ret) {
		pr_err("kstrtoint failed. ret=%d\n", ret);
		return ret;
	}

	ret = dsi_display_set_fp_hbm_mode(connector, op_hbm_mode);
	if (ret)
		pr_err("set hbm mode(%d) fail\n", op_hbm_mode);

	return count;
}

static ssize_t aod_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int aod_mode = 0;

	aod_mode = dsi_display_get_aod_mode(connector);

	ret = scnprintf(buf, PAGE_SIZE, "%d\n", aod_mode);
	return ret;
}

static ssize_t aod_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int aod_mode = 0;

	ret = kstrtoint(buf, 10, &aod_mode);
	if (ret) {
		pr_err("kstrtoint failed. ret=%d\n", ret);
		return ret;
	}
	pr_err("node aod_mode=%d\n", aod_mode);
	ret = dsi_display_set_aod_mode(connector, aod_mode);
	if (ret)
		pr_err("set AOD mode(%d) fail\n", aod_mode);
	return count;
}

static ssize_t aod_disable_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int aod_disable = 0;

	aod_disable = dsi_display_get_aod_disable(connector);

	ret = scnprintf(buf, PAGE_SIZE, "AOD disable = %d\n"
									"0--AOD enable\n"
									"1--AOD disable\n",
									aod_disable);
	return ret;
}

static ssize_t aod_disable_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int aod_disable = 0;

	ret = kstrtoint(buf, 10, &aod_disable);
	if (ret) {
		pr_err("kstrtoint failed. ret=%d\n", ret);
		return ret;
	}

	ret = dsi_display_set_aod_disable(connector, aod_disable);
	if (ret)
		pr_err("set AOD disable(%d) fail\n", aod_disable);

	return count;
}

static ssize_t DCI_P3_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int dci_p3_mode = 0;

	dci_p3_mode = dsi_display_get_dci_p3_mode(connector);

	ret = scnprintf(buf, PAGE_SIZE, "dci-p3 mode = %d\n"
									"0--dci-p3 mode Off\n"
									"1--dci-p3 mode On\n",
									dci_p3_mode);
	return ret;
}

static ssize_t DCI_P3_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int dci_p3_mode = 0;

	ret = kstrtoint(buf, 10, &dci_p3_mode);
	if (ret) {
		pr_err("kstrtoint failed. ret=%d\n", ret);
		return ret;
	}

	ret = dsi_display_set_dci_p3_mode(connector, dci_p3_mode);
	if (ret)
		pr_err("set dci-p3 mode(%d) fail\n", dci_p3_mode);

	return count;
}

static ssize_t night_mode_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int night_mode = 0;

	night_mode = dsi_display_get_night_mode(connector);

	ret = scnprintf(buf, PAGE_SIZE, "night mode = %d\n"
									"0--night mode Off\n"
									"1--night mode On\n",
									night_mode);
	return ret;
}

static ssize_t night_mode_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int night_mode = 0;

	ret = kstrtoint(buf, 10, &night_mode);
	if (ret) {
		pr_err("kstrtoint failed. ret=%d\n", ret);
		return ret;
	}

	ret = dsi_display_set_night_mode(connector, night_mode);
	if (ret)
		pr_err("set night mode(%d) fail\n", night_mode);

	return count;
}

static ssize_t native_display_p3_mode_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int native_display_p3_mode = 0;

	native_display_p3_mode = dsi_display_get_native_display_p3_mode(connector);

	ret = scnprintf(buf, PAGE_SIZE, "native display p3 mode = %d\n"
									"0--native display p3 mode Off\n"
									"1--native display p3 mode On\n",
									native_display_p3_mode);
	return ret;
}

static ssize_t native_display_p3_mode_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int native_display_p3_mode = 0;

	ret = kstrtoint(buf, 10, &native_display_p3_mode);
	if (ret) {
		pr_err("kstrtoint failed. ret=%d\n", ret);
		return ret;
	}

	ret = dsi_display_set_native_display_p3_mode(connector, native_display_p3_mode);
	if (ret)
		pr_err("set native_display_p3  mode(%d) fail\n", native_display_p3_mode);

	return count;
}
static ssize_t native_display_wide_color_mode_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int native_display_wide_color_mode = 0;

	native_display_wide_color_mode = dsi_display_get_native_display_wide_color_mode(connector);

	ret = scnprintf(buf, PAGE_SIZE, "native display wide color mode = %d\n"
									"0--native display wide color mode Off\n"
									"1--native display wide color mode On\n",
									native_display_wide_color_mode);
	return ret;
}

static ssize_t native_display_loading_effect_mode_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int native_display_loading_effect_mode = 0;

	ret = kstrtoint(buf, 10, &native_display_loading_effect_mode);
	if (ret) {
		pr_err("kstrtoint failed. ret=%d\n", ret);
		return ret;
	}

	ret = dsi_display_set_native_loading_effect_mode(connector, native_display_loading_effect_mode);
	if (ret)
		pr_err("set loading effect  mode(%d) fail\n", native_display_loading_effect_mode);

	return count;
}

static ssize_t native_display_loading_effect_mode_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int native_display_loading_effect_mode = 0;

	native_display_loading_effect_mode = dsi_display_get_native_display_loading_effect_mode(connector);

	ret = scnprintf(buf, PAGE_SIZE, "native display loading effect mode = %d\n"
									"0--native display loading effect mode Off\n"
									"1--native display loading effect mode On\n",
									native_display_loading_effect_mode);
	return ret;
}

static ssize_t native_display_customer_p3_mode_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int native_display_customer_p3_mode = 0;

	ret = kstrtoint(buf, 10, &native_display_customer_p3_mode);
	if (ret) {
		pr_err("kstrtoint failed. ret=%d\n", ret);
		return ret;
	}

	ret = dsi_display_set_customer_p3_mode(connector, native_display_customer_p3_mode);
	if (ret)
		pr_err("set customer p3  mode(%d) fail\n", native_display_customer_p3_mode);

	return count;
}

static ssize_t native_display_customer_p3_mode_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int native_display_customer_p3_mode = 0;

	native_display_customer_p3_mode = dsi_display_get_customer_p3_mode(connector);

	ret = scnprintf(buf, PAGE_SIZE, "native display customer p3 mode = %d\n"
									"0--native display customer p3 mode Off\n"
									"1--native display customer p3 mode On\n",
									native_display_customer_p3_mode);
	return ret;
}
static ssize_t native_display_customer_srgb_mode_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int native_display_customer_srgb_mode = 0;

	ret = kstrtoint(buf, 10, &native_display_customer_srgb_mode);
	if (ret) {
		pr_err("kstrtoint failed. ret=%d\n", ret);
		return ret;
	}

	ret = dsi_display_set_customer_srgb_mode(connector, native_display_customer_srgb_mode);
	if (ret)
		pr_err("set customer srgb  mode(%d) fail\n", native_display_customer_srgb_mode);

	return count;
}

static ssize_t native_display_customer_srgb_mode_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int native_display_customer_srgb_mode = 0;

	native_display_customer_srgb_mode = dsi_display_get_customer_srgb_mode(connector);

	ret = scnprintf(buf, PAGE_SIZE, "native display customer srgb mode = %d\n"
									"0--native display customer srgb mode Off\n"
									"1--native display customer srgb mode On\n",
									native_display_customer_srgb_mode);
	return ret;
}


static ssize_t native_display_wide_color_mode_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int native_display_wide_color_mode = 0;

	ret = kstrtoint(buf, 10, &native_display_wide_color_mode);
	if (ret) {
		pr_err("kstrtoint failed. ret=%d\n", ret);
		return ret;
	}

	ret = dsi_display_set_native_display_wide_color_mode(connector, native_display_wide_color_mode);
	if (ret)
		pr_err("set native_display_p3  mode(%d) fail\n", native_display_wide_color_mode);

	return count;
}

static ssize_t native_display_srgb_color_mode_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int native_display_srgb_color_mode = 0;

	native_display_srgb_color_mode = dsi_display_get_native_display_srgb_color_mode(connector);

	ret = scnprintf(buf, PAGE_SIZE, "native display srgb color mode = %d\n"
									"0--native display srgb color mode Off\n"
									"1--native display srgb color mode On\n",
									native_display_srgb_color_mode);
	return ret;
}

static ssize_t native_display_srgb_color_mode_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int native_display_srgb_color_mode = 0;

	ret = kstrtoint(buf, 10, &native_display_srgb_color_mode);
	if (ret) {
		pr_err("kstrtoint failed. ret=%d\n", ret);
		return ret;
	}

	ret = dsi_display_set_native_display_srgb_color_mode(connector, native_display_srgb_color_mode);
	if (ret)
		pr_err("set native_display_srgb  mode(%d) fail\n", native_display_srgb_color_mode);

	return count;
}

static ssize_t mca_setting_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int mca_setting_mode = 0;

	mca_setting_mode = dsi_display_get_mca_setting_mode(connector);

	ret = scnprintf(buf, PAGE_SIZE, "%d\n", mca_setting_mode);
	return ret;
}

static ssize_t mca_setting_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int mca_setting_mode = 0;

	ret = kstrtoint(buf, 10, &mca_setting_mode);
	if (ret) {
		pr_err("Kstrtoint failed, ret=%d\n", ret);
		return ret;
	}

	ret = dsi_display_set_mca_setting_mode(connector, mca_setting_mode);
	if (ret)
		pr_err("Set mca setting mode %d failed\n", mca_setting_mode);

	return count;
}

static ssize_t gamma_test_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int gamma_test_flag = 0;
	int panel_stage_info = 0;
	int pvt_mp_panel_flag = 0;

	if (dsi_panel_name == DSI_PANEL_SAMSUNG_S6E3HC2) {
		ret = dsi_display_update_gamma_para(connector);
		if (ret)
			pr_err("Failed to update gamma para!\n");

		if ((gamma_para[0][18] == 0xFF) && (gamma_para[0][19] == 0xFF) && (gamma_para[0][20] == 0xFF))
			gamma_test_flag = 0;
		else
			gamma_test_flag = 1;

		dsi_display_get_serial_number(connector);
		panel_stage_info = dsi_display_get_stage_info(connector);

		if ((panel_stage_info == 0x07) || (panel_stage_info == 0x10) ||
			(panel_stage_info == 0x11) || (panel_stage_info == 0x16))
			pvt_mp_panel_flag = 1;
		else
			pvt_mp_panel_flag = 0;

		ret = scnprintf(buf, PAGE_SIZE, "%d\n", (gamma_test_flag << 1) + pvt_mp_panel_flag);
	} else if (dsi_panel_name == DSI_PANEL_SAMSUNG_SOFEF03F_M) {
		dsi_display_get_serial_number(connector);
		panel_stage_info = dsi_display_get_stage_info(connector);

		if (panel_stage_info == 0x27)
			pvt_mp_panel_flag = 1;
		else
			pvt_mp_panel_flag = 0;

		ret = scnprintf(buf, PAGE_SIZE, "%d\n", pvt_mp_panel_flag);
	} else {
		ret = scnprintf(buf, PAGE_SIZE, "%d\n", 3);
		pr_err("Gamma test is not supported!\n");
	}

	return ret;
}

extern char buf_Lotid[6];
static ssize_t panel_serial_number_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;

	int panel_year = 0;
	int panel_mon = 0;
	int panel_day = 0;
	int panel_hour = 0;
	int panel_min = 0;
	int panel_sec = 0;
	int panel_msec_int = 0;
	int panel_msec_rem = 0;
	int panel_code_info = 0;
	int panel_stage_info = 0;
	int panel_production_info = 0;

	int panel_ic_v_info = 0;
	int ddic_check_info = 0;
	int panel_tool = 0;
	int ddic_y = 0;
	int ddic_x = 0;

	char *stage_string_info = NULL;
	char *production_string_info = NULL;
	char *ddic_check_result = NULL;
	char *panel_tool_result = NULL;
	char *buf_select = NULL;

	dsi_display_get_serial_number(connector);

	panel_year = dsi_display_get_serial_number_year(connector);
	panel_mon = dsi_display_get_serial_number_mon(connector);
	panel_day = dsi_display_get_serial_number_day(connector);
	panel_hour = dsi_display_get_serial_number_hour(connector);
	panel_min = dsi_display_get_serial_number_min(connector);
	panel_sec = dsi_display_get_serial_number_sec(connector);
	panel_msec_int = dsi_display_get_serial_number_msec_int(connector);
	panel_msec_rem = dsi_display_get_serial_number_msec_rem(connector);
	panel_code_info = dsi_display_get_code_info(connector);
	panel_stage_info = dsi_display_get_stage_info(connector);
	panel_production_info = dsi_display_get_production_info(connector);

	if (dsi_panel_name == DSI_PANEL_SAMSUNG_S6E3HC2) {
		if (panel_code_info == 0xED) {
			if (panel_stage_info == 0x02)
				stage_string_info = "STAGE: EVT2";
			else if (panel_stage_info == 0x03)
				stage_string_info = "STAGE: EVT2(NEW_DIMMING_SET)";
			else if (panel_stage_info == 0x99)
				stage_string_info = "STAGE: EVT2(113MHZ_OSC)";
			else if (panel_stage_info == 0x04)
				stage_string_info = "STAGE: DVT1";
			else if (panel_stage_info == 0x05)
				stage_string_info = "STAGE: DVT2";
			else if (panel_stage_info == 0x06)
				stage_string_info = "STAGE: DVT3";
			else if (panel_stage_info == 0x07)
				stage_string_info = "STAGE: PVT/MP(112MHZ_OSC)";
			else if (panel_stage_info == 0x10)
				stage_string_info = "STAGE: PVT/MP(113MHZ_OSC)";
			else if (panel_stage_info == 0x11)
				stage_string_info = "STAGE: PVT(113MHZ_OSC+X_TALK_IMPROVEMENT)";
			else
				stage_string_info = "STAGE: UNKNOWN";

			if (panel_production_info == 0x0C)
				production_string_info = "TPIC: LSI\nCOVER: JNTC\nOTP_GAMMA: 90HZ";
			else if (panel_production_info == 0x0E)
				production_string_info = "TPIC: LSI\nCOVER: LENS\nOTP_GAMMA: 90HZ";
			else if (panel_production_info == 0x1C)
				production_string_info = "TPIC: STM\nCOVER: JNTC\nOTP_GAMMA: 90HZ";
			else if (panel_production_info == 0x6C)
				production_string_info = "TPIC: LSI\nCOVER: JNTC\nOTP_GAMMA: 60HZ";
			else if (panel_production_info == 0x6E)
				production_string_info = "TPIC: LSI\nCOVER: LENS\nOTP_GAMMA: 60HZ";
			else if (panel_production_info == 0x1E)
				production_string_info = "TPIC: STM\nCOVER: LENS\nOTP_GAMMA: 90HZ";
			else if (panel_production_info == 0x0D)
				production_string_info = "TPIC: LSI\nID3: 0x0D\nOTP_GAMMA: 90HZ";
			else
				production_string_info = "TPIC: UNKNOWN\nCOVER: UNKNOWN\nOTP_GAMMA: UNKNOWN";

			ret = scnprintf(buf, PAGE_SIZE, "%04d/%02d/%02d %02d:%02d:%02d\n%s\n%s\nID: %02X %02X %02X\n",
					panel_year, panel_mon, panel_day, panel_hour, panel_min, panel_sec,
						stage_string_info, production_string_info, panel_code_info,
							panel_stage_info, panel_production_info);
		}

		if (panel_code_info == 0xEE) {
			if (panel_stage_info == 0x12)
				stage_string_info = "STAGE: T0/EVT1";
			else if (panel_stage_info == 0x13)
				stage_string_info = "STAGE: EVT2";
			else if (panel_stage_info == 0x14)
				stage_string_info = "STAGE: EVT2";
			else if (panel_stage_info == 0x15)
				stage_string_info = "STAGE: EVT3";
			else if (panel_stage_info == 0x16)
				stage_string_info = "STAGE: DVT";
			else if (panel_stage_info == 0x17)
				stage_string_info = "STAGE: DVT";
			else if (panel_stage_info == 0x19)
				stage_string_info = "STAGE: PVT/MP";
			else
				stage_string_info = "STAGE: UNKNOWN";

			ret = scnprintf(buf, PAGE_SIZE, "%04d/%02d/%02d %02d:%02d:%02d\n%s\nID: %02X %02X %02X\n",
					panel_year, panel_mon, panel_day, panel_hour, panel_min, panel_sec,
						stage_string_info, panel_code_info, panel_stage_info,
							panel_production_info);
		}

	} else if (dsi_panel_name == DSI_PANEL_SAMSUNG_SOFEF03F_M) {
		if (panel_stage_info == 0x01)
			stage_string_info = "STAGE: T0";
		else if (panel_stage_info == 0x21)
			stage_string_info = "STAGE: EVT1";
		else if (panel_stage_info == 0x22)
			stage_string_info = "STAGE: EVT2";
		else if (panel_stage_info == 0x24)
			stage_string_info = "STAGE: DVT1-1";
		else if (panel_stage_info == 0x26)
			stage_string_info = "STAGE: DVT1-2";
		else if (panel_stage_info == 0x25)
			stage_string_info = "STAGE: DVT2";
		else if (panel_stage_info == 0x28)
			stage_string_info = "STAGE: DVT3";
		else if (panel_stage_info == 0x27)
			stage_string_info = "STAGE: PVT/MP";

		ret = scnprintf(buf, PAGE_SIZE, "%04d/%02d/%02d %02d:%02d:%02d\n%s\nID: %02X %02X %02X\n",
				panel_year, panel_mon, panel_day, panel_hour, panel_min, panel_sec, stage_string_info,
					panel_code_info, panel_stage_info, panel_production_info);
	} else if (dsi_panel_name == DSI_PANEL_SAMSUNG_ANA6705) {
		if (panel_stage_info == 0x01)
			stage_string_info = "STAGE: T0";
		else if (panel_stage_info == 0x02)
			stage_string_info = "STAGE: EVT1";
		else if (panel_stage_info == 0x03)
			stage_string_info = "STAGE: EVT2";
		else if (panel_stage_info == 0x04)
			stage_string_info = "STAGE: DVT1";
		else if (panel_stage_info == 0x05)
			stage_string_info = "STAGE: DVT2";
		else if (panel_stage_info == 0x06)
			stage_string_info = "STAGE: PVT/MP";
		else
			stage_string_info = "STAGE: UNKNOWN";

	ddic_check_info = dsi_display_get_ddic_check_info(connector);
	if (ddic_check_info == 1)
		ddic_check_result = "OK";
	else if (ddic_check_info == 0)
		ddic_check_result = "NG";

		ret = scnprintf(buf, PAGE_SIZE,
		"%04d/%02d/%02d\n%02d:%02d:%02d:%03d.%01d\n%s\nID: %02X %02X %02X\nDDIC_Check_Result: %s\n",
				panel_year, panel_mon, panel_day, panel_hour, panel_min,
					panel_sec, panel_msec_int, panel_msec_rem, stage_string_info, panel_code_info,
							panel_stage_info, panel_production_info, ddic_check_result);
	} else if (dsi_panel_name == DSI_PANEL_SAMSUNG_ANA6706) {
		panel_ic_v_info = dsi_display_get_panel_ic_v_info(connector);

		if (panel_stage_info == 0x01)
			stage_string_info = "STAGE: T0";
		else if (panel_stage_info == 0x02)
			stage_string_info = "STAGE: EVT1-1";
		else if ((panel_stage_info == 0xA2) && (panel_ic_v_info == 1))
			stage_string_info = "STAGE: EVT2";
		else if ((panel_stage_info == 0xA3) && (panel_ic_v_info == 1))
			stage_string_info = "STAGE: EVT2-1";
		else if ((panel_stage_info == 0xA3) && (panel_ic_v_info == 0))
			stage_string_info = "STAGE: EVT2-2";
		else if (panel_stage_info == 0xA4)
			stage_string_info = "STAGE: DVT1";
		else if (panel_stage_info == 0xA5)
			stage_string_info = "STAGE: DVT2";
		else if (panel_stage_info == 0xA6)
			stage_string_info = "STAGE: PVT/MP";
		else
			stage_string_info = "STAGE: UNKNOWN";

		ddic_check_info = dsi_display_get_ddic_check_info(connector);
		if (ddic_check_info == 1)
			ddic_check_result = "OK";
		else if (ddic_check_info == 0)
			ddic_check_result = "NG";

		panel_tool = dsi_display_get_ToolsType_ANA6706(connector);
		if (panel_tool == 0)
			panel_tool_result = "ToolB";
		else if (panel_tool == 1)
			panel_tool_result = "ToolA";
		else if (panel_tool == 2)
			panel_tool_result = "ToolA_HVS30";
		else
			panel_tool_result = "Indistinguishable";

		ddic_y = dsi_display_get_ddic_coords_Y(connector);
		ddic_x = dsi_display_get_ddic_coords_X(connector);

		buf_select = dsi_display_get_ic_reg_buf(connector);
		if (buf_select == NULL)
			return ret;

		ret = scnprintf(buf, PAGE_SIZE,
		"%04d/%02d/%02d\n%02d:%02d:%02d:%03d.%01d\n%s\nID: %02X %02X %02X\n"
		"IC_V: %02d\nDDIC_Check_Result: %s\nTool: %s\nddic_y: %02d ddic_x: %02d\nLotid: %s\n"
		"reg: %02x %02x %02x %02x %02x %02x %02x\n",
			panel_year, panel_mon, panel_day, panel_hour, panel_min, panel_sec, panel_msec_int,
				panel_msec_rem, stage_string_info, panel_code_info, panel_stage_info,
					panel_production_info, panel_ic_v_info, ddic_check_result, panel_tool_result,
						ddic_y, ddic_x, buf_Lotid, buf_select[0], buf_select[1], buf_select[2],
							buf_select[3], buf_select[4], buf_select[5], buf_select[6]);
	} else if (dsi_panel_name == DSI_PANEL_SAMSUNG_AMB655XL) {
		if (panel_stage_info == 0x01)
			stage_string_info = "STAGE: T0";
		else if (panel_stage_info == 0x02)
			stage_string_info = "STAGE: EVT1";
		else if (panel_stage_info == 0x03)
			stage_string_info = "STAGE: DVT1";
		else if (panel_stage_info == 0x04)
			stage_string_info = "STAGE: DVT2";
		else if (panel_stage_info == 0x05)
			stage_string_info = "STAGE: PVT/MP";
		else
			stage_string_info = "STAGE: UNKNOWN";

		ret = scnprintf(buf, PAGE_SIZE, "%04d/%02d/%02d\n%02d:%02d:%02d:%03d.%01d\n%s\nID: %02X %02X %02X\n",
				panel_year, panel_mon, panel_day, panel_hour, panel_min, panel_sec, panel_msec_int,
					panel_msec_rem, stage_string_info, panel_code_info, panel_stage_info,
						panel_production_info);
	} else {
		ret = scnprintf(buf, PAGE_SIZE, "%04d/%02d/%02d %02d:%02d:%02d\nID: %02X %02X %02X\n",
				panel_year, panel_mon, panel_day, panel_hour, panel_min, panel_sec,
					panel_code_info, panel_stage_info, panel_production_info);
	}

	return ret;
}

static ssize_t panel_serial_number_AT_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;

	ret = scnprintf(buf, PAGE_SIZE, "%llu\n", dsi_display_get_serial_number_at(connector));

	return ret;
}

static ssize_t iris_recovery_mode_check_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int ret = 0;
	int result = 0;
	struct drm_connector *connector = to_drm_connector(dev);

	result = iris_loop_back_test(connector);
	pr_err("iris_loop_back_test result = %d", result);
	ret = scnprintf(buf, PAGE_SIZE, "%d\n", (result == 0) ? 1 : 0);

	return ret;
}

static ssize_t dsi_on_command_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;

	ret = dsi_display_get_dsi_on_command(connector, buf);

	return ret;
}

static ssize_t dsi_on_command_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;

	ret = dsi_display_update_dsi_on_command(connector, buf, count);
	if (ret)
		pr_err("Failed to update dsi on command, ret=%d\n", ret);

	return count;
}

static ssize_t dsi_panel_command_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;

	ret = dsi_display_get_dsi_panel_command(connector, buf);

	return ret;
}

static ssize_t dsi_panel_command_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;

	ret = dsi_display_update_dsi_panel_command(connector, buf, count);
	if (ret)
		pr_err("Failed to update dsi panel command, ret=%d\n", ret);

	return count;
}

static ssize_t dsi_seed_command_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;

	ret = dsi_display_get_dsi_seed_command(connector, buf);

	return ret;
}

static ssize_t dsi_seed_command_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;

	ret = dsi_display_update_dsi_seed_command(connector, buf, count);
	if (ret)
		pr_err("Failed to update dsi seed command, ret=%d\n", ret);

	return count;
}

static ssize_t dsi_panel_reg_len_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int ret = 0;

	ret = scnprintf(buf, PAGE_SIZE, "%d\n", reg_read_len);

	return ret;
}

static ssize_t dsi_panel_reg_len_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int ret = 0;
	int num = 0;

	ret = kstrtoint(buf, 10, &num);
	if (ret) {
		pr_err("kstrtoint failed. ret=%d\n", ret);
		return ret;
	}

	if (num <= 0)
		pr_err("Invalid length!\n");
	else
		reg_read_len = num;

	return count;
}

static ssize_t dsi_panel_reg_read_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;

	ret = dsi_display_get_reg_read_command_and_value(connector, buf);

	return ret;
}

static ssize_t dsi_panel_reg_read_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;

	ret = dsi_display_reg_read(connector, buf, count);
	if (ret)
		pr_err("Failed to update reg read command, ret=%d\n", ret);

	return count;
}

static ssize_t dsi_cmd_log_switch_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int ret = 0;

	ret = scnprintf(buf, PAGE_SIZE, "dsi cmd log switch = %d\n"
									"0     -- dsi cmd log switch off\n"
									"other -- dsi cmd log switch on\n",
										dsi_cmd_log_enable);

	return ret;
}

static ssize_t dsi_cmd_log_switch_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int ret = 0;

	ret = kstrtoint(buf, 10, &dsi_cmd_log_enable);
	if (ret) {
		pr_err("kstrtoint failed. ret=%d\n", ret);
		return ret;
	}

	return count;
}

int current_freq;
static ssize_t dynamic_dsitiming_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int ret = 0;

	ret = scnprintf(buf, PAGE_SIZE, "current_freq = %d\n",
											current_freq);
	return ret;
}

static ssize_t dynamic_dsitiming_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int ret = 0;
	int freq_value = 0;

	ret = kstrtoint(buf, 10, &freq_value);
	if (ret) {
		pr_err("kstrtoint failed. ret=%d\n", ret);
		return ret;
	}

	current_freq = freq_value;

	pr_err("freq setting=%d\n", current_freq);

	if (ret)
		pr_err("set dsi freq (%d) fail\n", current_freq);

	return count;
}

extern u32 mode_fps;
static ssize_t dynamic_fps_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int ret = 0;

	ret = scnprintf(buf, PAGE_SIZE, "%d\n", mode_fps);

	return ret;
}

static ssize_t panel_mismatch_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;
	int wrong_panel = 0;

	dsi_display_panel_mismatch_check(connector);

	wrong_panel = dsi_display_panel_mismatch(connector);
	ret = scnprintf(buf, PAGE_SIZE, "panel mismatch = %d\n"
									"0--(panel match)\n"
									"1--(panel mismatch)\n",
									wrong_panel);
	return ret;
}

int oneplus_panel_alpha;
int oneplus_force_screenfp;
int op_dimlayer_bl_enable;
int op_dp_enable;
int op_dither_enable;

static ssize_t dim_alpha_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", oneplus_get_panel_brightness_to_alpha());
}

static ssize_t dim_alpha_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int ret = 0;

	ret = kstrtoint(buf, 10, &oneplus_panel_alpha);
	if (ret)
		pr_err("kstrtoint failed. ret=%d\n", ret);

	return count;
}

static ssize_t force_screenfp_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{

	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;

	oneplus_force_screenfp = dsi_display_get_fp_hbm_mode(connector);

	ret = scnprintf(buf, PAGE_SIZE, "OP_FP mode = %d\n"
									"0--finger-hbm mode(off)\n"
									"1--finger-hbm mode(600)\n",
									oneplus_force_screenfp);
	return snprintf(buf, PAGE_SIZE, "%d\n", oneplus_force_screenfp);
}

static ssize_t force_screenfp_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	//sscanf(buf, "%x", &oneplus_force_screenfp);
	struct drm_connector *connector = to_drm_connector(dev);
	int ret = 0;

	ret = kstrtoint(buf, 10, &oneplus_force_screenfp);
	if (ret) {
		pr_err("kstrtoint failed. ret=%d\n", ret);
		return ret;
	}

	ret = dsi_display_set_fp_hbm_mode(connector, oneplus_force_screenfp);
	if (ret)
		pr_err("set hbm mode(%d) fail\n", oneplus_force_screenfp);
	return count;
}

static ssize_t dimlayer_bl_en_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", op_dimlayer_bl_enable);
}

static ssize_t dimlayer_bl_en_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int ret = 0;

	ret = kstrtoint(buf, 10, &op_dimlayer_bl_enable);
	if (ret)
		pr_err("kstrtoint failed. ret=%d\n", ret);

	pr_err("op_dimlayer_bl_enable : %d\n", op_dimlayer_bl_enable);

	return count;
}

static ssize_t dither_en_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int ret = 0;

	ret = scnprintf(buf, PAGE_SIZE, "dither switch = %d\n"
					"0     -- dither switch off\n"
					"other -- dither switch on\n",
					op_dither_enable);

	return ret;
}

static ssize_t dither_en_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{

	int ret = 0;

	ret = kstrtoint(buf, 10, &op_dither_enable);
	if (ret) {
		pr_err("kstrtoint failed. ret=%d\n", ret);
		return ret;
	}

	return count;
}

static ssize_t dp_en_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", op_dp_enable);
}

static ssize_t dp_en_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int ret = 0;

	ret = kstrtoint(buf, 10, &op_dp_enable);
	if (ret)
		pr_err("kstrtoint failed. ret=%d\n", ret);

	return count;
}

static DEVICE_ATTR_RW(status);
static DEVICE_ATTR_RO(enabled);
static DEVICE_ATTR_RO(dpms);
static DEVICE_ATTR_RO(modes);
static DEVICE_ATTR_RW(acl);
static DEVICE_ATTR_RW(hbm);
static DEVICE_ATTR_RW(hbm_brightness);
static DEVICE_ATTR_RW(op_friginer_print_hbm);
static DEVICE_ATTR_RW(aod);
static DEVICE_ATTR_RW(aod_disable);
static DEVICE_ATTR_RW(DCI_P3);
static DEVICE_ATTR_RW(night_mode);
static DEVICE_ATTR_RW(native_display_p3_mode);
static DEVICE_ATTR_RW(native_display_wide_color_mode);
static DEVICE_ATTR_RW(native_display_loading_effect_mode);
static DEVICE_ATTR_RW(native_display_srgb_color_mode);
static DEVICE_ATTR_RW(native_display_customer_p3_mode);
static DEVICE_ATTR_RW(native_display_customer_srgb_mode);
static DEVICE_ATTR_RW(mca_setting);
static DEVICE_ATTR_RO(gamma_test);
static DEVICE_ATTR_RO(panel_serial_number);
static DEVICE_ATTR_RO(panel_serial_number_AT);
static DEVICE_ATTR_RO(iris_recovery_mode_check);
static DEVICE_ATTR_RW(dsi_on_command);
static DEVICE_ATTR_RW(dsi_panel_command);
static DEVICE_ATTR_RW(dsi_seed_command);
static DEVICE_ATTR_RW(dsi_panel_reg_len);
static DEVICE_ATTR_RW(dsi_panel_reg_read);
static DEVICE_ATTR_RW(dsi_cmd_log_switch);
static DEVICE_ATTR_RW(dynamic_dsitiming);
static DEVICE_ATTR_RO(panel_mismatch);
static DEVICE_ATTR_RO(dynamic_fps);
static DEVICE_ATTR_RW(dim_alpha);
static DEVICE_ATTR_RW(force_screenfp);
static DEVICE_ATTR_WO(notify_fppress);
static DEVICE_ATTR_WO(notify_dim);
static DEVICE_ATTR_WO(notify_aod);
static DEVICE_ATTR_RW(dimlayer_bl_en);
static DEVICE_ATTR_RW(dp_en);
static DEVICE_ATTR_RW(dither_en);
static DEVICE_ATTR_RW(seed_lp);
static struct attribute *connector_dev_attrs[] = {
	&dev_attr_status.attr,
	&dev_attr_enabled.attr,
	&dev_attr_dpms.attr,
	&dev_attr_modes.attr,
	&dev_attr_acl.attr,
	&dev_attr_hbm.attr,
	&dev_attr_hbm_brightness.attr,
	&dev_attr_op_friginer_print_hbm.attr,
	&dev_attr_aod.attr,
	&dev_attr_aod_disable.attr,
	&dev_attr_DCI_P3.attr,
	&dev_attr_night_mode.attr,
	&dev_attr_native_display_p3_mode.attr,
	&dev_attr_native_display_wide_color_mode.attr,
	&dev_attr_native_display_loading_effect_mode.attr,
	&dev_attr_native_display_srgb_color_mode.attr,
	&dev_attr_native_display_customer_p3_mode.attr,
	&dev_attr_native_display_customer_srgb_mode.attr,
	&dev_attr_mca_setting.attr,
	&dev_attr_gamma_test.attr,
	&dev_attr_panel_serial_number.attr,
	&dev_attr_panel_serial_number_AT.attr,
	&dev_attr_iris_recovery_mode_check.attr,
	&dev_attr_dsi_on_command.attr,
	&dev_attr_dsi_panel_command.attr,
	&dev_attr_dsi_seed_command.attr,
	&dev_attr_dsi_panel_reg_len.attr,
	&dev_attr_dsi_panel_reg_read.attr,
	&dev_attr_dsi_cmd_log_switch.attr,
	&dev_attr_dynamic_dsitiming.attr,
	&dev_attr_panel_mismatch.attr,
	&dev_attr_force_screenfp.attr,
	&dev_attr_dim_alpha.attr,
	&dev_attr_dynamic_fps.attr,
	&dev_attr_notify_fppress.attr,
	&dev_attr_notify_dim.attr,
	&dev_attr_notify_aod.attr,
	&dev_attr_dimlayer_bl_en.attr,
	&dev_attr_dp_en.attr,
	&dev_attr_dither_en.attr,
	&dev_attr_seed_lp.attr,
	NULL
};

static struct bin_attribute edid_attr = {
	.attr.name = "edid",
	.attr.mode = 0444,
	.size = 0,
	.read = edid_show,
};

static struct bin_attribute *connector_bin_attrs[] = {
	&edid_attr,
	NULL
};

static const struct attribute_group connector_dev_group = {
	.attrs = connector_dev_attrs,
	.bin_attrs = connector_bin_attrs,
};

static const struct attribute_group *connector_dev_groups[] = {
	&connector_dev_group,
	NULL
};

int drm_sysfs_connector_add(struct drm_connector *connector)
{
	struct drm_device *dev = connector->dev;

	if (connector->kdev)
		return 0;

	connector->kdev =
		device_create_with_groups(drm_class, dev->primary->kdev, 0,
					  connector, connector_dev_groups,
					  "card%d-%s", dev->primary->index,
					  connector->name);
	DRM_DEBUG("adding \"%s\" to sysfs\n",
		  connector->name);

	if (IS_ERR(connector->kdev)) {
		DRM_ERROR("failed to register connector device: %ld\n", PTR_ERR(connector->kdev));
		return PTR_ERR(connector->kdev);
	}

	/* Let userspace know we have a new connector */
	drm_sysfs_hotplug_event(dev);

	return 0;
}

void drm_sysfs_connector_remove(struct drm_connector *connector)
{
	if (!connector->kdev)
		return;
	DRM_DEBUG("removing \"%s\" from sysfs\n",
		  connector->name);

	device_unregister(connector->kdev);
	connector->kdev = NULL;
}

void drm_sysfs_lease_event(struct drm_device *dev)
{
	char *event_string = "LEASE=1";
	char *envp[] = { event_string, NULL };

	DRM_DEBUG("generating lease event\n");

	kobject_uevent_env(&dev->primary->kdev->kobj, KOBJ_CHANGE, envp);
}

/**
 * drm_sysfs_hotplug_event - generate a DRM uevent
 * @dev: DRM device
 *
 * Send a uevent for the DRM device specified by @dev.  Currently we only
 * set HOTPLUG=1 in the uevent environment, but this could be expanded to
 * deal with other types of events.
 */
void drm_sysfs_hotplug_event(struct drm_device *dev)
{
	char *event_string = "HOTPLUG=1";
	char *envp[] = { event_string, NULL };

	DRM_DEBUG("generating hotplug event\n");

	kobject_uevent_env(&dev->primary->kdev->kobj, KOBJ_CHANGE, envp);
}
EXPORT_SYMBOL(drm_sysfs_hotplug_event);

static void drm_sysfs_release(struct device *dev)
{
	kfree(dev);
}

struct device *drm_sysfs_minor_alloc(struct drm_minor *minor)
{
	const char *minor_str;
	struct device *kdev;
	int r;

	if (minor->type == DRM_MINOR_RENDER)
		minor_str = "renderD%d";
	else
		minor_str = "card%d";

	kdev = kzalloc(sizeof(*kdev), GFP_KERNEL);
	if (!kdev)
		return ERR_PTR(-ENOMEM);

	device_initialize(kdev);
	kdev->devt = MKDEV(DRM_MAJOR, minor->index);
	kdev->class = drm_class;
	kdev->type = &drm_sysfs_device_minor;
	kdev->parent = minor->dev->dev;
	kdev->release = drm_sysfs_release;
	dev_set_drvdata(kdev, minor);

	r = dev_set_name(kdev, minor_str, minor->index);
	if (r < 0)
		goto err_free;

	return kdev;

err_free:
	put_device(kdev);
	return ERR_PTR(r);
}

/**
 * drm_class_device_register - register new device with the DRM sysfs class
 * @dev: device to register
 *
 * Registers a new &struct device within the DRM sysfs class. Essentially only
 * used by ttm to have a place for its global settings. Drivers should never use
 * this.
 */
int drm_class_device_register(struct device *dev)
{
	if (!drm_class || IS_ERR(drm_class))
		return -ENOENT;

	dev->class = drm_class;
	return device_register(dev);
}
EXPORT_SYMBOL_GPL(drm_class_device_register);

/**
 * drm_class_device_unregister - unregister device with the DRM sysfs class
 * @dev: device to unregister
 *
 * Unregisters a &struct device from the DRM sysfs class. Essentially only used
 * by ttm to have a place for its global settings. Drivers should never use
 * this.
 */
void drm_class_device_unregister(struct device *dev)
{
	return device_unregister(dev);
}
EXPORT_SYMBOL_GPL(drm_class_device_unregister);
