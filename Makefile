##===- libepid/Makefile ------------------------------------*- Makefile -*-===##
#
# This is the main makefile for libepid.
#
##===----------------------------------------------------------------------===##

#
# Indicates our relative path to the top of the project's root directory.
#
LEVEL = .
DIRS = lib tools
EXTRA_DIST = include

#
# Include the Master Makefile that knows how to build all.
#
include $(LEVEL)/Makefile.common

