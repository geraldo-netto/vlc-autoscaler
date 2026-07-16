# Top-level Makefile for VLC AutoUpscale
#
# Targets:
#   make             — build the VLC plugin
#   make plugin      — same
#   make test        — build and run unit tests (no VLC needed)
#   make fuzz-smoke  — build and run deterministic smoke fuzzers
#   make fuzz        — build libFuzzer targets (clang only)
#   make analyze     — run cppcheck across the source tree
#   make scan-build  — Clang-analyze both production plugin configurations
#   make install     — install the built plugin into VLC's plugins dir
#   make uninstall
#   make clean
#   make info        — print discovered toolchain paths

.DEFAULT_GOAL := all

PLUGIN := libautoupscale_plugin

CC      ?= gcc
CLANG   ?= clang
INSTALL ?= install
BUILD   ?= build
ifeq ($(strip $(BUILD)),)
$(error BUILD must be a non-empty dedicated path)
endif
ifneq ($(BUILD),$(strip $(BUILD)))
$(error BUILD must not have surrounding whitespace)
endif
ifneq ($(words $(BUILD)),1)
$(error BUILD must not contain whitespace)
endif
BUILD_CONFIG := $(BUILD)/.build-config
BUILD_MARKER := $(BUILD)/.vlc-autoscaler-build-root

# x86-64 host detection. The runtime SIMD dispatcher (usm_pool_dispatch.c) and
# its unit test are x86-only (the TU #errors on other targets), so gate them on
# this. Empty string on non-x86.
IS_X86 := $(findstring x86_64,$(shell $(CC) -dumpmachine 2>/dev/null))

define compiler_supports_flag
$(shell printf 'int vlc_autoscaler_flag_probe;\n' | \
	$(1) $(2) -Werror -x c -c -o /dev/null - >/dev/null 2>&1 && printf 1)
endef

CC_SUPPORTS_X86_64_LEVELS = $(and \
    $(call compiler_supports_flag,$(CC),-march=x86-64-v3), \
    $(call compiler_supports_flag,$(CC),-march=x86-64-v4))
CLANG_SUPPORTS_X86_64_LEVELS = $(and \
    $(call compiler_supports_flag,$(CLANG),-march=x86-64-v3), \
    $(call compiler_supports_flag,$(CLANG),-march=x86-64-v4))

REQUESTED_GOALS := $(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)
CC_LEVEL_GOALS := test check fuzz-smoke
NEED_CC_X86_64_LEVELS = $(or $(filter 1,$(MULTIVERSION)), \
    $(filter $(CC_LEVEL_GOALS),$(REQUESTED_GOALS)))
NEED_CLANG_X86_64_LEVELS := $(filter fuzz,$(REQUESTED_GOALS))

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
ifneq ($(strip $(VLC_PLUGIN_BASE)),)
  VLC_PLUGIN_DIR := $(VLC_PLUGIN_BASE)/video_filter
else
  VLC_PLUGIN_DIR :=
endif

# Fail fast, at parse time, when a plugin build is requested without the
# required SDKs — otherwise the first object compile dies on a cryptic
# "vlc_common.h: No such file" long before any friendly message.
PLUGIN_GOALS := all plugin install scan-build abi-layout-check \
                check-visibility check-multiversion-isa check-hardening \
                $(BUILD)/$(PLUGIN).so
ifneq ($(filter $(PLUGIN_GOALS),$(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)),)
  ifeq ($(strip $(VLC_LIBS)),)
    $(error vlc-plugin pkg-config not found. Install libvlccore-dev (Debian/Ubuntu) or vlc-devel (Fedora))
  endif
  ifeq ($(strip $(SWS_LIBS)),)
    $(error libswscale pkg-config not found. Install libswscale-dev / libavutil-dev)
  endif
endif

# --------- common flags ---------
# PORT-1: pin the language dialect on every compile so the built code is
# checked against the same C11 the static analyzer uses (cppcheck --std=c11),
# not the compiler's version-dependent default GNU dialect. `-D_GNU_SOURCE`
# (below) keeps the POSIX/GNU library surface the pthread/affinity/aligned_alloc
# code relies on. `$(WARN)` is referenced by every *_CFLAGS, so this one line
# covers plugin, test, fuzz, smoke, stress, bench, coverage, and zimg builds.
CSTD := -std=c11
WARN := $(CSTD) -D_GNU_SOURCE -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes

# Threaded coverage binaries update their counters concurrently. Atomic
# profile updates prevent lost/corrupt increments; both coverage modes share
# this exact instrumentation set.
COVERAGE_PROFILE_FLAGS := --coverage -fprofile-arcs -ftest-coverage \
                          -fprofile-update=atomic

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
# runtime. The rest of the plugin still uses MARCH, so its baseline must be
# supported by every deployment CPU.
#
# Decoded MD5 is byte-identical across all SIMD widths because the
# kernels do bytewise saturating arithmetic; SIMD just runs more lanes
# in parallel.
MARCH        ?= native
ifneq ($(MARCH),)
  MARCH_FLAG := -march=$(MARCH)
endif

COMMON_CFLAGS := -O2 $(MARCH_FLAG) -fPIC -DPIC $(WARN) -MMD -MP -fstack-protector-strong -D_FORTIFY_SOURCE=2 -flto $(EXTRA_CFLAGS)
# ABI-1: hide internal symbols (up_*, scaler_*) so generic names cannot
# collide in embedders loading plugins with RTLD_GLOBAL. VLC's plugin
# macros mark the vlc_entry* points visibility("default") themselves;
# `make check-visibility` asserts nothing else leaks.
PLUGIN_CFLAGS := $(COMMON_CFLAGS) -fvisibility=hidden \
                 -DMODULE_STRING=\"autoupscale\" \
                 -D__PLUGIN__ $(VLC_CFLAGS) $(SWS_CFLAGS)
# ABI-2: the LTO link is where cross-TU diagnostics are raised
# (-Wlto-type-mismatch, and -Wstringop-overflow / -Warray-bounds arising from
# cross-TU inlining). gcc reports them and still exits 0, so a link that
# carries no warning flags cannot fail on them — CI's `EXTRA_CFLAGS=-Werror`
# only ever reached the compile step. Pass $(WARN) and $(EXTRA_CFLAGS) to the
# link too, so a prototype/type mismatch between TUs is a build failure.
PLUGIN_LDFLAGS := -shared -Wl,-z,defs,-z,relro,-z,now -flto $(WARN) $(EXTRA_CFLAGS) $(EXTRA_LDFLAGS)
PLUGIN_LIBS    := $(VLC_LIBS) $(SWS_LIBS) -lpthread

ifdef HAVE_ZIMG
  PLUGIN_CFLAGS += -DHAVE_ZIMG $(ZIMG_CFLAGS)
  PLUGIN_LIBS   += $(ZIMG_LIBS)
endif

TEST_CFLAGS  := -O2 -g $(MARCH_FLAG) $(WARN) -MMD -MP -fsanitize=address,undefined $(EXTRA_CFLAGS)
TEST_LDFLAGS := -fsanitize=address,undefined
# The completion barrier uses a dedicated monotonic condition variable.
BARRIER_WRAP_LDFLAGS := -Wl,--wrap=pthread_cond_timedwait \
	-Wl,--wrap=pthread_cond_signal -Wl,--wrap=pthread_mutex_lock \
	-Wl,--wrap=pthread_cond_broadcast
# test_threading fault-injects condition initialization, thread creation, and
# deadline clocks.
THREADING_WRAP_LDFLAGS := -Wl,--wrap=pthread_cond_init \
	-Wl,--wrap=pthread_create -Wl,--wrap=clock_gettime \
	$(BARRIER_WRAP_LDFLAGS)
USM_POOL_WRAP_LDFLAGS := $(BARRIER_WRAP_LDFLAGS) -Wl,--wrap=aligned_alloc \
	-Wl,--wrap=pthread_create
# worker_pool fault injection and owned thread-lifecycle accounting.
WORKER_POOL_WRAP_LDFLAGS := $(BARRIER_WRAP_LDFLAGS) -Wl,--wrap=aligned_alloc \
	-Wl,--wrap=pthread_create -Wl,--wrap=pthread_join \
	-Wl,--wrap=pthread_setcancelstate
WORKER_POOL_FUZZ_WRAP_LDFLAGS := -Wl,--wrap=pthread_create
# CONC-1: the lost-wake regression waits out the barrier deadline, so shrink it
# from the shipped 10 s to something a test suite can afford.
WORKER_POOL_TEST_CFLAGS := -DUP_POOL_BARRIER_TIMEOUT_MS=200
THREADING_TEST_CFLAGS := -DUP_POOL_BARRIER_TIMEOUT_MS=200

FUZZ_SAN     := -fsanitize=fuzzer,address,undefined
FUZZ_CFLAGS  := -O1 -g $(MARCH_FLAG) $(WARN) -MMD -MP $(FUZZ_SAN) $(EXTRA_CFLAGS)

SMOKE_CFLAGS := -O2 -g $(MARCH_FLAG) $(WARN) -MMD -MP -fsanitize=address,undefined -DFUZZ_MAIN $(EXTRA_CFLAGS)
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
# runtime dispatcher (usm_pool_dispatch.c) that selects a variant at .so load
# time through complete x86-64-v3/v4 runtime probes. Pair this with an
# appropriate MARCH baseline (e.g. x86-64-v3 or x86-64) so the rest of the
# plugin uses the intended baseline:
#
#   make MARCH=x86-64-v3 MULTIVERSION=1   # compatible with Haswell/Zen 1+,
#                                          # selects a USM variant at load
#   make MARCH=x86-64    MULTIVERSION=1   # original x86-64/SSE2 baseline
MULTIVERSION ?= 0

