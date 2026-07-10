# Top-level Makefile for VLC AutoUpscale
#
# Targets:
#   make             — build the VLC plugin
#   make plugin      — same
#   make test        — build and run unit tests (no VLC needed)
#   make fuzz-smoke  — build and run deterministic smoke fuzzers
#   make fuzz        — build libFuzzer targets (clang only)
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
WARN := -D_GNU_SOURCE -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes

# CPU baseline. Defaults to `native` because this plugin is a source
# distribution: every user builds it on the same machine they run it on.
# Build-host tuning can use extra scheduling and ISA features on the per-frame
# hot path. The .so loads only on
# a CPU supporting the emitted ISA; that's the right default for a
# build-and-run workflow. For a wider Linux x86-64 ISA baseline, override:
#
#   make MARCH=x86-64-v4    # AVX-512F + BW + CD + DQ + VL
#                           # — Skylake-X 2017+, AMD Zen 4 2022+
#   make MARCH=x86-64-v3    # full v3 level (AVX2, BMI1/2, FMA, etc.)
#                           # — Haswell 2013+, Zen 1 2017+
#   make MARCH=x86-64       # legacy SSE2 only — runs anywhere x86-64
#
# Combine with MULTIVERSION=1 to include several SIMD variants selected at
# runtime. The whole plugin must still use a baseline supported by every
# deployment CPU; the variant-guard limitation is tracked as PORT-6.
#
# Decoded MD5 is byte-identical across all SIMD widths because the
# kernels do bytewise saturating arithmetic; SIMD just runs more lanes
# in parallel.
MARCH        ?= native
ifneq ($(MARCH),)
  MARCH_FLAG := -march=$(MARCH)
endif

COMMON_CFLAGS := -O2 $(MARCH_FLAG) -fPIC -DPIC $(WARN) -MMD -MP -fstack-protector-strong -D_FORTIFY_SOURCE=2 -flto $(EXTRA_CFLAGS)
PLUGIN_CFLAGS := $(COMMON_CFLAGS) -DMODULE_STRING=\"autoupscale\" \
                 -D__PLUGIN__ $(VLC_CFLAGS) $(SWS_CFLAGS)
PLUGIN_LDFLAGS := -shared -flto $(EXTRA_LDFLAGS)
PLUGIN_LIBS    := $(VLC_LIBS) $(SWS_LIBS) -lpthread

ifdef HAVE_ZIMG
  PLUGIN_CFLAGS += -DHAVE_ZIMG $(ZIMG_CFLAGS)
  PLUGIN_LIBS   += $(ZIMG_LIBS)
endif

TEST_CFLAGS  := -O2 -g $(MARCH_FLAG) $(WARN) -fsanitize=address,undefined
TEST_LDFLAGS := -fsanitize=address,undefined
BARRIER_WRAP_LDFLAGS := -Wl,--wrap=sem_wait -Wl,--wrap=pthread_cond_broadcast

FUZZ_SAN     := -fsanitize=fuzzer,address,undefined
FUZZ_CFLAGS  := -O1 -g $(MARCH_FLAG) $(WARN) $(FUZZ_SAN)

SMOKE_CFLAGS := -O2 -g $(MARCH_FLAG) $(WARN) -fsanitize=address,undefined -DFUZZ_MAIN
SMOKE_LDFLAGS := -fsanitize=address,undefined

PLUGIN_SRCS := src/autoupscale.c src/scaler.c src/scaler_swscale.c
ifdef HAVE_ZIMG
  PLUGIN_SRCS += src/scaler_zimg.c
endif
PLUGIN_OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(PLUGIN_SRCS))

# --------- multi-versioned USM pool ---------
# Defaults to MULTIVERSION=0 because the build-time MARCH default is
# `native`: the binary is already tuned for the local CPU, so the runtime
# dispatcher and non-selected SIMD variants would add dead code. Pair this
# default with MARCH=native (default) for a build
# that maximizes performance on the build host.
#
# Set MULTIVERSION=1 to ship the SIMD variants of usm_pool.c plus a thin
# runtime dispatcher
# (usm_pool_dispatch.c) that selects a variant at .so load time via
# __builtin_cpu_supports(). The full-level guard gap remains PORT-6. Pair this
# with an appropriate MARCH baseline (e.g.
# x86-64-v3 or x86-64) so the rest of the plugin uses the intended baseline:
#
#   make MARCH=x86-64-v3 MULTIVERSION=1   # compatible with Haswell/Zen 1+,
#                                          # selects a USM variant at load
#   make MARCH=x86-64    MULTIVERSION=1   # original x86-64/SSE2 baseline
MULTIVERSION ?= 0

ifeq ($(MULTIVERSION),1)
USM_OBJS := \
    $(BUILD)/usm_pool_sse2.o \
    $(BUILD)/usm_pool_avx2.o \
    $(BUILD)/usm_pool_avx512.o \
    $(BUILD)/usm_pool_dispatch.o

# usm_pool.c is the per-frame hot path: worker_main calls inlined hblur
# and combine kernels for every row of every frame. Compiling it at -O3
# (vs the rest of the plugin's -O2) enables more aggressive inlining and loop
# transforms in the worker dispatch and row-sweep helpers, which the lower
# optimization level may not perform. The rest of the plugin
# (autoupscale.c, scaler*.c, scaler_zimg.c) stays
# at -O2 since it is not pixel-loop heavy and the binary-size /
# compile-time win matters more there.
USM_POOL_CFLAGS := $(subst -O2,-O3,$(PLUGIN_CFLAGS))

# Each variant is the SAME usm_pool.c compiled at its own -march level
# with USM_VARIANT macro renaming the public symbols. We pass the level
# AFTER USM_POOL_CFLAGS so it overrides any earlier -march from MARCH_FLAG.
$(BUILD)/usm_pool_sse2.o: src/usm_pool.c src/usm.h src/usm_pool.h | $(BUILD)
	$(CC) $(USM_POOL_CFLAGS) -march=x86-64    -DUSM_VARIANT=sse2   -c -o $@ $<
