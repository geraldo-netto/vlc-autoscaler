# Top-level Makefile for VLC AutoUpscale
#
# Targets:
#   make             — build the VLC plugin
#   make plugin      — same
#   make test        — build and run unit tests (no VLC needed)
#   make fuzz-smoke  — build and run a deterministic 100k-iter smoke fuzzer
#   make fuzz        — build a libFuzzer target (clang only) at build/fuzz
#   make analyze     — run cppcheck across the source tree
#   make install     — install the built plugin into VLC's plugins dir
#   make uninstall
#   make clean
#   make info        — print discovered toolchain paths

PLUGIN := libautoupscale_plugin

CC      ?= gcc
CLANG   ?= clang
INSTALL ?= install
BUILD   ?= build

# --------- pkg-config (only needed for the plugin itself) ---------
VLC_CFLAGS := $(shell pkg-config --cflags vlc-plugin 2>/dev/null)
VLC_LIBS   := $(shell pkg-config --libs   vlc-plugin 2>/dev/null)
SWS_CFLAGS := $(shell pkg-config --cflags libswscale libavutil 2>/dev/null)
SWS_LIBS   := $(shell pkg-config --libs   libswscale libavutil 2>/dev/null)

# zimg is optional. If present, we compile an additional backend and
# default to it; if absent, swscale is the only backend.
ZIMG_CFLAGS := $(shell pkg-config --cflags zimg 2>/dev/null)
ZIMG_LIBS   := $(shell pkg-config --libs   zimg 2>/dev/null)
ifneq ($(strip $(ZIMG_LIBS)),)
  HAVE_ZIMG := 1
endif

VLC_PLUGIN_BASE := $(shell pkg-config --variable=pluginsdir vlc-plugin 2>/dev/null)
VLC_PLUGIN_DIR  := $(VLC_PLUGIN_BASE)/video_filter

# --------- common flags ---------
WARN := -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes
COMMON_CFLAGS := -O2 -fPIC -DPIC $(WARN) $(EXTRA_CFLAGS)
PLUGIN_CFLAGS := $(COMMON_CFLAGS) -DMODULE_STRING=\"autoupscale\" \
                 -D__PLUGIN__ $(VLC_CFLAGS) $(SWS_CFLAGS)
PLUGIN_LDFLAGS := -shared $(EXTRA_LDFLAGS)
PLUGIN_LIBS    := $(VLC_LIBS) $(SWS_LIBS) -lpthread

ifdef HAVE_ZIMG
  PLUGIN_CFLAGS += -DHAVE_ZIMG $(ZIMG_CFLAGS)
  PLUGIN_LIBS   += $(ZIMG_LIBS)
endif

TEST_CFLAGS  := -O2 -g $(WARN) -fsanitize=address,undefined
TEST_LDFLAGS := -fsanitize=address,undefined

FUZZ_SAN     := -fsanitize=fuzzer,address,undefined
FUZZ_CFLAGS  := -O1 -g $(WARN) $(FUZZ_SAN)

SMOKE_CFLAGS := -O2 -g $(WARN) -fsanitize=address,undefined -DFUZZ_MAIN
SMOKE_LDFLAGS := -fsanitize=address,undefined

PLUGIN_SRCS := src/autoupscale.c src/scaler.c src/scaler_swscale.c src/usm_pool.c
ifdef HAVE_ZIMG
  PLUGIN_SRCS += src/scaler_zimg.c
endif
PLUGIN_OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(PLUGIN_SRCS))

.PHONY: all plugin test fuzz fuzz-smoke analyze install uninstall clean info

all: plugin

# --------- plugin ---------
plugin: $(BUILD)/$(PLUGIN).so

$(BUILD)/$(PLUGIN).so: $(PLUGIN_OBJS) | $(BUILD)
	@if [ -z "$(VLC_LIBS)" ]; then \
		echo "ERROR: vlc-plugin pkg-config not found."; \
		echo "Install libvlccore-dev (Debian/Ubuntu) or vlc-devel (Fedora)."; \
		exit 1; \
	fi
	@if [ -z "$(SWS_LIBS)" ]; then \
		echo "ERROR: libswscale pkg-config not found."; \
		echo "Install libswscale-dev / libavutil-dev."; \
		exit 1; \
	fi
	@if [ -n "$(HAVE_ZIMG)" ]; then echo "  zimg backend: ENABLED"; \
	 else echo "  zimg backend: disabled (libzimg-dev not found)"; fi
	$(CC) $(PLUGIN_LDFLAGS) -o $@ $(PLUGIN_OBJS) $(PLUGIN_LIBS)

