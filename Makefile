CEXT_NAME=gc-arena

CC?=clang
CFLAGS+=-isystem include -I../include -I. -fPIC
DEBUG_FLAGS=-g -O0
PRODUCTION_FLAGS=-O2

SYS:=$(shell ${CC} -dumpmachine)
ifneq ($(findstring linux, $(SYS)),)
    DYLIB_PATH=native/linux-amd64/
    DYLIB_EXTENSION=.so
else ifneq ($(findstring mingw, $(SYS)),)
    CFLAGS=--sysroot=$(MINGW_DIR) --target=x86_64-w64-mingw32 -fuse-ld=lld -lws2_32
    DYLIB_PATH=/native/windows-amd64
    DYLIB_EXTENSION=.dll

else ifneq ($(findstring cygwin, $(SYS)),)
    # Do Cygwin things
else ifneq ($(findstring darwin, $(SYS)),)
    CFLAGS+=-arch x86_64 -arch arm64
    DYLIB_PATH=native/macos/
    DYLIB_EXTENSION=.dylib
endif

test: create-dirs
	$(CC) $(DEBUG_FLAGS) $(CFLAGS) -o build/bin/tests tests.c
	./build/bin/tests

demo: install-demo
	./dragonruby demo

debug: create-dirs
	$(CC) $(DEBUG_FLAGS) $(CFLAGS) -shared -o build/$(DYLIB_PATH)$(CEXT_NAME)$(DYLIB_EXTENSION) $(CEXT_NAME).c

production: create-dirs
	$(CC) $(PRODUCTION_FLAGS) $(CFLAGS) -shared -o build/$(DYLIB_PATH)$(CEXT_NAME)$(DYLIB_EXTENSION) $(CEXT_NAME).c

install-demo: debug
	cp -R build/native demo/

create-dirs:
	mkdir -p build/bin
	mkdir -p build/$(DYLIB_PATH)

clean:
	rm -fr build/
	rm -fr demo/native/