# The SIMD variants are hardcoded at -march=x86-64-v3/v4; on any other
# architecture the build would only die deep into the compile (or at the
# usm_pool_dispatch.c #error). Check the compiler's target up front.
ifeq ($(MULTIVERSION),1)
  CC_TARGET := $(shell $(CC) -dumpmachine 2>/dev/null)
  ifeq ($(findstring x86_64,$(CC_TARGET)),)
    $(error MULTIVERSION=1 requires an x86-64 compiler target ($(CC) -dumpmachine says '$(CC_TARGET)'); build with MULTIVERSION=0 instead)
  endif
endif

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
$(BUILD)/usm_pool_sse2.o: src/usm_pool.c src/usm.h src/usm_pool.h src/usm_pool_variants.h $(BUILD_CONFIG) | $(BUILD) check-cc-x86-level-flags
	$(CC) $(USM_POOL_CFLAGS) -march=x86-64    -DUSM_VARIANT=sse2   -c -o $@ $<
$(BUILD)/usm_pool_avx2.o: src/usm_pool.c src/usm.h src/usm_pool.h src/usm_pool_variants.h $(BUILD_CONFIG) | $(BUILD) check-cc-x86-level-flags
	$(CC) $(USM_POOL_CFLAGS) -march=x86-64-v3 -DUSM_VARIANT=avx2   -c -o $@ $<
$(BUILD)/usm_pool_avx512.o: src/usm_pool.c src/usm.h src/usm_pool.h src/usm_pool_variants.h $(BUILD_CONFIG) | $(BUILD) check-cc-x86-level-flags
	$(CC) $(USM_POOL_CFLAGS) -march=x86-64-v4 -DUSM_VARIANT=avx512 -c -o $@ $<

# Dispatcher must be at the lowest baseline so it runs on ANY CPU. It just
# does CPU-feature checks and indirect calls — no SIMD work itself.
$(BUILD)/usm_pool_dispatch.o: src/usm_pool_dispatch.c src/usm_pool.h src/usm_pool_variants.h src/cpu_level.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(PLUGIN_CFLAGS) -march=x86-64 -c -o $@ $<
else
USM_OBJS := $(BUILD)/usm_pool.o
# Single-baseline build: same -O3 reasoning as the multi-versioned case.
USM_POOL_CFLAGS := $(subst -O2,-O3,$(PLUGIN_CFLAGS))
$(BUILD)/usm_pool.o: src/usm_pool.c src/usm.h src/usm_pool.h src/usm_pool_variants.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(USM_POOL_CFLAGS) -c -o $@ $<
endif

PLUGIN_OBJS += $(USM_OBJS)

# Every artifact rule names this content-stable manifest as a normal
# prerequisite. GNU make does not otherwise notice command-line variable
# changes, so reusing a build directory after changing compilers, ISA, optional
# features, or flags could silently retain incompatible objects.
.PHONY: FORCE
FORCE:

$(BUILD_CONFIG): FORCE | $(BUILD_MARKER)
	@tmp="$@.tmp"; \
	{ \
	    printf '%s\n' \
	        "MAKEFILE_CKSUM=$$(cksum Makefile)" \
	        "CC=$(CC)" \
	        "CLANG=$(CLANG)" \
	        "MARCH=$(MARCH)" \
	        "MULTIVERSION=$(MULTIVERSION)" \
	        "HAVE_ZIMG=$(HAVE_ZIMG)" \
	        "VLC_CFLAGS=$(VLC_CFLAGS)" \
	        "VLC_LIBS=$(VLC_LIBS)" \
	        "SWS_CFLAGS=$(SWS_CFLAGS)" \
	        "SWS_LIBS=$(SWS_LIBS)" \
	        "ZIMG_CFLAGS=$(ZIMG_CFLAGS)" \
	        "ZIMG_LIBS=$(ZIMG_LIBS)" \
	        "COMMON_CFLAGS=$(COMMON_CFLAGS)" \
	        "PLUGIN_CFLAGS=$(PLUGIN_CFLAGS)" \
	        "PLUGIN_LDFLAGS=$(PLUGIN_LDFLAGS)" \
	        "PLUGIN_LIBS=$(PLUGIN_LIBS)" \
	        "TEST_CFLAGS=$(TEST_CFLAGS)" \
	        "TEST_LDFLAGS=$(TEST_LDFLAGS)" \
	        "BARRIER_WRAP_LDFLAGS=$(BARRIER_WRAP_LDFLAGS)" \
	        "THREADING_WRAP_LDFLAGS=$(THREADING_WRAP_LDFLAGS)" \
	        "USM_POOL_WRAP_LDFLAGS=$(USM_POOL_WRAP_LDFLAGS)" \
	        "WORKER_POOL_WRAP_LDFLAGS=$(WORKER_POOL_WRAP_LDFLAGS)" \
	        "WORKER_POOL_FUZZ_WRAP_LDFLAGS=$(WORKER_POOL_FUZZ_WRAP_LDFLAGS)" \
	        "WORKER_POOL_TEST_CFLAGS=$(WORKER_POOL_TEST_CFLAGS)" \
	        "THREADING_TEST_CFLAGS=$(THREADING_TEST_CFLAGS)" \
	        "FUZZ_CFLAGS=$(FUZZ_CFLAGS)" \
	        "SMOKE_CFLAGS=$(SMOKE_CFLAGS)" \
	        "SMOKE_LDFLAGS=$(SMOKE_LDFLAGS)" \
	        "STRESS_CFLAGS_ASAN=$(STRESS_CFLAGS_ASAN)" \
	        "STRESS_CFLAGS_TSAN=$(STRESS_CFLAGS_TSAN)" \
	        "STRESS_LDFLAGS_ASAN=$(STRESS_LDFLAGS_ASAN)" \
	        "STRESS_LDFLAGS_TSAN=$(STRESS_LDFLAGS_TSAN)" \
	        "ZIMG_H_CFLAGS=$(ZIMG_H_CFLAGS)" \
	        "ZIMG_H_LIBS=$(ZIMG_H_LIBS)" \
	        "ZIMG_TEST_WRAP_LDFLAGS=$(ZIMG_TEST_WRAP_LDFLAGS)" \
	        "BENCH_CFLAGS=$(BENCH_CFLAGS)" \
	        "COV_CC=$(COV_CC)" \
	        "COV_CFLAGS=$(COV_CFLAGS)" \
	        "COV_LDFLAGS=$(COV_LDFLAGS)" \
	        "EXTRA_CFLAGS=$(EXTRA_CFLAGS)" \
	        "EXTRA_LDFLAGS=$(EXTRA_LDFLAGS)"; \
	} > "$$tmp"; \
	if [ -r "$@" ] && cmp -s "$@" "$$tmp"; then \
	    rm -f "$$tmp"; \
	else \
	    mv -f "$$tmp" "$@"; \
	fi

.PHONY: check-cc-x86-level-flags check-clang-x86-level-flags
check-cc-x86-level-flags:
	@if [ "$(CC_SUPPORTS_X86_64_LEVELS)" != "1" ]; then \
	    echo "$(CC) must support -march=x86-64-v3 and -march=x86-64-v4 (GCC 11+, Clang 12+, or equivalent)" >&2; \
	    exit 2; \
	fi

check-clang-x86-level-flags:
	@if [ "$(CLANG_SUPPORTS_X86_64_LEVELS)" != "1" ]; then \
	    echo "$(CLANG) must support -march=x86-64-v3 and -march=x86-64-v4 (Clang 12+ or equivalent)" >&2; \
	    exit 2; \
	fi

ifneq ($(NEED_CC_X86_64_LEVELS),)
$(BUILD_CONFIG): | check-cc-x86-level-flags
endif
ifneq ($(NEED_CLANG_X86_64_LEVELS),)
$(BUILD_CONFIG): | check-clang-x86-level-flags
endif