$(BUILD)/usm_pool_avx2.o: src/usm_pool.c src/usm.h src/usm_pool.h | $(BUILD)
	$(CC) $(USM_POOL_CFLAGS) -march=x86-64-v3 -DUSM_VARIANT=avx2   -c -o $@ $<
$(BUILD)/usm_pool_avx512.o: src/usm_pool.c src/usm.h src/usm_pool.h | $(BUILD)
	$(CC) $(USM_POOL_CFLAGS) -march=x86-64-v4 -DUSM_VARIANT=avx512 -c -o $@ $<

# Dispatcher must be at the lowest baseline so it runs on ANY CPU. It just
# does CPU-feature checks and indirect calls — no SIMD work itself.
$(BUILD)/usm_pool_dispatch.o: src/usm_pool_dispatch.c src/usm_pool.h src/cpu_level.h | $(BUILD)
	$(CC) $(PLUGIN_CFLAGS) -march=x86-64 -c -o $@ $<
else
USM_OBJS := $(BUILD)/usm_pool.o
# Single-baseline build: same -O3 reasoning as the multi-versioned case.
USM_POOL_CFLAGS := $(subst -O2,-O3,$(PLUGIN_CFLAGS))
$(BUILD)/usm_pool.o: src/usm_pool.c src/usm.h src/usm_pool.h | $(BUILD)
	$(CC) $(USM_POOL_CFLAGS) -c -o $@ $<
endif

PLUGIN_OBJS += $(USM_OBJS)

.PHONY: all plugin test fuzz fuzz-smoke fuzz-seam analyze scan-build build-bench install uninstall clean info bench bench-flatskip test-zimg stress stress-zimg bench-zimg coverage-zimg

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
	@echo "  CPU baseline:    -march=$(MARCH)$(if $(filter native,$(MARCH)), (build-host ISA),$(if $(filter x86-64-v4,$(MARCH)), (Intel Skylake-X 2017+ / AMD Zen 4 2022+),$(if $(filter x86-64-v3,$(MARCH)), (Intel Haswell 2013+ / AMD Zen 1 2017+),$(if $(filter x86-64,$(MARCH)), (Linux x86-64 / SSE2 baseline),))))"
	@if [ "$(MULTIVERSION)" = "1" ]; then \
	    echo "  USM SIMD:        multi-versioned (SSE2 + AVX2 + AVX-512, runtime dispatch)"; \
	 else \
	    echo "  USM SIMD:        single-baseline at -march=$(MARCH) (no runtime dispatcher)"; \
	 fi
	@if [ -n "$(HAVE_ZIMG)" ]; then echo "  zimg backend:    ENABLED"; \
	 else echo "  zimg backend:    disabled (libzimg-dev not found)"; fi
	$(CC) $(PLUGIN_LDFLAGS) -o $@ $(PLUGIN_OBJS) $(PLUGIN_LIBS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(PLUGIN_CFLAGS) -c -o $@ $<

# --------- unit tests ---------
test: complexity $(BUILD)/test_upscale_logic $(BUILD)/test_geometry_edge_cases $(BUILD)/test_usm $(BUILD)/test_perfmon $(BUILD)/test_cli_parse $(BUILD)/test_threading $(BUILD)/test_zimg_helpers $(BUILD)/test_chroma_classify $(BUILD)/test_usm_pool $(BUILD)/test_content_probe $(BUILD)/test_scaler_pick $(BUILD)/test_scaler_swscale $(BUILD)/test_picture_view $(BUILD)/test_lifetime $(BUILD)/test_usm_pool_variants
	@echo
	@echo "=== upscale_logic ==="
	@$(BUILD)/test_upscale_logic
	@echo
	@echo "=== usm ==="
	@$(BUILD)/test_usm
	@echo
	@echo "=== geometry_edge_cases ==="
	@$(BUILD)/test_geometry_edge_cases
	@echo
	@echo "=== perfmon ==="
	@$(BUILD)/test_perfmon
	@echo
	@echo "=== cli_parse ==="
	@$(BUILD)/test_cli_parse
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
	@echo
	@echo "=== content_probe ==="
	@$(BUILD)/test_content_probe
	@echo
	@echo "=== scaler_pick ==="
	@$(BUILD)/test_scaler_pick
	@echo
	@echo "=== scaler_swscale ==="
	@$(BUILD)/test_scaler_swscale
	@echo
	@echo "=== picture_view ==="
	@$(BUILD)/test_picture_view
	@echo
	@echo "=== lifetime / UAF ==="
	@$(BUILD)/test_lifetime
	@echo
	@echo "=== usm_pool_variants (cross-SIMD byte-equivalence) ==="
	@$(BUILD)/test_usm_pool_variants

$(BUILD)/test_upscale_logic: tests/test_upscale_logic.c src/upscale_logic.h | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_geometry_edge_cases: tests/test_geometry_edge_cases.c src/upscale_logic.h | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_usm: tests/test_usm.c tests/usm_test_util.h src/usm.h src/perfmon.h src/threading.h | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_perfmon: tests/test_perfmon.c src/perfmon.h | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_cli_parse: tests/test_cli_parse.c tests/cli_parse.h | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS)

