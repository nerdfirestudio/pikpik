# Contributing

Contributions are welcome. There are a few rules that are not negotiable, and
they come first because breaking one of them is the only kind of pull request
that cannot be fixed in review.

## The hard rules

**No game assets. Ever.** No textures, models, audio, video, save files from a
retail disc, disc images, or extracted data — in a commit, in an issue, in a
screenshot of a file listing, in a test fixture, anywhere. A pull request
containing any of it will be closed rather than amended, because the history
would need rewriting.

**No decompiled sources in this repository.** Changes to the game tree go in
`patches/`, regenerated with `./tools/regen_patches.sh`. New files go in
`core-additions/`. `vendor/` is gitignored; keep it that way. See
[docs/PATCHES.md](docs/PATCHES.md).

**Do not paste large excerpts of decompiled code into issues or pull request
descriptions.** A file and line reference into the upstream project does the
same job. A few lines to make a point is fine; a function body is not.

**Contribute only work you wrote.** Do not copy code in from other ports,
emulators, or decompilation projects, however permissively licensed, without
saying so explicitly so its licence and attribution can be handled properly.

## Where to send things

Changes to the **decompilation itself** — better names, better matches,
newly-decompiled functions — belong upstream at
[projectPiki/pikmin](https://github.com/projectPiki/pikmin), not here. This
project tracks a pinned commit and benefits from anything that lands there.

Changes to the **port** — the renderer, audio, input, saves, platform support,
build system, docs — belong here.

## Before you open a pull request

**Read the code around what you are changing, including the comments.** The
port layer's comments are unusually long on purpose: where behaviour is
non-obvious *because the GameCube hardware was strange*, that comment is often
the only place the knowledge exists. Several of them record explanations that
were tried and turned out to be wrong. Take them seriously before forming a
theory of your own.

**Build both platforms if you touched shared code.** `./build_mac.sh` and
`./build_ios.sh`. The iOS build compiles the same sources against a different
SDK and catches things the macOS build does not.

**Replay, do not merely compile.** A recording plays through and the frames
match, or the change is not verified. `tools/input_replay.sh`. "It builds and
seems fine" is not a claim this codebase supports — too much of its behaviour
is emergent from hardware emulation that no compiler checks.

**Say what you measured.** The convention in this project is that a claim comes
with the observation that supports it. "Fixed the flicker" is much less useful
than "the consumer-side cap was 262144 and the producer was emitting 265761;
raising it fixes the flicker and the count now matches". The second one can be
checked a year later.

## Style

Match the file you are in. The port layer favours long explanatory comments over
short clever code, particularly where something is non-obvious *because the
hardware was strange* — that comment is often the only place that knowledge
exists. Keep them.

C++ in the game tree is gnu++03 with no exceptions, because that is what the
decompilation targets. The port layer is not so constrained but stays close.

## Reporting a bug

Include: the platform and OS version, whether it is a macOS or iOS build,
`-O0` or `-O2`, the game data revision, and what you were doing. A recording
(`tools/input_record.sh`) with the save snapshot it was made from is worth more
than any description — it is the difference between a report someone can
reproduce and one they can only sympathise with.

## Licence

Contributions are accepted under the repository's MIT licence. See
[LICENSE](LICENSE) and [docs/LEGAL.md](docs/LEGAL.md).