# ABI-1: prove each SIMD variant in the shipped LTO-linked plugin actually
# emits its target ISA. The cross-variant test asserts byte-identical *output*;
# that deliberately masks an accidental collapse to the baseline caused by a
# vectorization-disabling pragma, broken -march mapping, or compiler regression
# — three identical SSE2 kernels
# would still pass it. usm_pool_run_worker_<variant> is a retained noinline
# anchor around the hot sweep: SSE2 has no VEX/EVEX instructions, v3 emits YMM
# through VEX but no EVEX, and v4 emits EVEX-encoded AVX-512. The load-time
# selector constructor must also remain free of AVX vector instructions before
# feature selection runs. This gate does not classify unrelated scalar ISA.
.PHONY: check-multiversion-isa
ifeq ($(MULTIVERSION),1)
check-multiversion-isa: $(BUILD)/$(PLUGIN).so
	@set -e; so=$<; fail=0; \
	 symbols=$$(nm -S -P --defined-only "$$so"); \
	 for spec in \
	   "usm_pool_run_worker_sse2:baseline" \
	   "usm_pool_run_worker_avx2:avx2" \
	   "usm_pool_run_worker_avx512:avx512" \
	   "up_usm_pool_dispatch_init:baseline"; do \
	    sym=$${spec%%:*}; want=$${spec##*:}; \
	    matches=$$(printf '%s\n' "$$symbols" | awk -v name="$$sym" '$$1 == name { n++ } END { print n + 0 }'); \
	    if [ "$$matches" -ne 1 ]; then \
	        echo "  [FAIL] linked ISA anchor $$sym occurs $$matches times"; fail=1; continue; \
	    fi; \
	    entry=$$(printf '%s\n' "$$symbols" | awk -v name="$$sym" '$$1 == name { print; exit }'); \
	    set -- $$entry; type=$$2; size=$$4; \
	    if { [ "$$type" != t ] && [ "$$type" != T ]; } || [ $$((0x$$size)) -eq 0 ]; then \
	        echo "  [FAIL] linked ISA anchor $$sym has type=$$type size=$$size"; fail=1; continue; \
	    fi; \
	    body=$$(objdump -d --disassemble="$$sym" "$$so"); \
	    ymm=$$(printf '%s\n' "$$body" | grep -c ymm || true); \
	    zmm=$$(printf '%s\n' "$$body" | grep -c zmm || true); \
	    vex=$$(printf '%s\n' "$$body" | awk '$$2 == "c4" || $$2 == "c5" { n++ } END { print n + 0 }'); \
	    evex=$$(printf '%s\n' "$$body" | awk '$$2 == "62" { n++ } END { print n + 0 }'); \
	    case $$want in \
	      baseline) if [ "$$vex" -ne 0 ] || [ "$$evex" -ne 0 ] || [ "$$ymm" -ne 0 ] || [ "$$zmm" -ne 0 ]; then echo "  [FAIL] $$sym: vex=$$vex evex=$$evex ymm=$$ymm zmm=$$zmm, expected no AVX"; fail=1; else echo "  [ok] $$sym: no AVX/EVEX"; fi ;; \
	      avx2) if [ "$$vex" -eq 0 ] || [ "$$ymm" -eq 0 ] || [ "$$zmm" -ne 0 ] || [ "$$evex" -ne 0 ]; then echo "  [FAIL] $$sym: vex=$$vex evex=$$evex ymm=$$ymm zmm=$$zmm, expected AVX2"; fail=1; else echo "  [ok] $$sym: AVX2 ($$vex VEX, $$ymm YMM ops)"; fi ;; \
	      avx512) if [ "$$evex" -eq 0 ]; then echo "  [FAIL] $$sym: no EVEX instructions, expected AVX-512"; fail=1; else echo "  [ok] $$sym: AVX-512 ($$evex EVEX ops)"; fi ;; \
	    esac; \
	 done; \
	 if [ "$$fail" -ne 0 ]; then echo "  linked multiversion ISA check FAILED"; exit 1; fi; \
	 echo "  linked multiversion ISA check passed"
else
check-multiversion-isa:
	@echo "check-multiversion-isa requires MULTIVERSION=1" >&2
	@exit 2
endif

.PHONY: all plugin abi-layout-check check-hardening check-multiversion-isa test check check-visibility fuzz fuzz-smoke fuzz-seam analyze semantic-analysis scan-build build-bench install uninstall clean info bench bench-flatskip test-zimg stress stress-zimg bench-zimg coverage-zimg

all: plugin

# --------- plugin ---------
plugin: $(BUILD)/$(PLUGIN).so abi-layout-check
	@chmod 0644 $<

# ABI-1: the unit/coverage tests compile production sources (scaler_swscale.c,
# picture_view.h) against the hand-written layouts in tests/stubs/. abi_assert.c
# static-asserts those field names/types against the REAL VLC headers (no
# -Itests/stubs on the path), so an upstream rename/retype fails the build
# instead of silently diverging the stub-built tests from the shipped .so.
# Syntax-only — emits no object; runs on every plugin build (VLC headers are
# already required there).
.PHONY: abi-layout-check
abi-layout-check:
	@$(CC) $(WARN) $(EXTRA_CFLAGS) -fsyntax-only $(VLC_CFLAGS) \
	    -D__PLUGIN__ -DMODULE_STRING=\"autoupscale\" tests/abi_assert.c
	@echo "  [ok] tests/stubs layout matches real VLC headers"

$(BUILD)/$(PLUGIN).so: $(PLUGIN_OBJS) $(BUILD_CONFIG) | $(BUILD)
	@echo "  CPU baseline:    -march=$(MARCH)$(if $(filter native,$(MARCH)), (build-host ISA),$(if $(filter x86-64-v4,$(MARCH)), (Intel Skylake-X 2017+ / AMD Zen 4 2022+),$(if $(filter x86-64-v3,$(MARCH)), (Intel Haswell 2013+ / AMD Zen 1 2017+),$(if $(filter x86-64,$(MARCH)), (Linux x86-64 / SSE2 baseline),))))"
	@if [ "$(MULTIVERSION)" = "1" ]; then \
	    echo "  USM SIMD:        multi-versioned (SSE2 + AVX2 + AVX-512, runtime dispatch)"; \
	 else \
	    echo "  USM SIMD:        single-baseline at -march=$(MARCH) (no runtime dispatcher)"; \
	 fi
	@if [ -n "$(HAVE_ZIMG)" ]; then echo "  zimg backend:    ENABLED"; \
	 else echo "  zimg backend:    disabled (libzimg-dev not found)"; fi
	$(CC) $(PLUGIN_LDFLAGS) -o $@ $(PLUGIN_OBJS) $(PLUGIN_LIBS)
	chmod 0644 $@

$(BUILD)/%.o: src/%.c $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(PLUGIN_CFLAGS) -c -o $@ $<

# ABI-1 regression gate: the .so must export VLC's vlc_entry* plugin entry
# points — and nothing else. Any other defined dynamic symbol is a leak of an
# internal name into embedders' global namespaces.
#
# Both halves matter. Asserting only "nothing extra" passes a .so that exports
# NOTHING AT ALL: an empty symbol list has no unexpected names in it, so a
# visibility regression that hid vlc_entry itself would go green here while
# VLC's loader silently skipped the plugin (it would never appear in
# `vlc --list`). Assert the entry points exist first.
check-visibility: $(BUILD)/$(PLUGIN).so
	@exported=$$(nm -D --defined-only $< | awk '$$2 != "U" {print $$NF}'); \
	 entries=$$(echo "$$exported" | grep -c '^vlc_entry' || true); \
	 if [ "$$entries" -eq 0 ]; then \
	     echo "check-visibility FAILED — no vlc_entry* symbol is exported;"; \
	     echo "  VLC's loader would silently skip this plugin."; exit 1; \
	 fi; \
	 bad=$$(echo "$$exported" | grep -v '^vlc_entry' || true); \
	 if [ -n "$$bad" ]; then \
	     echo "check-visibility FAILED — unexpected exported symbols:"; \
	     echo "$$bad"; exit 1; \
	 fi; \
	 echo "check-visibility OK: $$entries vlc_entry* symbol(s) exported, nothing else"

check-hardening: $(BUILD)/$(PLUGIN).so
	@readelf -lW $< | grep -q 'GNU_RELRO' || { \
	    echo "check-hardening FAILED: GNU_RELRO segment missing"; exit 1; }
	@readelf -dW $< | grep -q 'BIND_NOW' || { \
	    echo "check-hardening FAILED: BIND_NOW dynamic flag missing"; exit 1; }
	@echo "check-hardening OK: RELRO + immediate binding enabled"

# --------- unit tests ---------
# `test` runs the suites only; `check` adds the lizard complexity gate.
# CI runs `check` so CCN regressions still fail there, while `make test`
# works on machines without lizard installed.
check: complexity test

test: $(BUILD)/test_upscale_logic $(BUILD)/test_geometry_edge_cases $(BUILD)/test_usm $(BUILD)/test_cli_parse $(BUILD)/test_threading $(BUILD)/test_threading_noaffinity $(BUILD)/test_worker_pool $(BUILD)/test_zimg_helpers $(BUILD)/test_chroma_classify $(BUILD)/test_usm_pool $(BUILD)/test_content_probe $(BUILD)/test_scaler_pick $(BUILD)/test_scaler_swscale $(BUILD)/test_autoupscale_lifecycle $(BUILD)/test_picture_view $(BUILD)/test_lifetime $(BUILD)/test_usm_pool_variants $(if $(IS_X86),$(BUILD)/test_usm_pool_dispatch $(BUILD)/test_usm_pool_dispatch_fallback)
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
	@echo "=== cli_parse ==="
	@$(BUILD)/test_cli_parse
	@echo
	@echo "=== threading ==="
	@$(BUILD)/test_threading
	@echo
	@echo "=== threading (no-affinity fallback) ==="
	@$(BUILD)/test_threading_noaffinity
	@echo
	@echo "=== zimg_helpers ==="
	@$(BUILD)/test_zimg_helpers
	@echo
	@echo "=== chroma_classify ==="
	@$(BUILD)/test_chroma_classify
	@echo
	@echo "=== worker_pool (shared lifecycle) ==="
	@$(BUILD)/test_worker_pool
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
	@echo "=== autoupscale lifecycle ==="
	@$(BUILD)/test_autoupscale_lifecycle
	@echo
	@echo "=== picture_view ==="
	@$(BUILD)/test_picture_view
	@echo
	@echo "=== lifetime / UAF ==="
	@$(BUILD)/test_lifetime
	@echo
	@echo "=== usm_pool_variants (cross-SIMD byte-equivalence) ==="
	@$(BUILD)/test_usm_pool_variants
	@echo
	@set -e; if [ -n "$(IS_X86)" ]; then \
	    echo "=== usm_pool_dispatch (runtime SIMD selection) ==="; \
	    $(BUILD)/test_usm_pool_dispatch; \
	    echo "=== usm_pool_dispatch (forced CPUID fallback) ==="; \
	    $(BUILD)/test_usm_pool_dispatch_fallback; \
	 else echo "=== usm_pool_dispatch (skipped: non-x86 host) ==="; fi
	@echo
	@echo "=== install action ==="
	@sh tests/test_install_action.sh
	@echo
	@echo "=== plugin install paths ==="
	@sh tests/test_plugin_install.sh
	@echo
	@echo "=== safe cleanup roots ==="
	@sh tests/test_safe_rm_tree.sh
	@echo
	@echo "=== benchmark matrix parser ==="
	@sh tests/test_bench_matrix.sh
	@echo
	@echo "=== USM benchmark scripts ==="
	@sh tests/test_bench_usm_scripts.sh
	@echo
	@echo "=== coverage parsers ==="
	@sh tests/test_coverage_parsers.sh

