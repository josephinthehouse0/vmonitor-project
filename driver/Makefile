obj-m += vmonitor.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

# vmonitor_uapi.h ../include altinda oldugu icin include yolunu ekliyoruz
ccflags-y += -I$(PWD)/../include

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

load:
	sudo insmod vmonitor.ko

unload:
	sudo rmmod vmonitor

reload: unload load

.PHONY: all clean load unload reload
