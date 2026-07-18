// SPDX-License-Identifier: GPL-2.0-only
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#include "internal.h"

static bool st_is_configuration_command(unsigned int command)
{
	switch (command) {
	case ST_IOC_ADD_PROGRAM:
	case ST_IOC_REMOVE_PROGRAM:
	case ST_IOC_ADD_UID:
	case ST_IOC_REMOVE_UID:
	case ST_IOC_ADD_SYSCALL:
	case ST_IOC_REMOVE_SYSCALL:
	case ST_IOC_SET_MAX:
	case ST_IOC_ENABLE:
	case ST_IOC_DISABLE:
	case ST_IOC_RESET_STATS:
		return true;
	default:
		return false;
	}
}

static long st_device_ioctl(struct file *file, unsigned int command,
			    unsigned long argument)
{
	struct st_config config;
	__u32 max_per_second;

	(void)file;

	if (_IOC_TYPE(command) != ST_IOC_MAGIC)
		return -ENOTTY;

	if (st_is_configuration_command(command) &&
	    !uid_eq(current_euid(), GLOBAL_ROOT_UID))
		return -EPERM;

	switch (command) {
	case ST_IOC_SET_MAX:
		if (copy_from_user(&max_per_second, (void __user *)argument,
				   sizeof(max_per_second)))
			return -EFAULT;
		return st_state_set_max(max_per_second);

	case ST_IOC_ENABLE:
		st_state_set_enabled(true);
		return 0;

	case ST_IOC_DISABLE:
		st_state_set_enabled(false);
		return 0;

	case ST_IOC_GET_CONFIG:
		st_state_get_config(&config);
		if (copy_to_user((void __user *)argument, &config, sizeof(config)))
			return -EFAULT;
		return 0;

	case ST_IOC_ADD_PROGRAM:
	case ST_IOC_REMOVE_PROGRAM:
	case ST_IOC_ADD_UID:
	case ST_IOC_REMOVE_UID:
	case ST_IOC_ADD_SYSCALL:
	case ST_IOC_REMOVE_SYSCALL:
	case ST_IOC_GET_PROGRAM:
	case ST_IOC_GET_UID:
	case ST_IOC_GET_SYSCALL:
	case ST_IOC_GET_STATS:
	case ST_IOC_RESET_STATS:
		return -EOPNOTSUPP;

	default:
		return -ENOTTY;
	}
}

static int st_device_open(struct inode *inode, struct file *file)
{
	(void)inode;
	(void)file;
	return 0;
}

static int st_device_release(struct inode *inode, struct file *file)
{
	(void)inode;
	(void)file;
	return 0;
}

static const struct file_operations st_device_operations = {
	.owner = THIS_MODULE,
	.open = st_device_open,
	.release = st_device_release,
	.unlocked_ioctl = st_device_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = st_device_ioctl,
#endif
};

static struct miscdevice st_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = ST_DEVICE_NAME,
	.fops = &st_device_operations,
	.mode = 0666,
};

int st_device_register(void)
{
	return misc_register(&st_device);
}

void st_device_unregister(void)
{
	misc_deregister(&st_device);
}
