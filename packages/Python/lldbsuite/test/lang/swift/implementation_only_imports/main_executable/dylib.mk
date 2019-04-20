LEVEL = ../../../../make
DYLIB_ONLY := YES
DYLIB_NAME := $(BASENAME)
DYLIB_SWIFT_SOURCES := $(DYLIB_NAME).swift
# Disable debug info for the library.
SWIFTFLAGS := -gnone

include $(LEVEL)/Makefile.rules
