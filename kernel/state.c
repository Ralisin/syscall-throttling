// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/string.h>

#include "internal.h"

static DEFINE_MUTEX(st_config_lock);
static struct st_config st_config;

void st_state_initialize(void)
{
	mutex_lock(&st_config_lock);
	memset(&st_config, 0, sizeof(st_config));
	st_config.max_per_second = 1;
	st_config.generation = 1;
	mutex_unlock(&st_config_lock);
}

void st_state_get_config(struct st_config *config)
{
	mutex_lock(&st_config_lock);
	*config = st_config;
	mutex_unlock(&st_config_lock);
}

int st_state_set_max(__u32 max_per_second)
{
	if (max_per_second == 0 || max_per_second > ST_MAX_LIMIT)
		return -ERANGE;

	mutex_lock(&st_config_lock);
	st_config.max_per_second = max_per_second;
	st_config.generation++;
	mutex_unlock(&st_config_lock);

	return 0;
}

void st_state_set_enabled(bool enabled)
{
	mutex_lock(&st_config_lock);
	st_config.enabled = enabled;
	st_config.generation++;
	mutex_unlock(&st_config_lock);
}
