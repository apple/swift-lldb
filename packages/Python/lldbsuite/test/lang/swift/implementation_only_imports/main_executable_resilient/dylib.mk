LEVEL = ../../../../make
DYLIB_ONLY := YES
DYLIB_NAME := $(BASENAME)
DYLIB_SWIFT_SOURCES := $(DYLIB_NAME).swift
# Disable debug info for the library.
SWIFTFLAGS := -gnone -enable-library-evolution

include $(LEVEL)/Makefile.rules
