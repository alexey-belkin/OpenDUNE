About
-----
OpenDUNE is an open source re-creation of the popular game "Dune II",
 originally made by Westwood Studios, and released by Virgin Entertainment.
It attempts to re-create the original game and apply modern technology to it
 to allow it to be run natively on most operating systems.

OpenDUNE is licensed under the GNU General Public License version 2.0. For
 more information, see the COPYING file included with every release and source
 download of the game.


Contact
-------
The latest version of OpenDUNE is always available at:
  http://www.opendune.org/
The latest development version is at:
  https://github.com/OpenDUNE/OpenDUNE
Our IRC (chat) is at:
  irc://irc.oftc.net/OpenDUNE
You can report bugs at:
  https://github.com/OpenDUNE/OpenDUNE/issues
Our wiki is at:
  https://github.com/OpenDUNE/OpenDUNE/wiki


Supported Platforms
-------------------
OpenDUNE is written in ANSI C (C89), and only requires SDL/SDL2 to run. In result,
 OpenDUNE should run on all platforms supported by SDL or SDL2.
Currently we officially support the following platforms:

  - Linux
  - FreeBSD
  - Mac OS X (PowerPC or Intel i686 / x86_64, 10.4+)
  - Windows (i686 / x86_64)
  - Atari TOS (68030+ CPU, TT, Falcon and accelerated ST/STE supported)
  - OS/2
  - Haiku


Requirements
------------
OpenDUNE doesn't require any library to run on Windows. For Mac OS X we make
 so-called static-compiled binaries, which means all libraries it depends on
 (mostly LibSDL) are already included in the binary, and no action is
 required on your part
For Linux/FreeBSD, you need to install LibSDL yourself. It is available in every
 package manager, and the chances are high it is already installed on your
 system.
In order to use sounds and music on Linux, you need a working ALSA driver.
OpenDUNE also supports OSS and PulseAudio for digitized sound output.
Music is sent to MIDI Out port of Atari machines.
It is also possible to build with Munt MT32 emulator http://munt.sourceforge.net/
to have MT32 music : Windows users should just install the mt32emu_win32drv and
the right MIDI device will be selected when mt32midi option in opendune.ini
is on. Linux/FreeBSD users should install munt on their system and rebuild.
FluidSynth is also supported.


Installation & Running
----------------------
Extract OpenDUNE.
Copy the original Dune2 1.07 data files (including dune2.exe) to data/.
 All three existing versions of the Dune 1.07 data files (eu, hs and us) will
  work, but only with the eu/hs data files the French language will work, and
  only with the eu data files the German language will work.
Start 'opendune'.

OS X/macOS : data files are searched additionaly in the Contents/Resources/data
subdirectory of the application bundle, and in
~/Library/Application Support/OpenDUNE/data

Additional options may be specified using an opendune.ini file located
in the data/ directory, in the current directory or in %APPDATA%\OpenDUNE
(on Windows) or ~/Library/Application Support/OpenDUNE (on Mac OS X) or
~/.config/opendune (on Linux/FreeBSD) or B_USER_SETTINGS_DIRECTORY/opendune
(on Haiku). All options must be in an [opendune] section.

Available options are :
- language : english / french / german
- datadir : directory where Dune data files are
- savedir : directory for Dune personal data files (savegames)
- scalefactor : 1 (no upscaling), 2 (default), 3, 4
- scalefilter : nearest (default), scale2x, hqx
- framerate : maximum frame rate (60 FPS default)
- fullscreen : 0(default)/1 starts the game in full screen mode if possible
- mt32midi : 0(default)/1 send MT32 init, use .XMI files
- mt32rompath : directory containing CM32L_CONTROL.ROM/CM32L_PCM.ROM files
                for Munt MT32 emulator.
- fs_soundfont : SoundFont2 file for FluidSynth
- fs_audiodriver : FluidSynth audio driver name (alsa, jack, oss, etc.)
- midideviceid : Windows MIDI Device ID to use (default is 0)

debug options (for developpers) :
- dune2_enhanced : 0 = game acts like the original Dune II, including bugs
                   1(default) = enable OpenDUNE enhancements
- debug_game : 0(default) = normal game behavior
               1 = The player can control the AI
- debug_scenario : 0(defaut) = normal game behavior
                   1 = The player can review the scenario. There is no fog.
                       The game is not running. The player can click on tiles.
- debug_skip_dialogs : 0(default) = normal game behavior
                       1 = skip all intros and go immediately to house select.
- debug_log_game : game record / replay
                   0(default) = off
                   1 = record game to 'dune.log'
                   2 = playback game stored in 'dune.log'
