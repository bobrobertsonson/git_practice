# RASP bench firmware for the Daisy Pod. Same build/flash steps as ../Makefile:
#   make LIBDAISY_DIR=/path/to/libDaisy && make LIBDAISY_DIR=... program-boot (once) && ... program-dfu
TARGET = rasp_pod
CPP_SOURCES = main.cpp
LIBDAISY_DIR ?= ../../../libDaisy
APP_TYPE = BOOT_SRAM
OPT = -O2
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