$(BUILD)/test_usm_pool_dispatch: tests/test_usm_pool_dispatch.c src/usm_pool_dispatch.c src/usm_pool_variants.h src/usm_pool.h src/cpu_level.h tests/test_harness.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_usm_pool_dispatch_fallback: tests/test_usm_pool_dispatch.c src/usm_pool_dispatch.c src/usm_pool_variants.h src/usm_pool.h src/cpu_level.h tests/test_harness.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(TEST_CFLAGS) -march=x86-64 -DUP_CPU_LEVEL_FORCE_FALLBACK=1 \
	    -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_upscale_logic: tests/test_upscale_logic.c src/upscale_logic.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_geometry_edge_cases: tests/test_geometry_edge_cases.c src/upscale_logic.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_autoupscale_lifecycle: tests/test_autoupscale_lifecycle.c src/autoupscale.c src/scaler.h src/usm_pool.h tests/test_harness.h tests/lifecycle_stubs/vlc_common.h tests/lifecycle_stubs/vlc_filter.h tests/lifecycle_stubs/vlc_picture.h tests/lifecycle_stubs/vlc_plugin.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(TEST_CFLAGS) -march=x86-64 -Itests/lifecycle_stubs -Itests/stubs -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_usm: tests/test_usm.c tests/usm_test_util.h src/usm.h src/threading.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_cli_parse: tests/test_cli_parse.c tests/cli_parse.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS)

# --------- libFuzzer (clang) ---------
FUZZ_TARGET_NAMES := upscale_logic usm threading worker_pool copy_plane \
                     stripe_bounds decide_tile_grid frame_shape scaler_chroma \
                     scaler_open content_probe picture_view usm_variants \
                     $(if $(HAVE_ZIMG),scaler_seam)
FUZZ_TARGETS := $(addprefix $(BUILD)/fuzz_,$(FUZZ_TARGET_NAMES))

fuzz: $(FUZZ_TARGETS)
	@echo "Built libFuzzer targets:"
	@for target in $(FUZZ_TARGETS); do echo "  $$target"; done
	@echo ""
	@echo "Run from random bytes:    $(BUILD)/fuzz_upscale_logic -max_total_time=60"
	@echo "Run with seed corpus:     mkdir -p fuzz_corpus &&"
	@echo "                          cp tests/corpus_usm_variants/* fuzz_corpus/ &&"
	@echo "                          $(BUILD)/fuzz_usm_variants fuzz_corpus -max_total_time=60"
	@echo ""
	@echo "(Always copy seeds to a working dir; libFuzzer writes new finds back"
	@echo " into whatever directory you pass it, polluting the curated corpus.)"

$(BUILD)/fuzz_upscale_logic: tests/fuzz_upscale_logic.c src/upscale_logic.h $(BUILD_CONFIG) | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_usm: tests/fuzz_usm.c tests/usm_test_util.h src/usm.h src/threading.h $(BUILD_CONFIG) | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_threading: tests/fuzz_threading.c tests/cli_parse.h src/threading.h $(BUILD_CONFIG) | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_worker_pool: tests/fuzz_worker_pool.c src/worker_pool.h src/threading.h $(BUILD_CONFIG) | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $< $(WORKER_POOL_FUZZ_WRAP_LDFLAGS) -lpthread

$(BUILD)/fuzz_copy_plane: tests/fuzz_copy_plane.c tests/cli_parse.h src/plane_utils.h $(BUILD_CONFIG) | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_stripe_bounds: tests/fuzz_stripe_bounds.c tests/cli_parse.h src/plane_utils.h $(BUILD_CONFIG) | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_decide_tile_grid: tests/fuzz_decide_tile_grid.c src/zimg_helpers.h $(BUILD_CONFIG) | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_frame_shape: tests/fuzz_frame_shape.c src/chroma_classify.h src/zimg_helpers.h $(BUILD_CONFIG) | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_scaler_chroma: tests/fuzz_scaler_chroma.c src/scaler_zimg_chroma.h src/chroma_classify.h $(BUILD_CONFIG) | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_scaler_open: tests/fuzz_scaler_open.c src/scaler_pick_logic.h $(BUILD_CONFIG) | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_content_probe: tests/fuzz_content_probe.c tests/prng.h src/content_probe.h $(BUILD_CONFIG) | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $<

$(BUILD)/fuzz_picture_view: tests/fuzz_picture_view.c tests/cli_parse.h src/picture_view.h src/chroma_classify.h tests/stubs/vlc_common.h tests/stubs/vlc_picture.h $(BUILD_CONFIG) | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -Itests/stubs -o $@ $<

# libFuzzer variant fuzzer: needs the three SIMD .o files compiled with the
# same FUZZ_CFLAGS (libFuzzer + ASan + UBSan). Each variant TU is at its
# own -march level via -DUSM_VARIANT=<name>.
$(BUILD)/fuzz_lf_usm_pool_sse2.o:   src/usm_pool.c src/usm.h src/usm_pool.h src/usm_pool_variants.h $(BUILD_CONFIG) | $(BUILD) check-clang-x86-level-flags
	$(CLANG) $(FUZZ_CFLAGS) -march=x86-64    -DUSM_VARIANT=sse2   -c -o $@ $<
$(BUILD)/fuzz_lf_usm_pool_avx2.o:   src/usm_pool.c src/usm.h src/usm_pool.h src/usm_pool_variants.h $(BUILD_CONFIG) | $(BUILD) check-clang-x86-level-flags
	$(CLANG) $(FUZZ_CFLAGS) -march=x86-64-v3 -DUSM_VARIANT=avx2   -c -o $@ $<
$(BUILD)/fuzz_lf_usm_pool_avx512.o: src/usm_pool.c src/usm.h src/usm_pool.h src/usm_pool_variants.h $(BUILD_CONFIG) | $(BUILD) check-clang-x86-level-flags
	$(CLANG) $(FUZZ_CFLAGS) -march=x86-64-v4 -DUSM_VARIANT=avx512 -c -o $@ $<

$(BUILD)/fuzz_usm_variants: tests/fuzz_usm_variants.c \
    $(BUILD)/fuzz_lf_usm_pool_sse2.o $(BUILD)/fuzz_lf_usm_pool_avx2.o \
    $(BUILD)/fuzz_lf_usm_pool_avx512.o \
    tests/prng.h src/usm_pool.h $(BUILD_CONFIG) | $(BUILD)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $< \
	    $(BUILD)/fuzz_lf_usm_pool_sse2.o $(BUILD)/fuzz_lf_usm_pool_avx2.o \
	    $(BUILD)/fuzz_lf_usm_pool_avx512.o \
	    -lpthread

# --------- smoke fuzz (no libFuzzer needed) ---------
fuzz-smoke: $(BUILD)/fuzz_smoke $(BUILD)/fuzz_usm_smoke $(BUILD)/fuzz_threading_smoke $(BUILD)/fuzz_worker_pool_smoke $(BUILD)/fuzz_copy_plane_smoke $(BUILD)/fuzz_stripe_bounds_smoke $(BUILD)/fuzz_decide_tile_grid_smoke $(BUILD)/fuzz_frame_shape_smoke $(BUILD)/fuzz_scaler_chroma_smoke $(BUILD)/fuzz_scaler_open_smoke $(BUILD)/fuzz_content_probe_smoke $(BUILD)/fuzz_picture_view_smoke $(BUILD)/fuzz_usm_variants_smoke
	@echo
	@echo "=== upscale_logic ==="
	@$(BUILD)/fuzz_smoke
	@echo
	@echo "=== usm ==="
	@$(BUILD)/fuzz_usm_smoke
	@echo
	@echo "=== threading ==="
	@$(BUILD)/fuzz_threading_smoke
	@echo
	@echo "=== worker_pool ==="
	@$(BUILD)/fuzz_worker_pool_smoke
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

$(BUILD)/fuzz_smoke: tests/fuzz_upscale_logic.c src/upscale_logic.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_usm_smoke: tests/fuzz_usm.c tests/usm_test_util.h src/usm.h src/threading.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_threading_smoke: tests/fuzz_threading.c tests/cli_parse.h src/threading.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_worker_pool_smoke: tests/fuzz_worker_pool.c src/worker_pool.h src/threading.h tests/fuzz_smoke.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS) \
	    $(WORKER_POOL_FUZZ_WRAP_LDFLAGS) -lpthread

$(BUILD)/fuzz_copy_plane_smoke: tests/fuzz_copy_plane.c tests/cli_parse.h src/plane_utils.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_stripe_bounds_smoke: tests/fuzz_stripe_bounds.c tests/cli_parse.h src/plane_utils.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_decide_tile_grid_smoke: tests/fuzz_decide_tile_grid.c src/zimg_helpers.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_frame_shape_smoke: tests/fuzz_frame_shape.c src/chroma_classify.h src/zimg_helpers.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_scaler_chroma_smoke: tests/fuzz_scaler_chroma.c src/scaler_zimg_chroma.h src/chroma_classify.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_scaler_open_smoke: tests/fuzz_scaler_open.c src/scaler_pick_logic.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_content_probe_smoke: tests/fuzz_content_probe.c tests/prng.h src/content_probe.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(SMOKE_CFLAGS) -o $@ $< $(SMOKE_LDFLAGS)

