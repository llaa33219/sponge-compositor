# sponge_compositor — G0 scaffold for the future Mojo-first compositor.
#
# Buildable but explicitly unimplemented: main.cc only logs
# "not implemented: sponge_compositor". No Gui/Capture/Event handling.
# G1 adds the two-opaque-view slice behind the thin C++ adapter.

TARGET   := sponge_compositor
SRC_CC   := main.cc
LIBS     := base
