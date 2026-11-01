# Docs changes

A branch that changes the known issues, the roadmap or the roadmap table of the README does not edit those files. It adds ONE file here, `docs/changes/<branch-name>.md` (a fragment), and the merge of the branch applies the fragment to the docs and removes the file. Two branches then never edit the same lines of the same file, so their merges never conflict over the docs.

## The format

A fragment is a list of changes. A change is a header line that starts with `@`, and for `add` and `set` the new line directly under it:

```
# Changes of my-branch (a line that starts with # is a comment, and so are blank lines)

@known-issues set Combat / Finishers
| **Combat / Finishers** | The whole new text of the row. | A workaround. | Open. |

@known-issues add
| **Creatures / Burn** | A burning creature loses Health every second. | None. | Open. |

@known-issues remove Creatures / Packs

@roadmap-left set Summoner pets
- Summoner pets and their abilities.

@roadmap-in-progress add
- Creature effects: burn, in the first part.
```

| Header | What it does |
|---|---|
| `@<target> add` | appends the line under it at the end of that list or table |
| `@<target> set <key>` | replaces the one line that the key names with the line under it |
| `@<target> remove <key>` | removes the one line that the key names (nothing goes under it) |

| Target | The lines | The key |
|---|---|---|
| `known-issues` | the rows of the table in `docs/KNOWN_ISSUES.md` | the area: the first cell, without the `**` |
| `roadmap-achieved` | the bullets under "Achieved" in `docs/ROADMAP.md` | the beginning of the bullet's text, after `- ` |
| `roadmap-in-progress` | the bullets under "In progress" | the same |
| `roadmap-left` | the bullets under "Left" | the same |
| `roadmap-milestones` | the numbered lines under "Milestones" | the phase number |
| `readme-roadmap` | the rows of the phase table in the roadmap section of `README.md` | the phase number |

## The rules

- The new line is exactly one line, in the same form as the other lines of its list: a `- ` bullet, a table row with all its cells (`| ... |`; a `|` inside a cell is written `\|`), or a numbered line (`N. ...`).
- A key names exactly one line. For a bullet it is the beginning of its text; if two bullets begin the same way, give more of it. For a row or a phase it has to be the whole area or the whole number.
- A `set` keeps the key of a row or a numbered line: the new row has the same area, the new numbered line the same number. To rename an area or renumber a phase, remove the line and add the new one. A bullet can change freely.
- An added line must not be there yet (change an existing one with `set`), and one line is changed by one change only, also across the fragments of different branches.
- Several fragments are applied in the order of their file names (plain character order), and the added lines of each list come in that order.
- The `**Last updated:**` line of a file that changed is set when the fragments are applied: leave it out of a fragment.
- A change that needs several places to agree (a phase that finishes: the counts, the bar and the diagram of the README), and an item that runs over more than one line, are edited by hand in a separate change.
- When anything in a fragment does not fit, the merge stops with one message per problem that names the fragment and the line, and nothing is changed. Fix the fragment and merge again.
- The file names in this folder are the README and the fragments (`<branch-name>.md`: lower-case letters, digits and hyphens). Nothing else belongs here.
