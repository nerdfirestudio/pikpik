# Legal position

This document states plainly what this project is, what it distributes, and
where the genuine uncertainty lies. It is written to be accurate rather than
reassuring; nothing here is legal advice, and the authors are not lawyers.

---

## 1. In one paragraph

pikpik is a host port layer. It contains no game code and no game assets. To
produce a running program it needs two things this repository does not
provide: the *Pikmin* decompilation, which `setup.sh` fetches from a separate
upstream project, and an extracted copy of the retail GameCube disc, which you
must supply from a disc you own. Everything in this repository — the renderer,
the audio and DSP emulation, the platform layer, the build system, the patch
files, the documentation — is original work by the pikpik authors and is
licensed MIT.

## 2. This is not a clean-room reimplementation

It is important to be exact about this, because the term gets used loosely and
using it wrongly here would be a misrepresentation.

A clean-room reimplementation separates two teams: one examines the original
and writes a functional specification containing no expressive content; the
other, having never seen the original, implements from that specification
alone. The point of the firewall is that the second team's output is provably
not a copy.

**No such firewall exists here.** The game logic comes from a *decompilation*:
source code produced by analysing the original compiled binary and iterating
until a recompile reproduces that binary instruction for instruction. That is
the opposite process. A decompilation is, by construction and by intent, a
representation of the original program.

What pikpik contributes on top of that is genuinely independent work — a Metal
renderer written against the GameCube's graphics register semantics, a DSP
mixer, a memory-card emulation, an LP64 porting effort — but the game it drives
is decompiled, and this project does not claim otherwise.

## 3. On the decompilation's CC0 declaration

The upstream project, [projectPiki/pikmin](https://github.com/projectPiki/pikmin),
publishes its work under CC0 1.0. That declaration is made in good faith and it
is meaningful as far as it goes: it waives the upstream contributors' own
rights in their own contribution — the naming, the structuring, the header
organisation, the years of analysis.

It does not, and as a matter of law cannot, license the underlying work. Nobody
can place into the public domain a copyright they do not hold, and a
decompilation reproduces the structure, sequence, and organisation of code
whose copyright belongs to its original author. A CC0 grant from a
decompilation project is therefore best read as "we assert no rights against
you", not as "this material is unencumbered".

Anyone building or redistributing this software should understand that and
decide for themselves. This project's response to that uncertainty is section 4.

## 4. Why the decompilation is fetched rather than vendored

This repository could have vendored the decompiled sources, as several
comparable projects do. It deliberately does not.

`setup.sh` clones the upstream project at a pinned commit and applies
`patches/`. The consequence is that **this repository distributes no code
derived from Nintendo's binary at all.** What it distributes is a set of
diffs — its own modifications — plus original port code.

Two honest caveats:

- A unified diff carries context lines from the file it patches, so
  `patches/` contains short fragments of the decompiled sources. They are
  incidental to expressing a change and useless without the files they apply
  to, but they are not zero.
- A few documents and code comments quote short fragments to explain a
  specific bug. Again small, elided, and explanatory.

The point of the arrangement is not that it makes anything bulletproof. It is
that the smaller the surface, the less there is to argue about, and a
maintainer who never redistributes the bulk material is in a materially
different position from one who does.

## 5. Game assets

None are included, and none ever will be. The build reads an extracted disc
filesystem at a path you configure; without it the program starts, finds no
data, and stops.

That extends to screenshots. The README shows one, because documentation that
cannot show what the software does is poor documentation — but what it depicts
is Nintendo's artwork rendered by this port, so it is hosted in a separate
repository and embedded by URL rather than committed here. The distinction is a
fine one and it is drawn deliberately: it keeps section 2's claim literally
true, and it means a request to take the image down is satisfied somewhere else
entirely, without touching this repository or its history.

Supply it yourself, from your own disc. Whether ripping a disc you own is
lawful depends on where you live: some jurisdictions permit a personal backup,
others prohibit circumventing the copy protection required to make one, and
some manage both at once. That is your determination to make, not this
project's, and this repository provides no tool and no instruction for
obtaining game data by any means.

## 6. Trademark, and why the app is not called "Pikmin"

PIKMIN and NINTENDO are Nintendo's trademarks. This project is not affiliated
with or endorsed by Nintendo.

The documentation says "Pikmin" where it must — describing what the software is
a port of is nominative use, and there is no accurate alternative. But the
shipped artifacts do not: the project, the application, the macOS bundle, the
iOS target, and the bundle identifiers are all "pikpik". Trademark protects
against consumer confusion about origin, so nothing built from this repository
should ever present itself as a Nintendo product. Please keep it that way if
you fork.

The icon is an original drawing generated by `assets/make_icon.py`, so that the
build has no artwork of contested provenance anywhere in it.

## 7. If you are a rights holder

Open an issue, or use GitHub's designated agent process. A notice that
identifies specific material will be acted on promptly and without argument;
the maintainers would rather take something down than litigate it.

Note for that purpose what is actually here: patch files and an original port
layer. The decompiled sources are hosted by
[projectPiki/pikmin](https://github.com/projectPiki/pikmin), a separate project
with separate maintainers, and are not within this project's control.

## 8. If you contribute

- Do not submit game assets, disc data, or extracted binaries. Ever.
- Do not paste large excerpts of decompiled source into issues or pull
  requests. Point at file and line in the upstream project instead.
- Changes to the game tree belong in `patches/`, regenerated by
  `tools/regen_patches.sh` — not pasted in as whole files.
- Contribute only work you wrote yourself.

See [CONTRIBUTING.md](../CONTRIBUTING.md).

## 9. Warranty

None. See LICENSE. This software is a hobby project that drives a decompiled
1990s-toolchain codebase through a modern optimising compiler; treat every
output as provisional.
