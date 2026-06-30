MODULE_NAME := perseus
OBJDIR := ../build/local

CFLAGS			:= $(shell python3 ../makefil.py --cflags)
LDFLAGS			:= $(shell python3 ../makefil.py --ldflags)
EXTENSION_SUFFIX	:= $(shell python3 ../makefil.py --suffix)
TARGET := $(MODULE_NAME)$(EXTENSION_SUFFIX)

# Default target: Link the shared library object
$(TARGET): perseus.o $(OBJDIR)/import_quisk_api.o
	$(CC) -o $@ perseus.o $(LDFLAGS) $(OBJDIR)/import_quisk_api.o

# Compile the C source files into object files
perseus.o: perseus.c ../quisk.h ../filter.h
	$(CC) -o $@ -c $< $(CFLAGS) -I..

.PHONY: clean
clean:
	-rm -f  *.o 
	-rm -f  $(TARGET)

.PHONY: perseus-old
perseus-old:
	python3 setup.py build_ext --force --inplace