- starport_unit_cap : 0 = Allows to overflow unit limit using starport (default)
                       1 = unit limit is enforced in starport

Combat class balance
--------------------
The optional class-balance module is enabled by default. To configure it, create
an opendune.ini file in the location described above (on macOS:
~/Library/Application Support/OpenDUNE/opendune.ini). Put all settings under an
[opendune] heading. Restart OpenDUNE after changing the file.

Set class_balance_enabled=0 to restore original Dune II damage and original
Barracks/WOR availability. With the module enabled,
class_balance_infantry_all_houses=1 opens both infantry factories to every
House, each keeping its own roster: the Barracks trains Soldier and Infantry,
WOR trains Trooper and Troopers. Rocket infantry therefore costs a second
building, which every House can now put up. Set the key to 0 to retain original
production while keeping the damage matrix. The former name of this key,
class_balance_shared_infantry, is still accepted.

Damage values are integer percentages: 100 means x1.00, 250 means x2.50 and 30
means x0.30. The five classes are P (Soldier, Infantry), RP (Trooper, Troopers),
LT (Trike, Raider Trike, Quad), TT (Tank, Siege Tank, Devastator) and AR
(Launcher, Sonic Tank). AR is the artillery: the only two units that outrange a
Rocket Turret, bought at the price of their hitpoints, and light vehicles hit
them at x2.50 by default. 'Thopter, Deviator, Saboteur, Harvester and all other
unlisted units are neutral (x1.00) on both sides of the matrix.

class_range_p_bonus=1 adds one map cell of firing range to Soldier and Infantry.
Set it to 0 to retain their original range while keeping the damage matrix.

class_speed_p_over_rp=120 sets light infantry walking speed as a percentage of
the matching rocket infantry: Soldier is derived from Trooper and Infantry from
Troopers. At 120 light infantry keeps pace with a Siege Tank and outruns a
Devastator. Set it to 0 to retain the original walking speeds.

The default matrix keys, grouped by attacker, are:
- P:  class_damage_p_vs_p=100, class_damage_p_vs_rp=250,
      class_damage_p_vs_lt=75, class_damage_p_vs_tt=60,
      class_damage_p_vs_ar=100
- RP: class_damage_rp_vs_p=30, class_damage_rp_vs_rp=100,
      class_damage_rp_vs_lt=125, class_damage_rp_vs_tt=130,
      class_damage_rp_vs_ar=100
- LT: class_damage_lt_vs_p=125, class_damage_lt_vs_rp=135,
      class_damage_lt_vs_lt=100, class_damage_lt_vs_tt=70,
      class_damage_lt_vs_ar=250
- TT: class_damage_tt_vs_p=85, class_damage_tt_vs_rp=90,
      class_damage_tt_vs_lt=130, class_damage_tt_vs_tt=100,
      class_damage_tt_vs_ar=100
- AR: class_damage_ar_vs_p=100, class_damage_ar_vs_rp=100,
      class_damage_ar_vs_lt=100, class_damage_ar_vs_tt=100,
      class_damage_ar_vs_ar=100

Individual units are tuned with unit_damage_NAME and unit_rate_NAME, both
percentages of the table value. NAME is the unit's name lowercased with every
run of non-letters replaced by one underscore: launcher, sonic_tank, siege_tank,
raider_trike, thopter. Damage scales the shot before the House bonus and the
class matrix, so it applies to structures as well as units; rate scales shots
per minute rather than the delay between them, and takes the short gap inside a
double shot into account. The defaults are unit_rate_launcher=150,
unit_damage_sonic_tank=150, unit_damage_siege_tank=115,
unit_damage_devastator=115 and unit_rate_raider_trike=120; every other unit
defaults to 100 on both keys.

Rate is usually the better lever of the two. A shot deals a fixed amount and the
excess is lost, so extra damage buys nothing at all until it crosses the number
of shots a target takes to kill: raising the Launcher from 75 to 113 changed
nothing against Soldier, Trooper, Infantry or Quad. The same increase spent on
its rate of fire pays against every target.

House identity bonus keys are class_bonus_atreides_p=120,
class_bonus_harkonnen_rp=110 and class_bonus_ordos_trike=110. The Ordos bonus
applies to Trike and Raider Trike, not Quad. House bonuses multiply the base
shot before the class matrix and also affect damage to structures. Deviator
still deals zero HP damage and uses its normal area-deviation effect.

Turning on the move
-------------------
Set move_rolling_turn=0 to restore the original behaviour, where every change of
direction costs a unit its turning time. With it on -- the default -- a turn of
45 degrees that arises *while the unit is moving* is applied at once and the unit
drives on without stopping. Turns from a standstill and turns of 90 degrees or
more still cost what they always did, so a tank has not stopped being a tank.