# --------- libFuzzer (clang) ---------
fuzz: $(BUILD)/fuzz_upscale_logic $(BUILD)/fuzz_usm $(BUILD)/fuzz_perfmon $(BUILD)/fuzz_threading $(BUILD)/fuzz_copy_plane $(BUILD)/fuzz_stripe_bounds $(BUILD)/fuzz_decide_tile_grid $(BUILD)/fuzz_frame_shape $(BUILD)/fuzz_scaler_chroma $(BUILD)/fuzz_scaler_open $(BUILD)/fuzz_content_probe $(BUILD)/fuzz_picture_view $(BUILD)/fuzz_usm_variants
	@echo "Built libFuzzer targets:"
	@echo "  $(BUILD)/fuzz_upscale_logic"
	@echo "  $(BUILD)/fuzz_usm"
	@echo "  $(BUILD)/fuzz_perfmon"
	@echo "  $(BUILD)/fuzz_threading"
	@echo "  $(BUILD)/fuzz_copy_plane"
	@echo "  $(BUILD)/fuzz_stripe_bounds"
	@echo "  $(BUILD)/fuzz_decide_tile_grid"
	@echo "  $(BUILD)/fuzz_frame_shape"
	@echo "  $(BUILD)/fuzz_scaler_chroma"
	@echo "  $(BUILD)/fuzz_scaler_open"
	@echo "  $(BUILD)/fuzz_content_probe"
	@echo "  $(BUILD)/fuzz_picture_view"
	@echo "  $(BUILD)/fuzz_usm_variants"
	@echo ""
	@echo "Run from random bytes:    $(BUILD)/fuzz_upscale_logic -max_total_time=60"
	@echo "Run with seed corpus:     mkdir -p fuzz_corpus &&"
	@echo "                          cp tests/corpus_usm_variants/* fuzz_corpus/ &&"
	@echo "                          $(BUILD)/fuzz_usm_variants fuzz_corpus -max_total_time=60"
	@echo ""
	@echo "(Always copy seeds to a working dir; libFuzzer writes new finds back"
	@echo " into whatever directory you pass it, polluting the curated corpus.)"

$(BUILD)/fuzz_upscale_logic: tests/fuzz_upscale_logic.c src/upscale_logic.h | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_usm: tests/fuzz_usm.c tests/usm_test_util.h src/usm.h src/perfmon.h src/threading.h | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_perfmon: tests/fuzz_perfmon.c tests/cli_parse.h src/perfmon.h | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_threading: tests/fuzz_threading.c tests/cli_parse.h src/threading.h | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_copy_plane: tests/fuzz_copy_plane.c tests/cli_parse.h src/zimg_helpers.h | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_stripe_bounds: tests/fuzz_stripe_bounds.c tests/cli_parse.h src/zimg_helpers.h | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_decide_tile_grid: tests/fuzz_decide_tile_grid.c src/zimg_helpers.h | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_frame_shape: tests/fuzz_frame_shape.c src/chroma_classify.h src/zimg_helpers.h | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_scaler_chroma: tests/fuzz_scaler_chroma.c src/scaler_zimg_chroma.h src/chroma_classify.h | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_scaler_open: tests/fuzz_scaler_open.c src/scaler_pick_logic.h | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_content_probe: tests/fuzz_content_probe.c src/content_probe.h | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_picture_view: tests/fuzz_picture_view.c tests/cli_parse.h src/picture_view.h tests/stubs/vlc_common.h tests/stubs/vlc_picture.h | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -Itests/stubs -o $@ $<

# libFuzzer variant fuzzer: needs the three SIMD .o files compiled with the
# same FUZZ_CFLAGS (libFuzzer + ASan + UBSan). Each variant TU is at its
# own -march level via -DUSM_VARIANT=<name>.
$(BUILD)/fuzz_lf_usm_pool_sse2.o:   src/usm_pool.c src/usm.h src/usm_pool.h | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -march=x86-64    -DUSM_VARIANT=sse2   -c -o $@ $<
$(BUILD)/fuzz_lf_usm_pool_avx2.o:   src/usm_pool.c src/usm.h src/usm_pool.h | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -march=x86-64-v3 -DUSM_VARIANT=avx2   -c -o $@ $<
$(BUILD)/fuzz_lf_usm_pool_avx512.o: src/usm_pool.c src/usm.h src/usm_pool.h | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -march=x86-64-v4 -DUSM_VARIANT=avx512 -c -o $@ $<

$(BUILD)/fuzz_usm_variants: tests/fuzz_usm_variants.c \
    $(BUILD)/fuzz_lf_usm_pool_sse2.o $(BUILD)/fuzz_lf_usm_pool_avx2.o \
    $(BUILD)/fuzz_lf_usm_pool_avx512.o \
    src/usm_pool.h | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $< \
	    $(BUILD)/fuzz_lf_usm_pool_sse2.o $(BUILD)/fuzz_lf_usm_pool_avx2.o \
	    $(BUILD)/fuzz_lf_usm_pool_avx512.o \
	    -lpthread

# --------- smoke fuzz (no libFuzzer needed) ---------
fuzz-smoke: $(BUILD)/fuzz_smoke $(BUILD)/fuzz_usm_smoke $(BUILD)/fuzz_perfmon_smoke $(BUILD)/fuzz_threading_smoke $(BUILD)/fuzz_copy_plane_smoke $(BUILD)/fuzz_stripe_bounds_smoke $(BUILD)/fuzz_decide_tile_grid_smoke $(BUILD)/fuzz_frame_shape_smoke $(BUILD)/fuzz_scaler_chroma_smoke $(BUILD)/fuzz_scaler_open_smoke $(BUILD)/fuzz_content_probe_smoke $(BUILD)/fuzz_picture_view_smoke $(BUILD)/fuzz_usm_variants_smoke
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
	@echo "=== decide_tile_grid ==="
	@$(BUILD)/fuzz_decide_tile_grid_smoke
	@echo
	@echo "=== frame_shape ==="
	@$(BUILD)/fuzz_frame_shape_smoke
	@echo
	@echo "=== scaler_chroma ==="
	@$(BUILD)/fuzz_scaler_chroma_smoke
	@echo
	@echo "=== scaler_open ==="
	@$(BUILD)/fuzz_scaler_open_smoke
	@echo
	@echo "=== content_probe ==="
	@$(BUILD)/fuzz_content_probe_smoke
	@echo
	@echo "=== picture_view ==="
	@$(BUILD)/fuzz_picture_view_smoke
	@echo
	@echo "=== usm_variants (cross-SIMD byte-equivalence) ==="
	@$(BUILD)/fuzz_usm_variants_smoke

