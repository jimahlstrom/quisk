MODULE_NAME := soapy
OBJDIR := ../build/local

CFLAGS			:= $(shell python3 ../makefil.py --cflags)
LDFLAGS			:= $(shell python3 ../makefil.py --ldflags)
EXTENSION_SUFFIX	:= $(shell python3 ../makefil.py --suffix)
TARGET := $(MODULE_NAME)$(EXTENSION_SUFFIX)

# Default target: Link the shared library object
$(TARGET): soapy.o $(OBJDIR)/import_quisk_api.o
	$(CC) -o $@ soapy.o $(LDFLAGS) -lSoapySDR $(OBJDIR)/import_quisk_api.o

# Compile the C source files into object files
soapy.o: soapy.c ../quisk.h ../filter.h
	$(CC) -o $@ -c $< $(CFLAGS) -I..

.PHONY: clean
clean:
	-rm -f  *.o 
	-rm -f  $(MODULE_NAME)*.so

.PHONY: soapy-old
soapy-old:
	python3 setup.py build_ext --force --inplace
