/*
 * avrdude - A Downloader/Uploader for AVR device programmers
 * Support for using spidev userspace drivers to communicate directly over SPI
 * 
 * Copyright (C) 2013 Kevin Cuzner <kevin@kevincuzner.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 * 
 * Support for inversion of reset pin, Tim Chilton 02/05/2014
 */
 
#include "linuxspi.h"

#include "ac_cfg.h"

#include "avrdude.h"
#include "avr.h"
#include "pindefs.h"

#if HAVE_SPIDEV

/**
 * Linux Kernel SPI Drivers
 * 
 * Copyright (C) 2006 SWAPP
 *      Andrea Paterniani <a.paterniani@swapp-eng.it>
 * Copyright (C) 2007 David Brownell (simplification, cleanup)
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <linux/spi/spidev.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if HAVE_LIBGPIOD
#include <gpiod.h>
#endif

/**
 * Data for the programmer
 */

struct pdata
{
    unsigned int speedHz;
#if HAVE_LIBGPIOD
    struct gpiod_chip *gpio_chip;
    struct gpiod_line *gpio_line;
    int used_libgpiod;  /* Flag to track if libgpiod was used */
#endif
};

typedef enum {
    LINUXSPI_GPIO_DIRECTION,
    LINUXSPI_GPIO_VALUE,
    LINUXSPI_GPIO_EXPORT,
    LINUXSPI_GPIO_UNEXPORT
} LINUXSPI_GPIO_OP;

#define PDATA(pgm) ((struct pdata *)(pgm->cookie))
#define IMPORT_PDATA(pgm) struct pdata *pdata = PDATA(pgm)

/**
 * Function Prototypes
 */

//linuxspi specific functions
static int linuxspi_spi_duplex(PROGRAMMER* pgm, unsigned char* tx, unsigned char* rx, int len);
static int linuxspi_gpio_op_wr(PROGRAMMER* pgm, LINUXSPI_GPIO_OP op, int gpio, char* val);
static int linuxspi_gpio_modern(PROGRAMMER* pgm, int gpio_num, int value);
static int linuxspi_gpio_op_wr_legacy(PROGRAMMER* pgm, LINUXSPI_GPIO_OP op, int gpio, char* val);
static int linuxspi_gpio_init(PROGRAMMER* pgm, int gpio_num);
static int linuxspi_gpio_set_direction_value(PROGRAMMER* pgm, int gpio_num, const char* direction, int value);
//interface - management
static void linuxspi_setup(PROGRAMMER* pgm);
static void linuxspi_teardown(PROGRAMMER* pgm);
//interface - prog
static int linuxspi_open(PROGRAMMER* pgm, char* port);
static void linuxspi_close(PROGRAMMER* pgm);
// dummy functions
static void linuxspi_disable(PROGRAMMER * pgm);
static void linuxspi_enable(PROGRAMMER * pgm);
static void linuxspi_display(PROGRAMMER * pgm, const char * p);
//universal
static int linuxspi_initialize(PROGRAMMER* pgm, AVRPART* p);
// SPI specific functions
static int linuxspi_cmd(PROGRAMMER * pgm, unsigned char cmd[4], unsigned char res[4]);
static int linuxspi_program_enable(PROGRAMMER * pgm, AVRPART * p);
static int linuxspi_chip_erase(PROGRAMMER * pgm, AVRPART * p);

/**
 * @brief Sends/receives a message in full duplex mode
 * @return -1 on failure, otherwise number of bytes sent/recieved
 */
static int linuxspi_spi_duplex(PROGRAMMER* pgm, unsigned char* tx, unsigned char* rx, int len)
{
    int fd = open(pgm->port, O_RDWR);
    if (fd < 0)
    {
        fprintf(stderr, "\n%s: error: Unable to open SPI port %s", progname, pgm->port);
        return -1; //error
    }
    
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)tx,
        .rx_buf = (unsigned long)rx,
        .len = len,
        .delay_usecs = 1,
        //should settle around 400Khz, a standard SPI speed. Adjust using baud parameter (-b)
        .speed_hz = pgm->baudrate == 0 ? 400000 : pgm->baudrate,
        .bits_per_word = 8,
    };
    
    int ret = ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
    close(fd);
    
    if (ret != len)
    {
        fprintf(stderr, "\n%s: error: Unable to send SPI message\n", progname);
        return -1;
    }
    
    return ret;
}

/**
 * @brief Legacy GPIO operation using sysfs interface.
 * @param op Operation to perform
 * @param gpio 
 * @return -1 if failed, 0 otherwise
 */
