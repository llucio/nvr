# Cross-compiles a fully static aarch64 (musl) uvcstream with zig cc.
# Usage: make            -> build/uvcstream
#        make install ROUTER=192.168.8.1
TARGET   ?= aarch64-linux-musl
ZIG      ?= zig
BUILD    := build
SYSROOT  := $(BUILD)/sysroot
LIBUSB_V := 1.0.27
LIBUVC_REF := master
CC       := $(ZIG) cc -target $(TARGET)
ROUTER   ?= 192.168.8.1

all: $(BUILD)/uvcstream

$(SYSROOT)/lib/libusb-1.0.a:
	mkdir -p $(BUILD)/src $(SYSROOT)
	curl -sL https://github.com/libusb/libusb/releases/download/v$(LIBUSB_V)/libusb-$(LIBUSB_V).tar.bz2 | tar xj -C $(BUILD)/src
	cd $(BUILD)/src/libusb-$(LIBUSB_V) && CC="$(CC)" AR="$(ZIG) ar" RANLIB="$(ZIG) ranlib" \
		./configure --host=$(TARGET) --prefix=$(abspath $(SYSROOT)) --disable-udev --enable-static --disable-shared
	$(MAKE) -C $(BUILD)/src/libusb-$(LIBUSB_V) install

$(SYSROOT)/lib/libuvc.a: $(SYSROOT)/lib/libusb-1.0.a
	rm -rf $(BUILD)/src/libuvc
	git clone -q --depth 1 -b $(LIBUVC_REF) https://github.com/libuvc/libuvc.git $(BUILD)/src/libuvc
	cd $(BUILD)/src/libuvc && sed -e 's/@libuvc_VERSION_MAJOR@/0/;s/@libuvc_VERSION_MINOR@/0/;s/@libuvc_VERSION_PATCH@/7/;s/@libuvc_VERSION@/0.0.7/;s|#cmakedefine LIBUVC_HAS_JPEG 1||' \
		include/libuvc/libuvc_config.h.in > include/libuvc/libuvc_config.h
	mkdir -p $(BUILD)/src/libuvc/o $(SYSROOT)/include/libuvc
	for f in ctrl ctrl-gen device diag frame init misc stream; do \
		$(CC) -O2 -c -I$(BUILD)/src/libuvc/include -I$(SYSROOT)/include/libusb-1.0 \
			$(BUILD)/src/libuvc/src/$$f.c -o $(BUILD)/src/libuvc/o/$$f.o || exit 1; done
	$(ZIG) ar rcs $@ $(BUILD)/src/libuvc/o/*.o
	cp $(BUILD)/src/libuvc/include/libuvc/*.h $(SYSROOT)/include/libuvc/

$(BUILD)/uvcstream: uvcstream.c $(SYSROOT)/lib/libuvc.a
	$(CC) -Os -s -Wall -Wextra -static -I$(SYSROOT)/include -I$(SYSROOT)/include/libusb-1.0 \
		$< $(SYSROOT)/lib/libuvc.a $(SYSROOT)/lib/libusb-1.0.a -lpthread -o $@

# Installs onto a router over SSH (prompts for the root password).
install: $(BUILD)/uvcstream
	ssh root@$(ROUTER) 'cat > /usr/bin/uvcstream && chmod 755 /usr/bin/uvcstream' < $(BUILD)/uvcstream
	ssh root@$(ROUTER) 'cat > /etc/init.d/uvcstream && chmod 755 /etc/init.d/uvcstream' < files/uvcstream.init
	ssh root@$(ROUTER) '[ -f /etc/config/uvcstream ] || cat > /etc/config/uvcstream' < files/uvcstream.config
	ssh root@$(ROUTER) '/etc/init.d/uvcstream enable && /etc/init.d/uvcstream restart'

# Removes everything `make install` added (binary, service, config, sysupgrade entries).
uninstall:
	ssh root@$(ROUTER) '/etc/init.d/uvcstream stop; /etc/init.d/uvcstream disable; \
		rm -f /usr/bin/uvcstream /etc/init.d/uvcstream /etc/config/uvcstream; \
		sed -i "/uvcstream/d" /etc/sysupgrade.conf'

clean:
	rm -rf $(BUILD)

.PHONY: all install uninstall clean
