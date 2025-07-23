# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

AVRDUDE is a command-line tool for programming AVR microcontrollers through various hardware programmers. It uses GNU Autotools for building and supports a wide range of programmer types and communication interfaces.

## Build Commands

```bash
# Initial setup (required after fresh checkout)
./bootstrap

# Configure build (adjust prefix as needed)
./configure --prefix=/usr/local

# Build
make

# Install (may require sudo)
make install

# Clean build
make clean
make distclean
```

### Build Configuration

Important environment variables for building:
- `CPPFLAGS`: Additional include paths (e.g., `-I/path/to/libusb/include`)
- `LDFLAGS`: Additional library paths (e.g., `-L/path/to/libusb/lib`)

Linux dependencies:
- libusb-devel
- libelf-devel
- libftdi-devel

## Dependencies for GPIO Control

Modern GPIO support (Raspberry Pi):
- libgpiod-dev (>= 2.0)

Build with GPIO support:
```bash
# Install dependencies
sudo apt-get install libgpiod-dev

# Configure with GPIO support
./configure --enable-libgpiod
```

Legacy fallback available for older systems without libgpiod.

## Code Architecture

### Core Components

1. **Main Program** (`main.c`): Entry point and command-line parsing
2. **Terminal Mode** (`term.c/h`): Interactive programming mode
3. **AVR Programming** (`avr.c/h`): Core device programming routines
4. **Configuration System** (`config.c/h`, `config_gram.y`, `lexer.l`): Parses avrdude.conf for device/programmer definitions

### Programmer Abstraction

- **Generic Interface** (`pgm.c/h`): Base programmer interface that all programmers implement
- **Programmer Types** (`pgm_type.c/h`): Registry of available programmer types
- Each programmer (e.g., `arduino.c`, `stk500.c`, `usbasp.c`) implements the pgm interface

### Communication Layers

- **Serial**: `ser_posix.c` (Unix), `ser_win32.c` (Windows)
- **USB**: `usb_libusb.c` using libusb
- **Parallel**: `par.c/h`, `ppi.c/h` for parallel port
- **SPI/GPIO**: `linuxspi.c/h`, `linuxgpio.c/h` for Linux-specific interfaces

### Memory Operations

- **File I/O** (`fileio.c/h`): Handles various file formats (Intel HEX, Motorola S-record, ELF)
- **Update** (`update.c/h`): Memory comparison and programming operations
- **Safe Mode** (`safemode.c/h`): Protection for fuse programming

## Key Development Notes

1. **No Test Framework**: The project relies on manual testing with actual hardware
2. **Configuration-Driven**: Device and programmer definitions are in `avrdude.conf`
3. **Platform-Specific Code**: Separate implementations for POSIX and Windows
4. **Programmer Addition**: To add a new programmer, implement the pgm interface and register in `pgm_type.c`

## Common Development Tasks

```bash
# Regenerate configuration parser after changes to config_gram.y or lexer.l
make clean
make

# Build with debugging symbols
./configure CFLAGS="-g -O0"
make

# Check for compilation warnings
make CFLAGS="-Wall -Wextra"
```

## Important Files for Understanding the Codebase

1. `avrdude.h`: Global definitions and structures
2. `avrpart.h`: AVR device structure definitions
3. `pgm.h`: Programmer interface definition
4. `avrdude.conf.in`: Template for device/programmer configurations