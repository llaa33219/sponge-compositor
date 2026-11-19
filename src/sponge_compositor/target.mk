# sponge_compositor — Mojo-first compositor (G1 adapter layer).
#
# The C++ side is the thin Genode adapter only (AGENTS.md §3.6):
# 'Component::construct' entry glue, the Gui/Framebuffer/Input RPC
# adapters, the G2 input pipeline, the capture/event adapters, and the
# 'Genode::Heap' KGEN shim. The scene/painter/input core is Mojo-owned
# (D16C.3) and enters the target as pre-compiled objects through
# 'SRC_O' (D16C.4): each Mojo
# unit is emitted with the pinned toolchain via
# '$(SPONGE_ROOT)/tool/mojo_objects' (exact flags, loud drift).

TARGET   := sponge_compositor

SRC_CC   := main.cc \
            gui_adapter.cc \
            fb_adapter.cc \
            input_adapter.cc \
            input_pipeline.cc \
            capture_adapter.cc \
            event_adapter.cc \
            kgen_shim.cc

SRC_O    := build/scene.o build/painter.o build/input.o

LIBS     := base

INC_DIR  := $(PRG_DIR)

# The top-level build.conf defines SPONGE_ROOT for the first make stage,
# but its per-target sub-makes (prg.mk) only receive the exported build
# variables. Re-derive the umbrella root here when it is absent so the
# recipe below resolves in every build flow.
SPONGE_ROOT ?= $(abspath $(REP_DIR)/../..)

#
# Mojo -> object build seam (D16C.4). '$<' is the Mojo source and '$@'
# the emitted PIC object below build/. The undefined-symbol set of every
# emitted object is audited against the kgen_shim.cc/adapter ABI surface
# (docs/plans/phase16-sponge-compositor.md §6 rule 4).
#
# '--target-cpu=x86-64' pins the code-generation baseline. The Mojo
# codegen otherwise targets the host CPU and vectorizes the scene-state
# initialization with AVX (vxorps/vmovups %ymm), which raises
# 'seL4_Fault_UserException' on CPUs without AVX at 'sc_scene_init'.
# The baseline CPU is a reproducible codegen floor for the native
# component; semantics are unchanged (only the instruction selection).
#
build/%.o: mojo/%.mojo | build
	$(MSG_COMP)$@
	$(VERBOSE)$(SPONGE_ROOT)/tool/mojo_objects --emit object --relocation-model=pic --target-cpu=x86-64 -o $@ $<

#
# Build directory for the emitted Mojo objects
#
build:
	$(VERBOSE)mkdir -p $@