$(BUILD)/fuzz_picture_view_smoke: tests/fuzz_picture_view.c tests/cli_parse.h src/picture_view.h src/chroma_classify.h tests/stubs/vlc_common.h tests/stubs/vlc_picture.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(SMOKE_CFLAGS) -Itests/stubs -o $@ $< $(SMOKE_LDFLAGS)

# Cross-variant smoke fuzzer: needs the same three SIMD .o files as the
# variant-equivalence test and uses the selected compiler like the rest of
# the deterministic verification suite.
$(BUILD)/fuzz_usm_pool_sse2.o:   src/usm_pool.c src/usm.h src/usm_pool.h src/usm_pool_variants.h $(BUILD_CONFIG) | $(BUILD) check-cc-x86-level-flags
	$(CC) $(SMOKE_CFLAGS) -march=x86-64    -DUSM_VARIANT=sse2   -c -o $@ $<
$(BUILD)/fuzz_usm_pool_avx2.o:   src/usm_pool.c src/usm.h src/usm_pool.h src/usm_pool_variants.h $(BUILD_CONFIG) | $(BUILD) check-cc-x86-level-flags
	$(CC) $(SMOKE_CFLAGS) -march=x86-64-v3 -DUSM_VARIANT=avx2   -c -o $@ $<
$(BUILD)/fuzz_usm_pool_avx512.o: src/usm_pool.c src/usm.h src/usm_pool.h src/usm_pool_variants.h $(BUILD_CONFIG) | $(BUILD) check-cc-x86-level-flags
	$(CC) $(SMOKE_CFLAGS) -march=x86-64-v4 -DUSM_VARIANT=avx512 -c -o $@ $<

$(BUILD)/fuzz_usm_variants_smoke: tests/fuzz_usm_variants.c \
    $(BUILD)/fuzz_usm_pool_sse2.o $(BUILD)/fuzz_usm_pool_avx2.o \
    $(BUILD)/fuzz_usm_pool_avx512.o \
    tests/prng.h src/usm_pool.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(SMOKE_CFLAGS) -o $@ $< \
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

STRESS_CFLAGS_ASAN := -O2 -g $(MARCH_FLAG) $(WARN) -MMD -MP -fsanitize=address,undefined $(EXTRA_CFLAGS)
STRESS_LDFLAGS_ASAN := -fsanitize=address,undefined -lpthread
STRESS_CFLAGS_TSAN := -O1 -g $(MARCH_FLAG) $(WARN) -MMD -MP -fsanitize=thread $(EXTRA_CFLAGS)
STRESS_LDFLAGS_TSAN := -fsanitize=thread -lpthread

$(BUILD)/usm_pool_stress_asan.o: src/usm_pool.c $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(STRESS_CFLAGS_ASAN) -c -o $@ $<

$(BUILD)/usm_pool_stress_tsan.o: src/usm_pool.c $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(STRESS_CFLAGS_TSAN) -c -o $@ $<

$(BUILD)/stress_usm_pool: tests/stress_usm_pool.c $(BUILD)/usm_pool_stress_asan.o $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(STRESS_CFLAGS_ASAN) -o $@ $< $(BUILD)/usm_pool_stress_asan.o $(STRESS_LDFLAGS_ASAN)

$(BUILD)/stress_usm_pool_tsan: tests/stress_usm_pool.c $(BUILD)/usm_pool_stress_tsan.o $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(STRESS_CFLAGS_TSAN) -o $@ $< $(BUILD)/usm_pool_stress_tsan.o $(STRESS_LDFLAGS_TSAN)

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
.PHONY: require-zimg
require-zimg:
	@if [ -z "$(HAVE_ZIMG)" ]; then \
	    echo "error: zimg support is required for this verification gate" >&2; \
	    exit 2; \
	 fi

ifdef HAVE_ZIMG
ZIMG_H_CFLAGS  := -g $(MARCH_FLAG) $(WARN) $(VLC_CFLAGS) $(ZIMG_CFLAGS) $(EXTRA_CFLAGS)
ZIMG_H_LIBS    := $(VLC_LIBS) $(ZIMG_LIBS) -lpthread
# test_scaler_zimg defines these wrappers; only links of that file may use
# them. The TSan harness deliberately links without any --wrap shims: TSan
# provides its own libc/pthread interceptors, while the dedicated ASan/UBSan
# binary and worker-pool tests already cover the injected fault paths.
ZIMG_TEST_COMMON_WRAP_LDFLAGS := -Wl,--wrap=aligned_alloc \
    -Wl,--wrap=zimg_filter_graph_process -Wl,--wrap=sched_getaffinity \
    -Wl,--wrap=sysconf -Wl,--wrap=pthread_setaffinity_np \
    -Wl,--wrap=__sched_cpualloc -Wl,--wrap=vlc_Log
ZIMG_TEST_WRAP_LDFLAGS := $(BARRIER_WRAP_LDFLAGS) \
    $(ZIMG_TEST_COMMON_WRAP_LDFLAGS)

# scaler_zimg.c compiled once per sanitizer/optimization mode; header
# dependencies come from -MMD (the coverage-zimg recipe rebuilds from
# scratch every run, so it alone stays a one-step compile).
$(BUILD)/scaler_zimg_asan.o: src/scaler_zimg.c $(BUILD_CONFIG) | $(BUILD)
	$(CC) -O2 $(ZIMG_H_CFLAGS) -fsanitize=address,undefined -MMD -MP -c -o $@ $<

$(BUILD)/scaler_zimg_tsan.o: src/scaler_zimg.c $(BUILD_CONFIG) | $(BUILD)
	$(CC) -O1 $(ZIMG_H_CFLAGS) -fsanitize=thread -MMD -MP -c -o $@ $<

