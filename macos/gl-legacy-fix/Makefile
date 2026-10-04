# x86_64: Wine runs under Rosetta. The CLT linker can't read the 27 SDK's stubs.
SDK := $(lastword $(sort $(wildcard $(shell xcode-select -p)/SDKs/MacOSX26*.sdk)))

gl_legacy_fix.dylib: gl_legacy_fix.c
	clang -arch x86_64 -O2 -Wall $(if $(SDK),-isysroot $(SDK)) -dynamiclib -o $@ $< -framework OpenGL

clean:
	rm -f gl_legacy_fix.dylib