$(BUILD)/fuzz_smoke: tests/fuzz_upscale_logic.c src/upscale_logic.h | $(BUILD)
	$(CLANG) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_usm_smoke: tests/fuzz_usm.c tests/usm_test_util.h src/usm.h src/perfmon.h src/threading.h | $(BUILD)
	$(CLANG) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_perfmon_smoke: tests/fuzz_perfmon.c tests/cli_parse.h src/perfmon.h | $(BUILD)
	$(CLANG) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_threading_smoke: tests/fuzz_threading.c tests/cli_parse.h src/threading.h | $(BUILD)
	$(CLANG) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_copy_plane_smoke: tests/fuzz_copy_plane.c tests/cli_parse.h src/zimg_helpers.h | $(BUILD)
	$(CLANG) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_stripe_bounds_smoke: tests/fuzz_stripe_bounds.c tests/cli_parse.h src/zimg_helpers.h | $(BUILD)
	$(CLANG) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_decide_tile_grid_smoke: tests/fuzz_decide_tile_grid.c src/zimg_helpers.h | $(BUILD)
	$(CLANG) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_frame_shape_smoke: tests/fuzz_frame_shape.c src/chroma_classify.h src/zimg_helpers.h | $(BUILD)
	$(CLANG) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_scaler_chroma_smoke: tests/fuzz_scaler_chroma.c src/scaler_zimg_chroma.h src/chroma_classify.h | $(BUILD)
	$(CLANG) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_scaler_open_smoke: tests/fuzz_scaler_open.c src/scaler_pick_logic.h | $(BUILD)
	$(CLANG) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_content_probe_smoke: tests/fuzz_content_probe.c src/content_probe.h | $(BUILD)
	$(CLANG) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_picture_view_smoke: tests/fuzz_picture_view.c tests/cli_parse.h src/picture_view.h tests/stubs/vlc_common.h tests/stubs/vlc_picture.h | $(BUILD)
	$(CLANG) $(SMOKE_CFLAGS) -Itests/stubs -o $@ $< $(SMOKE_LDFLAGS)

# Cross-variant smoke fuzzer: needs the same three SIMD .o files as the
# variant-equivalence test. Built with clang because the smoke fuzzer
# infra is clang-based; clang's -DUSM_VARIANT path is identical to gcc's.
$(BUILD)/fuzz_usm_pool_sse2.o:   src/usm_pool.c src/usm.h src/usm_pool.h | $(BUILD)
	$(CLANG) $(SMOKE_CFLAGS) -march=x86-64    -DUSM_VARIANT=sse2   -c -o $@ $<
$(BUILD)/fuzz_usm_pool_avx2.o:   src/usm_pool.c src/usm.h src/usm_pool.h | $(BUILD)
	$(CLANG) $(SMOKE_CFLAGS) -march=x86-64-v3 -DUSM_VARIANT=avx2   -c -o $@ $<
$(BUILD)/fuzz_usm_pool_avx512.o: src/usm_pool.c src/usm.h src/usm_pool.h | $(BUILD)
	$(CLANG) $(SMOKE_CFLAGS) -march=x86-64-v4 -DUSM_VARIANT=avx512 -c -o $@ $<

$(BUILD)/fuzz_usm_variants_smoke: tests/fuzz_usm_variants.c \
    $(BUILD)/fuzz_usm_pool_sse2.o $(BUILD)/fuzz_usm_pool_avx2.o \
    $(BUILD)/fuzz_usm_pool_avx512.o \
    src/usm_pool.h | $(BUILD)
	$(CLANG) $(SMOKE_CFLAGS) -o $@ $< \
	    $(BUILD)/fuzz_usm_pool_sse2.o $(BUILD)/fuzz_usm_pool_avx2.o \
	    $(BUILD)/fuzz_usm_pool_avx512.o \
	    $(SMOKE_LDFLAGS) -lpthread

# --------- concurrency stress test ---------
# Two builds:
#   stress_usm_pool       - ASan+UBSan, default
#   stress_usm_pool_tsan  - ThreadSanitizer (catches data races even when
#                            output is bitwise correct)
#
# Bit-identical output to single-threaded reference is required across
# repeated frames at unusual (n_threads, w, h) combinations, including worker
# clamping and the single-worker path.

STRESS_CFLAGS_ASAN := -O2 -g $(MARCH_FLAG) $(WARN) -fsanitize=address,undefined
STRESS_LDFLAGS_ASAN := -fsanitize=address,undefined -lpthread
STRESS_CFLAGS_TSAN := -O1 -g $(MARCH_FLAG) $(WARN) -fsanitize=thread
STRESS_LDFLAGS_TSAN := -fsanitize=thread -lpthread

$(BUILD)/stress_usm_pool: tests/stress_usm_pool.c tests/usm_test_util.h src/usm_pool.c src/usm_pool.h src/usm.h | $(BUILD)
	$(CLANG) $(STRESS_CFLAGS_ASAN) -o $@ $< src/usm_pool.c $(STRESS_LDFLAGS_ASAN)

$(BUILD)/stress_usm_pool_tsan: tests/stress_usm_pool.c tests/usm_test_util.h src/usm_pool.c src/usm_pool.h src/usm.h | $(BUILD)
	$(CLANG) $(STRESS_CFLAGS_TSAN) -o $@ $< src/usm_pool.c $(STRESS_LDFLAGS_TSAN)

stress: $(BUILD)/stress_usm_pool $(BUILD)/stress_usm_pool_tsan
	@echo
	@echo "=== usm_pool stress (ASan + UBSan) ==="
	@$(BUILD)/stress_usm_pool
	@echo
	@echo "=== usm_pool stress (ThreadSanitizer) ==="
	@$(BUILD)/stress_usm_pool_tsan

