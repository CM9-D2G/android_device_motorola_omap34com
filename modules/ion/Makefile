#
# Makefile for the ion memory module
#

subdir-ccflags-y += -DCONFIG_ION -DCONFIG_ION_OMAP -DCONFIG_HAVE_MEMBLOCK

obj-m    += ion.o

ion-objs := omap_ion.o #hook.o
ion-objs += memblock.o genalloc.o

ion-objs += gpu/ion.o gpu/ion_heap.o gpu/ion_system_heap.o gpu/ion_carveout_heap.o gpu/omap/omap_ion.o gpu/omap/omap_ion_alloc.o gpu/omap/omap_tiler_heap.o

