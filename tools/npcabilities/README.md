# npcabilities

Reads what the creatures of the game attack with out of your own game files, for the creatures that play their attacks with their own animation.

    dotnet tools/npcabilities/bin/Release/net8.0/npcabilities.dll read --dir AOC/Content/DesignAssets/Population/NPC/Beast/Rabid [--dir ...] [--out-dir dist/content] [--check]

`scripts/extract_game_data.ps1` runs it as the `npcabilities` step (`-Only tools,npcabilities`); it needs only the game's containers (no map read, a few seconds).
The folders it reads are listed in the script (`$NpcFolders`): one per creature kind, holding the kind's Blueprints and the animation montages beside them.
A kind that is not listed keeps the single swing every creature had before. `--check` writes nothing and exits 1 when `--out-dir` holds different files.

It writes three tables:

- `npc_abilities.tsv`: one line per use-ability task of a creature Blueprint's state tree: the Blueprint (as `npc_motion.tsv` names it), the ability record, the task's name,
  the seconds the creature waits before it uses the ability again, whether it stands still to use it, the task's place in the tree (`order`) and the conditions the tree puts on it:
  `min_distance`, `max_distance` and `max_angle` the target must satisfy (-1: none), `gates` the cooldown names that must be over, and `unknown` = 1 when a condition is of a kind the
  tool does not say (`why` names it). `fight` is 1 for a task of the state the creature fights in (the openings, idle and ambient abilities are 0). `conds` holds the other conditions as
  short atoms, one group per state of the task's way down (a roll, the creature's own health, the seconds into the fight, line of sight, the distance between the centres; a state whose
  list uses `or` or brackets is kept whole, in the order the tree has it). `starts` are the cooldowns the creature starts when it takes the task besides its own: the tasks of the states
  above it may start some. A Blueprint without a tree of its own plays its parent's, and a task below a state the designers switched off is left out.
  `rotate` is "speed:duration" of the Rotate To task (turn towards the target) that runs with the ability, in its state or one above it: while the ability plays the creature keeps turning to its
  target at `speed` degrees a second (-1: the task names none, so the creature's own rotation rate) for `duration` seconds (0: until the ability is over); empty when no such task runs with it (the Grem's
  Snap and Fury do not turn it; the wolf's bite, the bear's swings and the goblin's swings do).
  A tree takes some settings from its parameters (the distance of a bite, whether the creature may pounce); the tool reads them from the Blueprint's reference to the tree and the tree's
  own defaults and folds them in: a condition that compares parameters is decided when the table is made, and a task a parameter switches off for the creature has `why` = `off`.
- `npc_montages.tsv`: one line per animation montage: its object path as an ability record names it, the ids the network layer knows the montage and its package by,
  its length in seconds at a play rate of 1, its rate scale, its sections (`name:start:next`), its hit notifies (`time:key`; the key picks which hit of the ability lands) and
  its root motion (`time:x:y:z`, in cm from where the creature stood when the montage began, x ahead of it and y to its right: how far the animation carries the creature,
  read from the root bone of the animation the montage plays; empty for a montage that stays in place) and `root_unreadable`: 1 for a montage whose animation moves the creature in a
  form the tool cannot read (an animation stored with the ACL codec, a looping or composite segment); its root motion is empty then, and a creature does not play an ability whose
  montage is unreadable. The client moves a creature along that travel by itself while the montage plays.
- `npc_targeting.tsv` (`targeting.py`, from content.db): where each listed ability's hit lands, one line per targeting record of the ability (`key`):
  - `discovery` is how the record finds its target: `none`, `prefers_active` (what the creature fights now, else the one the cast was aimed at), `only_locked` (the one the cast was aimed at),
    `only_active` or `prefers_locked`.
  - `check_validity` says whether the record measures anything. At 0 the hit lands on that target wherever it stands, and the distance and angle on the line are only what the record states, not
    tested (the wolf's and the bear's bites and swings, the Grem's snap, the goblin's swings are of this kind). At 1 the target part tests: `cone_check` 1 = the target's body is no farther than `distance`
    (cm) from the point `distance_center` ahead of the creature, no nearer than `min_distance`, and within half of `angle` (degrees) either side of the creature's facing turned by `angle_center` (an
    angle of 0 or 360 is all round); `los_check` 1 = the creature has to see the target.
  - `volumes` is how many shapes the record sweeps, and `shapes` lists them: `sphere:ox:oy:oz:radius:los` and `box:ox:oy:oz:hx:hy:hz:yaw:los` (centred ox ahead of the creature, oy to its right and oz above
    its feet, in cm; `los` 1 = the target has to be seen), and `other:why` for a volume the world cannot place (a cone, a capsule, a point, a hollow or moving one, another origin). An enemy whose body overlaps
    a volume is hit; an ability with a volume of the last kind is not animated, and the creature keeps its single swing.
  - `freeze` (none, pause or stop), `freeze_distance` (cm ahead of the creature's scaled body), `freeze_scale` (the creature's own radius counts this many times) and `ignore_non_target` (1: only the
    ability's target counts) say what the ability record says its root motion does when a body comes ahead of the creature. A creature whose lunge `stop`s halts it for the rest of the montage once a body is
    in that box; one whose lunge `pause`s is held while a body is in it, with `bRootMotionObstructed` going out (the client then holds its own copy too), and goes on by what the montage still travels when
    the box is clear. The box decides only where a lunge halts; it never decides whether a hit lands.

worldd reads all three (`--npc-abilities`, `--npc-montages`, `--npc-targeting`). A creature whose Blueprint is in the first table uses its abilities in the order the tree tries them, among
the tasks of its fight whose conditions the tables can say (the conditions are asked afresh at every try, a roll included); starts each attack with the ability's montage, lands every hit when its notify says, by the hit's targeting record (on the target wherever it stands unless the record measures, and on everyone inside its volumes), stands still for the montage when the task says so,
moves the creature along the montage's root motion as the client does (it stops at a player's capsule and at the edge of the walkable ground, is held or stopped by its freeze box, and the
world says where it is at every step), turns it to its target while the ability plays when the tree says so (the lunge then goes the way the creature faces at each step), keeps the AI's walk orders waiting until
the montage is over, and starts the next attack when the montage is over.  A creature chases its target by a walk that follows it at every step (it never runs to where the target was and waits there) and turns at
its rotation rate: its facing never jumps, and it is never stopped only to turn.

The tool is a project of its own (it builds on `tools/mapc`'s `Game` class) so that a change to it restamps only its own step.
