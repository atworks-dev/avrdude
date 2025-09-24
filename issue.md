# GPIO Modernization for linuxspi on Raspberry Pi

## Problem Description

The current `linuxspi.c` implementation uses the deprecated sysfs GPIO interface (`/sys/class/gpio/`) which is incompatible with newer Linux kernels on Raspbian 12 (Bookworm). This affects GPIO24 reset control on Raspberry Pi 3.

## Target Environment

- **OS**: Raspbian GNU/Linux 12 (bookworm)
- **Hardware**: Raspberry Pi 3
- **GPIO Chip**: gpiochip0
- **Reset Pin**: GPIO24
- **Programming Interface**: linuxspi (SPI + GPIO reset)

## Current Implementation Issues

The following functions in `avrdude/linuxspi.c` use deprecated sysfs GPIO:

1. `linuxspi_gpio_op_wr()` - lines 143-195
   - Uses `/sys/class/gpio/export` 
   - Uses `/sys/class/gpio/gpio{N}/direction`
   - Uses `/sys/class/gpio/gpio{N}/value`

2. `linuxspi_open()` - lines 212-249
   - Exports GPIO via sysfs
   - Sets direction to output

3. `linuxspi_close()` - lines 251-262
   - Unexports GPIO via sysfs

## Required Changes

### 1. Create New Branch
```bash
git checkout -b gpio-modernization
```

### 2. Update Build System

Add to `configure.ac`:
```autoconf
# Check for libgpiod
PKG_CHECK_MODULES([LIBGPIOD], [libgpiod >= 2.0], [have_libgpiod=yes], [have_libgpiod=no])
if test "$have_libgpiod" = "yes"; then
    AC_DEFINE([HAVE_LIBGPIOD], [1], [Define if libgpiod is available])
fi
AM_CONDITIONAL([HAVE_LIBGPIOD], [test "$have_libgpiod" = "yes"])
```

### 3. Modify linuxspi.c

#### Add New Headers
```c
#if HAVE_LIBGPIOD
#include <gpiod.h>
#endif
```

#### Update pdata Structure
```c
struct pdata
{
    unsigned int speedHz;
#if HAVE_LIBGPIOD
    struct gpiod_chip *gpio_chip;
    struct gpiod_line_request *gpio_request;
#endif
};
```

#### Replace linuxspi_gpio_op_wr() Function

Create new function `linuxspi_gpio_modern()` that:
1. **Primary**: Uses libgpiod v2 API with gpiochip0
2. **Fallback**: Uses legacy sysfs interface
3. **Error handling**: Provides clear error messages

Implementation approach:
```c
static int linuxspi_gpio_modern(PROGRAMMER* pgm, int gpio_num, int value)
{
#if HAVE_LIBGPIOD
    // Try libgpiod first
    struct gpiod_chip *chip = gpiod_chip_open("/dev/gpiochip0");
    if (chip) {
        // Use libgpiod v2 API
        // gpiod_line_request_set_values(), etc.
        return 0;
    }
#endif
    // Fallback to sysfs
    return linuxspi_gpio_op_wr_legacy(pgm, gpio_num, value);
}
```

#### Update linuxspi_open() Function

Replace GPIO setup:
```c
// Modern GPIO initialization for GPIO24
if (linuxspi_gpio_init(pgm, 24) < 0) {
    fprintf(stderr, "%s: error: Failed to initialize GPIO24\n", progname);
    return -1;
}

// Set GPIO24 as output, initial state LOW (reset active)
if (linuxspi_gpio_set_direction_value(pgm, 24, "out", 0) < 0) {
    return -1;
}
```

#### Update linuxspi_close() Function

Add proper cleanup:
```c
#if HAVE_LIBGPIOD
if (pdata->gpio_request) {
    gpiod_line_request_release(pdata->gpio_request);
    pdata->gpio_request = NULL;
}
if (pdata->gpio_chip) {
    gpiod_chip_close(pdata->gpio_chip);
    pdata->gpio_chip = NULL;
}
#endif
```

### 4. Implementation Details

#### libgpiod v2 API Usage
```c
// Open chip
struct gpiod_chip *chip = gpiod_chip_open("/dev/gpiochip0");

// Configure line for output
struct gpiod_line_settings *settings = gpiod_line_settings_new();
gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_INACTIVE);

// Create line config
struct gpiod_line_config *config = gpiod_line_config_new();
gpiod_line_config_add_line_settings(config, &gpio_num, 1, settings);

// Request line
struct gpiod_line_request *request = gpiod_chip_request_lines(chip, NULL, config);
```

#### Reset Control
```c
// Assert reset (LOW)
gpiod_line_request_set_value(request, gpio_num, GPIOD_LINE_VALUE_ACTIVE);

// Release reset (HIGH) 
gpiod_line_request_set_value(request, gpio_num, GPIOD_LINE_VALUE_INACTIVE);
```

### 5. Testing Requirements

1. **Verify GPIO24 control**:
   ```bash
   # Test with actual AVR device
   avrdude -c linuxspi -P /dev/spidev0.0 -p atmega328p -U flash:r:test.hex:i
   ```

2. **Test fallback mechanism**:
   - Test with libgpiod available
   - Test with libgpiod unavailable (fallback to sysfs)

3. **Verify no regressions**:
   - Test on older systems
   - Verify SPI communication still works

### 6. Update Documentation

Update `CLAUDE.md`:
```markdown
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
```

### 7. Expected Behavior

After implementation:
- GPIO24 on Raspberry Pi 3 controls AVR reset
- Uses `/dev/gpiochip0` character device
- Maintains backward compatibility
- Clear error messages for GPIO failures
- Proper resource cleanup

### 8. Files to Modify

1. `avrdude/configure.ac` - Add libgpiod detection
2. `avrdude/linuxspi.c` - Implement modern GPIO control
3. `avrdude/linuxspi.h` - Add any new function declarations if needed
4. `CLAUDE.md` - Update build documentation

### 9. Compilation Commands

```bash
# After modifications
./bootstrap
./configure
make
make install
```

### 10. Validation

Successful implementation should:
1. Compile without warnings
2. Detect and use libgpiod if available
3. Fall back to sysfs if needed  
4. Successfully program AVR devices via SPI
5. Properly control GPIO24 reset on Raspberry Pi 3