# --------- zimg backend harness (requires libzimg + VLC headers) ---------
# scaler_zimg.c is VLC-typed but never touches VLC's picture pool/logging at
# runtime, so these link a private copy compiled under sanitizers against real
# VLC + zimg headers and drive open/process/close on hand-built pictures (see
# tests/zimg_test_util.h). Kept OUT of `make test` (which stays VLC-free);
# run explicitly. Built only when libzimg was detected.
ifdef HAVE_ZIMG
ZIMG_H_CFLAGS  := -g $(MARCH_FLAG) $(WARN) $(VLC_CFLAGS) $(ZIMG_CFLAGS)
ZIMG_H_LIBS    := $(VLC_LIBS) $(ZIMG_LIBS) -lpthread
ZIMG_H_DEPS    := tests/zimg_test_util.h src/scaler_zimg.c src/scaler.h \
                  src/scaler_status.h \
                  src/picture_view.h \
                  tests/barrier_fault_inject.h \
                  src/zimg_helpers.h src/scaler_zimg_chroma.h \
                  src/upscale_logic.h src/threading.h

$(BUILD)/test_scaler_zimg: tests/test_scaler_zimg.c $(ZIMG_H_DEPS) | $(BUILD)
	$(CC) -O2 $(ZIMG_H_CFLAGS) -fsanitize=address,undefined -o $@ \
	    $< src/scaler_zimg.c -fsanitize=address,undefined \
	    $(BARRIER_WRAP_LDFLAGS) $(ZIMG_H_LIBS)

$(BUILD)/test_scaler_zimg_tsan: tests/test_scaler_zimg.c $(ZIMG_H_DEPS) | $(BUILD)
	$(CLANG) -O1 $(ZIMG_H_CFLAGS) -fsanitize=thread -o $@ \
	    $< src/scaler_zimg.c -fsanitize=thread \
	    $(BARRIER_WRAP_LDFLAGS) $(ZIMG_H_LIBS)

$(BUILD)/bench_scaler_zimg: tests/bench_scaler_zimg.c tests/cli_parse.h $(ZIMG_H_DEPS) | $(BUILD)
	$(CC) -O2 $(ZIMG_H_CFLAGS) -o $@ $< src/scaler_zimg.c $(ZIMG_H_LIBS)

# SCAL-3 seam fuzzer: randomized geometry/chroma/thread-count, asserts the
# tiled-vs-single-graph seam stays <= SEAM_MAX_DELTA on smooth content.
# Smoke variant (own main) runs in the standard harness; libFuzzer variant
# (clang) explores the geometry space.
$(BUILD)/fuzz_scaler_seam_smoke: tests/fuzz_scaler_seam.c $(ZIMG_H_DEPS) | $(BUILD)
	$(CC) -O2 $(ZIMG_H_CFLAGS) -DFUZZ_MAIN -fsanitize=address,undefined -o $@ \
	    $< src/scaler_zimg.c -fsanitize=address,undefined $(ZIMG_H_LIBS)

$(BUILD)/fuzz_scaler_seam: tests/fuzz_scaler_seam.c $(ZIMG_H_DEPS) | $(BUILD)
	$(CLANG) -O1 -g $(MARCH_FLAG) $(WARN) $(VLC_CFLAGS) $(ZIMG_CFLAGS) $(FUZZ_SAN) \
	    -o $@ $< src/scaler_zimg.c $(FUZZ_SAN) $(ZIMG_H_LIBS)

fuzz-seam: $(BUILD)/fuzz_scaler_seam_smoke
	@echo
	@echo "=== scaler_seam smoke (ASan+UBSan, randomized geometry) ==="
	@$(BUILD)/fuzz_scaler_seam_smoke

test-zimg: $(BUILD)/test_scaler_zimg $(BUILD)/fuzz_scaler_seam_smoke
	@echo
	@echo "=== scaler_zimg invariants (ASan + UBSan) ==="
	@$(BUILD)/test_scaler_zimg
	@echo
	@echo "=== scaler_seam smoke (ASan + UBSan, randomized geometry) ==="
	@$(BUILD)/fuzz_scaler_seam_smoke

stress-zimg: $(BUILD)/test_scaler_zimg $(BUILD)/test_scaler_zimg_tsan
	@echo
	@echo "=== scaler_zimg invariants (ASan + UBSan) ==="
	@$(BUILD)/test_scaler_zimg
	@echo
	@echo "=== scaler_zimg invariants (ThreadSanitizer) ==="
	@$(BUILD)/test_scaler_zimg_tsan

# Informational line coverage for scaler_zimg.c via the harness. NOT part of
# the gated `coverage` target (which stays VLC-free and requires every tracked
# file and function to reach the configured threshold):
# scaler_zimg.c cannot reach complete coverage in a unit harness —
# log_zimg_open's msg_Info
# needs a live VLC logger object, the partial-construction retry is unreachable
# given zimg_open's stripe clamp, and a couple of zimg-internal failure returns
# need fault injection. The harness reports its coverage as informational.
coverage-zimg:
	@rm -rf $(BUILD)/covz && mkdir -p $(BUILD)/covz
	$(CC) -O0 -g --coverage $(ZIMG_H_CFLAGS) -o $(BUILD)/covz/tz \
	    tests/test_scaler_zimg.c src/scaler_zimg.c --coverage \
	    $(BARRIER_WRAP_LDFLAGS) $(ZIMG_H_LIBS)
	@$(BUILD)/covz/tz >/dev/null 2>&1 || true
	@gcov -m -o $(BUILD)/covz $(BUILD)/covz/tz-scaler_zimg.gcda >/dev/null 2>&1 || true
	@awk -F: 'NF>=2{c=$$1;gsub(/[ \t]/,"",c); if(c!="-"&&c!=""){t++; if(c=="#####"||c=="=====")u++}} \
	    END{printf "scaler_zimg.c: %d/%d lines = %.1f%% (harness; informational)\n", t-u, t, (t-u)*100.0/t}' \
	    scaler_zimg.c.gcov
	@rm -f *.gcov

