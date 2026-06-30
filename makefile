# This makefile replaces the old Python "setup.py build_ext" which has been deprecated.
# See "quisk-old" below to make Quisk using the old setup.py.

MODULE_NAME := _quisk
OBJDIR := build/local
SUBDIRS := afedrinet

ifeq ($(shell python3 makefil.py --soapy), 1)
SUBDIRS := $(SUBDIRS) soapypkg
endif

SRCS := \
base64.c \
extdemod.c \
filter.c \
freedv.c \
handshake.c \
is_key_down.c \
microphone.c \
quisk.c \
quisk_wdsp.c \
sha1.c \
sound.c\
sound_alsa.c \
sound_directx.c \
sound_portaudio.c \
sound_pulseaudio.c \
sound_wasapi.c \
tci.c \
utf8.c \
utility.c \
ws.c

CFLAGS			:= $(shell python3 makefil.py --cflags)
LDFLAGS			:= $(shell python3 makefil.py --ldflags)
EXTENSION_SUFFIX	:= $(shell python3 makefil.py --suffix)
TARGET := $(MODULE_NAME)$(EXTENSION_SUFFIX)

#$(info Info CFLAGS $(CFLAGS))
#$(info Info LDFLAGS $(LDFLAGS))
#$(info Info TARGET $(TARGET))
#$(info Info SUBDIRS $(SUBDIRS))

OBJS := $(patsubst %.c, $(OBJDIR)/%.o, $(SRCS))

.PHONY: all clean test
.PHONY: $(SUBDIRS)

# Default target
all: $(OBJDIR) $(OBJDIR)/import_quisk_api.o ac2yd/remote.o $(TARGET) $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@

$(OBJDIR):
	@mkdir -p $(OBJDIR)

# Link the shared library object
$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) ac2yd/remote.o $(LDFLAGS)

# Define the location for dependency files
DEPS = $(patsubst %.o,$(OBJDIR)/%.d,$(notdir $(OBJS)))
DEPFLAGS = -MMD -MP -MF $(OBJDIR)/$(notdir $*).d
-include $(DEPS)

# Compile the C source files into object files
ac2yd/remote.o:	ac2yd/remote.c quisk.h filter.h
	$(CC) -o $@ -c $< $(CFLAGS) $(DEPFLAGS)

$(OBJDIR)/import_quisk_api.o: import_quisk_api.c
	$(CC) -o $@ -c $< $(CFLAGS) $(DEPFLAGS)

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	$(CC) -o $@ -c $< $(CFLAGS) $(DEPFLAGS)

clean:
	-rm -f  $(OBJDIR)/*.o $(OBJDIR)/*.d
	-rm -f	ac2yd/*.o
	-rm -f	$(MODULE_NAME)*.so
	for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir clean; \
	done

# Run a test script to verify that the module can be imported
test: $(TARGET)
	@python3 -c "import $(MODULE_NAME); print('Successfully imported:', $(MODULE_NAME).__file__)"



.PHONY: quisk-old
quisk-old:
	python3 setup.py build_ext --force --inplace
