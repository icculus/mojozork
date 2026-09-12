# A second build system, for one platform only.
#
# Everything else the libretro buildbot builds goes through CMake, and so does
# every other consumer of this repository. PlayStation 3 is the exception: it
# is the one libretro target whose build template has no CMake variant - the
# infrastructure offers ctr-static-cmake, vita-static-cmake, wii-static-cmake
# and so on, but for psl1ght only a Makefile-based one. Without this file that
# job fails before it starts, with "make: Makefile: No such file or directory".
#
# So this is deliberately not a general build system and should not grow into
# one: CMakeLists.txt remains the place where the core is built. The core is a
# single translation unit - mojozork-libretro.c includes mojozork.c - which is
# what keeps this small enough to be worth having.

TARGET_NAME := mojozork
SOURCES_C   := mojozork-libretro.c

ifeq ($(platform),)
   platform = unix
endif

ifeq ($(platform), psl1ght)
   TARGET  := $(TARGET_NAME)_libretro_psl1ght.a
   CC       = $(PS3DEV)/ppu/bin/ppu-gcc$(EXE_EXT)
   AR       = $(PS3DEV)/ppu/bin/ppu-ar$(EXE_EXT)
   CFLAGS  += -D__PSL1GHT__ -D__PS3__ -DMSB_FIRST=1
   STATIC_LINKING = 1
else
   # Not what the buildbot uses - it is here so this file can be exercised on a
   # desktop, since the PS3 toolchain is not something most people have.
   TARGET  := $(TARGET_NAME)_libretro.so
   fpic    := -fPIC
   SHARED  := -shared
endif

OBJECTS := $(SOURCES_C:.c=.o)
CFLAGS  += -O2 -Wall

all: $(TARGET)

ifeq ($(STATIC_LINKING), 1)
$(TARGET): $(OBJECTS)
	$(AR) rcs $@ $(OBJECTS)
else
$(TARGET): $(OBJECTS)
	$(CC) $(fpic) $(SHARED) -o $@ $(OBJECTS) $(LDFLAGS) -lm
endif

%.o: %.c
	$(CC) $(CFLAGS) $(fpic) -c -o $@ $<

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: all clean
