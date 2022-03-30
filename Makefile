CONFIG_MODULE_SIG=y 
CONFIG_MODULE_SIG_ALL=y 
CONFIG_MODULE_SIG_SHA512=y 
CONFIG_MODULE_SIG_HASH="sha512"

obj-m := pstree_driver.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) M=$(shell pwd) modules	
install:
	$(MAKE) -C $(KDIR) M=$(shell pwd) module_install
clean: 
	$(MAKE) -C $(KDIR) M=$(shell pwd) clean

