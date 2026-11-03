# Ark build.
#   make gen EXEFS=path/to/exefs     translate the game's code to C (needs Unicorn's Python bindings; tools/setup_unicorn.sh)
#   make -j objs                     compile everything under generated/ into build/obj/
#   make -j ark                      runtime + game objects -> build/ark (Linux; Vulkan renderer, SDL3 window)
#        HEADLESS=1                  ... without window or renderer (NVN calls are logged and return 0)
#        DEPS=<prefix>               where SDL3 / Vulkan / shaderc / VMA are, if not system-wide
#                                    (tools/deps/build_linux_deps.sh builds them)
#   make mkromfs                     build/mkromfs: RomFS image from a romfs folder
#   make launcher                    build/ArkLauncher.exe (Windows): pick options and start the game
#   make test-lift EXEFS=...         differential test of the translator against Unicorn
EXEFS    ?= exefs
GEN      ?= generated
B        ?= build
# make predefines CC=cc; the generated code needs clang (musttail), so default to it unless CC is given.
ifeq ($(origin CC),default)
CC       := clang
endif
ifeq ($(origin CXX),default)
CXX      := clang++
endif
ARCH     := $(shell uname -m)
ifeq ($(filter arm64 aarch64,$(ARCH)),)
SIMD     := c
MFLAGS   := -mcx16 -mavx2
else
SIMD     := asm
MFLAGS   :=
endif
GENFLAGS := -O1 $(MFLAGS) -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -Iruntime/include

GEN_SRC  := $(wildcard $(GEN)/*/*.c)
GEN_OBJ  := $(patsubst $(GEN)/%.c,$(B)/obj/%.o,$(GEN_SRC))

ifeq ($(OS),Windows_NT)
# Windows: MSYS2 UCRT64 shell (tools/setup_windows.ps1, docs/BUILD-windows.md)
PLATFORM := runtime/platform/windows.c
EXE      := $(B)/ark.exe
OSFLAGS  := -D_WIN32_WINNT=0x0A00 -D_FILE_OFFSET_BITS=64 -D_POSIX_THREAD_SAFE_FUNCTIONS -D_CRT_RAND_S \
            -DWIN32_LEAN_AND_MEAN -DNOMINMAX
PY       ?= $(if $(wildcard tools/venv/Scripts/python.exe),tools/venv/Scripts/python.exe,python)
else ifeq ($(shell uname -s),Darwin)
# macOS on Apple silicon (PLA recomp Phase 1): Xcode command-line tools' clang; Homebrew for SDL3/shaderc/opus.
PLATFORM := runtime/platform/macos.c
EXE      := $(B)/ark
OSFLAGS  :=
PY       ?= python3
DEPS     ?= $(shell brew --prefix 2>/dev/null)
else
PLATFORM := runtime/platform/linux.c
EXE      := $(B)/ark
OSFLAGS  :=
PY       ?= python3
endif
GENFLAGS += $(OSFLAGS)

RT_SRC   := $(wildcard runtime/*.c) $(PLATFORM) $(wildcard runtime/services/*.c) $(wildcard runtime/hle/*.c)
HEADLESS ?= 0
ifeq ($(HEADLESS),1)
# no renderer, no SDL: the NVN layer, input and the menu hooks stay out
RT_SRC   := $(filter-out runtime/ui_none.c runtime/ui_prompt.c runtime/hle/input.c runtime/hle/bindings.c runtime/hotkeys.c,$(RT_SRC))
RTDEFS   := -DARK_HEADLESS -Iruntime/headless $(if $(DEPS),-I$(DEPS)/include)
LDLIBS   := $(if $(DEPS),-L$(DEPS)/lib) -lopus -lpthread -lm -ldl
else
RT_SRC   += $(wildcard runtime/nvn/*.c) $(wildcard runtime/nvn/maxwell/*.c) $(wildcard runtime/nvn/vulkan/*.c)
RT_CXX   := $(wildcard runtime/nvn/vulkan/*.cpp)
RTDEFS   := $(if $(DEPS),-I$(DEPS)/include) -Iruntime/nvn -I$(B)/inc -Ithird_party/smaa/Textures
ifeq ($(OS),Windows_NT)
LDLIBS   := -fuse-ld=lld -Wl,--stack,16777216 $(if $(DEPS),-L$(DEPS)/lib) -lSDL3 -lvulkan-1 -lshaderc_shared -lopus \
            -lstdc++ -lwinmm -lole32 -lshell32 -lcomdlg32 -luser32 -luuid -lpthread
else ifeq ($(shell uname -s),Darwin)
# macOS: Vulkan through MoltenVK (Homebrew vulkan-loader + molten-vk); Apple's C++ library is libc++
LDLIBS   := $(if $(DEPS),-L$(DEPS)/lib -Wl,-rpath,$(DEPS)/lib) -lSDL3 -lvulkan -lshaderc_shared -lopus -lc++ -lpthread -lm -lobjc -framework AppKit
else
LDLIBS   := $(if $(DEPS),-L$(DEPS)/lib -Wl,-rpath,$(DEPS)/lib) -lSDL3 -lvulkan -lshaderc_shared -lopus -lstdc++ -lpthread -lm -ldl
endif
endif
# extra compiler flags for the runtime, e.g. ARK_CFLAGS=-I<folder with vma/vk_mem_alloc.h> (header-only, not in Homebrew)
ARK_CFLAGS ?=
RTDEFS   += $(ARK_CFLAGS)
RT_OBJ   := $(patsubst runtime/%.c,$(B)/rt/%.o,$(RT_SRC)) $(patsubst runtime/%.cpp,$(B)/rt/%.o,$(RT_CXX))
RTFLAGS  := -O1 -g -std=gnu11 $(MFLAGS) $(OSFLAGS) -fno-strict-aliasing -Iruntime/include -Iruntime $(RTDEFS) \
            -Wall -Wno-unused-function -Wno-format
RT_HDRS  := $(wildcard runtime/*.h runtime/nvn/*.h runtime/nvn/*/*.h) runtime/include/cpu.h