The case it addresses is a diagonal route, where the direction alternates
between two neighbouring octants and the unit therefore stopped every other
tile. Measured over twelve AI matches it is worth about a seventh more spice
refined, because harvesters spend their lives driving.

Route finding
-------------
Set pathfinder_astar=0 to restore Westwood's original router. It walks straight
at the destination and, when the next tile is blocked, feels its way round the
obstacle clockwise and anti-clockwise for up to a hundred tiles and keeps
whichever went better. It can only see one tile ahead, which is why units drive
into dead ends between buildings and then grope back out along the wall.

With it on -- the default -- the whole route is worked out at once over the whole
map, and it is the shortest route rather than merely a route. What "shortest"
means here is time, not tiles: the cost of entering a tile is how long the unit
will really take to cross it, so concrete is preferred where concrete is quicker
and a diagonal is charged the extra ground it covers. A unit that is itself
moving is treated as traffic to wait out rather than as a wall, and a unit whose
next tile is briefly occupied waits a moment instead of throwing its route away,
so a group under one order travels as a group.

Measured over twelve AI matches it is worth about forty percent more spice
refined, and it removes the early harvester losses that came from harvesters
walking into places they could not get out of.

Concrete on sand
----------------
Set build_slab_on_sand=0 to restore the original rule, where a slab may only be
laid on rock and a base can grow no further than the rock the map gave it. With
it on -- the default -- a slab is also a road: the landscape table already gives
concrete a movement speed of 255 for every kind of unit against sand's 112, so
paving costs credits and buys both a foundation to build on and the fastest
surface in the game to drive on.

Buildings themselves are unchanged: sand is still not somewhere a structure may
stand, which is what makes paving a purchase rather than a decoration. Walls are
deliberately left out of the rule -- a wall offers no foundation, and letting one
go up on open sand would fence off the desert. The "must touch something of your
own" rule still applies to every slab, so a road grows outwards from the base one
tile at a time.

The ground a base is built on
----------------------------
A generated map is now left as the generator drew it. Both bases used to be laid
out inside a rectangle carved into solid rock, which is the one thing on such a
map that could not have grown there -- and, since concrete may be poured on sand,
no longer necessary: a base stands on the concrete it paves for itself. In a
match that means the ground beyond your Construction Yard and its apron is
whatever the desert put there, and building outwards is paving outwards.

skirmish_base_rock=1 brings the old rock rectangles back. It is worth knowing
that the AI plays measurably better on them -- rock is the fastest ground there
is short of concrete, and an army leaves a base on rock sooner.

The build queue
---------------
A factory takes repeat orders: left click on its picture in the sidebar orders
one more, right click one fewer. The Construction Yard does the same now, and
because a finished building has to wait for you to find a spot, the yard can be
holding several at once. The counter under the picture reads N/M -- N ready to
put down, M still owed altogether, including the one on the bench. The clicks
move M.

When at least one is ready a "Place it" button appears on the row below. Press
it once and you stay in placement mode until the last one is down, so ten slabs
or five turrets go up in ten or five clicks rather than ten or five trips back to
the sidebar. N counts them off as you go.

Right clicking below one order gives the money back: a queued order was never
charged for, an unfinished one is refunded for the part not yet built, and a
finished one is refunded in full. Choosing a different building in the build
list gives back everything the yard was holding, which is what the game always
did with the single building it could hold before.

A small concrete slab now takes a quarter of the time of the large one, matching
its quarter of the price and its quarter of the area. The original game gave
both the same build time, so paving a square one tile at a time took four times
as long as paving it in one piece.

The build list
--------------
The full-screen list that opens when you pick a factory or the Construction
Yard shows everything you can build at once. It used to show four items of a
strip you scrolled with an arrow at each end, which for a Construction Yard
meant paging through eighteen buildings four at a time. The list is a grid now
and it grows to fit: a Light Factory keeps its single column beside the picture,
and a Construction Yard or a Starport spreads into the empty half of the window
until every item is on screen.

Nothing scrolls any more. The two arrow buttons moved to the bottom of the
window and step the highlight one item along, and the number keys 1 to 9 and 0
pick the first ten items outright. The Starport's quantity buttons moved to the
bottom right, beside "Send order", to leave the list its space.