$(BUILD)/%.o: src/%.c src/scaler.h src/upscale_logic.h src/usm.h src/perfmon.h src/threading.h | $(BUILD)
	$(CC) $(PLUGIN_CFLAGS) -c -o $@ $<

# --------- unit tests ---------
test: $(BUILD)/test_upscale_logic $(BUILD)/test_usm $(BUILD)/test_perfmon $(BUILD)/test_threading $(BUILD)/test_zimg_helpers $(BUILD)/test_chroma_classify $(BUILD)/test_usm_pool
	@echo
	@echo "=== upscale_logic ==="
	@$(BUILD)/test_upscale_logic
	@echo
	@echo "=== usm ==="
	@$(BUILD)/test_usm
	@echo
	@echo "=== perfmon ==="
	@$(BUILD)/test_perfmon
	@echo
	@echo "=== threading ==="
	@$(BUILD)/test_threading
	@echo
	@echo "=== zimg_helpers ==="
	@$(BUILD)/test_zimg_helpers
	@echo
	@echo "=== chroma_classify ==="
	@$(BUILD)/test_chroma_classify
	@echo
	@echo "=== usm_pool ==="
	@$(BUILD)/test_usm_pool

$(BUILD)/test_upscale_logic: tests/test_upscale_logic.c src/upscale_logic.h | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_usm: tests/test_usm.c src/usm.h src/perfmon.h src/threading.h | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_perfmon: tests/test_perfmon.c src/perfmon.h | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS)

# --------- libFuzzer (clang) ---------
fuzz: $(BUILD)/fuzz_upscale_logic $(BUILD)/fuzz_usm $(BUILD)/fuzz_perfmon $(BUILD)/fuzz_threading $(BUILD)/fuzz_copy_plane $(BUILD)/fuzz_stripe_bounds $(BUILD)/fuzz_frame_shape
	@echo "Built libFuzzer targets:"
	@echo "  $(BUILD)/fuzz_upscale_logic"
	@echo "  $(BUILD)/fuzz_usm"
	@echo "  $(BUILD)/fuzz_perfmon"
	@echo "  $(BUILD)/fuzz_threading"
	@echo "  $(BUILD)/fuzz_copy_plane"
	@echo "  $(BUILD)/fuzz_stripe_bounds"
	@echo "  $(BUILD)/fuzz_frame_shape"
	@echo "Run e.g.: $(BUILD)/fuzz_upscale_logic -max_total_time=60"

$(BUILD)/fuzz_upscale_logic: tests/fuzz_upscale_logic.c src/upscale_logic.h | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_usm: tests/fuzz_usm.c src/usm.h src/perfmon.h src/threading.h | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_perfmon: tests/fuzz_perfmon.c src/perfmon.h | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_threading: tests/fuzz_threading.c src/threading.h | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_copy_plane: tests/fuzz_copy_plane.c src/zimg_helpers.h | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_stripe_bounds: tests/fuzz_stripe_bounds.c src/zimg_helpers.h | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_frame_shape: tests/fuzz_frame_shape.c src/chroma_classify.h src/zimg_helpers.h | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

