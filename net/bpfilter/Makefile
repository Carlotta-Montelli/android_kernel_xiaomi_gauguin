# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the Linux BPFILTER layer.
#

hostprogs-y := bpfilter_umh
bpfilter_umh-objs := main.o
KBUILD_HOSTCFLAGS += -I $(srctree)/. -I $(srctree)/tools/include/ -I $(srctree)/tools/include/uapi \
			$(filter -m32 -m64, $(KBUILD_CFLAGS))
HOSTCC := $(CC)

ifeq ($(CONFIG_BPFILTER_UMH), y)
# builtin bpfilter_umh should be compiled with -static
# since rootfs isn't mounted at the time of __init
# function is called and do_execv won't find elf interpreter
KBUILD_HOSTLDFLAGS += -static $(filter -m32 -m64, $(KBUILD_CFLAGS))
endif

$(obj)/bpfilter_umh_blob.o: $(obj)/bpfilter_umh

obj-$(CONFIG_BPFILTER_UMH) += bpfilter.o
bpfilter-objs += bpfilter_kern.o bpfilter_umh_blob.o