static int linuxspi_gpio_op_wr_legacy(PROGRAMMER* pgm, LINUXSPI_GPIO_OP op, int gpio, char* val)
{
    char* fn = malloc(PATH_MAX); //filename
    gpio &= ~PIN_INVERSE; // Remove the inversion flag

    switch(op)
    {
        case LINUXSPI_GPIO_DIRECTION:
            sprintf(fn, "/sys/class/gpio/gpio%d/direction", gpio);
            break;
        case LINUXSPI_GPIO_EXPORT:
            sprintf(fn, "/sys/class/gpio/export");
            break;
        case LINUXSPI_GPIO_UNEXPORT:
            sprintf(fn, "/sys/class/gpio/unexport");
            break;
        case LINUXSPI_GPIO_VALUE:
            sprintf(fn, "/sys/class/gpio/gpio%d/value", gpio);
            break;
        default:
            fprintf(stderr, "%s: linuxspi_gpio_op_wr(): Unknown op %d", progname, op);
            return -1;
    }
    
    FILE* f = fopen(fn, "w");
    
    int fopen_retries = 0;
    while (!f && (fopen_retries < 100))
    {
        usleep(20000);
        f = fopen(fn, "w");
        fopen_retries++;
    }

    if (!f)
    {
        fprintf(stderr, "%s: linuxspi_gpio_op_wr(): Unable to open file %s", progname, fn);
        free(fn); //we no longer need the path
        return -1;
    }
    
    if (fprintf(f, val) < 0)
    {
        fprintf(stderr, "%s: linuxspi_gpio_op_wr(): Unable to write file %s with %s", progname, fn, val);
        free(fn); //we no longer need the path
        return -1;
    }
    
    fclose(f);
    free(fn); //we no longer need the path
    
    return 0;
}

/**
 * @brief Initialize GPIO using modern libgpiod interface
 * @param pgm Programmer data
 * @param gpio_num GPIO number
 * @return -1 if failed, 0 otherwise
 */
static int linuxspi_gpio_init(PROGRAMMER* pgm, int gpio_num)
{
    IMPORT_PDATA(pgm);
    
#if HAVE_LIBGPIOD
    // Try libgpiod v1.6 API first
    pdata->gpio_chip = gpiod_chip_open("/dev/gpiochip0");
    if (pdata->gpio_chip) {
        unsigned int gpio_offset = gpio_num & ~PIN_INVERSE;
        pdata->gpio_line = gpiod_chip_get_line(pdata->gpio_chip, gpio_offset);
        
        if (pdata->gpio_line) {
            // Request line as output with initial value LOW (reset active)
            int ret = gpiod_line_request_output(pdata->gpio_line, "avrdude-linuxspi", 0);
            if (ret == 0) {
                fprintf(stderr, "%s: info: Using libgpiod for GPIO%d control\n", progname, gpio_offset);
                pdata->used_libgpiod = 1;
                return 0;
            } else {
                fprintf(stderr, "%s: error: Failed to request GPIO%d as output\n", progname, gpio_offset);
                pdata->gpio_line = NULL;
            }
        } else {
            fprintf(stderr, "%s: error: Failed to get GPIO%d line\n", progname, gpio_offset);
        }
        
        gpiod_chip_close(pdata->gpio_chip);
        pdata->gpio_chip = NULL;
    }
#endif
    
    fprintf(stderr, "%s: info: libgpiod not available, falling back to sysfs GPIO\n", progname);
    return 0; // fallback to legacy will be handled in gpio operations
}

/**
 * @brief Set GPIO direction and initial value using modern interface
 * @param pgm Programmer data  
 * @param gpio_num GPIO number
 * @param direction "in" or "out"
 * @param value Initial value for output (0 or 1)
 * @return -1 if failed, 0 otherwise
 */
static int linuxspi_gpio_set_direction_value(PROGRAMMER* pgm, int gpio_num, const char* direction, int value)
{
    IMPORT_PDATA(pgm);
    
#if HAVE_LIBGPIOD
    if (pdata->gpio_line) {
        // Using libgpiod - direction was set during init, just set value
        int gpio_value = (gpio_num & PIN_INVERSE) ? !value : value;
        
        if (gpiod_line_set_value(pdata->gpio_line, gpio_value) == 0) {
            return 0;
        } else {
            fprintf(stderr, "%s: error: Failed to set GPIO%d value via libgpiod\n", progname, gpio_num & ~PIN_INVERSE);
            return -1;
        }
    }
#endif
    
    // Fallback to legacy sysfs interface
    char* buf = malloc(32);
    sprintf(buf, "%d", gpio_num & ~PIN_INVERSE);
    
    // Export GPIO
    if (linuxspi_gpio_op_wr_legacy(pgm, LINUXSPI_GPIO_EXPORT, gpio_num, buf) < 0) {
        free(buf);
        return -1;
    }
    
    // Set direction with initial value
    const char* dir_value = (strcmp(direction, "out") == 0) ? 
                           (gpio_num & PIN_INVERSE ? (value ? "low" : "high") : (value ? "high" : "low")) : 
                           "in";
    
    if (linuxspi_gpio_op_wr_legacy(pgm, LINUXSPI_GPIO_DIRECTION, gpio_num, (char*)dir_value) < 0) {
        free(buf);
        return -1;
    }
    
    free(buf);
    return 0;
}

