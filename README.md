
# MojoZork

```
>read leaflet
```

Hello sailor!

This is an implementation of Infocom's Z-Machine. The Z-Machine is a virtual
machine that's something like a high-level CPU. To keep their games portable
and easier to write, Infocom's games all use this fake processor and ship
with a platform-specific Z-Machine "emulator" ... so a game could run wherever
someone had implemented the Z-Machine.

This project is just for fun; everyone should write this at some point as an
educational activity. If you want a serious Z-Machine implementation, there
are certainly better ones out there (I personally recommend
["Frotz"](http://frotz.sourceforge.net/) and there are many others, too).

This program currently supports most of the Version 3 Z-Machine. This is
enough to play the vast majority of Infocom's catalog. Later Infocom games
used version 4, 5, and 6 of the Z-Machine, and those will currently not run
here. Most modern Interactive Fiction is built with a tool called Inform and
usually targets version 5 at the lowest; these games currently don't work
with this project. Maybe later.

Activision, who acquired Infocom in the 1990's, gives out Zork I, II, and III
for free, so I've included Zork I's data files with this project. If you want
to see Zork I run through from start to finish, you can run a pre-written
script to complete the entire game from the command line, like this:

```
./mojozork ./zork1.dat ./zork1-script.txt
```

If you want to write your own Z-Machine, there is an "official" specification
on how to implement it, written by people that spent significant time
reverse engineering the originals from Infocom, and extending the ecosystem
with new tools. You can find that specification
[here](http://inform-fiction.org/zmachine/standards/).

As usual, Wikipedia offers a wonderful rabbit hole to fall down, too, in
their [Z-machine article](https://en.wikipedia.org/wiki/Z-machine).

# MultiZork

On top of the MojoZork code, there is a telnet server called `multizorkd` that
lets several people connect _to the same game_ and play it as a multiplayer
experience. Each player has their own inventory and can move independent of
others. This probably only works on Linux/Unix systems, but the single-player
mojozork program should work on just about anything.

You can play MultiZork right now by pointing a telnet client at
multizork.icculus.org (or running `nc multizork.icculus.org 23`).

[A post on my Patreon](https://www.patreon.com/posts/54997062) explains the
motivations and technical details of multizork. If you like this, please
consider throwing in a dollar so I can keep doing wild things like this!

# libretro core

Ever want to play Z-Machine games under RetroArch? Now you can. The libretro
core handles all the tapdancing to make text-based games run in a graphical
window, and can be played with a keyboard (in RetroArch focus mode), or with
a mouse or game controller on an included virtual keyboard.

# Standalone SDL3 app

Want the graphical interface of the libretro core without RetroArch? There's a
standalone app that uses SDL3 for the graphics. It's a no-frills sort of
thing, but it could be useful in the right circumstances.


Enjoy!

--ryan.

