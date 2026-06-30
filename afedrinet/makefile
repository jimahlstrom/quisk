MODULE_NAME := afedrinet_io
OBJDIR := ../build/local

CFLAGS			:= $(shell python3 ../makefil.py --cflags)
LDFLAGS			:= $(shell python3 ../makefil.py --ldflags)
EXTENSION_SUFFIX	:= $(shell python3 ../makefil.py --suffix)
TARGET := $(MODULE_NAME)$(EXTENSION_SUFFIX)

# Default target: Link the shared library object
$(TARGET): afedrinet_io.o $(OBJDIR)/import_quisk_api.o $(OBJDIR)/is_key_down.o
	$(CC) -o $@ afedrinet_io.o $(LDFLAGS) $(OBJDIR)/import_quisk_api.o $(OBJDIR)/is_key_down.o

# Compile the C source files into object files
afedrinet_io.o: afedrinet_io.c ../quisk.h
	$(CC) -o $@ -c $< $(CFLAGS) -I..

.PHONY: clean
clean:
	-rm -f  *.o 
	-rm -f  $(MODULE_NAME)*.so

.PHONY: afedrinet-old
afedrinet-old:
	python3 setup.py build_ext --force --inplace