bench-zimg: $(BUILD)/bench_scaler_zimg
	@echo "threads,chroma,src,dst,frames,zc,us_per_frame"
	@$(BUILD)/bench_scaler_zimg 1  i420 854 480 1920 1080 200 1
	@$(BUILD)/bench_scaler_zimg 2  i420 854 480 1920 1080 200 1
	@$(BUILD)/bench_scaler_zimg 4  i420 854 480 1920 1080 200 1
	@$(BUILD)/bench_scaler_zimg 8  i420 854 480 1920 1080 200 1
	@$(BUILD)/bench_scaler_zimg 16 i420 854 480 1920 1080 200 1
	@$(BUILD)/bench_scaler_zimg 8  i420 640 360 1280 720  200 1
	@$(BUILD)/bench_scaler_zimg 8  i420 854 480 1920 1080 200 0
else
test-zimg stress-zimg bench-zimg coverage-zimg:
	@echo "libzimg not detected (pkg-config zimg); zimg harness unavailable."
endif

# --------- micro-benchmark ---------
# Wall-clock us/frame for the USM pool at -O3 (matches the production
# USM_POOL_CFLAGS optimization level). No sanitizers — this measures
# real throughput. `bench` runs the default kernel; `bench-flatskip`
# builds the SAME source with USM_POOL_FLAT_SKIP=1 so the per-stripe
# flat-detection path is actually compiled and exercised — the call
# site that opt-in feature exists for — and prints a rand-vs-flat
# comparison so the skip's payoff is visible.
BENCH_CFLAGS := -O3 $(MARCH_FLAG) $(WARN)

build-bench: $(BUILD)/bench_usm_pool $(BUILD)/bench_usm_pool_flatskip $(if $(HAVE_ZIMG),$(BUILD)/bench_scaler_zimg)

$(BUILD)/bench_usm_pool: tests/bench_usm_pool.c tests/cli_parse.h src/usm_pool.c src/usm_pool.h src/usm.h | $(BUILD)
	$(CC) $(BENCH_CFLAGS) -o $@ $< src/usm_pool.c -lpthread
$(BUILD)/bench_usm_pool_flatskip: tests/bench_usm_pool.c tests/cli_parse.h src/usm_pool.c src/usm_pool.h src/usm.h | $(BUILD)
	$(CC) $(BENCH_CFLAGS) -DUSM_POOL_FLAT_SKIP=1 -o $@ $< src/usm_pool.c -lpthread

bench: $(BUILD)/bench_usm_pool
	@echo "threads,width,height,frames,amount,fill,us_per_frame"
	@$(BUILD)/bench_usm_pool 1 1920 1080 300 20 rand
	@$(BUILD)/bench_usm_pool 2 1920 1080 300 20 rand
	@$(BUILD)/bench_usm_pool 4 1920 1080 300 20 rand
	@$(BUILD)/bench_usm_pool 8 1920 1080 300 20 rand
	@$(BUILD)/bench_usm_pool 4 1280 720 300 20 rand
	@$(BUILD)/bench_usm_pool 8 3840 2160 100 20 rand

bench-flatskip: $(BUILD)/bench_usm_pool $(BUILD)/bench_usm_pool_flatskip
	@echo "kernel,threads,width,height,frames,amount,fill,us_per_frame"
	@printf "default,"  ; $(BUILD)/bench_usm_pool          8 1920 1080 300 20 flat
	@printf "flatskip," ; $(BUILD)/bench_usm_pool_flatskip 8 1920 1080 300 20 flat
	@printf "default,"  ; $(BUILD)/bench_usm_pool          8 1920 1080 300 20 mixed
	@printf "flatskip," ; $(BUILD)/bench_usm_pool_flatskip 8 1920 1080 300 20 mixed
	@printf "default,"  ; $(BUILD)/bench_usm_pool          8 1920 1080 300 20 rand
	@printf "flatskip," ; $(BUILD)/bench_usm_pool_flatskip 8 1920 1080 300 20 rand

# --------- coverage ---------
# Build the unit tests and deterministic fuzzers with gcov instrumentation,
# run them, then report per-file and per-function line coverage. ASan is
# dropped here because it conflicts with --coverage on some toolchains and we
# already test under ASan elsewhere.
#
# Coverage is meaningful for the testable header/.c surface only. The VLC-
# typed translation units (autoupscale.c, scaler_zimg.c) are NOT covered
# by direct unit tests — their pure logic was extracted into header
# modules (upscale_logic.h, content_probe.h, etc.) precisely so it CAN be
# unit-tested. See `make coverage-summary` for the per-module % numbers.
COV_BUILD := $(BUILD)/cov
# Coverage profiles are compiler-specific; override these as a matched GCC pair.
COV_CC      ?= gcc
GCOV        ?= gcov
COV_CFLAGS  := -O0 -g $(MARCH_FLAG) $(WARN) --coverage -fprofile-arcs -ftest-coverage
COV_LDFLAGS := --coverage

COV_TESTS := \
    $(COV_BUILD)/test_upscale_logic \
    $(COV_BUILD)/test_usm \
    $(COV_BUILD)/test_perfmon \
    $(COV_BUILD)/test_cli_parse \
    $(COV_BUILD)/test_threading \
    $(COV_BUILD)/test_zimg_helpers \
    $(COV_BUILD)/test_chroma_classify \
    $(COV_BUILD)/test_usm_pool \
    $(COV_BUILD)/test_content_probe \
    $(COV_BUILD)/test_scaler_pick \
    $(COV_BUILD)/test_scaler_swscale \
    $(COV_BUILD)/test_picture_view \
    $(COV_BUILD)/test_lifetime