/**
 * @brief Modern GPIO control function with libgpiod and sysfs fallback
 * @param pgm Programmer data
 * @param gpio_num GPIO number  
 * @param value Value to set (0 or 1)
 * @return -1 if failed, 0 otherwise
 */
static int linuxspi_gpio_modern(PROGRAMMER* pgm, int gpio_num, int value)
{
    IMPORT_PDATA(pgm);
    
#if HAVE_LIBGPIOD
    if (pdata->gpio_line) {
        int gpio_value = (gpio_num & PIN_INVERSE) ? !value : value;
        
        if (gpiod_line_set_value(pdata->gpio_line, gpio_value) == 0) {
            return 0;
        } else {
            fprintf(stderr, "%s: error: Failed to set GPIO%d value via libgpiod\n", progname, gpio_num & ~PIN_INVERSE);
            return -1;
        }
    }
#endif
    
    // Fallback to legacy sysfs interface
    const char* val_str = (gpio_num & PIN_INVERSE) ? (value ? "0" : "1") : (value ? "1" : "0");
    return linuxspi_gpio_op_wr_legacy(pgm, LINUXSPI_GPIO_VALUE, gpio_num, (char*)val_str);
}

/**
 * @brief Wrapper function that maintains compatibility with existing interface
 * @param op Operation to perform  
 * @param gpio GPIO number
 * @param val Value string
 * @return -1 if failed, 0 otherwise
 */
static int linuxspi_gpio_op_wr(PROGRAMMER* pgm, LINUXSPI_GPIO_OP op, int gpio, char* val)
{
    // For VALUE operations, try modern GPIO first
    if (op == LINUXSPI_GPIO_VALUE) {
        int value = atoi(val);
        return linuxspi_gpio_modern(pgm, gpio, value);
    }
    
    // For other operations, use legacy interface
    return linuxspi_gpio_op_wr_legacy(pgm, op, gpio, val);
}

static void linuxspi_setup(PROGRAMMER* pgm)
{
    if ((pgm->cookie = malloc(sizeof(struct pdata))) == 0)
    {
        fprintf(stderr, "%s: linuxspi_setup(): Unable to allocate private memory.\n", progname);
        exit(1);
    }
    memset(pgm->cookie, 0, sizeof(struct pdata));
    
#if HAVE_LIBGPIOD
    IMPORT_PDATA(pgm);
    pdata->gpio_chip = NULL;
    pdata->gpio_line = NULL;
    pdata->used_libgpiod = 0;
#endif
}

static void linuxspi_teardown(PROGRAMMER* pgm)
{
    free(pgm->cookie);
}

static int linuxspi_open(PROGRAMMER* pgm, char* port)
{   
    char* buf;
    
    if (port == 0 || strcmp(port, "unknown") == 0) //unknown port
    {
        fprintf(stderr, "%s: error: No port specified. Port should point to an SPI interface.\n", progname);
        exit(1);
    }
    
    if (pgm->pinno[PIN_AVR_RESET] == 0)
    {
        fprintf(stderr, "%s: error: No pin assigned to AVR RESET.\n", progname);
        exit(1);
    }

    // Initialize GPIO for reset control
    if (linuxspi_gpio_init(pgm, pgm->pinno[PIN_AVR_RESET]) < 0)
    {
        fprintf(stderr, "%s: error: Failed to initialize GPIO%d\n", progname, pgm->pinno[PIN_AVR_RESET] & ~PIN_INVERSE);
        return -1;
    }
    
    // Set GPIO as output with initial state LOW (reset active)
    // This prevents glitches during initialization
    if (linuxspi_gpio_set_direction_value(pgm, pgm->pinno[PIN_AVR_RESET], "out", 0) < 0)
    {
        return -1;
    }
    
    //save the port to our data
    strcpy(pgm->port, port);
    
    return 0;
}

