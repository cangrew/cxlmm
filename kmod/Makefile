# kmod/Makefile — out-of-tree kbuild for cxlmm.ko
#
# Usage:
#   make KDIR=/path/to/linux/source
#   make KDIR=/path/to/linux/source clean

KDIR ?= /home/andres/project_os/linux-6.12.74

# Extra include path so kernel sources can find cxlmm_uapi.h
EXTRA_CFLAGS := -I$(PWD)/../include -Wall -Werror

obj-m += cxlmm.o

cxlmm-objs := \
	cxlmm_main.o     \
	cxlmm_scanner.o  \
	cxlmm_bw_monitor.o \
	cxlmm_procfs.o   \
	cxlmm_ioctl.o

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules EXTRA_CFLAGS="$(EXTRA_CFLAGS)"

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
