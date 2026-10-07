#!/usr/bin/env python3
"""Read the Switch port's profile.csv (port/switch/host/switch_profile.c).

The file counts the game's main thread's samples by place and by the game
function they were for. This names both with the build's symbols and adds
them up by function: where the thread works (game and opence.elf
functions), what it waits in, and for each game function that calls into
opence.elf (the GL driver, mostly) the time spent there and in which of its
functions.

Usage: switch_profile.py profile.csv halo_guest.elf opence.elf [--top N]

The symbols are those of the build that wrote the file (its first line
names it): the switch-builds branch has both programs.
"""

import argparse
import collections
import subprocess


def symbolize(elf, addresses):
    """function names for addresses, with llvm-symbolizer"""
    addresses = sorted(set(addresses))
    if not addresses:
        return {}
    text = "\n".join(f"0x{address:x}" for address in addresses) + "\n"
    output = subprocess.run(["llvm-symbolizer", f"--obj={elf}", "--functions=linkage", "--no-inlines", "-C"],
                            input=text, capture_output=True, text=True, check=True).stdout
    names = [block.split("\n")[0] for block in output.strip().split("\n\n")]
    return dict(zip(addresses, names))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("profile")
    parser.add_argument("guest")
    parser.add_argument("host")
    parser.add_argument("--top", type=int, default=30)
    arguments = parser.parse_args()

    rows = []
    with open(arguments.profile, encoding="utf-8") as file:
        header = file.readline().strip()
        file.readline()
        for line in file:
            kind, address, caller, count = line.strip().split(",")
            rows.append((kind, int(address, 16), int(caller, 16), int(count)))
    total = sum(row[3] for row in rows)
    print(header)

    game = symbolize(arguments.guest, [row[1] for row in rows if row[0] == "g"] + [row[2] for row in rows if row[2]])
    host = symbolize(arguments.host, [row[1] for row in rows if row[0] in "hw"])

    def name(kind, address):
        if kind == "g":
            return "game: " + game.get(address, f"{address:x}")
        if kind == "h":
            return "opence: " + host.get(address, f"+{address:x}")
        if kind == "w":
            return "waits, from " + host.get(address, f"+{address:x}")
        return "elsewhere"

    places = collections.Counter()
    callers = collections.Counter()
    caller_places = collections.defaultdict(collections.Counter)
    kinds = collections.Counter()
    for kind, address, caller, count in rows:
        kinds[kind] += count
        places[name(kind, address)] += count
        if kind in "hw":
            caller_name = game.get(caller, f"{caller:x}") if caller else "(no game frame)"
            callers[caller_name] += count
            caller_places[caller_name][name(kind, address)] += count

    print(f"{total} samples: game {100 * kinds['g'] / total:.1f}%, opence.elf {100 * kinds['h'] / total:.1f}%, "
          f"waiting {100 * kinds['w'] / total:.1f}%, elsewhere {100 * kinds['o'] / total:.1f}%")
    print("\nby function:")
    for place, count in places.most_common(arguments.top):
        print(f"  {100 * count / total:5.1f}%  {place}")
    print("\nopence.elf's time, by the game function it was for:")
    for caller, count in callers.most_common(arguments.top // 2):
        print(f"  {100 * count / total:5.1f}%  {caller}")
        for place, inner in caller_places[caller].most_common(5):
            print(f"           {100 * inner / total:5.1f}%  {place}")


if __name__ == "__main__":
    main()
