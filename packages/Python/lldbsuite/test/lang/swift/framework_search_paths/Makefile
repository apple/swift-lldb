LEVEL = ../../../make

SWIFT_SOURCES := main.swift

include $(LEVEL)/Makefile.rules

_framework:
	xcodebuild -configuration Release -project Framework/Framework.xcodeproj CONFIGURATION_BUILD_DIR=$(PWD)

cleanup:
	rm -rf Framework/build *.framework *.dSYM
