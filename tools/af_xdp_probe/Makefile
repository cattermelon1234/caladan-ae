CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra
CPPFLAGS += -D_GNU_SOURCE

.PHONY: all check-deps clean

all: af_xdp_probe

check-deps:
	@test "$$(uname -s)" = Linux || { echo "AF_XDP requires Linux." >&2; exit 1; }
	@command -v $(PKG_CONFIG) >/dev/null || { echo "Install pkg-config and libxdp development files." >&2; exit 1; }
	@$(PKG_CONFIG) --exists libxdp || { echo "pkg-config cannot find libxdp development files." >&2; exit 1; }

af_xdp_probe: main.c | check-deps
	$(CC) $(CPPFLAGS) $(CFLAGS) $(shell $(PKG_CONFIG) --cflags libxdp 2>/dev/null) $< $(LDFLAGS) $(shell $(PKG_CONFIG) --libs libxdp 2>/dev/null) $(LDLIBS) -o $@

clean:
	$(RM) af_xdp_probe
