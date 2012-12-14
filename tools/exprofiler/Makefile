##===- libepid/tools/exprofiler/Makefile ---------------*- Makefile -*-===##

#
# Indicate where we are relative to the top of the source tree.
#
LEVEL=../..

#
# Give the name of the tool.
#
TOOLNAME=exprofiler

#
# List libraries that we'll need
#
USEDLIBS = epid.a

LINK_COMPONENTS = linker bitreader bitwriter asmparser ipo

#
# Enable C++0x
#
CPPFLAGS += -std=c++11

#
# Include Makefile.common so we know what to do.
#
include $(LEVEL)/Makefile.common

