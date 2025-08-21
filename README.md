# UDP Delayed Convergence Layer Adapters

This repository contains three high-performance UDP delay convergence layer implementations for ION-DTN that simulate realistic space communication delays with continuous monitoring and link loss simulation.

## Overview

Production-ready implementations providing three separate, self-contained variants:

1. **Mars Delay CL** (`udpmarsdelaycli/clo`) - Simulates Earth-Mars communication delays (~3-22 minutes)
2. **Moon Delay CL** (`udpmoondelaycli/clo`) - Simulates Earth-Moon communication delays (~1.3 seconds)  
3. **Preset Delay CL** (`udppresetdelaycli/clo`) - Uses a configurable preset delay (default: 10 seconds)

## Key Features

- **Continuous Monitoring**: Independent monitoring thread ensures precise timing regardless of ION activity
- **Enhanced Capacity**: 100 bundle queue with thread-safe operations
- **Link Loss Simulation**: Configurable random packet dropping (0-100%)
- **Precise Timing**: Bundles delivered at exact arrival_time + delay regardless of processing order
- **Self-contained**: No shared libraries or ION core modifications required
- **Realistic Delays**: Mars and Moon delays based on orbital mechanics calculations
- **Production Ready**: Handles high throughput with proper resource management

## Building

### Prerequisites

- ION-DTN installed with development headers
- GCC compiler
- Standard development tools (make)

### Quick Build

```bash
make all
```

### Custom Parameters

Build with custom delay and link loss parameters:

```bash
# Build with 5-second delay and 2% link loss
make PRESET_DELAY=5.0 LINK_LOSS=2.0 preset-delay

# Build Mars delay with 10% link loss
make LINK_LOSS=10.0 mars-delay

# Build all with 1% link loss
make LINK_LOSS=1.0 all
```

### Installation

```bash
# Install to default ION location (/usr/local/bin)
make install

# Install to custom location
make ION_PREFIX=/opt/ion install
```

## Usage

### ION Configuration

Replace the standard UDP convergence layer daemons in your ION configuration:

#### Mars Delay Example

```bash
## In your .rc file (bpadmin section)
a protocol udp 1400 100
a outduct udp 192.168.0.56:4556 udpmarsdelayclo
a induct udp 0.0.0.0:4556 udpmarsdelaycli

## In ipnadmin section  
a plan 268484820 udp/192.168.0.56:4556
```

#### Moon Delay Example

```bash
## In your .rc file (bpadmin section)
a protocol udp 1400 100
a outduct udp 192.168.0.56:4556 udpmoondelayclo
a induct udp 0.0.0.0:4556 udpmoondelaycli

## In ipnadmin section
a plan 268484820 udp/192.168.0.56:4556
```

#### Preset Delay Example

```bash
## In your .rc file (bpadmin section)
a protocol udp 1400 100
a outduct udp 192.168.0.56:4556 udppresetdelayclo
a induct udp 0.0.0.0:4556 udppresetdelaycli

## In ipnadmin section
a plan 268484820 udp/192.168.0.56:4556
```

### Command Line Usage

The daemons can also be started manually:

```bash
# Start Mars delay output daemon
udpmarsdelayclo 192.168.0.56:4556

# Start Moon delay input daemon  
udpmoondelaycli 0.0.0.0:4556

# Start preset delay output daemon
udppresetdelayclo 192.168.0.56:4556
```

## Delay Calculations

### Mars Delay

- Uses simplified 2D orbital model with real-time calculations
- Earth orbital period: 365.25 days
- Mars orbital period: 687 days  
- Distance varies from ~54.6 million km to ~401 million km
- One-way delay: ~3 to ~22 minutes
- Calculated individually for each bundle based on current orbital positions

### Moon Delay

- Uses sinusoidal variation around average distance
- Average distance: 384,400 km (±20,000 km variation)
- Lunar orbital period: 27.3 days
- One-way delay: ~1.2 to ~1.4 seconds
- Calculated individually for each bundle based on current lunar position

### Preset Delay

- Configurable fixed delay value
- Default: 10.0 seconds
- Customizable via `PRESET_DELAY_SECONDS` compile-time parameter
- Consistent delay applied to all bundles

