// SPDX-License-Identifier: GPL-2.0-only
#include <linux/init.h>
#include <linux/module.h>

#include "internal.h"

static int __init st_module_init(void) {
	int result;

	st_state_initialize();
	result = st_monitor_initialize();
	if (result)
		return result;

	result = st_device_register();
	if (result) {
		st_monitor_destroy();
		return result;
	}

	result = st_hooks_register();
	if (result) {
		st_device_unregister();
		st_monitor_destroy();
		return result;
	}

	return 0;
}

static void __exit st_module_exit(void) {
	st_hooks_unregister();
	st_device_unregister();
	st_monitor_destroy();
}

module_init(st_module_init);
module_exit(st_module_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("System call throttling monitor");
