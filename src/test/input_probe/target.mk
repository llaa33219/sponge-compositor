# compositor-input-probe — G2 input probe client.
#
# Two overlapping views (top: alpha + input-mask with a hole; bottom:
# opaque) on two Gui sessions, QMP-TARGET click markers for host PS/2
# clicks, per-session 'Input::Connection' witnesses plus the per-session
# Gui input channels that carry the routed events, deterministic batches
# injected through the public 'Event::Connection', and the nitpicker
# 'pointer' report read through the report ROM. Asserts top-view clicks,
# alpha/input-mask hole fall-through, focus-transition order, real Event
# injection delivery, and pointer-report sanity, then logs bounded PASS
# markers and an FNV-1a-64 canonical trace digest
# (type/session/x/y/key) for native-vs-upstream comparison.
# Public Genode APIs only.

TARGET   := compositor-input-probe
SRC_CC   := main.cc
LIBS     := base