COV_FUZZERS := \
    $(COV_BUILD)/fuzz_upscale_logic \
    $(COV_BUILD)/fuzz_usm \
    $(COV_BUILD)/fuzz_perfmon \
    $(COV_BUILD)/fuzz_threading \
    $(COV_BUILD)/fuzz_copy_plane \
    $(COV_BUILD)/fuzz_stripe_bounds \
    $(COV_BUILD)/fuzz_decide_tile_grid \
    $(COV_BUILD)/fuzz_frame_shape \
    $(COV_BUILD)/fuzz_scaler_chroma \
    $(COV_BUILD)/fuzz_scaler_open \
    $(COV_BUILD)/fuzz_content_probe \
    $(COV_BUILD)/fuzz_picture_view

COV_BINS := $(COV_TESTS) $(COV_FUZZERS)

$(COV_BUILD):
	mkdir -p $(COV_BUILD)

$(COV_BUILD)/test_upscale_logic: tests/test_upscale_logic.c src/upscale_logic.h | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/test_usm: tests/test_usm.c tests/usm_test_util.h src/usm.h | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/test_perfmon: tests/test_perfmon.c src/perfmon.h | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/test_cli_parse: tests/test_cli_parse.c tests/cli_parse.h | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/test_threading: tests/test_threading.c src/threading.h | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/test_zimg_helpers: tests/test_zimg_helpers.c src/zimg_helpers.h | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/test_chroma_classify: tests/test_chroma_classify.c src/chroma_classify.h | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/test_usm_pool: tests/test_usm_pool.c tests/usm_test_util.h tests/barrier_fault_inject.h src/usm_pool.c src/usm_pool.h src/usm.h | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -o $@ $< src/usm_pool.c $(COV_LDFLAGS) \
	    $(BARRIER_WRAP_LDFLAGS) -lpthread
$(COV_BUILD)/test_content_probe: tests/test_content_probe.c src/content_probe.h | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/test_scaler_pick: tests/test_scaler_pick.c src/scaler_pick_logic.h src/scaler_status.h | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/test_scaler_swscale: tests/test_scaler_swscale.c src/scaler_swscale.c src/scaler.h src/scaler_status.h src/picture_view.h tests/stubs/vlc_common.h tests/stubs/vlc_picture.h tests/stubs/libswscale/swscale.h tests/stubs/libavutil/pixfmt.h | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -Itests/stubs -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/test_picture_view: tests/test_picture_view.c src/picture_view.h tests/stubs/vlc_common.h tests/stubs/vlc_picture.h | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -Itests/stubs -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/test_lifetime: tests/test_lifetime.c src/usm_pool.c src/usm_pool.h src/usm.h | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -o $@ $< src/usm_pool.c $(COV_LDFLAGS) -lpthread

$(COV_BUILD)/fuzz_upscale_logic: tests/fuzz_upscale_logic.c src/upscale_logic.h | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -DFUZZ_MAIN -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/fuzz_usm: tests/fuzz_usm.c tests/usm_test_util.h src/usm.h | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -DFUZZ_MAIN -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/fuzz_perfmon: tests/fuzz_perfmon.c tests/cli_parse.h src/perfmon.h | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -DFUZZ_MAIN -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/fuzz_threading: tests/fuzz_threading.c tests/cli_parse.h src/threading.h | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -DFUZZ_MAIN -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/fuzz_copy_plane: tests/fuzz_copy_plane.c tests/cli_parse.h src/zimg_helpers.h | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -DFUZZ_MAIN -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/fuzz_stripe_bounds: tests/fuzz_stripe_bounds.c tests/cli_parse.h src/zimg_helpers.h | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -DFUZZ_MAIN -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/fuzz_decide_tile_grid: tests/fuzz_decide_tile_grid.c src/zimg_helpers.h | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -DFUZZ_MAIN -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/fuzz_frame_shape: tests/fuzz_frame_shape.c src/chroma_classify.h src/zimg_helpers.h | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -DFUZZ_MAIN -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/fuzz_scaler_chroma: tests/fuzz_scaler_chroma.c src/scaler_zimg_chroma.h src/chroma_classify.h | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -DFUZZ_MAIN -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/fuzz_scaler_open: tests/fuzz_scaler_open.c src/scaler_pick_logic.h | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -DFUZZ_MAIN -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/fuzz_content_probe: tests/fuzz_content_probe.c src/content_probe.h | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -DFUZZ_MAIN -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/fuzz_picture_view: tests/fuzz_picture_view.c tests/cli_parse.h src/picture_view.h tests/stubs/vlc_common.h tests/stubs/vlc_picture.h | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -DFUZZ_MAIN -Itests/stubs -o $@ $< $(COV_LDFLAGS)

