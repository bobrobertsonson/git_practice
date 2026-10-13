# RASP firmware for the Daisy Seed.
# The program (~146 KB) is bigger than the Seed's 128 KB internal flash, so it
# uses Electrosmith's bootloader: stored in the 8 MB QSPI flash, run from SRAM.
#   make LIBDAISY_DIR=/path/to/libDaisy                    build
#   make LIBDAISY_DIR=/path/to/libDaisy program-boot       once: install the bootloader (USB, BOOT+RESET)
#   make LIBDAISY_DIR=/path/to/libDaisy program-dfu        flash RASP (press RESET, then within 2 s run this)
TARGET = rasp
APP_TYPE = BOOT_SRAM
CPP_SOURCES = main.cpp
LIBDAISY_DIR ?= ../../libDaisy
OPT = -O2
# make LEAN=1: the under-$300 build with 8 knobs (BUILD.md, "Lean build")
ifeq ($(LEAN),1)
C_DEFS = -DRASP_LEAN
endif
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
