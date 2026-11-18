# compositor-view-client — G1 opaque-view probe client.
#
# Reads a <client .../> config node, paints a solid Pixel_rgb888 fill with a
# 1 px border into a public Gui::Connection buffer (alpha=false), creates a
# top-level view with the configured geometry, applies absolute Front/Back
# through the public command buffer, and stays alive with Timer sleeps.
# Public Genode APIs only.

TARGET   := compositor-view-client
SRC_CC   := main.cc
LIBS     := base
