# StuntVis

Stunt visualisation plugin (ASI) for GTA San Andreas and GTA Vice City, built with
[plugin-sdk](https://github.com/DK22Pac/plugin-sdk).

## Install

Copy the file for your game into its `scripts` folder (needs an ASI loader):

- `bin/StuntVis.SA.asi` — GTA San Andreas 1.0 US
- `bin/StuntVis.VC.asi` — GTA Vice City 1.0

## Keys

| Key | Action |
|---|---|
| K | Predicted trajectory (while airborne) on/off (toggles on release) |
| K + `+` / `-` | Hold K and press plus/minus to change the trajectory thickness (1–10) |
| U | Collision spheres of tracked vehicles on/off (toggles on release) |
| U + `+` / `-` | Hold U and press plus/minus to change the opacity of all collision drawing (10–100%) |
| U + S | Switch collision between wireframe and solid: shaded spheres; boxes (back faces culled) and collision meshes coloured by normal, with dark edges and darker with distance (normal-buffer colours: +x red, +y green, +z blue, flat floor light blue) |
| U + D | Switch the depth test for wireframe collision on/off (on = hidden behind walls); solid collision is always depth tested |
| U + R | Hide the models of the objects selected with G, so only their collision is drawn (collision itself unchanged) |
| N | Collision boxes + collision mesh of tracked vehicles on/off |
| M | Collision spheres, boxes and mesh of your own vehicle on/off |
| J | Track / untrack a vehicle (the one you're in, or the nearest within 20 m) |
| G | Select / deselect the objects you're touching and show their collision; G while touching nothing clears. The message lists the model id (and name, SA) of what you touch, marked `(foliage)` if Y treats it as foliage. |
| X | Cull zones with extra air resistance on/off (SA only; red, yellow = the zone you're in) |
| Y | Foliage (grass, plants, trees, palms, bushes, hedges): shown → its shaded collision → hidden → shown (SA only; collision stays) |

Foliage for Y is found by the tree/palm flag and by model name (read from the game's IDE files).
To add or exclude models, create `scripts\StuntVis_foliage.txt` with one model id per line
(`-id` excludes, `#` starts a comment); it is read when the game starts.

The integrity check runs at start-up, every 5 seconds and on H. It shows `0000` when everything is fine,
otherwise error codes, and locks all visualisations until it passes again.

## Build

Open `StuntVis.sln` in Visual Studio (v141_xp toolset, Win32) with the `PLUGIN_SDK_DIR` environment
variable pointing at plugin-sdk, and `GTA_SA_DIR` / `GTA_VC_DIR` at the game folders (the output is written
to their `scripts` folder). Configurations: `GTASA Release`, `GTAVC Release` (and `zDebug` variants).
