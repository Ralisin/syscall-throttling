KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all kernel user tests clean load unload

all: kernel user tests

kernel:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/kernel modules

user:
	$(MAKE) -C user

tests:
	$(MAKE) -C tests

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/kernel clean
	$(MAKE) -C user clean
	$(MAKE) -C tests clean

load: kernel
	sudo insmod kernel/syscall_throttle.ko

unload:
	sudo rmmod syscall_throttle
