# Dynamic Bottle Liquids SE

Dynamic Bottle Liquids SE animates liquid inside Skyrim Special Edition bottles. Tilt or shake a supported bottle to see the liquid shift, form waves and carry bubbles to the surface.

**Version 1.1.0** supports the Steam runtimes 1.5.97, 1.6.1170 and 1.7.104.

**[Download the mod and compatibility patches on Nexus Mods](https://www.nexusmods.com/games/skyrimspecialedition/mods/199666)**

Install the runtime with the matching SKSE and Address Library. Install compatibility patches after DBL and the bottle replacers they target.

The 1.1.0 core includes the vanilla bottle meshes based on Assorted Mesh Fixes. For vanilla bottles, leave **General Potion Replacers** on **Skip**. If you use RUSTIC ANIMATED POTIONS and POISONS, select it directly on that page.

For Unified Potions PBR, XMead PBR Clear, or Destrier Wine PBR, rerun PG Patcher after installing the DBL patch. Give the DBL meshes priority among PG Patcher's inputs, then let the generated output overwrite the DBL patch.

## What's new in 1.1.0

- Reduced shadow and caustic preparation when the native renderer can verify which lights reach a bottle.
- Refitted liquid cavities across supported bottle replacers, including TMD Winery, XMead, and ElSopa.
- Corrected ElSopa and Xtudo no-emission variants and added moving liquid to the Zerofrost models for Esbern's Potion, Sleeping Tree Sap, and Soul Husk Extract.

With Light Placer and CS Light or another configuration pack, choose **Emission** for ElSopa or Medieval Potions and **Original glow only** for Skyking. RUSTIC retains its original animated glow. Light placement depends on the enabled rules and the winning models.

## Shader compatibility

Reverse-Z detection was verified with Open Shaders 2.23.1 on Skyrim 1.6.1170 with InverseZ enabled.

## Optional performance overlay

Install [SKSE Menu Framework 3](https://www.nexusmods.com/skyrimspecialedition/mods/120352) to enable the overlay. Ctrl+F8 toggles the compact CPU/GPU display. Open **Dynamic Bottle Liquids SE / Performance** in the framework panel for detailed timings and shortcut settings. Select the shortcut button and press your preferred key combination. Press Esc to cancel. Measurements run while a DBL view is visible. The plugin writes `Dynamic Bottle Liquids SE.log` in the Skyrim Special Edition SKSE log folder.

## Rendering performance

All supported bottles, including compatibility patches, use half as many cavity trace steps as the original renderer at each distance. In one in-game test with 95 tracked bottles, the compact overlay measured 0.782 ms of CPU time and 1.733 ms of GPU time per frame for DBL. Bottle draws accounted for 1.574 ms and preparation for 0.159 ms. These averages cover 525 GPU samples in that scene.

## Building the runtime

[Download the 1.1.0 source package from GitHub Releases](https://github.com/103Sakamoto/Dynamic-Bottle-Liquids-SE/releases/tag/v1.1.0).

Use 64-bit MSVC with C++23, CMake 3.25 or newer, Git, and vcpkg. Configure with the vcpkg toolchain and build the Release configuration.

~~~powershell
cmake -S . -B build -A x64 "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
python compile_shaders.py
~~~

The SKSE DLL keeps the filename SkyrimBottlePrototype.dll for compatibility with the installed mod. The Nexus download supplies the bottle assets, profiles and installer.

## License

The runtime and shader source are distributed under the [GNU General Public License, version 3](LICENSE).