$(BUILD)/scaler_zimg_bench.o: src/scaler_zimg.c $(BUILD_CONFIG) | $(BUILD)
	$(CC) -O2 $(ZIMG_H_CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/test_scaler_zimg: tests/test_scaler_zimg.c $(BUILD)/scaler_zimg_asan.o $(BUILD_CONFIG) | $(BUILD)
	$(CC) -O2 $(ZIMG_H_CFLAGS) -fsanitize=address,undefined -MMD -MP -o $@ \
	    $< $(BUILD)/scaler_zimg_asan.o -fsanitize=address,undefined \
	    $(ZIMG_TEST_WRAP_LDFLAGS) $(ZIMG_H_LIBS)

$(BUILD)/test_scaler_zimg_tsan: tests/test_scaler_zimg.c $(BUILD)/scaler_zimg_tsan.o $(BUILD_CONFIG) | $(BUILD)
	$(CC) -O1 $(ZIMG_H_CFLAGS) -DZIMG_TEST_SKIP_BARRIER_FAULTS \
	    -DZIMG_TEST_NO_WRAP_FAULTS -fsanitize=thread -MMD -MP -o $@ \
	    $< $(BUILD)/scaler_zimg_tsan.o -fsanitize=thread \
	    $(ZIMG_H_LIBS)

$(BUILD)/bench_scaler_zimg: tests/bench_scaler_zimg.c $(BUILD)/scaler_zimg_bench.o $(BUILD_CONFIG) | $(BUILD)
	$(CC) -O2 $(ZIMG_H_CFLAGS) -MMD -MP -o $@ $< $(BUILD)/scaler_zimg_bench.o $(ZIMG_H_LIBS)

# SCAL-3 seam fuzzer: randomized geometry/chroma/thread-count, asserts the
# tiled-vs-single-graph seam stays <= SEAM_MAX_DELTA on smooth content.
# Smoke variant (own main) runs in the standard harness; libFuzzer variant
# (clang) explores the geometry space.
$(BUILD)/fuzz_scaler_seam_smoke: tests/fuzz_scaler_seam.c $(BUILD)/scaler_zimg_asan.o $(BUILD_CONFIG) | $(BUILD)
	$(CC) -O2 $(ZIMG_H_CFLAGS) -DFUZZ_MAIN -fsanitize=address,undefined -MMD -MP -o $@ \
	    $< $(BUILD)/scaler_zimg_asan.o -fsanitize=address,undefined $(ZIMG_H_LIBS)

$(BUILD)/scaler_zimg_fuzz.o: src/scaler_zimg.c $(BUILD_CONFIG) | $(BUILD)
	$(CLANG) -O1 -g $(MARCH_FLAG) $(WARN) $(VLC_CFLAGS) $(ZIMG_CFLAGS) $(FUZZ_SAN) \
	    -MMD -MP -c -o $@ $<

$(BUILD)/fuzz_scaler_seam: tests/fuzz_scaler_seam.c $(BUILD)/scaler_zimg_fuzz.o $(BUILD_CONFIG) | $(BUILD)
	$(CLANG) -O1 -g $(MARCH_FLAG) $(WARN) $(VLC_CFLAGS) $(ZIMG_CFLAGS) $(FUZZ_SAN) \
	    -MMD -MP -o $@ $< $(BUILD)/scaler_zimg_fuzz.o $(FUZZ_SAN) $(ZIMG_H_LIBS)

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
coverage-zimg: | $(BUILD_MARKER)
	@set -eu; \
	covz="$(abspath $(BUILD))/covz"; \
	harness="$$covz/tz"; \
	harness_log="$$covz/harness.log"; \
	gcov_log="$$covz/gcov.log"; \
	gcno="$$covz/tz-scaler_zimg.gcno"; \
	gcda="$$covz/tz-scaler_zimg.gcda"; \
	gcov_file="$$covz/scaler_zimg.c.gcov"; \
	rm -rf -- "$$covz"; \
	mkdir -p -- "$$covz"; \
	$(COV_CC) -O0 $(COVERAGE_PROFILE_FLAGS) $(ZIMG_H_CFLAGS) \
	    -o "$$harness" tests/test_scaler_zimg.c src/scaler_zimg.c \
	    $(COV_LDFLAGS) $(ZIMG_TEST_WRAP_LDFLAGS) $(ZIMG_H_LIBS); \
	ln -s -- "$(abspath src)" "$$covz/src"; \
	ln -s -- "$(abspath tests)" "$$covz/tests"; \
	harness_status=0; \
	"$$harness" >"$$harness_log" 2>&1 || harness_status=$$?; \
	if [ "$$harness_status" -ne 0 ]; then \
	    printf 'FAIL: zimg coverage harness exited with status %d\n' \
	        "$$harness_status" >&2; \
	    cat -- "$$harness_log" >&2 || :; \
	fi; \
	report_status=0; \
	if [ ! -s "$$gcno" ]; then \
	    printf 'ERROR: missing or empty zimg coverage notes: %s\n' "$$gcno" >&2; \
	    report_status=2; \
	fi; \
	if [ ! -s "$$gcda" ]; then \
	    printf 'ERROR: missing or empty zimg coverage data: %s\n' "$$gcda" >&2; \
	    report_status=2; \
	fi; \
	gcov_status=0; \
	if [ -s "$$gcno" ] && [ -s "$$gcda" ]; then \
	    (cd "$$covz" && $(GCOV) -m -o . "$${gcda##*/}") \
	        >"$$gcov_log" 2>&1 || gcov_status=$$?; \
	    if [ "$$gcov_status" -ne 0 ]; then \
	        printf 'FAIL: gcov exited with status %d\n' "$$gcov_status" >&2; \
	        cat -- "$$gcov_log" >&2 || :; \
	    fi; \
	fi; \
	if [ ! -s "$$gcov_file" ]; then \
	    printf 'ERROR: missing or empty zimg gcov report: %s\n' \
	        "$$gcov_file" >&2; \
	    report_status=2; \
	else \
	    source=$$(awk 'index($$0, ":Source:") { \
	        sub(/^.*:Source:/, "", $$0); print $$0; exit \
	    }' "$$gcov_file"); \
	    if [ "$$source" != 'src/scaler_zimg.c' ]; then \
	        printf 'ERROR: unexpected zimg gcov source: %s\n' "$$source" >&2; \
	        report_status=2; \
	    else \
	        totals_status=0; \
	        totals=$$(awk -f scripts/gcov_line_totals.awk "$$gcov_file") \
	            || totals_status=$$?; \
	        if [ "$$totals_status" -ne 0 ]; then \
	            printf 'ERROR: failed to parse zimg gcov report (status %d)\n' \
	                "$$totals_status" >&2; \
	            report_status=2; \
	        else \
	            set -f; set -- $$totals; set +f; \
	            if [ "$$#" -ne 2 ]; then \
	                printf 'ERROR: malformed zimg coverage totals: %s\n' \
	                    "$$totals" >&2; \
	                report_status=2; \
	            else \
	                total=$$1; covered=$$2; totals_valid=1; \
	                case "$$total" in ''|*[!0-9]*) totals_valid=0;; esac; \
	                case "$$covered" in ''|*[!0-9]*) totals_valid=0;; esac; \
	                if [ "$$totals_valid" -ne 1 ]; then \
	                    printf 'ERROR: malformed zimg coverage totals: %s\n' \
	                        "$$totals" >&2; \
	                    report_status=2; \
	                elif [ "$$total" -eq 0 ]; then \
	                    printf 'ERROR: zimg gcov report has no executable lines\n' >&2; \
	                    report_status=2; \
	                elif [ "$$covered" -gt "$$total" ]; then \
	                    printf 'ERROR: zimg covered lines exceed total: %s\n' \
	                        "$$totals" >&2; \
	                    report_status=2; \
	                else \
	                    percent=$$(awk -v covered="$$covered" -v total="$$total" \
	                        'BEGIN { printf "%.1f", covered * 100.0 / total }'); \
	                    printf 'scaler_zimg.c: %s/%s lines = %s%% (harness; informational)\n' \
	                        "$$covered" "$$total" "$$percent"; \
	                fi; \
	            fi; \
	        fi; \
	    fi; \
	fi; \
	if [ "$$harness_status" -ne 0 ]; then exit "$$harness_status"; fi; \
	if [ "$$gcov_status" -ne 0 ]; then exit "$$gcov_status"; fi; \
	exit "$$report_status"

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
BENCH_CFLAGS := -O3 $(MARCH_FLAG) $(WARN) -MMD -MP $(EXTRA_CFLAGS)

build-bench: $(BUILD)/bench_usm_pool $(BUILD)/bench_usm_pool_flatskip $(BUILD)/bench_worker_pool $(if $(HAVE_ZIMG),$(BUILD)/bench_scaler_zimg $(BUILD)/bench_pipeline)

$(BUILD)/usm_pool_bench.o: src/usm_pool.c $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(BENCH_CFLAGS) -c -o $@ $<
$(BUILD)/usm_pool_bench_flatskip.o: src/usm_pool.c $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(BENCH_CFLAGS) -DUSM_POOL_FLAT_SKIP=1 -c -o $@ $<
$(BUILD)/bench_usm_pool: tests/bench_usm_pool.c tests/prng.h $(BUILD)/usm_pool_bench.o $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(BENCH_CFLAGS) -o $@ $< $(BUILD)/usm_pool_bench.o -lpthread
$(BUILD)/bench_usm_pool_flatskip: tests/bench_usm_pool.c tests/prng.h $(BUILD)/usm_pool_bench_flatskip.o $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(BENCH_CFLAGS) -o $@ $< $(BUILD)/usm_pool_bench_flatskip.o -lpthread
$(BUILD)/bench_worker_pool: tests/bench_worker_pool.c src/worker_pool.h src/threading.h tests/cli_parse.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(BENCH_CFLAGS) -o $@ $< -lpthread
$(BUILD)/bench_pipeline: tests/bench_pipeline.c tests/zimg_test_util.h $(BUILD)/scaler_zimg_bench.o $(BUILD)/usm_pool_bench.o $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(BENCH_CFLAGS) $(VLC_CFLAGS) -o $@ $< $(BUILD)/scaler_zimg_bench.o $(BUILD)/usm_pool_bench.o $(VLC_LIBS) $(ZIMG_LIBS) -lpthread

bench: $(BUILD)/bench_usm_pool
	@echo "requested_threads,effective_threads,width,height,frames,amount,fill,us_per_frame"
	@$(BUILD)/bench_usm_pool 1 1920 1080 300 20 rand
	@$(BUILD)/bench_usm_pool 2 1920 1080 300 20 rand
	@$(BUILD)/bench_usm_pool 4 1920 1080 300 20 rand
	@$(BUILD)/bench_usm_pool 8 1920 1080 300 20 rand
	@$(BUILD)/bench_usm_pool 4 1280 720 300 20 rand
	@$(BUILD)/bench_usm_pool 8 3840 2160 100 20 rand

bench-usm-halo: $(BUILD)/bench_usm_pool
	@./scripts/bench_usm_halo.sh $(BUILD)/bench_usm_pool

bench-worker-pool: $(BUILD)/bench_worker_pool
	@echo "workers,iterations,us_per_dispatch,last_completion_skew_us"
	@for n in 1 2 4 8 12 16 24 32; do $(BUILD)/bench_worker_pool $$n 10000; done

bench-pipeline: $(BUILD)/bench_pipeline
	@echo "threads,frames,pin,zimg_lines,usm_lines,lazy_us,max_rss_kb,us_per_frame"
	@for n in 1 4 8 12 16; do $(BUILD)/bench_pipeline $$n 200; done

bench-flatskip: $(BUILD)/bench_usm_pool $(BUILD)/bench_usm_pool_flatskip
	@echo "kernel,requested_threads,effective_threads,width,height,frames,amount,fill,us_per_frame"
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
# Coverage includes the VLC-facing lifecycle contract through a narrow stub
# boundary. scaler_zimg.c remains separately reported because it needs its
# optional zimg runtime, while its deterministic seams are fuzzed directly.
COV_BUILD := $(BUILD)/cov
COVERAGE_THRESHOLD ?= 80
# Coverage profiles are compiler-specific; override these as a matched GCC pair.
COV_CC      ?= gcc
GCOV        ?= gcov
COV_CFLAGS  := -O0 -g $(MARCH_FLAG) $(WARN) -MMD -MP \
               $(COVERAGE_PROFILE_FLAGS) $(EXTRA_CFLAGS)
COV_LDFLAGS := --coverage

COV_TESTS := \
    $(COV_BUILD)/test_upscale_logic \
    $(COV_BUILD)/test_usm \
    $(COV_BUILD)/test_cli_parse \
    $(COV_BUILD)/test_threading \
    $(COV_BUILD)/test_worker_pool \
    $(COV_BUILD)/test_zimg_helpers \
    $(COV_BUILD)/test_chroma_classify \
    $(COV_BUILD)/test_usm_pool \
    $(COV_BUILD)/test_content_probe \
    $(COV_BUILD)/test_scaler_pick \
    $(COV_BUILD)/test_scaler_swscale \
    $(COV_BUILD)/test_autoupscale_lifecycle \
    $(COV_BUILD)/test_picture_view \
    $(COV_BUILD)/test_lifetime \
    $(if $(IS_X86),$(COV_BUILD)/test_usm_pool_dispatch \
        $(COV_BUILD)/test_usm_pool_dispatch_fallback)

