PREFIX_FTDI := $(shell brew --prefix libftdi)
PREFIX_USB  := $(shell brew --prefix libusb)
# the CLT linker here can't read the 27 SDK's stubs; build against the running OS's SDK
SDK := $(lastword $(sort $(wildcard $(shell xcode-select -p)/SDKs/MacOSX26*.sdk)))

all: ftd2xx.dll ftdi-bridge

# 32-bit Windows DLL loaded by PCLink under Wine
ftd2xx.dll: ftd2xx.c ftd2xx.def protocol.h
	i686-w64-mingw32-gcc -O2 -Wall -shared -o $@ ftd2xx.c ftd2xx.def \
	    -lws2_32 -static-libgcc -Wl,--enable-stdcall-fixup

# native macOS helper that owns the USB device
ftdi-bridge: ftdi-bridge.c protocol.h
	cc -O2 -Wall $(if $(SDK),-isysroot $(SDK)) -o $@ ftdi-bridge.c -I$(PREFIX_FTDI)/include/libftdi1 \
	    -I$(PREFIX_USB)/include/libusb-1.0 -L$(PREFIX_FTDI)/lib -L$(PREFIX_USB)/lib -lftdi1 -lusb-1.0

clean:
	rm -f ftd2xx.dll ftdi-bridge
