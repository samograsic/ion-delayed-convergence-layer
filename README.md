# UDP Delayed Convergence Layer Adapters

UDP delay convergence layer implementations for ION-DTN that simulate space communication delays with link loss simulation.

## Overview

Three variants for different delay scenarios:

1. **Mars Delay CL** (`udpmarsdelaycli/clo`) - Earth-Mars communication delays (3-22 minutes)
2. **Moon Delay CL** (`udpmoondelaycli/clo`) - Earth-Moon communication delays (~1.3 seconds)  
3. **Preset Delay CL** (`udppresetdelaycli/clo`) - Configurable preset delay (default: 10 seconds)

## Features

- Continuous monitoring thread for precise timing
- Link loss simulation (0-100% configurable)
- Bundle queue with thread-safe operations
- No ION core modifications required
- Realistic delays based on orbital mechanics

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

```bash
# Build with custom preset delay and link loss
make PRESET_DELAY=5.0 LINK_LOSS=2.0

# Build with link loss simulation
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
- Based on 780-day synodic period between Earth-Mars oppositions
- Distance varies from 54.6M km (closest) to 401M km (farthest)
- One-way delays: 3 to 22 minutes

### Moon Delay
- Average distance: 384,400 km with ±20,000 km variation
- Lunar orbital period: 27.3 days
- One-way delays: ~1.2 to 1.4 seconds

### Preset Delay
- Configurable fixed delay (default: 10 seconds)
- Set via `PRESET_DELAY_SECONDS` compile parameter

## Link Loss Simulation

Configurable packet loss simulation (0-100%) set via `LINK_LOSS_PERCENTAGE` parameter.

## License

Based on original ION-DTN UDP convergence layer code.
Author: Samo Grasic (samo@grasic.net), LateLab AB, Sweden

Copyright (c) 2006, California Institute of Technology.
ALL RIGHTS RESERVED. U.S. Government Sponsorship acknowledged.