COV_FUZZERS := \
    $(COV_BUILD)/fuzz_upscale_logic \
    $(COV_BUILD)/fuzz_usm \
    $(COV_BUILD)/fuzz_threading \
    $(COV_BUILD)/fuzz_worker_pool \
    $(COV_BUILD)/fuzz_copy_plane \
    $(COV_BUILD)/fuzz_stripe_bounds \
    $(COV_BUILD)/fuzz_decide_tile_grid \
    $(COV_BUILD)/fuzz_frame_shape \
    $(COV_BUILD)/fuzz_scaler_chroma \
    $(COV_BUILD)/fuzz_scaler_open \
    $(COV_BUILD)/fuzz_content_probe \
    $(COV_BUILD)/fuzz_picture_view

COV_BINS := $(COV_TESTS) $(COV_FUZZERS)

$(COV_BUILD): | $(BUILD_MARKER)
	mkdir -p "$(COV_BUILD)"

$(COV_BUILD)/test_upscale_logic: tests/test_upscale_logic.c src/upscale_logic.h $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/test_usm: tests/test_usm.c tests/usm_test_util.h src/usm.h $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/test_cli_parse: tests/test_cli_parse.c tests/cli_parse.h $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/test_threading: tests/test_threading.c src/threading.h tests/barrier_fault_inject.h $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) $(THREADING_TEST_CFLAGS) -o $@ $< $(COV_LDFLAGS) $(THREADING_WRAP_LDFLAGS) -lpthread
$(COV_BUILD)/test_zimg_helpers: tests/test_zimg_helpers.c src/zimg_helpers.h $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/test_chroma_classify: tests/test_chroma_classify.c src/chroma_classify.h $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/test_worker_pool: tests/test_worker_pool.c src/worker_pool.h src/threading.h tests/barrier_fault_inject.h tests/test_harness.h $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) $(WORKER_POOL_TEST_CFLAGS) -o $@ $< $(COV_LDFLAGS) \
	    $(WORKER_POOL_WRAP_LDFLAGS) -lpthread
$(COV_BUILD)/fuzz_worker_pool: tests/fuzz_worker_pool.c src/worker_pool.h src/threading.h $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -DFUZZ_MAIN -o $@ $< $(COV_LDFLAGS) \
	    $(WORKER_POOL_FUZZ_WRAP_LDFLAGS) -lpthread

$(COV_BUILD)/usm_pool_cov.o: src/usm_pool.c $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -c -o $@ $<
$(COV_BUILD)/test_usm_pool: tests/test_usm_pool.c $(COV_BUILD)/usm_pool_cov.o $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -o $@ $< $(COV_BUILD)/usm_pool_cov.o $(COV_LDFLAGS) \
	    $(USM_POOL_WRAP_LDFLAGS) -lpthread
$(COV_BUILD)/test_usm_pool_dispatch: tests/test_usm_pool_dispatch.c src/usm_pool_dispatch.c src/usm_pool_variants.h src/usm_pool.h src/cpu_level.h tests/test_harness.h $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/test_usm_pool_dispatch_fallback: tests/test_usm_pool_dispatch.c src/usm_pool_dispatch.c src/usm_pool_variants.h src/usm_pool.h src/cpu_level.h tests/test_harness.h $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -march=x86-64 \
	    -DUP_CPU_LEVEL_FORCE_FALLBACK=1 -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/test_content_probe: tests/test_content_probe.c tests/prng.h src/content_probe.h $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/test_scaler_pick: tests/test_scaler_pick.c src/scaler_pick_logic.h src/scaler_status.h $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/test_scaler_swscale: tests/test_scaler_swscale.c src/scaler_swscale.c src/scaler.h src/scaler_status.h src/picture_view.h src/chroma_classify.h tests/stubs/vlc_common.h tests/stubs/vlc_picture.h tests/stubs/libswscale/swscale.h tests/stubs/libavutil/pixfmt.h $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -Itests/stubs -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/test_autoupscale_lifecycle: tests/test_autoupscale_lifecycle.c src/autoupscale.c src/scaler.h src/usm_pool.h tests/test_harness.h tests/lifecycle_stubs/vlc_common.h tests/lifecycle_stubs/vlc_filter.h tests/lifecycle_stubs/vlc_picture.h tests/lifecycle_stubs/vlc_plugin.h $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -march=x86-64 -Itests/lifecycle_stubs -Itests/stubs -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/test_picture_view: tests/test_picture_view.c src/picture_view.h src/chroma_classify.h tests/stubs/vlc_common.h tests/stubs/vlc_picture.h $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -Itests/stubs -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/test_lifetime: tests/test_lifetime.c tests/prng.h $(COV_BUILD)/usm_pool_cov.o $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -o $@ $< $(COV_BUILD)/usm_pool_cov.o $(COV_LDFLAGS) -lpthread

$(COV_BUILD)/fuzz_upscale_logic: tests/fuzz_upscale_logic.c src/upscale_logic.h $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -DFUZZ_MAIN -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/fuzz_usm: tests/fuzz_usm.c tests/usm_test_util.h src/usm.h $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -DFUZZ_MAIN -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/fuzz_threading: tests/fuzz_threading.c tests/cli_parse.h src/threading.h $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -DFUZZ_MAIN -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/fuzz_copy_plane: tests/fuzz_copy_plane.c tests/cli_parse.h src/plane_utils.h $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -DFUZZ_MAIN -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/fuzz_stripe_bounds: tests/fuzz_stripe_bounds.c tests/cli_parse.h src/plane_utils.h $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -DFUZZ_MAIN -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/fuzz_decide_tile_grid: tests/fuzz_decide_tile_grid.c src/zimg_helpers.h $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -DFUZZ_MAIN -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/fuzz_frame_shape: tests/fuzz_frame_shape.c src/chroma_classify.h src/zimg_helpers.h $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -DFUZZ_MAIN -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/fuzz_scaler_chroma: tests/fuzz_scaler_chroma.c src/scaler_zimg_chroma.h src/chroma_classify.h $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -DFUZZ_MAIN -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/fuzz_scaler_open: tests/fuzz_scaler_open.c src/scaler_pick_logic.h $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -DFUZZ_MAIN -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/fuzz_content_probe: tests/fuzz_content_probe.c tests/prng.h src/content_probe.h $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -DFUZZ_MAIN -o $@ $< $(COV_LDFLAGS)
$(COV_BUILD)/fuzz_picture_view: tests/fuzz_picture_view.c tests/cli_parse.h src/picture_view.h src/chroma_classify.h tests/stubs/vlc_common.h tests/stubs/vlc_picture.h $(BUILD_CONFIG) | $(COV_BUILD)
	$(COV_CC) $(COV_CFLAGS) -DFUZZ_MAIN -Itests/stubs -o $@ $< $(COV_LDFLAGS)

