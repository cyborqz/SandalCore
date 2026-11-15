# waterbodies

Reads the lakes and rivers of the Riverlands that can be fished, out of your own game files.

    dotnet tools/waterbodies/bin/Release/net8.0/waterbodies.dll read [--out-dir dist/content] [--check]
    dotnet tools/waterbodies/bin/Release/net8.0/waterbodies.dll list

`scripts/extract_game_data.ps1` runs it as the `waterbodies` step (`-Only tools,waterbodies`); it needs only the game's containers and the Riverlands' rectangles (`tools/mapc/region_rects.tsv`): it reads the 47 small cells of the map's Water grid that touch the region (a few seconds, about 1.4 GB). `--check` writes nothing and exits 1 when `--out-dir` holds a different file. `list` prints every fishable body and writes nothing; `dump CELL NAME-PART` prints the properties of a cell's exports whose name holds the part (a diagnostic).

It builds on `tools/mapc` (its `Game`, `Cells`, `Region` and `Scope` classes) and is a project of its own, like `tools/npcabilities`, so that a change here does not restamp the extractor steps that mapc's code feeds.

## What it reads

The Water grid holds each lake (`BP_WaterLake_C`, also named `BP_ProcLake_C_...`) and river (`BP_WaterSpline_C`, `BP_ProcSpline_C_...`) as an actor. A body that names fishing data (the `FishingData` property: the guid of a `GatheringInteractionData`, the rod, the certification, the channel's length and the reward table of that water) can be fished; the others cannot. Of the 208 bodies of the Riverlands' cells 106 name fishing data (145 lakes and 63 rivers in all; 64 fishable lakes and 42 fishable rivers).

- **The outline** of a lake: its closed spline (`SplineCurves.Position.Points`, a cubic Hermite through the points: a segment leaves a point with its leave tangent and arrives at the next with its arrive tangent), sampled about every 2 m and put through the component's world transform.
- **The swim volumes** (`SwimVolumeTransforms`): the boxes the game generated to tell where a player swims, in the actor's own frame. The game's swimming volume is a box of half sizes 50 x 50 x 25 cm at scale 1, so a transform of scale (sx, sy, sz) is a box of half sizes (50 sx, 50 sy, 25 sz) with the transform's yaw. A river is the chain of its boxes. The boxes of a lake are a coarse axis-aligned partition of its bounding area that overshoots the shore, so a lake is tested by its outline and its boxes are kept for a lake that has none.

## The file

`dist/content/fishing_water.tsv`, world cm, in the order of the actors' names:

    # fishing_water v1 ...
    body     id  class(lake|river)  actor  fishing_guid  x  y  z        the actor's place; z is a lake's surface
    outline  id  x,y  x,y  ...                                          a lake's outline, closed (the first point is not repeated)
    box      id  cx  cy  cz  hx  hy  hz  yaw                            a swim volume: its centre, half sizes and yaw in degrees

worldd (`server/worldd/fishing/water_map.*`) answers "is this point on water, and which fishing data does it name" from it.