static void linuxspi_close(PROGRAMMER* pgm)
{
    IMPORT_PDATA(pgm);
    
#if HAVE_LIBGPIOD
    // Set reset to HIGH (inactive/released state) before cleanup
    if (pdata->gpio_line) {
        // Release reset by setting it HIGH (this matches the "in" behavior of sysfs)
        int release_value = (pgm->pinno[PIN_AVR_RESET] & PIN_INVERSE) ? 0 : 1;
        gpiod_line_set_value(pdata->gpio_line, release_value);
    }
    
    // Clean up libgpiod resources
    if (pdata->gpio_line) {
        gpiod_line_release(pdata->gpio_line);
        pdata->gpio_line = NULL;
    }
    if (pdata->gpio_chip) {
        gpiod_chip_close(pdata->gpio_chip);
        pdata->gpio_chip = NULL;
    }
#endif
    
    // If we used sysfs, clean it up
    if (!pdata->used_libgpiod) {
        char* buf;
        
        //set reset to input (this releases reset to HIGH state)
        linuxspi_gpio_op_wr_legacy(pgm, LINUXSPI_GPIO_DIRECTION, pgm->pinno[PIN_AVR_RESET], "in");
        
        //unexport reset
        buf = malloc(32);
        sprintf(buf, "%d", pgm->pinno[PIN_AVR_RESET] & ~PIN_INVERSE);
        linuxspi_gpio_op_wr_legacy(pgm, LINUXSPI_GPIO_UNEXPORT, pgm->pinno[PIN_AVR_RESET], buf);
        free(buf);
    }
}

static void linuxspi_disable(PROGRAMMER* pgm)
{
    //do nothing
}

static void linuxspi_enable(PROGRAMMER* pgm)
{
    //do nothing
}

static void linuxspi_display(PROGRAMMER* pgm, const char* p)
{
    //do nothing
}

static int linuxspi_initialize(PROGRAMMER* pgm, AVRPART* p)
{
    int tries, rc;
    
    if (p->flags & AVRPART_HAS_TPI)
    {
        //we do not support tpi..this is a dedicated SPI thing
        fprintf(stderr, "%s: error: Programmer %s does not support TPI\n", progname, pgm->type);
        return -1;
    }
    
    //enable programming on the part
    tries = 0;
    do
    {
        rc = pgm->program_enable(pgm, p);
        if (rc == 0 || rc == -1)
            break;
        tries++;
    }
    while(tries < 65);
    
    if (rc)
    {
        fprintf(stderr, "%s: error: AVR device not responding\n", progname);
        return -1;
    }
    
    return 0;
}

static int linuxspi_cmd(PROGRAMMER* pgm, unsigned char cmd[4], unsigned char res[4])
{
    return linuxspi_spi_duplex(pgm, cmd, res, 4);
}

static int linuxspi_program_enable(PROGRAMMER* pgm, AVRPART* p)
{
    unsigned char cmd[4];
    unsigned char res[4];
    
    if (p->op[AVR_OP_PGM_ENABLE] == NULL)
    {
        fprintf(stderr, "%s: error: program enable instruction not defined for part \"%s\"\n", progname, p->desc);
        return -1;
    }
    
    memset(cmd, 0, sizeof(cmd));
    avr_set_bits(p->op[AVR_OP_PGM_ENABLE], cmd); //set the cmd
    pgm->cmd(pgm, cmd, res);
    
    if (res[2] != cmd[1])
        return -2;
    
    return 0;
}

static int linuxspi_chip_erase(PROGRAMMER* pgm, AVRPART* p)
{
    unsigned char cmd[4];
    unsigned char res[4];
    
    if (p->op[AVR_OP_CHIP_ERASE] == NULL)
    {
        fprintf(stderr, "%s: error: chip erase instruction not defined for part \"%s\"\n", progname, p->desc);
        return -1;
    }
    
    memset(cmd, 0, sizeof(cmd));

    avr_set_bits(p->op[AVR_OP_CHIP_ERASE], cmd);
    pgm->cmd(pgm, cmd, res);
    usleep(p->chip_erase_delay);
    pgm->initialize(pgm, p);
    
    return 0;
}

void linuxspi_initpgm(PROGRAMMER * pgm)
{
    strcpy(pgm->type, "linuxspi");
    
    pgm_fill_old_pins(pgm); // TODO to be removed if old pin data no longer needed
    
    /*
     * mandatory functions
     */

    pgm->initialize     = linuxspi_initialize;
    pgm->display        = linuxspi_display;
    pgm->enable         = linuxspi_enable;
    pgm->disable        = linuxspi_disable;
    pgm->program_enable = linuxspi_program_enable;
    pgm->chip_erase     = linuxspi_chip_erase;
    pgm->cmd            = linuxspi_cmd;
    pgm->open           = linuxspi_open;
    pgm->close          = linuxspi_close;
    pgm->read_byte      = avr_read_byte_default;
    pgm->write_byte     = avr_write_byte_default;

    /*
     * optional functions
     */
    pgm->setup          = linuxspi_setup;
    pgm->teardown       = linuxspi_teardown;
}

const char linuxspi_desc[] = "SPI using Linux spidev driver";

#else

void linuxspi_initpgm(PROGRAMMER * pgm)
{
    fprintf(stderr,
      "%s: Linux SPI driver not available in this configuration\n",
      progname);
}

const char linuxspi_desc[] = "SPI using Linux spidev driver (not available)";

#endif
