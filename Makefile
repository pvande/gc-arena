CEXT_NAME=gc-arena

CC?=clang
CFLAGS+=-isystem include -fPIC
DEBUG_FLAGS=-g -O0
PRODUCTION_FLAGS=-O2

DYLIB_CFLAGS?=-shared

SYS:=$(shell ${CC} -dumpmachine)
ifneq ($(findstring linux, $(SYS)),)
    DYLIB_PATH=native/linux-amd64/
    DYLIB_EXTENSION=.so
else ifneq ($(findstring mingw, $(SYS)),)
    CFLAGS+=--sysroot=$(MINGW_DIR) -fuse-ld=lld
    EXEC_CFLAGS=-mconsole
    EXEC_EXTENSION=.exe
    DYLIB_PATH=native/windows-amd64
    DYLIB_EXTENSION=.dll
else ifneq ($(findstring cygwin, $(SYS)),)
    # Do Cygwin things
else ifneq ($(findstring darwin, $(SYS)),)
    CFLAGS+=-arch arm64
    CFLAGS+=-arch x86_64
    DYLIB_PATH=native/macos/
    DYLIB_EXTENSION=.dylib
endif

build: build-tests build-debug build-production

docs: src/$(CEXT_NAME).c
	yard doc --yardopts .yardoc/yardopts

test: build-tests
	./build/bin/tests${EXEC_EXTENSION}

demo: install-demo
	./dragonruby demo

install-demo: build-debug
	cp -R build/native demo/

build-debug: create-dirs src/$(CEXT_NAME).c
	$(CC) $(DEBUG_FLAGS) $(CFLAGS) ${DYLIB_CFLAGS} -o build/$(DYLIB_PATH)$(CEXT_NAME)-debug$(DYLIB_EXTENSION) src/$(CEXT_NAME).c

build-production: create-dirs src/$(CEXT_NAME).c
	$(CC) $(PRODUCTION_FLAGS) $(CFLAGS) ${DYLIB_CFLAGS} -o build/$(DYLIB_PATH)$(CEXT_NAME)$(DYLIB_EXTENSION) src/$(CEXT_NAME).c

build-tests: create-dirs src/$(CEXT_NAME).c src/tests.c
	$(CC) $(DEBUG_FLAGS) $(CFLAGS) ${EXEC_CFLAGS} -o build/bin/tests${EXEC_EXTENSION} src/tests.c

create-dirs:
	mkdir -p build/bin
	mkdir -p build/$(DYLIB_PATH)

clean:
	rm -fr build/
	rm -fr demo/native/