.PHONY: coverage coverage-summary
coverage: $(COV_BINS)
	@rm -f $(COV_BUILD)/*.gcda
	@set -e; for t in $(COV_BINS); do $$t > /dev/null 2>&1; done
	@# Invoke gcov from the project root so embedded relative source
	@# paths resolve correctly (e.g. "tests/../src/upscale_logic.h").
	@# A header compiled into several test binaries (e.g. usm.h) yields a
	@# usm.h.gcov per binary with the SAME name; emitting them all into one
	@# dir would let the last clobber the rest and lose coverage proved by
	@# another binary. Keep each binary's gcov set in its own subdir and let
	@# coverage_report.sh union them per line.
	@rm -rf $(COV_BUILD)/gcov $(COV_BUILD)/gcov-json
	@mkdir -p $(COV_BUILD)/gcov $(COV_BUILD)/gcov-json
	@set -e; for gcda in $(COV_BUILD)/*.gcda; do \
	    stem=$$(basename "$$gcda" .gcda); \
	    gcda=$$(realpath "$$gcda"); \
	    cov_dir=$(abspath $(COV_BUILD)); \
	    text_dir=$$cov_dir/gcov/$$stem; \
	    json_dir=$$cov_dir/gcov-json/$$stem; \
	    mkdir -p "$$text_dir" "$$json_dir"; \
	    ln -s $(abspath src) "$$text_dir/src"; \
	    ln -s $(abspath tests) "$$text_dir/tests"; \
	    ln -s $(abspath src) "$$json_dir/src"; \
	    ln -s $(abspath tests) "$$json_dir/tests"; \
	    (cd "$$text_dir" && $(GCOV) -r -m -o "$$cov_dir" "$$gcda" > /dev/null); \
	    (cd "$$json_dir" && $(GCOV) -j -r -m -o "$$cov_dir" "$$gcda" > /dev/null); \
	done
	@COV_DIR=$(COV_BUILD) THRESHOLD=80 ./scripts/coverage_report.sh
	@COV_DIR=$(COV_BUILD) THRESHOLD=80 ./scripts/coverage_per_function.sh

coverage-summary: coverage

# --------- static analysis ---------
.PHONY: complexity
# Cyclomatic-complexity gate. lizard exits non-zero if any function has
# CCN > 10 (project policy: every function stays at or below 10).
complexity:
	@command -v lizard >/dev/null 2>&1 || { \
		echo "lizard not installed. pip: lizard"; exit 1; }
	lizard -C 10 src/ tests/

analyze: complexity
	@command -v cppcheck >/dev/null 2>&1 || { \
		echo "cppcheck not installed. apt: cppcheck"; exit 1; }
	# autoupscale.c is excluded — it depends on VLC's macro-heavy headers
	# that cppcheck cannot reasonably parse without a full include path.
	# The interesting logic is all in *_logic.h / usm.h, exercised via tests.
	cppcheck --enable=warning,style,performance,portability \
		--inline-suppr --std=c11 --error-exitcode=2 \
		--suppress=missingIncludeSystem \
		-I src src/upscale_logic.h src/usm.h src/perfmon.h src/threading.h src/zimg_helpers.h src/chroma_classify.h src/scaler_zimg_chroma.h src/content_probe.h src/scaler_pick_logic.h src/usm_pool.h src/usm_pool.c tests/

scan-build:
	@command -v scan-build >/dev/null 2>&1 || { echo "scan-build not found"; exit 1; }
	$(MAKE) clean
	scan-build --status-bugs $(MAKE) plugin

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

# Include dependency files generated by -MMD
-include $(wildcard $(BUILD)/*.d)
$(BUILD)/test_threading: tests/test_threading.c src/threading.h | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_zimg_helpers: tests/test_zimg_helpers.c src/zimg_helpers.h | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_chroma_classify: tests/test_chroma_classify.c src/chroma_classify.h | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_usm_pool: tests/test_usm_pool.c tests/usm_test_util.h tests/barrier_fault_inject.h src/usm_pool.c src/usm_pool.h src/usm.h | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< src/usm_pool.c $(TEST_LDFLAGS) \
	    $(BARRIER_WRAP_LDFLAGS) -lpthread

# Cross-variant byte-equivalence test: links all three SIMD variants and the
# dispatcher's variant_name symbol. Each variant .o is the same usm_pool.c
# compiled at a different -march level. CPU feature gating in the test
# itself skips the AVX2/AVX-512 variants when not supported by the runner.
$(BUILD)/test_usm_pool_sse2.o:   src/usm_pool.c src/usm.h src/usm_pool.h | $(BUILD)
	$(CC) $(TEST_CFLAGS) -march=x86-64    -DUSM_VARIANT=sse2   -c -o $@ $<
$(BUILD)/test_usm_pool_avx2.o:   src/usm_pool.c src/usm.h src/usm_pool.h | $(BUILD)
	$(CC) $(TEST_CFLAGS) -march=x86-64-v3 -DUSM_VARIANT=avx2   -c -o $@ $<
$(BUILD)/test_usm_pool_avx512.o: src/usm_pool.c src/usm.h src/usm_pool.h | $(BUILD)
	$(CC) $(TEST_CFLAGS) -march=x86-64-v4 -DUSM_VARIANT=avx512 -c -o $@ $<
$(BUILD)/test_usm_pool_dispatch.o: src/usm_pool_dispatch.c src/usm_pool.h src/cpu_level.h | $(BUILD)
	$(CC) $(TEST_CFLAGS) -march=x86-64 -c -o $@ $<

$(BUILD)/test_usm_pool_variants: tests/test_usm_pool_variants.c \
    $(BUILD)/test_usm_pool_sse2.o $(BUILD)/test_usm_pool_avx2.o \
    $(BUILD)/test_usm_pool_avx512.o $(BUILD)/test_usm_pool_dispatch.o \
    src/usm.h src/usm_pool.h src/cpu_level.h | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< \
	    $(BUILD)/test_usm_pool_sse2.o $(BUILD)/test_usm_pool_avx2.o \
	    $(BUILD)/test_usm_pool_avx512.o $(BUILD)/test_usm_pool_dispatch.o \
	    $(TEST_LDFLAGS) -lpthread

$(BUILD)/test_content_probe: tests/test_content_probe.c src/content_probe.h | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_scaler_pick: tests/test_scaler_pick.c src/scaler_pick_logic.h src/scaler_status.h | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_scaler_swscale: tests/test_scaler_swscale.c src/scaler_swscale.c src/scaler.h src/scaler_status.h src/picture_view.h tests/stubs/vlc_common.h tests/stubs/vlc_picture.h tests/stubs/libswscale/swscale.h tests/stubs/libavutil/pixfmt.h | $(BUILD)
	$(CC) $(TEST_CFLAGS) -Itests/stubs -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_picture_view: tests/test_picture_view.c src/picture_view.h tests/stubs/vlc_common.h tests/stubs/vlc_picture.h | $(BUILD)
	$(CC) $(TEST_CFLAGS) -Itests/stubs -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_lifetime: tests/test_lifetime.c src/usm_pool.c src/usm_pool.h src/usm.h | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< src/usm_pool.c $(TEST_LDFLAGS) -lpthread