# --------- smoke fuzz (no libFuzzer needed) ---------
fuzz-smoke: $(BUILD)/fuzz_smoke $(BUILD)/fuzz_usm_smoke $(BUILD)/fuzz_perfmon_smoke $(BUILD)/fuzz_threading_smoke $(BUILD)/fuzz_copy_plane_smoke $(BUILD)/fuzz_stripe_bounds_smoke $(BUILD)/fuzz_frame_shape_smoke
	@echo
	@echo "=== upscale_logic ==="
	@$(BUILD)/fuzz_smoke
	@echo
	@echo "=== usm ==="
	@$(BUILD)/fuzz_usm_smoke
	@echo
	@echo "=== perfmon ==="
	@$(BUILD)/fuzz_perfmon_smoke
	@echo
	@echo "=== threading ==="
	@$(BUILD)/fuzz_threading_smoke
	@echo
	@echo "=== copy_plane ==="
	@$(BUILD)/fuzz_copy_plane_smoke
	@echo
	@echo "=== stripe_bounds ==="
	@$(BUILD)/fuzz_stripe_bounds_smoke
	@echo
	@echo "=== frame_shape ==="
	@$(BUILD)/fuzz_frame_shape_smoke

$(BUILD)/fuzz_smoke: tests/fuzz_upscale_logic.c src/upscale_logic.h | $(BUILD)
	$(CLANG) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_usm_smoke: tests/fuzz_usm.c src/usm.h src/perfmon.h src/threading.h | $(BUILD)
	$(CLANG) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_perfmon_smoke: tests/fuzz_perfmon.c src/perfmon.h | $(BUILD)
	$(CLANG) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_threading_smoke: tests/fuzz_threading.c src/threading.h | $(BUILD)
	$(CLANG) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_copy_plane_smoke: tests/fuzz_copy_plane.c src/zimg_helpers.h | $(BUILD)
	$(CLANG) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_stripe_bounds_smoke: tests/fuzz_stripe_bounds.c src/zimg_helpers.h | $(BUILD)
	$(CLANG) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_frame_shape_smoke: tests/fuzz_frame_shape.c src/chroma_classify.h src/zimg_helpers.h | $(BUILD)
	$(CLANG) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

# --------- static analysis ---------
analyze:
	@command -v cppcheck >/dev/null 2>&1 || { \
		echo "cppcheck not installed. apt: cppcheck"; exit 1; }
	# autoupscale.c is excluded — it depends on VLC's macro-heavy headers
	# that cppcheck cannot reasonably parse without a full include path.
	# The interesting logic is all in *_logic.h / usm.h, exercised via tests.
	cppcheck --enable=warning,style,performance,portability \
		--inline-suppr --std=c11 --error-exitcode=2 \
		--suppress=missingIncludeSystem \
		-I src src/upscale_logic.h src/usm.h src/perfmon.h src/threading.h src/zimg_helpers.h src/chroma_classify.h src/usm_pool.h src/usm_pool.c tests/

# --------- install ---------
install: $(BUILD)/$(PLUGIN).so
	@if [ -z "$(VLC_PLUGIN_DIR)" ]; then \
		echo "ERROR: cannot determine VLC plugin directory"; exit 1; fi
	$(INSTALL) -d $(DESTDIR)$(VLC_PLUGIN_DIR)
	$(INSTALL) -m 0755 $(BUILD)/$(PLUGIN).so $(DESTDIR)$(VLC_PLUGIN_DIR)/
	@echo "Installed to $(DESTDIR)$(VLC_PLUGIN_DIR)/$(PLUGIN).so"
	@echo "If VLC doesn't pick it up, run:"
	@echo "  vlc-cache-gen $(VLC_PLUGIN_BASE)"

uninstall:
	rm -f $(DESTDIR)$(VLC_PLUGIN_DIR)/$(PLUGIN).so

clean:
	rm -rf $(BUILD)

info:
	@echo "VLC plugin dir : $(VLC_PLUGIN_DIR)"
	@echo "VLC cflags     : $(VLC_CFLAGS)"
	@echo "VLC libs       : $(VLC_LIBS)"
	@echo "swscale cflags : $(SWS_CFLAGS)"
	@echo "swscale libs   : $(SWS_LIBS)"
	@echo "CC             : $(CC)"
	@echo "CLANG          : $(CLANG)"

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_threading: tests/test_threading.c src/threading.h | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_zimg_helpers: tests/test_zimg_helpers.c src/zimg_helpers.h | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_chroma_classify: tests/test_chroma_classify.c src/chroma_classify.h | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_usm_pool: tests/test_usm_pool.c src/usm_pool.c src/usm_pool.h src/usm.h | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< src/usm_pool.c $(TEST_LDFLAGS) -lpthread
