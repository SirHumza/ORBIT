# ORBIT - OpenOrbis PS4 package build
# Modified for ORBIT 0.1.0 on 2026-11-24.
TITLE       := ORBIT
VERSION     := 0.1.0
TITLE_ID    := ORBT00001
CONTENT_ID  := IV0000-ORBT00001_00-ORBIT00000000001
OUTPUT_PKG  := ORBIT.pkg

TOOLCHAIN   := $(OO_PS4_TOOLCHAIN)
PROJDIR     := src
INTDIR      := build
CDIR        := linux
UNAME_S     := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
CDIR        := macos
FSELF_TOOL  := $(TOOLCHAIN)/bin/$(CDIR)/create-fself-macos
else
FSELF_TOOL  := $(TOOLCHAIN)/bin/$(CDIR)/create-fself
endif

CC          := clang
CXX         := clang++
LD          := ld.lld
HOST_CXX    ?= c++
ifeq ($(UNAME_S),Darwin)
CC          := /usr/local/opt/llvm/bin/clang
CXX         := /usr/local/opt/llvm/bin/clang++
LD          := /usr/local/opt/llvm/bin/ld.lld
endif

LIBS := -lc -lkernel -lc++ -lSceUserService -lSceSystemService -lSceSysmodule \
        -lSceNet -lSceSsl -lSceHttp -lSceAppInstUtil -lSceBgft -lSceVideoOut \
        -lSceAudioOut -lScePad -lSDL2
CFLAGS   := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -c -DORBIS -D_GNU_SOURCE \
            -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include
CXXFLAGS := $(CFLAGS) -std=c++14 -MMD -MP -isystem $(TOOLCHAIN)/include/c++/v1 \
            -DBOOST_ASIO_ENABLE_CANCELIO -DBOOST_ASIO_HAS_STD_CHRONO \
            -DBOOST_ASIO_NO_DEPRECATED -DBOOST_EXCEPTION_DISABLE \
            -DTORRENT_DISABLE_ENCRYPTION -DTORRENT_NO_DEPRECATE \
            -DFIONBIO=0x8004667e -DFIONREAD=0x4004667f \
            -DTORRENT_USE_IFADDRS=1 -DTORRENT_USE_SYSCTL=0 \
            -DTORRENT_USE_IFCONF=0 -DTORRENT_HAS_PTHREAD_SET_NAME=0 \
            -I$(INTDIR)/deps/torrent/libtorrent-source/include \
            -I$(INTDIR)/deps/torrent/boost
LDFLAGS  := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr \
            -L$(TOOLCHAIN)/lib
LINK_LIBS := $(INTDIR)/deps/torrent/libtorrent-rasterbar.a $(LIBS) $(TOOLCHAIN)/lib/crt1.o
TORRENT_LIB := $(INTDIR)/deps/torrent/libtorrent-rasterbar.a

CPPFILES := $(wildcard $(PROJDIR)/*.cpp)
OBJS     := $(patsubst $(PROJDIR)/%.cpp,$(INTDIR)/%.o,$(CPPFILES))
DEPS     := $(OBJS:.o=.d)
PACKAGE_ASSETS := catalog.json assets/cacert.pem
RUNTIME_MODULES := sce_module/libSceFios2.prx sce_module/libc.prx
PACKAGE_FILES := eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png \
                 $(RUNTIME_MODULES) $(PACKAGE_ASSETS)
MODULE_DATA := $(TOOLCHAIN)/src/modules
HOST_TEST := $(INTDIR)/host-contract-tests
HOST_TEST_SOURCES := tests/contract_tests.cpp src/catalog.cpp src/package_verify.cpp \
                     src/sha256.cpp src/pem_bundle.cpp src/thread_exit_compat.cpp

-include $(DEPS)

.PHONY: all clean prepare validate host-tests
all: validate $(OUTPUT_PKG)

validate:
	python3 scripts/validate_catalog.py catalog/catalog.json

host-tests: $(HOST_TEST)
	$(HOST_TEST)
	python3 -m unittest discover -s tests -p 'test_*.py'

$(HOST_TEST): $(HOST_TEST_SOURCES) src/catalog.hpp src/mini_json.hpp src/package_verify.hpp \
              src/sha256.hpp src/pem_bundle.hpp | $(INTDIR)
	$(HOST_CXX) -std=c++11 -Wall -Wextra -Werror -pthread -Isrc -o $@ $(HOST_TEST_SOURCES)

prepare: sce_sys/about/right.sprx sce_sys/icon0.png $(RUNTIME_MODULES) $(PACKAGE_ASSETS)

catalog.json: catalog/catalog.json
	cp $< $@

$(INTDIR):
	mkdir -p $(INTDIR)

$(TORRENT_LIB): scripts/build_libtorrent_ps4.sh patches/libtorrent-openorbis.patch
	OO_PS4_TOOLCHAIN="$(TOOLCHAIN)" CXX="$(CXX)" sh scripts/build_libtorrent_ps4.sh $(INTDIR)/deps/torrent

$(INTDIR)/%.o: $(PROJDIR)/%.cpp $(TORRENT_LIB) | $(INTDIR)
	$(CXX) $(CXXFLAGS) -o $@ $<

eboot.bin: $(OBJS)
	$(LD) $(OBJS) -o $(INTDIR)/ORBIT.elf $(LDFLAGS) $(LINK_LIBS)
	$(FSELF_TOOL) -in=$(INTDIR)/ORBIT.elf \
		-out=$(INTDIR)/ORBIT.oelf --eboot eboot.bin --paid 0x3800000000000011

sce_sys/about/right.sprx:
	mkdir -p sce_sys/about
	cp $(MODULE_DATA)/right.sprx $@

sce_module/libSceFios2.prx:
	mkdir -p sce_module
	cp $(MODULE_DATA)/libSceFios2.prx $@

sce_module/libc.prx:
	mkdir -p sce_module
	cp $(MODULE_DATA)/libc.prx $@

sce_sys/icon0.png: tools/generate_icon.py
	mkdir -p sce_sys
	python3 tools/generate_icon.py $@

sce_sys/param.sfo: Makefile
	mkdir -p sce_sys
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_new $@
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(VERSION)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '$(VERSION)'

pkg.gp4: validate prepare eboot.bin sce_sys/param.sfo $(PACKAGE_ASSETS)
	$(TOOLCHAIN)/bin/$(CDIR)/create-gp4 -out $@ --content-id=$(CONTENT_ID) --files "$(PACKAGE_FILES)"

$(CONTENT_ID).pkg: pkg.gp4
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core pkg_build $< .

$(OUTPUT_PKG): $(CONTENT_ID).pkg
	cp $< $@

clean:
	rm -rf $(INTDIR) eboot.bin pkg.gp4 $(CONTENT_ID).pkg $(OUTPUT_PKG) sce_sys sce_module catalog.json
