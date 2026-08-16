"""Put the MSYS2 mingw64 toolchain on PATH for the `sim` environment.

PlatformIO's `native` platform builds with whatever `gcc` it finds on PATH, and
MSYS2 deliberately does not add itself there -- it would shadow system tools in
every other shell. Rather than requiring the developer to remember to launch VS
Code from a MinGW shell (and getting a build that works for one person and not
another), the environment is fixed up here, where it is version controlled and
visible.

The same directory holds the runtime DLLs -- SDL2.dll, libstdc++, libwinpthread
-- so this also makes `-t exec` able to actually start the program. A build that
links and then dies with "SDL2.dll not found" is the failure this avoids.

Set MSYS2_ROOT in the environment to override the default location.
"""

import os

Import("env")  # noqa: F821  (injected by SCons)

MSYS2_ROOT = os.environ.get("MSYS2_ROOT", r"C:\msys64")
MINGW_BIN = os.path.join(MSYS2_ROOT, "mingw64", "bin")

if os.path.isdir(MINGW_BIN):
    env.PrependENVPath("PATH", MINGW_BIN)
    print("sim: using toolchain at %s" % MINGW_BIN)
else:
    # Loud, and phrased as the fix rather than the symptom. Without this the
    # failure is SCons reporting that it cannot find `gcc`, which does not
    # suggest "install the MinGW packages" to anyone who has not done it before.
    print("sim: WARNING - %s not found." % MINGW_BIN)
    print("sim: install the toolchain with:")
    print("sim:   winget install --id MSYS2.MSYS2 -e")
    print("sim:   C:\\msys64\\usr\\bin\\bash -lc "
          "'pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2'")
