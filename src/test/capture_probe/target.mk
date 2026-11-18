# compositor-capture-probe — G1 golden/differential capture probe.
#
# mode="golden" polls the real Capture::Connection::capture_at output until
# byte-exact against the expected test-fixture raster and logs
# "g1-capture: PASS checksum=0x...". mode="diff" opens two Capture
# connections labeled "native" and "upstream" and compares both real
# rasters against each other and the expected fixture ("g1-diff: MATCH").
# Public Genode APIs only.

TARGET   := compositor-capture-probe
SRC_CC   := main.cc
LIBS     := base
