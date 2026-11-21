# RebaneOS uses only its native device surface and the library RAM surface.
DIR_SOURCES := surface.c ram.c rebane.c
include $(NSBUILD)/Makefile.subdir
