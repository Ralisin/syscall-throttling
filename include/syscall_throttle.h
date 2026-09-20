/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SYSCALL_THROTTLE_H
#define SYSCALL_THROTTLE_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define ST_DEVICE_NAME "syscall_throttle"
#define ST_DEVICE_PATH "/dev/" ST_DEVICE_NAME

#define ST_PROGRAM_PATH_LEN 256U
#define ST_MAX_LIMIT 1000000U
#define ST_MAX_PROGRAMS 64U
#define ST_MAX_UIDS 64U
#define ST_MAX_SYSCALLS 64U

struct st_program {
	char path[ST_PROGRAM_PATH_LEN];
};

struct st_uid {
	__u32 value;
};

struct st_syscall {
	__s32 number;
};

struct st_config {
	__u64 generation;
	__u32 max_per_second;
	__u32 program_count;
	__u32 uid_count;
	__u32 syscall_count;
	__u8 enabled;
	__u8 reserved[7];
};

struct st_program_entry {
	__u64 generation;
	__u32 index;
	__u32 reserved;
	char path[ST_PROGRAM_PATH_LEN];
};

struct st_uid_entry {
	__u64 generation;
	__u32 index;
	__u32 value;
};

struct st_syscall_entry {
	__u64 generation;
	__u32 index;
	__s32 number;
};

struct st_stats {
	__u64 elapsed_ns;
	__u64 blocked_thread_time_ns;
	__u64 peak_delay_ns;
	__u64 throttled_calls;
	__u32 current_blocked_threads;
	__u32 peak_blocked_threads;
	__u32 peak_uid;
	__u32 reserved;
	char peak_program_path[ST_PROGRAM_PATH_LEN];
};

#define ST_IOC_MAGIC 0xB7

#define ST_IOC_ADD_PROGRAM       _IOW(ST_IOC_MAGIC, 0x01, struct st_program)
#define ST_IOC_REMOVE_PROGRAM    _IOW(ST_IOC_MAGIC, 0x02, struct st_program)
#define ST_IOC_ADD_UID           _IOW(ST_IOC_MAGIC, 0x03, struct st_uid)
#define ST_IOC_REMOVE_UID        _IOW(ST_IOC_MAGIC, 0x04, struct st_uid)
#define ST_IOC_ADD_SYSCALL       _IOW(ST_IOC_MAGIC, 0x05, struct st_syscall)
#define ST_IOC_REMOVE_SYSCALL    _IOW(ST_IOC_MAGIC, 0x06, struct st_syscall)
#define ST_IOC_SET_MAX           _IOW(ST_IOC_MAGIC, 0x07, __u32)
#define ST_IOC_ENABLE            _IO(ST_IOC_MAGIC, 0x08)
#define ST_IOC_DISABLE           _IO(ST_IOC_MAGIC, 0x09)
#define ST_IOC_GET_CONFIG        _IOR(ST_IOC_MAGIC, 0x0a, struct st_config)
#define ST_IOC_GET_PROGRAM       _IOWR(ST_IOC_MAGIC, 0x0b, struct st_program_entry)
#define ST_IOC_GET_UID           _IOWR(ST_IOC_MAGIC, 0x0c, struct st_uid_entry)
#define ST_IOC_GET_SYSCALL       _IOWR(ST_IOC_MAGIC, 0x0d, struct st_syscall_entry)
#define ST_IOC_GET_STATS         _IOR(ST_IOC_MAGIC, 0x0e, struct st_stats)
#define ST_IOC_RESET_STATS       _IO(ST_IOC_MAGIC, 0x0f)
#endif
