# Build with the MSYS2 MINGW64 toolchain:
#   pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-libvterm
#   make            (or mingw32-make)

CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter

all: wterm-module.dll wterm-conpty.exe

# Windows won't overwrite a DLL or exe that is in use (Emacs has the module
# loaded, running terminals use the bridge), but it does allow renaming one.
# Move the old file aside so the build succeeds; the module is picked up
# after restarting Emacs, the bridge by the next terminal.
MOVE_ASIDE = if [ -f $@ ]; then rm -f $@.old* 2>/dev/null; \
	mv -f $@ $@.old.$$$$ 2>/dev/null || true; fi

# libvterm is linked statically so the module has no extra DLL dependency.
wterm-module.dll: src/wterm-module.c
	@$(MOVE_ASIDE)
	$(CC) $(CFLAGS) -shared -o $@ $< -l:libvterm.a -static-libgcc

wterm-conpty.exe: src/wterm-conpty.c
	@$(MOVE_ASIDE)
	$(CC) $(CFLAGS) -o $@ $< -lshell32 -static-libgcc

clean:
	rm -f *.dll *.exe *.old.*

.PHONY: all clean
