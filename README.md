# Embedded Project - Standalone STM32F429ZI LCD

This project is a firmware for the STM32F429ZI (usually found on the STM32F429I-DISCO board) to drive the onboard LCD.
# <span style="color:red">Current Issue</span>
1. Not able to send the data to the board the python script is stuck after sending and not able to send the data onto the ACM port.
2. <span style="color:red">Urgent FIX needed.</span>
## Prerequisites

To build and flash this project, you need the ARM bare-metal toolchain and OpenOCD.

### Installing Dependencies (Linux / Ubuntu/Debian)

```bash
# Install the ARM GCC toolchain and build tools
sudo apt update
sudo apt install gcc-arm-none-eabi binutils-arm-none-eabi build-essential git

# Install OpenOCD for flashing the board
sudo apt install openocd
```

### Initializing the Project

This project uses `libopencm3` as a submodule. Before building for the first time, ensure submodules are initialized:

```bash
git submodule update --init
```

_(Note: The provided Makefile will attempt to auto-initialize `libopencm3` if the directory is missing.)_

## Building

To build the firmware, simply run:

```bash
make
```

This will automatically build `libopencm3` (if not already built) and then compile the project, generating `my_lcd_firmware.elf` and `my_lcd_firmware.bin`.

## Flashing

Connect your STM32F429I-DISCOVERY board via USB and run:

```bash
make flash
```

This will use `openocd` to flash the firmware onto the board.

## Clean

To clean the project objects and binaries:
```bash
make clean
```