## Link Loss Simulation

All variants support configurable link loss simulation:

- **Configurable**: Set via `LINK_LOSS_PERCENTAGE` compile-time parameter
- **Range**: 0.0% (no loss) to 100.0% (all packets dropped)
- **Method**: Uniform random distribution using system RNG
- **Application**: Applied after delay calculation, before transmission/processing

## Architecture

### Optimized Single-Thread Design

Each variant uses an optimized architecture for precise timing and resource efficiency:

**CLI (Input) Architecture:**
- **Main Thread**: Non-blocking UDP packet reception and queuing
- **Queue Processing**: Single-threaded delayed bundle processing with exact timing
- **Exact Timing**: Each bundle delivered at precisely arrival_time + calculated_delay

**CLO (Output) Architecture:**
- **Main Thread**: Non-blocking bundle dequeue from ION
- **Monitor Thread**: Continuous queue monitoring and bundle transmission at exact times
- **Queue Management**: Thread-safe bundle buffering with capacity management

### Technical Implementation

- **Thread-Safe Queuing**: Mutex-protected buffers for multi-thread coordination
- **Memory Management**: ION-compatible MTAKE/MRELEASE for proper integration
- **Resource Limits**: Configurable queue sizes with efficient memory usage
- **Self-contained**: All logic embedded in each binary, no shared dependencies

## Logging

All variants log their operation to the ION log file (`ion.log`) with enhanced information:

```
[i] udpmarsdelayclo is running, spec = '192.168.0.56:4556', Mars delay = 14.23 sec, link loss = 0.0% (continuous monitoring thread)
[i] udpmoondelaycli is running, spec=[0.0.0.0:4556], Moon delay = 1.345 sec, link loss = 2.0% (single-threaded queue)
[i] udppresetdelayclo is running, spec = '192.168.0.56:4556', preset delay = 10.0 sec, link loss = 5.0% (continuous monitoring thread)
```

## Advantages Over Standard UDP CL

1. **Precise Timing**: Continuous monitoring ensures exact delay timing regardless of bundle arrival order
2. **High Throughput**: Non-blocking architecture with optimized queue processing
3. **Link Loss Simulation**: Built-in configurable packet loss for realistic space communication testing
4. **No Configuration Drift**: Self-contained implementations eliminate parameter mismatches
5. **Production Ready**: Robust resource management and error handling
6. **Drop-in Replacement**: Uses standard ION UDP CL interface
7. **No ION Modifications**: Works with any ION installation

## Performance Characteristics

- **Queue Capacity**: 100 simultaneous bundles
- **Threading Model**: Optimized single-thread with continuous monitoring
- **Timing Precision**: Microsecond-accurate delay implementation
- **Memory Efficiency**: ION-compatible memory management (MTAKE/MRELEASE)
- **Thread Safety**: Mutex protection for queue operations
- **Resource Cleanup**: Proper cleanup on shutdown with pthread management

## Project Structure

```
udpdelayedcla/
├── udpmarsdelayclo.c / udpmarsdelaycli.c     # Mars delay implementation
├── udpmoondelayclo.c / udpmoondelaycli.c     # Moon delay implementation  
├── udppresetdelayclo.c / udppresetdelaycli.c # Preset delay implementation
├── ionheaders/                               # Required ION headers
│   ├── udpcla.h, bpP.h, cbor.h, cgr.h, etc. # Self-contained ION headers
├── old/                                      # Legacy/backup files
├── Makefile                                  # Build configuration with parameters
├── DEVELOPMENT_NOTES.md                      # Technical implementation details
└── README.md                                 # This documentation
```

## Development Notes

See `DEVELOPMENT_NOTES.md` for comprehensive technical documentation including:
- Complete project evolution and decision rationale
- Detailed architecture explanations with code examples
- Build system configuration and troubleshooting
- Critical implementation learnings and future development guidance

## License

Based on original ION-DTN UDP convergence layer code.
Author: Samo Grasic (samo@grasic.net), LateLab AB, Sweden

Copyright (c) 2006, California Institute of Technology.
ALL RIGHTS RESERVED. U.S. Government Sponsorship acknowledged.