Deviator
--------
A unit hit by the Ordos deviator changes sides for a while rather than for an
instant. It has always been meant to: the deviation is a budget the unit spends
by acting -- driving, shooting, or being given orders -- and it runs out on its
own after two minutes if the unit does none of those. Damage spends it too, but
the original game spent the whole budget on the first point of damage from any
source, which for two houses in three meant that a captured unit changed back
the moment anything scratched it. Harkonnen tanks did it to themselves: they
take a point of wear on their own every few tiles they drive. Damage now spends
the budget in proportion to how hard the unit is hit, so it takes a real beating
to shake a deviation loose, and the tougher the house the sooner it recovers.

Harvesters
----------
A loaded harvester picks one refinery and drives to it. With two or more
refineries it used to change its mind every second or so and end up shuffling
between two doors without reaching either, because the act of aiming at a
refinery marks that refinery busy and the harvester then read its own booking as
somebody else's. A base with several refineries was the case that suffered, and
it is the case that has most to gain.

Repair all
----------
Select a Repair facility and a "Repair All" button appears on the row below the
vehicle it is working on. It is a switch, not an order: while it is lit, every
damaged building of yours starts repairing itself as soon as it is hit, at the
same price per hitpoint the Repair button on each building charges. It is off
when the match starts, and the Repair facility is what sells it -- lose your
last one and the switch stops working until you build another, keeping its
setting for when you do. Several Repair facilities all show the same switch,
because it belongs to the base and not to the building.

It exists for the buildings that cannot ask for help: turrets, walls and
windtraps under fire, which are cheap to save and expensive to replace. A
factory that is building something is left alone -- repairing a factory stops
its production, and stopping every factory in the base is not what this is for.
Use the building's own Repair button when that is what you want.

The tech tree
-------------
tech_tree names which tech tree the game plays. "stock" is Westwood's and the
default; "mp" is the tree this fork plays in a match. In a campaign a building
is gated twice, by the mission number and by the buildings already standing; a
match has no mission number, so in multiplayer the prerequisites are the whole
tech tree and changing them is the only way to change the shape of an opening.

The mp tree hangs the early branches off the Refinery rather than the Outpost --
Barracks, Light Factory, the defence line and the Outpost itself all follow it --
puts the Heavy Factory and the Repair yard behind the Light Factory, and gives
the Outpost the three technology buildings: Hi-Tech, House of IX and Starport.
The Rocket Turret is bought with the House of IX instead of with two Construction
Yard upgrades, which moves the best defence in the game out of the opening, and
the Palace asks for Hi-Tech, IX and Starport together. WOR keeps both the
Barracks and the Refinery in its list: Harkonnen are waived the Barracks, and
without the Refinery beside it that waiver would leave them able to build WOR on
the first tick.

Either tree can be edited a line at a time from the ini: tech_req_<building> is
the list of buildings that must stand first, tech_upgrade_<building> the
Construction Yard upgrade level it also needs, tech_upgrade_levels_<building> how
many upgrade steps the building itself offers. bin/opendune.ini.sample lists both
trees side by side and every name the keys accept. A tree with a cycle, with a
building nothing can reach, or with an upgrade level the Construction Yard does
not offer is refused at start-up and the stock tree is kept.

Both players must be on the same tree. It is part of the digest the lobby folds
into its room name, so two players who disagree are put in different rooms and
never meet, which is a thing a person can act on -- unlike a desync a minute into
the match.

Starport
--------
The Starport is a trader rather than a second factory: the goods come from
off-world, so they cost more than the factory charges and take longer to
arrive. Its settings live under the same [opendune] heading.

A price is the factory price times (starport_markup/10 - 6 + two dice of 0..6)
divided by 10, so starport_markup=130 is a mean price 30% above the factory
price, with the same spread around it that the original had. There is no upper
cap on a price. 60 is the lowest markup accepted; below it the spread would fold
over itself. Ordos are the traders of the Imperium and deal at cost:
starport_markup_ordos=100.

starport_delivery=300 is the delivery time as a percentage of the house table's
ten starport ticks, a starport tick being three seconds -- so a minute and a
half. starport_delivery_ordos=150 gives Ordos half of that.

A type's opening stock is however many of it starport_stock_credits=1500 buys
at the factory price, rounded down and never less than one: ten Raider Trikes,
five Harvesters, one MCV. starport_stock_ceiling=200 is how far restocking may
refill a type, as a percentage of what it opened with.

starport_restock_ticks=1800 is the interval between top-ups, at 60 ticks per
second. Each top-up adds one of a single randomly chosen type, so lowering this
is what makes the shelves refill faster. A type that has sold out comes back; a
type the freighter never carried stays at zero.

A generated skirmish map has no scenario to stock the Starport from, so a match
between people stocks it as described above. An AI-only skirmish deliberately
leaves it empty.