.PHONY: coverage coverage-summary
coverage: $(COV_BINS)
	@rm -f $(COV_BUILD)/*.gcda
	@set -e; \
	log=$$(mktemp "$(COV_BUILD)/coverage-run.XXXXXX"); \
	trap 'rm -f "$$log"' 0 1 2 3 15; \
	for t in $(COV_BINS); do \
	    printf 'coverage: %s\n' "$$t"; \
	    if "$$t" > "$$log" 2>&1; then \
	        :; \
	    else \
	        status=$$?; \
	        printf 'FAIL: coverage binary %s exited with status %d\n' \
	            "$$t" "$$status" >&2; \
	        cat "$$log" >&2 || :; \
	        exit "$$status"; \
	    fi; \
	done
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
	@COV_DIR=$(COV_BUILD) THRESHOLD=$(COVERAGE_THRESHOLD) ./scripts/coverage_report.sh
	@COV_DIR=$(COV_BUILD) THRESHOLD=$(COVERAGE_THRESHOLD) ./scripts/coverage_per_function.sh

coverage-summary: coverage

# --------- static analysis ---------
.PHONY: complexity
# Cyclomatic-complexity gate. lizard exits non-zero if any function has
# CCN > 10 (project policy: every function stays at or below 10).
complexity:
	@command -v lizard >/dev/null 2>&1 || { \
		echo "lizard not installed. pip: lizard"; exit 1; }
	lizard -C 10 src/ tests/

semantic-analysis:
	@command -v shellcheck >/dev/null 2>&1 || { \
		echo "shellcheck not installed"; exit 1; }
	@command -v actionlint >/dev/null 2>&1 || { \
		echo "actionlint not installed"; exit 1; }
	shellcheck scripts/*.sh tests/*.sh
	actionlint .github/workflows/*.yml

analyze: complexity semantic-analysis
	@command -v cppcheck >/dev/null 2>&1 || { \
		echo "cppcheck not installed. apt: cppcheck"; exit 1; }
	# autoupscale.c is excluded — it depends on VLC's macro-heavy headers
	# that cppcheck cannot reasonably parse without a full include path.
	# The interesting logic is all in *_logic.h / usm.h, exercised via tests.
	# scaler_zimg.c IS analyzed, against the test VLC stubs (cppcheck chokes
	# on the real vlc_variables.h); needs libzimg headers, skipped otherwise.
	cppcheck --enable=warning,style,performance,portability \
		--inline-suppr --std=c11 --error-exitcode=2 \
		--suppress=missingIncludeSystem \
		-I src -I tests/stubs $(ZIMG_CFLAGS) \
		src/upscale_logic.h src/usm.h src/threading.h src/zimg_helpers.h src/chroma_classify.h src/scaler_zimg_chroma.h src/content_probe.h src/scaler_pick_logic.h src/usm_pool.h src/usm_pool.c $(if $(HAVE_ZIMG),src/scaler_zimg.c) tests/

SCAN_BUILD ?= scan-build
SCAN_CC ?= clang
SCAN_ROOT ?= $(BUILD)/scan-build
SCAN_MARKER := .vlc-autoscaler-scan-root
SCAN_SINGLE_BUILD = $(SCAN_ROOT)/single-build
SCAN_MULTI_BUILD = $(SCAN_ROOT)/multiversion-build
SCAN_SINGLE_REPORTS = $(SCAN_ROOT)/single-reports
SCAN_MULTI_REPORTS = $(SCAN_ROOT)/multiversion-reports
SCAN_IS_X86 ?= $(findstring x86_64,$(shell $(SCAN_CC) -dumpmachine 2>/dev/null))

scan-build:
	@command -v "$(SCAN_BUILD)" >/dev/null 2>&1 || { \
		echo "$(SCAN_BUILD) not found. apt: clang-tools"; exit 1; }
	@command -v "$(SCAN_CC)" >/dev/null 2>&1 || { \
		echo "$(SCAN_CC) not found. Set SCAN_CC to a Clang compiler"; exit 1; }
	@./scripts/safe-rm-tree.sh remove "$(SCAN_ROOT)" "$(SCAN_MARKER)" "$(CURDIR)"
	@./scripts/safe-rm-tree.sh init "$(SCAN_ROOT)" "$(SCAN_MARKER)" "$(CURDIR)"
	mkdir -p -- "$(SCAN_SINGLE_REPORTS)" "$(SCAN_MULTI_REPORTS)"
	$(SCAN_BUILD) --status-bugs --use-cc="$(SCAN_CC)" \
		-o "$(abspath $(SCAN_SINGLE_REPORTS))" \
		$(MAKE) plugin BUILD="$(abspath $(SCAN_SINGLE_BUILD))" \
		MULTIVERSION=0
ifneq ($(SCAN_IS_X86),)
	$(SCAN_BUILD) --status-bugs --use-cc="$(SCAN_CC)" \
		-o "$(abspath $(SCAN_MULTI_REPORTS))" \
		$(MAKE) plugin BUILD="$(abspath $(SCAN_MULTI_BUILD))" \
		MULTIVERSION=1
else
	@echo "scan-build: skipping MULTIVERSION=1 (SCAN_CC target is not x86-64)"
endif

# --------- install ---------
install: $(BUILD)/$(PLUGIN).so
	@if [ -z "$(strip $(VLC_PLUGIN_BASE))" ]; then \
		echo "ERROR: cannot determine VLC plugin directory"; exit 1; fi
	$(INSTALL) -d "$(DESTDIR)$(VLC_PLUGIN_DIR)"
	$(INSTALL) -m 0644 "$(BUILD)/$(PLUGIN).so" "$(DESTDIR)$(VLC_PLUGIN_DIR)/"
	@echo "Installed to $(DESTDIR)$(VLC_PLUGIN_DIR)/$(PLUGIN).so"
	@echo "If VLC doesn't pick it up, run:"
	@echo '  vlc-cache-gen "$(VLC_PLUGIN_BASE)"'

uninstall:
	@if [ -z "$(strip $(VLC_PLUGIN_BASE))" ]; then \
		echo "ERROR: cannot determine VLC plugin directory"; exit 1; fi
	rm -f -- "$(DESTDIR)$(VLC_PLUGIN_DIR)/$(PLUGIN).so"

clean:
	@./scripts/safe-rm-tree.sh remove "$(BUILD)" ".vlc-autoscaler-build-root" "$(CURDIR)"

info:
	@echo "VLC plugin dir : $(VLC_PLUGIN_DIR)"
	@echo "VLC cflags     : $(VLC_CFLAGS)"
	@echo "VLC libs       : $(VLC_LIBS)"
	@echo "swscale cflags : $(SWS_CFLAGS)"
	@echo "swscale libs   : $(SWS_LIBS)"
	@echo "CC             : $(CC)"
	@echo "CLANG          : $(CLANG)"

$(BUILD): | $(BUILD_MARKER)

$(BUILD_MARKER):
	@./scripts/safe-rm-tree.sh init "$(BUILD)" ".vlc-autoscaler-build-root" "$(CURDIR)"

# Include dependency files generated by -MMD (plugin, tests, fuzzers,
# stress, bench, coverage). Hand-written header prerequisites on the
# rules above are a readable summary only; the .d files are the source
# of truth for incremental correctness.
-include $(wildcard $(BUILD)/*.d)
-include $(wildcard $(COV_BUILD)/*.d)
$(BUILD)/test_threading: tests/test_threading.c src/threading.h tests/barrier_fault_inject.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(TEST_CFLAGS) $(THREADING_TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS) $(THREADING_WRAP_LDFLAGS) -lpthread

# PORT-1: same suite compiled with the affinity machinery forced off,
# proving the sysconf-only fallback (non-glibc libcs) builds and passes.
$(BUILD)/test_threading_noaffinity: tests/test_threading.c src/threading.h tests/barrier_fault_inject.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(TEST_CFLAGS) $(THREADING_TEST_CFLAGS) -DUP_NO_CPU_AFFINITY -o $@ $< $(TEST_LDFLAGS) $(THREADING_WRAP_LDFLAGS) -lpthread

$(BUILD)/test_zimg_helpers: tests/test_zimg_helpers.c src/zimg_helpers.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_chroma_classify: tests/test_chroma_classify.c src/chroma_classify.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/usm_pool_test.o: src/usm_pool.c $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

$(BUILD)/test_worker_pool: tests/test_worker_pool.c src/worker_pool.h src/threading.h tests/barrier_fault_inject.h tests/test_harness.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(TEST_CFLAGS) $(WORKER_POOL_TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS) \
	    $(WORKER_POOL_WRAP_LDFLAGS) -lpthread

$(BUILD)/test_usm_pool: tests/test_usm_pool.c $(BUILD)/usm_pool_test.o $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(BUILD)/usm_pool_test.o $(TEST_LDFLAGS) \
	    $(USM_POOL_WRAP_LDFLAGS) -lpthread

# Cross-variant byte-equivalence test: links all three SIMD variants and the
# dispatcher's variant_name symbol. Each variant .o is the same usm_pool.c
# compiled at a different -march level. CPU feature gating in the test
# itself skips the AVX2/AVX-512 variants when not supported by the runner.
$(BUILD)/test_usm_pool_sse2.o:   src/usm_pool.c src/usm.h src/usm_pool.h src/usm_pool_variants.h $(BUILD_CONFIG) | $(BUILD) check-cc-x86-level-flags
	$(CC) $(TEST_CFLAGS) -march=x86-64    -DUSM_VARIANT=sse2   -c -o $@ $<
$(BUILD)/test_usm_pool_avx2.o:   src/usm_pool.c src/usm.h src/usm_pool.h src/usm_pool_variants.h $(BUILD_CONFIG) | $(BUILD) check-cc-x86-level-flags
	$(CC) $(TEST_CFLAGS) -march=x86-64-v3 -DUSM_VARIANT=avx2   -c -o $@ $<
$(BUILD)/test_usm_pool_avx512.o: src/usm_pool.c src/usm.h src/usm_pool.h src/usm_pool_variants.h $(BUILD_CONFIG) | $(BUILD) check-cc-x86-level-flags
	$(CC) $(TEST_CFLAGS) -march=x86-64-v4 -DUSM_VARIANT=avx512 -c -o $@ $<
$(BUILD)/test_usm_pool_dispatch.o: src/usm_pool_dispatch.c src/usm_pool.h src/usm_pool_variants.h src/cpu_level.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(TEST_CFLAGS) -march=x86-64 -c -o $@ $<

$(BUILD)/test_usm_pool_variants: tests/test_usm_pool_variants.c \
    $(BUILD)/test_usm_pool_sse2.o $(BUILD)/test_usm_pool_avx2.o \
    $(BUILD)/test_usm_pool_avx512.o $(BUILD)/test_usm_pool_dispatch.o \
    tests/prng.h src/usm.h src/usm_pool.h src/cpu_level.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< \
	    $(BUILD)/test_usm_pool_sse2.o $(BUILD)/test_usm_pool_avx2.o \
	    $(BUILD)/test_usm_pool_avx512.o $(BUILD)/test_usm_pool_dispatch.o \
	    $(TEST_LDFLAGS) -lpthread

$(BUILD)/test_content_probe: tests/test_content_probe.c tests/prng.h src/content_probe.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_scaler_pick: tests/test_scaler_pick.c src/scaler_pick_logic.h src/scaler_status.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_scaler_swscale: tests/test_scaler_swscale.c src/scaler_swscale.c src/scaler.h src/scaler_status.h src/picture_view.h src/chroma_classify.h tests/stubs/vlc_common.h tests/stubs/vlc_picture.h tests/stubs/libswscale/swscale.h tests/stubs/libavutil/pixfmt.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(TEST_CFLAGS) -Itests/stubs -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_picture_view: tests/test_picture_view.c src/picture_view.h src/chroma_classify.h tests/stubs/vlc_common.h tests/stubs/vlc_picture.h $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(TEST_CFLAGS) -Itests/stubs -o $@ $< $(TEST_LDFLAGS)

$(BUILD)/test_lifetime: tests/test_lifetime.c tests/prng.h $(BUILD)/usm_pool_test.o $(BUILD_CONFIG) | $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(BUILD)/usm_pool_test.o $(TEST_LDFLAGS) -lpthread
