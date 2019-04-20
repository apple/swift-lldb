LEVEL = ../../../../make

EXE=a.out
SWIFT_SOURCES=main.swift
SWIFTFLAGS_EXTRAS= -I$(shell pwd)
MODULENAME=main
LD_EXTRAS=-L$(shell pwd) -lSomeLibrary

all: SomeLibrary.swiftmodule a.out

include $(LEVEL)/Makefile.rules

SomeLibrary.swiftmodule: SomeLibrary.swift
	$(MAKE) MAKE_DSYM=NO CC=$(CC) SWIFTC=$(SWIFTC) \
		ARCH=$(ARCH) DSYMUTIL=$(DSYMUTIL) \
		BASENAME=$(shell basename $< .swift) \
		VPATH=$(SRCDIR) -I $(SRCDIR) -f $(SRCDIR)/dylib.mk all

clean::
	rm -rf main *.dylib *.so *.dll *.dSYM *.swiftdoc *.swiftmodule *.swiftinterface

