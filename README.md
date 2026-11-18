# Dynamic Bottle Liquids SE

Dynamic Bottle Liquids SE animates liquid inside Skyrim Special Edition bottles. Tilt or shake a supported bottle to see the liquid shift, form waves and carry bubbles to the surface.

**Version 1.0.0** supports the Steam runtimes 1.5.97, 1.6.1170 and 1.7.104.

**[Download the mod and compatibility patches on Nexus Mods](https://www.nexusmods.com/games/skyrimspecialedition/mods/199666)**

Install the runtime with the matching SKSE and Address Library. Install compatibility patches after DBL and the bottle replacers they target.

## Building the runtime

Use 64-bit MSVC with C++23, CMake 3.25 or newer, Git, and vcpkg. Configure with the vcpkg toolchain and build the Release configuration.

~~~powershell
cmake -S . -B build -A x64 "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
python compile_shaders.py
~~~

The SKSE DLL keeps the filename SkyrimBottlePrototype.dll for compatibility with the installed mod. The Nexus download supplies the bottle assets, profiles and installer.

## License

The runtime and shader source are distributed under the [GNU General Public License, version 3](LICENSE).
