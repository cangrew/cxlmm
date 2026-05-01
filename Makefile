# cxlmm/Makefile — top-level orchestrator
#
# Usage:
#   make                        # build everything
#   make KDIR=/path/to/linux    # specify kernel source
#   make CXL_NODE=2 DDR_NODE=0  # for 'make install'
#   make install                # insmod with node params
#   make uninstall              # rmmod
#   make clean                  # clean all build artifacts

# KDIR    ?= /home/andres/project_os/linux-6.12.74
KDIR ?= /lib/modules/$(shell uname -r)/build
CXL_NODE ?= 1
DDR_NODE ?= 0

export KDIR CXL_NODE DDR_NODE

.PHONY: all kmod lib daemon smoke install uninstall clean

all: kmod lib daemon smoke

smoke: lib
	$(CC) -O2 -Wall -I$(CURDIR)/lib -o $(CURDIR)/../cxlmm_smoke \
		$(CURDIR)/../cxlmm_smoke.c \
		-L$(CURDIR)/lib -lcxlmm -lpthread

kmod:
	$(MAKE) -C kmod KDIR=$(KDIR)

lib:
	$(MAKE) -C lib

daemon: lib
	$(MAKE) -C daemon CFLAGS="-I../include -I../lib" LDFLAGS="-L../lib -lcxlmm"

install: kmod
	@echo "Loading cxlmm.ko with cxl_node=$(CXL_NODE) ddr_node=$(DDR_NODE)"
	insmod kmod/cxlmm.ko cxl_node=$(CXL_NODE) ddr_node=$(DDR_NODE)
	@echo "Loaded. Check: dmesg | grep cxlmm"

uninstall:
	rmmod cxlmm || true

clean:
	$(MAKE) -C kmod KDIR=$(KDIR) clean
	$(MAKE) -C lib clean
	$(MAKE) -C daemon clean
	rm -f $(CURDIR)/../cxlmm_smoke