What is on the shelves is the common roster: what your own factories could have
produced anyway. A House's own units are left to that House's own buildings --
the Deviator, the Devastator, the Sonic Tank and the Ornithopter want the House
of IX, and the Saboteur is what the Ordos Palace does. Sold off a freighter they
were a tech tree with a hole in it: a Starport and eight hundred credits, and the
House of IX never had to go up. starport_special_units=1 puts them back on sale.


Playing somebody over the internet
----------------------------------
"Play somebody" in the main menu sets up a one-against-one match. Both players
need the same build of the game, and both must reach the same relay: a small
server that passes messages between the two of you, because the game does not
connect the two computers directly. mp_relay in opendune.ini sets the address
the lobby offers by default; it accepts "host" or "host:port" and the port is
31337 unless you say otherwise. Anyone can run a relay -- it ships beside the
game -- and one relay is enough for both players:

    ./relay -listen 0.0.0.0:31337

When the connection breaks, the match waits rather than ends. The world stands
still, the corner of the screen says LINK LOST -- RECONNECTING or THEY DROPPED
-- WAITING with a count of seconds, and the game dials the relay again every
two seconds for two and a half minutes; the other player's game waits the same
way. When the link is back both sides resend their last moves, the corner says
"sync N, relinked 1", and the match carries on from where it stopped. Only
when the wait runs out does it end, and then the corner says so in red -- ENDED
with a reason -- and stays that way; the world keeps moving after that so the
screen can be looked at, but it is no longer the match the other player is in.

A connection can also die without saying so -- a VPN tunnel or a Wi-Fi link
that drops packets silently -- and the game does not trust the socket about
that: if nothing at all has come from the relay for fifteen seconds while the
other player is supposed to be there, it hangs up and dials again by itself.
Before that, while the world stands still because the other player's move has
not arrived yet, the corner says WAITING FOR THEM with a count of seconds; a
few of those in a row on one side means that side's connection is the one
flapping, whatever the rest of its internet looks like.

Everything a match says about its connection is also written to mp-live.log in
~/Library/Application Support/OpenDUNE/ (the same folder as opendune.ini), each
line with the time of day. If a match breaks or lags, that file from both
computers is what explains it -- it says which side lost the link, when, and
how long every wait was.

In the lobby, agree on one thing between you: the game code. Type the same code
on both machines. The map is made from it, so you do not have to agree on a map
as well. Then set the house pair to the same value on both machines, and set
"Player" to 1 on one machine and 2 on the other. Press Begin on both.

If anything differs -- the code, the house pair, the version of the game, or the
balance settings in opendune.ini -- the two of you are simply put in different
rooms and neither finds the other. This is deliberate. Two players who disagree
about the rules cannot see the same battle, and stopping before the match is
better than the game falling apart in the middle of it.

While a client is waiting it says so on screen. Press Escape to give up and go
back to the menu; otherwise it waits thirty seconds, tells you why it stopped,
and returns to the menu with your settings still in the lobby.


Ingame
------
The Game controls are the same as DUNE II. Usually, the first letter is a
keybard shortcut, for example B for build, Q for quit, etc.
F1 - open Mentat Screen
F2 - open the option menu
F3 - open the menu of the selected structure (construction yard/factory/etc.)

A few key controls are added in OpenDUNE, available depending on the
platform :
F8 - Toggle FPS display
CTRL-ENTER or F11 - Toggle full screen
[ - halve the game speed
] - double the game speed (up to x16; the factor is shown in the top right
    corner of the tactical view)


Enhancement over Dune2
-----------------------
See enhancement.txt.


Known Bugs
----------
If the digitized sound output does not work for some reason, you will very likely
be stuck in the house selection screen. Because when you click on the house button,
a voice is played, and the game waits for the voice to finish.
Please check your sound output options.


Changelog
---------
See changelog.txt.


Credits
-------
The OpenDUNE team (in alphabetical order):
  Albert Hofkamp (Alberth)             - Refactoring Dude
  Loic Guilloux (glx)                  - Windows Guru
  Patric Stout (TrueBrain)             - Lead Developer
  Steven Noorbergen (Xaroth)           - Lead Manager
  Thomas Bernard                       - Developer

Thanks To (in alphabetical order):
  Szabolcs Nagy (nsz)                  - ANSI C Guru
  Ingo von Borstel (planetmaker)       - For his many bug-reports
  tneo                                 - For his many bug-reports
  David Wang (wangds)                  - For his many patches and bug-reports

  Bug Reporters                        - Thank you all for all bug reports
  Westwood                             - For an amazing game