.PHONY: help gen objs rt ark mkromfs launcher test-lift clean
help:
	@echo "Ark build targets (see docs/BUILD-windows.md or README.md):"
	@echo "  make gen EXEFS=game/exefs   translate the game's code to C (folder with main, sdk, subsdk0, subsdk1)"
	@echo "  make -j\$$(nproc) objs        compile the translated code (the long step)"
	@echo "  make -j\$$(nproc) ark mkromfs  build the executable and the RomFS tool"

gen:
	$(PY) tools/gen_all.py $(EXEFS) $(GEN) --simd $(SIMD)

objs: $(GEN_OBJ)
	@echo "compiled $(words $(GEN_OBJ)) files"

$(B)/obj/%.o: $(GEN)/%.c runtime/include/cpu.h runtime/include/simd.h runtime/include/simd_fp.h
	@mkdir -p $(dir $@)
	@$(CC) $(GENFLAGS) -c $< -o $@

$(B)/rt/%.o: runtime/%.c $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -c $< -o $@

$(B)/rt/%.o: runtime/%.cpp $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) -O2 -g -std=c++17 $(MFLAGS) $(OSFLAGS) $(RTDEFS) -Iruntime -c $< -o $@

# SMAA's reference shader source as a C string (runtime/nvn/smaa_data.c)
$(B)/inc/smaa_hlsl.inc: third_party/smaa/SMAA.hlsl
	@mkdir -p $(dir $@)
	LC_ALL=C tr -d '\r' < $< | LC_ALL=C tr '\200-\377' '?' | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' > $@
$(B)/rt/nvn/smaa_data.o: $(B)/inc/smaa_hlsl.inc

rt: $(RT_OBJ)

# export the runtime's symbols for backtraces (plat_symbol): -rdynamic on Linux, -export_dynamic on macOS
ifeq ($(OS),Windows_NT)
EXPORT_DYN :=
else ifeq ($(shell uname -s),Darwin)
EXPORT_DYN := -Wl,-export_dynamic
else
EXPORT_DYN := -rdynamic
endif

ark: $(EXE)
# (the object list goes in a response file: ~1,800 paths are over Windows' command-line limit. macOS ships GNU make
# 3.81, which has no $(file ...): there printf writes it, as macOS' argument limit is 1 MB)
$(EXE): $(RT_OBJ) $(GEN_OBJ)
ifneq ($(filter 3.%,$(MAKE_VERSION)),)
	@printf '%s\n' $(RT_OBJ) $(GEN_OBJ) > $(B)/link.rsp
else
	$(file >$(B)/link.rsp,$(RT_OBJ) $(GEN_OBJ))
endif
	$(CC) -o $@ @$(B)/link.rsp $(LDLIBS) $(EXPORT_DYN)

mkromfs: $(B)/mkromfs
$(B)/mkromfs: tools/mkromfs/mkromfs.c runtime/romfs_build.c
	@mkdir -p $(dir $@)
	$(CC) -O2 $(OSFLAGS) -Iruntime -Iruntime/include -o $@ $^

# Windows: a window for picking Ark's options and starting the game (build/ArkLauncher.exe)
WINDRES ?= windres
launcher: $(B)/ArkLauncher.exe
$(B)/ArkLauncher.exe: tools/launcher/launcher.c tools/launcher/launcher.rc tools/launcher/launcher.manifest
	@mkdir -p $(dir $@)
	$(WINDRES) -I tools/launcher tools/launcher/launcher.rc -O coff -o $(B)/launcher_res.o
	$(CC) -O2 -municode -mwindows -o $@ $< $(B)/launcher_res.o -lcomctl32 -lshell32 -lole32 -luser32 -lgdi32

test-lift:
	cd tools/lifter && $(PY) test_lift.py ../../$(EXEFS)/main 0x7000000000 20000 1

clean:
	rm -rf $(B)
