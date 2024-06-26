# Required variables used to drive the compilation process. It is OK
# for many of these to be empty.
#
# The library names associated with .a files output that are linked
# (via, for example, -lcxx) into dependents. This list should be
# "cxx" for output files such as libcxx.a.
LIBRARY_OUTPUT = supc++ stdc++
# The .o files that are mandatorily linked into dependents. This is
# rarely used, and only when normal .a linking rules will avoid
# linking some necessary objects. This list is of names (for example,
# cxx) which will generate cxx.lib.o. Do NOT include the list of .o
# files here. Please note that using this list is *very rare* and
# should only be used when the .a support above is not appropriate.
OBJECT_OUTPUT =
# The path within this directory that holds the .h files for
# dependents to compile with (./ by default). Will be fed into the -I
# compiler arguments.
INCLUDE_PATHS = . include/
# The interfaces this component is dependent on for compilation (this
# is a list of directory names in interface/)
INTERFACE_DEPENDENCIES =
# The library dependencies this component is reliant on for
# compilation/linking (this is a list of directory names in lib/)
LIBRARY_DEPENDENCIES = component
# Note: Both the interface and library dependencies should be
# *minimal*. That is to say that removing a dependency should cause
# the build to fail. The build system does not validate this
# minimality; that's on you!

# There are two different *types* of Makefiles for libraries.
# 1. Those that are Composite-specific, and simply need an easy way to
#    compile and itegrate their code.
# 2. Those that aim to integrate external libraries into
#    Composite. These focus on "driving" the build process of the
#    external library, then pulling out the resulting files and
#    directories. These need to be flexible as all libraries are
#    different.

# Type 1, Composite library: This is the default Makefile for
# libraries written for composite. Get rid of this if you require a
# custom Makefile (e.g. if you use an existing
# (non-composite-specific) library. An example of this is `kernel`.

## Type 2, external library: If you need to specialize the Makefile
## for an external library, you can add the external code as a
## subdirectory, and drive its compilation, and integration with the
## system using a specialized Makefile. The Makefile must generate
## lib$(LIBRARY_OUTPUT).a and $(OBJECT_OUTPUT).lib.o, and have all of
## the necessary include paths in $(INCLUDE_PATHS).
##
## To access the Composite Makefile definitions, use the following. An
## example of a Makefile written in this way is in `ps/`.
#
# include Makefile.src Makefile.comp Makefile.dependencies
# .PHONY: all clean init distclean
## Fill these out with your implementation
# all:
# clean:
#
## Default rules:
# init: clean all
# distclean: clean

.PHONY: all clean distclean init i386_init x86_64_init

all:
clean:

libsupc++.a:
	@make -C ./libstdc++-v3-4.8/libsupc++

libstdc++.a:
	@make -C ./libstdc++-v3-4.8

distclean: clean
	@make -s -C ./libstdc++-v3-4.8/libsupc++ clean
	@make -C ./libstdc++-v3-4.8 clean
	rm -f *.o *.a

i386_init:
	$(info Building libcxx for i386...)
#	make LIBCXX_ARCH=-m32 -C ./libstdc++-v3-4.8/libsupc++
# This breaks in the current build, so we'd need to debug/update if we want to support c++.
#	make LIBCXX_ARCH=-m32 -C ./libstdc++-v3-4.8

x86_64_init:
	$(info Building libcxx for x86_64...NOT CURRENTLY SUPPORTED)
#	make LIBCXX_ARCH=-m64 -C ./libstdc++-v3-4.8/libsupc++
# This breaks in the current build, so we'd need to debug/update if we want to support c++.
#	make LIBCXX_ARCH=-m64 -C ./libstdc++-v3-4.8

armv7a_init:
	$(info Building libcxx for armv7-a...)
	$(info This is not supported currently, abort...)
	# make CXX=arm-none-eabi-g++  -C ./libstdc++-v3-4.8/libsupc++
	# make CXX=arm-none-eabi-g++  -C ./libstdc++-v3-4.8

init: $(PLATFORM)_init
