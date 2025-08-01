# Makefile for UDP Delayed Convergence Layer Adapters
# Self-contained build with local ION headers

# Configuration
ION_PREFIX ?= /usr/local
ION_INCLUDE = $(ION_PREFIX)/include
ION_LIB = $(ION_PREFIX)/lib

# Local headers directory
LOCAL_HEADERS = ./ionheaders

CC = gcc
CFLAGS = -Wall -O2 -g -I. -I$(LOCAL_HEADERS) -I$(ION_INCLUDE)
LDFLAGS = -L$(ION_LIB) -lici -lbp -ludpcla -lpthread -lm

# Preset delay can be customized at compile time (in seconds)
PRESET_DELAY ?= 10.0

# Link loss percentage can be customized at compile time (0.0 = no loss, 5.0 = 5% loss)
LINK_LOSS ?= 0.0

# Targets
TARGETS = udpmarsdelayclo udpmarsdelaycli udpmoondelayclo udpmoondelaycli udppresetdelayclo udppresetdelaycli

# Default target
all: $(TARGETS)

# Mars delay versions
udpmarsdelayclo: udpmarsdelayclo.c
	$(CC) $(CFLAGS) -DLINK_LOSS_PERCENTAGE=$(LINK_LOSS) -o $@ $< $(LDFLAGS)

udpmarsdelaycli: udpmarsdelaycli.c
	$(CC) $(CFLAGS) -DLINK_LOSS_PERCENTAGE=$(LINK_LOSS) -o $@ $< $(LDFLAGS)

# Moon delay versions
udpmoondelayclo: udpmoondelayclo.c
	$(CC) $(CFLAGS) -DLINK_LOSS_PERCENTAGE=$(LINK_LOSS) -o $@ $< $(LDFLAGS)

udpmoondelaycli: udpmoondelaycli.c
	$(CC) $(CFLAGS) -DLINK_LOSS_PERCENTAGE=$(LINK_LOSS) -o $@ $< $(LDFLAGS)

# Preset delay versions (customizable delay)
udppresetdelayclo: udppresetdelayclo.c
	$(CC) $(CFLAGS) -DPRESET_DELAY_SECONDS=$(PRESET_DELAY) -DLINK_LOSS_PERCENTAGE=$(LINK_LOSS) -o $@ $< $(LDFLAGS)

udppresetdelaycli: udppresetdelaycli.c
	$(CC) $(CFLAGS) -DPRESET_DELAY_SECONDS=$(PRESET_DELAY) -DLINK_LOSS_PERCENTAGE=$(LINK_LOSS) -o $@ $< $(LDFLAGS)

# Installation target
install: $(TARGETS)
	install -d $(ION_PREFIX)/bin
	install -m 755 $(TARGETS) $(ION_PREFIX)/bin/

# Uninstall target
uninstall:
	cd $(ION_PREFIX)/bin && rm -f $(TARGETS)

# Clean target
clean:
	rm -f $(TARGETS) *.o

# Custom preset delay build
preset-delay:
	@echo "Building preset delay CL with $(PRESET_DELAY) second delay and $(LINK_LOSS)% link loss..."
	$(MAKE) udppresetdelayclo udppresetdelaycli PRESET_DELAY=$(PRESET_DELAY) LINK_LOSS=$(LINK_LOSS)

# Help target
help:
	@echo "UDP Delayed Convergence Layer Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all              - Build all delay CL variants"
	@echo "  udpmarsdelayclo  - Build Mars delay output daemon"
	@echo "  udpmarsdelaycli  - Build Mars delay input daemon"
	@echo "  udpmoondelayclo  - Build Moon delay output daemon"
	@echo "  udpmoondelaycli  - Build Moon delay input daemon"
	@echo "  udppresetdelayclo - Build preset delay output daemon"
	@echo "  udppresetdelaycli - Build preset delay input daemon"
	@echo "  install          - Install all binaries to $(ION_PREFIX)/bin"
	@echo "  uninstall        - Remove installed binaries"
	@echo "  clean            - Remove built binaries"
	@echo "  preset-delay     - Build preset delay with custom delay value"
	@echo "  help             - Show this help message"
	@echo ""
	@echo "Variables:"
	@echo "  ION_PREFIX       - ION installation prefix (default: /usr/local)"
	@echo "  PRESET_DELAY     - Preset delay in seconds (default: 10.0)"
	@echo "  LINK_LOSS        - Link loss percentage (default: 0.0, e.g., 5.0 = 5% loss)"
	@echo ""
	@echo "Examples:"
	@echo "  make                                              # Build all with defaults"
	@echo "  make LINK_LOSS=2.5                               # Build all with 2.5% link loss"
	@echo "  make PRESET_DELAY=5.0 LINK_LOSS=1.0 preset-delay # Build 5-second preset delay with 1% loss"
	@echo "  make ION_PREFIX=/opt/ion install                  # Install to /opt/ion"

# Phony targets
.PHONY: all install uninstall clean preset-delay help