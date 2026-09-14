<p align="center">
  <img width="1920" height="600" alt="banner" src="https://github.com/user-attachments/assets/b02c6fe2-f50d-480f-92ef-936bd5d42227" />
</p>

BlitzX3D is a community-maintained fork of Blitz3D TSS, originally based on Blitz3D. It focuses on maintaining and extending the Blitz3D engine while preserving compatibility with existing projects and libraries.

### Support

Need help, have a question, or found a problem?

- [**Join the BlitzX3D Support Discord**](https://discord.gg/S7MXjp6ejW)
- [**Open a GitHub Issue**](https://github.com/krimbopple/BlitzX3D/issues) for bugs, feature requests, or other issues.

> **Note:** BlitzX3D is volunteer-maintained. Support is best-effort with no
> guaranteed response time

---

### Features
- DirectX 9 rendering support
- New ImGui-based IDE & Debugger replacing the legacy editors.
- Debugger runs as a separate process for better performance, and now has expandable arrays in variable trees.
- Built-in profiler with per-function timing, call counts, memory statistics, and a flame graph for identifying performance bottlenecks.
- [New scene system with commands for creating, switching, clearing, and querying scenes.](https://github.com/krimbopple/BlitzX3D/wiki/New-Commands#createscene--setscene--clearscene--getcurrentscene)
- Async texture loading, dramatically reducing boot and loading times.
- Improved rendering performance through render-state batching, draw-call sorting, brush-bucketed rendering, and faster font lookups.
- [Improved shader support, including .fx effects, runtime effect textures, and per-frame shader parameters.](https://github.com/krimbopple/BlitzX3D/wiki/New-Commands#loadeffect--setentityeffect--setbrusheffect)
- Better render-target control with SetBufferDepth and RenderEntity.
- Improved texture handling, including automatic alpha detection, animated texture grids, mipmapped filtering, cubemap fixes, and better transparency handling.
- Upgraded audio system now powered by BASS for improved compatibility, stability, and sound quality.

### Used in
* [**SCP – Containment Breach Multiplayer 1.3.0R**](https://store.steampowered.com/app/1782380/SCP_Containment_Breach_Multiplayer/)
* [**SCP – Containment Breach Ultimate Edition Reborn 1.6**](https://github.com/Jabka666/scpcb-ue-my/tree/1.5.x)
* [**SCP – Containment Breach Faerov Mod**](https://www.moddb.com/mods/scp-containment-breach-faerov-mod)
* [**SCP – Terror Hunt**](https://www.moddb.com/mods/scp-terror-hunt-mod)
* [**SCP – Containment Breach Amended**](https://www.moddb.com/mods/scp-amended1)
* [**SCP – Treachery**](https://www.moddb.com/mods/treachery)
* [**YOU ARE NOT IMPORTANT**](https://www.moddb.com/mods/you-are-not-important)

<table>
  <tr>
    <td align="center">
      <img
        src="https://github.com/user-attachments/assets/5d8a96fb-757a-439a-b2e6-aa7df22818e4"
        width="400"
        height="225"
        style="object-fit: cover;"
      />
      <br />
      <b>SCP – Containment Breach Ultimate Edition Reborn 1.6</b>
    </td>
    <td align="center">
      <img
        src="https://github.com/user-attachments/assets/931cc3fb-85f9-4cfc-a498-78ba3b36e72b"
        width="400"
        height="225"
        style="object-fit: cover;"
      />
      <br />
      <b>SCP – Terror Hunt</b>
    </td>
    <td align="center">
      <img
        src="https://github.com/user-attachments/assets/632a8ca7-0809-481a-8576-be22d758f3d1"
        width="400"
        height="225"
        style="object-fit: cover;"
      />
      <br />
      <b>SCP – Containment Breach Amended</b>
    </td>
    <td align="center">
      <img
        src="https://github.com/user-attachments/assets/38e30707-45a6-4bdb-a2c9-68ba96cb8f6a"
        width="400"
        height="225"
        style="object-fit: cover;"
      />
      <br />
      <b>YOU ARE NOT IMPORTANT</b>
    </td>
  </tr>
</table>

<table>
  <tr>
    <td align="center">
      <img
        src="https://github.com/user-attachments/assets/93e66689-5b9c-4c4f-a1c2-ebd72b11c68f"
        width="400"
        height="225"
        style="object-fit: cover;"
      />
      <br />
      <b>SCP – Treachery</b>
    </td>
  </tr>
</table>

---

## License

Please read the applicable license files before using, modifying, or
distributing this project.

BlitzX3D contains source code derived from the original Blitz3D project, as
well as third-party components. Those portions remain licensed under their
respective original licenses and are not relicensed by BlitzX3D.

Original contributions to BlitzX3D by Chris A. (krimbopple) are licensed under
the MIT License (see LICENSE).


---

## How to Build

### Prepare

- Visual Studio Community 2022
  - Desktop development with C++
  - C++ MFC for latest v143 build tools (x86 & x64)
  - C++ ATL for latest v143 build tools (x86 & x64)
  - ASP.NET and web development
- This repo vendors [SDL3](https://github.com/libsdl-org/SDL) and [SDL_image](https://github.com/libsdl-org/SDL_image) as **git submodules**. After cloning, initialize them before building:
  ```sh
  git submodule update --init
  ```
  (Or clone with `git clone --recurse-submodules`.) Both are linked statically, so no `SDL3.dll` / `SDL_image.dll` needs to be shipped alongside the builds.

### Before building `linker` or `bbruntime_dll`:

1. Copy `linker/cryptseed.h.example` to `linker/cryptseed.h`.
2. Open `cryptseed.h` and change `RUNTIME_KEY_SEED` to any nonzero value of your own choosing.
   
### Steps

1. Open `blitz3d.sln` in Visual Studio 2022.
2. Select the **Release**/**Debug** configuration and rebuild the entire solution.
3. All done! You can find the output files in the `_release` and `_release/bin` directories. Feel free to delete any `.pdb` and `.ilk` files.

## Properly Debugging

Because the launcher (`bblaunch`) spawns the IDE (`ide.exe`) and then exits, Visual Studio will lose the debug session by default. To debug properly:

1. Install the **Microsoft Child Process Debugging Power Tool 2022+** from the [Marketplace](https://marketplace.visualstudio.com/items?itemName=vsdbgplat.MicrosoftChildProcessDebuggingPowerTool2022).
2. In Visual Studio, go to **Debug -> Other Debug Targets -> Child Process Debugging Settings** and enable **"Enable child process debugging"**.

The debugger will now automatically attach to `ide.exe` when it launches, and you will now be able to properly debug your programs!

---

## In Memory of Mark Sibly

[Mark Sibly](https://github.com/blitz-research), the creator of Blitz3D, passed away on 12 December 2024. 🕯️
