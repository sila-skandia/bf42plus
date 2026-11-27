# Replay format

The file `recordReplays` writes, format version 5. The recorder is `src/replay.cpp` and the
`src/replay_*.cpp` files beside it; `src/replay_internal.h` says which writes what.

A replay is what one client saw of a round: every game event the server sent it, and a 10 Hz
sample of every object in its world. Nothing is decoded from the network stream. The recorder
reads the game's own state after the client has applied the server's updates, so a replay holds
what the player's game held, no more.

Where a field's meaning is not known, this document says so.

## The file

- Written to `replays/replay_<yyyymmdd-hhmmss>.ndjson` in the game directory, named by local time.
- One JSON object per line. Text from the game is ISO-8859-1, and every byte outside printable
  ASCII is written as a `\u00xx` escape (`\n`, `\r` and `\t` keep their short forms), so the file
  itself is plain ASCII and a JSON reader gets the right characters.
- A file opens when recording is on and the client receives an event or draws a frame in game, or
  the moment `plus.recordReplays 1` is typed.
- A file holds one join. When the server starts a new join (its first event is `serverInfo`), the
  current file closes and a new one opens. The client exits and relaunches between maps, so in
  practice a file is one map.
- A file closes on a new join, when the game exits, or when recording is switched off. A file that
  closed cleanly ends with an `end` record. The file is flushed every 2 seconds, so a crash loses at
  most that and leaves no `end`, and its last line may be cut short.

Every record has `k`, its kind. Every record but `h` has `t`: seconds since the file opened, by
the wall clock, to the millisecond. An event's `t` is when the client took it off its queue, not
when the server sent it. Samples are taken on rendered frames, at most `hz` times a second, so the
gap between them is at least 0.1 s and follows the frame rate. Every record of one sample has that
sample's `t`.

## Conventions

**Space.** Positions are world coordinates in metres. The world is left-handed with Y up. An
object's own axes are +X right, +Y up and +Z forward; a round leaves a weapon along its +Z.

**Rotations** are unit quaternions `[qx, qy, qz, qw]`, w last, taking the object's own axes to the
world's: the quaternion applied to (0, 0, 1) is the way the object faces. `createObject` is the
exception: it carries the server's angles in degrees, `[yaw, pitch, roll]`, yaw about +Y and then
pitch about X. Roll has only ever been seen as 0.

**Soldiers.** A soldier's sampled position is 1 m above his feet. His `createObject` position is
his spawn point on the ground.

**World time** is the server's round clock, in seconds. It goes back to 0 when a round starts
(`gameStatus` 3).

**IDs**

| name | what it is |
| --- | --- |
| `pid` | A player's id, 0 to 255. Reused: when a player leaves, the next to join may get his id. A pid means one person from his `createPlayer` to his `destroyPlayer`. |
| network id | The server's 16-bit id for a replicated object, the same on every client, so two recordings of one round agree on it. Called `id` in `o`, `s`, `d`, `a` and `st`, and `netId` (or `vehNetId`, `camNetId`, `kitNetId`) everywhere else; a part's or engine's vehicle is its `root`. It is how records refer to one another. A respawned vehicle gets a new id, and ids are reused later in a round. |
| seat id | A vehicle's seats take the network ids straight after the vehicle's own: a tank with id 532 has its hull gun at 533. |
| `gid` | The object's id in this client's object manager. The same each time an object comes back into view. A control point's `id` in `cp` is one of these. |
| `tid` | A template id. Template ids follow the order this client loaded its templates, so they only mean something within the file. `tmpl`, the template's name, is the stable one. |
| part and engine ids | In `jn`, `j` and `g`: the recorder's own numbers, from 1, once per file, one count for parts and engines together. A moving part or an engine has no network id of its own. |

**Teams** are 1 and 2 for the two sides: 1 Axis and 2 Allies in vanilla. Mods rename the sides.
0 is no team.

`-1` means none: no player, no object.

**Changes only.** The sampled records (`s`, `a`, `p`, `cp`, `tk`, `j`, `g`, `st`) each list only
what changed since it was last written. A reader keeps the last value of everything.

## Versions

`v` in the header is the format version. Within a version, records and fields are only ever
added, never changed or removed, so a reader must skip any record kind or field it does not know.
A change that breaks a reader would raise the version.

Some records depend on this exact BF1942.exe. The recorder checks the game's code before it reads
anything for them, and on another build of the game they are simply absent: `f`, hit points in
`a` and `o`, `jn`, `j`, `g`, `st`, `anim`, `kh` and `tk`.

## A file begun mid-round

Recording can be switched on at any time, and a file that opens after the join has missed the
events that name things: the server, the level, the players and every object made so far. The
recorder keeps those as they arrive whether or not a file is open, and a file that opens late
starts with them, each at `t` 0 with an extra field `ago`, the seconds before the file opened that
it arrived:

1. the latest `serverInfo`, `serverName`, `challenge`, `gameRules` and `setLevel`;
2. each player's latest `createPlayer`, for every player still in the game. Its `team` is his
   side when he joined;
3. a `createObject` for every object made and not destroyed since, in the order they came;
4. a `projPool` for every projectile pool still standing;
5. each player's latest `pickupKit`;
6. a `roster` of who is playing, with each player's side as the file begins.

These are the state of the round when the file began, not things that happened at its start. A
late file has no `simStart`, `mapList`, `welcome`, `sessionId`, `dbComplete` or `gameStatus` from
the join. A file that opens with the join has none of the above: it records the join as it comes.

## What one client can see

A replay is one player's view, and the server does not send every client everything.

- **Objects far away are named but not seen.** Positions, hit points, turrets, engines, soldiers'
  bodies and triggers are sent only for objects within the level's view distance plus 20 m of the
  player's viewpoint (about 520 m on Wake, 120 m on Berlin). The viewpoint is his soldier or
  vehicle, or his free camera while dead or choosing a spawn. Beyond that, the server still sends
  them for every living player on his own side, the enemy flag carrier and a spotted player. An
  object that goes out of range gets a `d`, and an `o` again when it comes back.
- **Events reach every client.** Joins, sides, spawns, seats, kits, kills, deaths, chat, every
  object's creation and removal, control points, tickets and the round's tallies arrive wherever
  the player is. A `createObject` names every object on the map, near or far.
- **Most projectiles are not objects.** Bullets, shells, rockets and bombs are simulated by each
  client from the shot, and never appear in `o` or `s`. Every round the client simulates is an
  `f` record instead. Grenades, explosive packs, land mines, floating mines and the binoculars'
  marker are networked: they come from a `projPool` and are sampled like any other object.
- **Who hit what is not sent.** `lastHitPlayer` in `a` is always -1 on a client. Damage appears
  only as hit points falling. Kills, which name both players, come as `score` events.
- **The recording player's own soldier is predicted.** Its position is where the client put it,
  which may differ a little from where the server had it.
- **Some records are the recording player's alone:** `hm`, the `hitFrom`, `special` and `fire`
  events, and the radio messages he heard.

## Records

| `k` | what it is |
| --- | --- |
| [`h`](#h-header) | header, the first line |
| [`e`](#e-events) | a game event |
| [`o`](#o-object-first-seen) | an object first seen |
| [`s`](#s-sample) | object positions and rotations |
| [`d`](#d-object-out-of-sight) | an object no longer sampled |
| [`a`](#a-hit-points) | hit points |
| [`p`](#p-players) | players: side, what they control, triggers |
| [`cp`](#cp-control-point) | a control point and its owner |
| [`tk`](#tk-tickets) | the two sides' tickets |
| [`jn`](#jn-moving-part-first-seen) | a vehicle's moving part first seen |
| [`j`](#j-moving-parts) | moving part rotations |
| [`g`](#g-engines) | engines |
| [`st`](#st-soldiers) | soldiers' animation states, aim and held item |
| [`anim`](#anim-animation-states) | the animation state table, once |
| [`kh`](#kh-kit-part-heat) | a jet pack's heat and state |
| [`f`](#f-a-round-fired) | a round fired |
| [`hm`](#hm-hit-mark) | the recording player's crosshair hit mark |
| [`chat`](#chat-a-line-in-the-chat-box) | a line shown in the chat box |
| [`roster`](#roster-who-is-playing) | who is playing, in a file begun mid-round |
| [`end`](#end-the-file-closed) | the file closed cleanly |

Within one sample the records come in this order: `o` (one per new object), `s`, `a`, `d`,
`anim` (once), `jn`, `j`, `g`, `st`, `kh`, `tk`, `p`, `hm`, `cp`. An object's `o` always comes before
the `s` that carries its first position.

### `h`: header

```json
{"k":"h","v":5,"plus":"2.0","build":"Oct 16 2026 21:14:03","start":"2026-10-17T07:00:41","hz":10}
```

| field | |
| --- | --- |
| `v` | format version |
| `plus` | the bf42++ version. It is not changed for every build. |
| `build` | when the DLL was compiled, which tells two recorders apart |
| `start` | local time the file opened, no time zone |
| `hz` | samples per second |

### `o`: object first seen

```json
{"k":"o","t":38.842,"id":7639,"gid":41217,"tmpl":"USSoldier","tid":26996,"team":2,"maxhp":30.0,"crit":0.0}
```

Written the first time a root object (not a part of another object) appears in the client's
world, and again each time it comes back after a `d`. Its position is in the same sample's `s`,
and its hit points, if it has any, in the same sample's `a`.

| field | |
| --- | --- |
| `id` | network id |
| `gid` | object manager id |
| `tmpl`, `tid` | template name and id |
| `team` | The object's team when seen: a soldier's or a projectile's side; for a vehicle, its occupants' side, 0 when empty; -1 for anything else, control points included (see `cp`). Not updated afterwards. |
| `maxhp` | full hit points. Only for objects that have hit points: soldiers and vehicles. |
| `crit` | the critical damage threshold. Below it, the object burns and loses hit points on its own. Only for objects that have hit points. |

### `s`: sample

```json
{"k":"s","t":38.304,"o":[[50,1153.09,94.01,1019.00,0.000,0.000,0.000,1.000]]}
```

`o` is a list of `[id, x, y, z, qx, qy, qz, qw]`: an object's position and rotation. An object is
listed when it is first seen and whenever it has moved more than 1 cm or turned; an object not
listed held still.

### `d`: object out of sight

```json
{"k":"d","t":39.880,"id":8289}
```

The object `id` is no longer sampled: it went out of range, was disabled, or was removed. A `d`
does not mean destroyed. Hit points reaching 0 in `a` is destruction, and `destroyObject` is
removal. A vehicle's turrets and engines can go on being written for a few seconds after its `d`.

### `a`: hit points

```json
{"k":"a","t":38.842,"a":[[3752,2575.0,-1],[6509,80.0,-1]]}
```

`a` is a list of `[id, hitPoints, lastHitPlayer]`, written when an object with hit points is seen,
and again when it comes back into view, and whenever its hit points change. Only a change is
damage. A steady fall below `crit` is burning, and 0 is destroyed: a vehicle's wreck stays about
10 seconds until its `destroyObject`. `lastHitPlayer` is always -1 on a client.

### `p`: players

```json
{"k":"p","t":38.304,"p":[[0,1,7504,7504,0,0],[11,1,7917,7916,1,0]]}
```

`p` is a list of `[pid, team, controlled, root, seat, triggers]`, written for a player when his
team, what he controls or his triggers change.

| field | |
| --- | --- |
| `controlled` | network id of what the player controls: his soldier, a seat, or his free camera while dead or choosing a spawn. -1 for none. |
| `root` | network id of the object `controlled` belongs to: the vehicle for a seat; the soldier or camera itself otherwise. -1 for none. |
| `seat` | `controlled - root`: 0 for the vehicle's own seat, or for a soldier or camera; 1 and up for the seats after it. |
| `triggers` | bit 1 fire held, bit 2 alternate fire held, as the server sees them, for every player in range, bots included. A tap shorter than the server's update interval can be missed. |

### `cp`: control point

```json
{"k":"cp","t":38.304,"id":35247,"name":"South_Base","tmpl":"southbase","pos":[1062.72,66.38,710.57],"team":-1}
{"k":"cp","t":39.160,"id":35247,"team":2}
```

On first sight, with `name`, `tmpl` and `pos`; after that `id` and `team`, whenever the owner
changes. `id` is an object manager id, not a network id. `name` is the point's name as the level
gives it, a localisation key rather than the text the game shows.

`team` is the owner: 1 or 2, 0 neutral, and -1 for a point whose side has not arrived yet, which
lasts under a second after it is first seen. A capture passes through neutral, so a point going
from 2 to 1 reads 2, 0, 1. Control points are sent at any range. Capture progress is not sent to
clients.

### `tk`: tickets

```json
{"k":"tk","t":39.260,"v":[597,384]}
```

`v` is `[team 1, team 2]`, the tickets as the HUD shows them, written whenever either changes. A
file begun mid-round may read `[0, 0]` until the server's first count arrives.

### `jn`: moving part first seen

```json
{"k":"jn","t":38.304,"o":[[530,1,"DualBofors_prim1_X_NoSeat",12.80,10.88,49.90]]}
```

A moving part is a turret, a gun mount or similar, turned relative to the vehicle it is on. `o` is
a list of `[root, part, tmpl, x, y, z]`: the vehicle's network id, the part's id, its template
name (the same as the part's node in the vehicle's model), and where it sits on the vehicle when
first seen, in metres along the vehicle's own X, Y and Z axes. The position tells identically
named parts apart, such as a ship's guns. The part's `j` follows in the same sample.

### `j`: moving parts

```json
{"k":"j","t":38.304,"o":[[530,3,-0.0000,-0.0042,0.0000,1.0000]]}
```

`o` is a list of `[root, part, qx, qy, qz, qw]`: the part's rotation relative to its vehicle,
written when first seen and whenever it turns. The part's rotation in the world is the vehicle's
rotation followed by this one: `q_part = q_root · q`. This is a turret's traverse and a gun's
elevation.

### `g`: engines

```json
{"k":"g","t":38.304,"o":[[603,0.000,0.000,1,1,107]]}
```

`o` is a list of `[root, revs, throttle, flags, gear, engine]`, written when an engine is first
seen and whenever one of them changes. `engine`, the engine's id, comes last.

| field | |
| --- | --- |
| `root` | network id of the vehicle |
| `revs` | engine speed. The engine sound and a propeller's spin follow its absolute value. |
| `throttle` | throttle position, in the vehicle template's own units: about -4000 to 5000 on aircraft and boats, -1 to 1 on land vehicles |
| `flags` | bit 1 running, bit 2 disabled by damage |
| `gear` | the engine's gear. What its values mean is not known. |

### `st`: soldiers

```json
{"k":"st","t":38.842,"o":[[7639,18666,4466,-0.9,-9.5,3,96]]}
```

`o` is a list of `[id, lower, upper, pitch, twist, item, bits]`, written for a soldier whenever
his states, item or bits change, or his aim moves by 1 or more.

| field | |
| --- | --- |
| `id` | network id of the soldier |
| `lower` | lower body (legs) animation state, an index into `anim` |
| `upper` | upper body animation state, an index into `anim` |
| `pitch` | aim pitch in degrees, up positive, as the body shows it: a round's pitch is about 2.5 times this |
| `twist` | torso twist in degrees off the body's heading, as the body shows it: a round's yaw off the heading is about -3 times this. Pitch and twist are both in the soldier's own frame, which tilts with the ground when he lies on a slope. |
| `item` | the item in his hands: its place in his kit, from 1 |
| `bits` | state bits, below |

What the soldier is doing comes from the two states in `anim`:

- **Stance** is in the lower state's flags: 0x20 crouching, 0x40 lying, 0x08 swimming, 0x10
  climbing, 0x80 jumping.
- **Firing, reloading and stabbing** are in the upper state's name: a name with `Fire` in it, not
  ending in `End`, is firing; `Ub_AltFire` and `Ub_LieAltFire` are a bayonet or knife stab; a name
  with `Reload` in it is reloading.

| bit | |
| --- | --- |
| 0x01, 0x02, 0x04, 0x08 | moving forward, back, left, right |
| 0x10 | parachute open |
| 0x20 | zoomed in (seen in recordings, not read in the game) |
| 0x40 | touching the ground |
| 0x80 | zooming (seen in recordings, not read in the game) |
| 0x1F00 | recoil count, while the weapon in hand has recoil |
| 0x2000 | falling free |
| 0x4000 | not known; seen when bailing out |

For soldiers other than the recording player's, the client shows the newest states it has
received, so `st` can run slightly ahead of the position in `s`.

### `anim`: animation states

```json
{"k":"anim","t":12.296,"states":[[0,"StopDummyState",0],[1,"Lb_Stand",0]]}
```

The game's animation state table, written once per file just before the first `st`. `states` is a
list of `[index, name, flags]`, with gaps where the table has an empty slot. `flags` holds the
stance flags above; its other bits are not known. The table belongs to the mod, so its indices
mean nothing in another mod's file: vanilla has 1,395 states, FHSW Europe 58,059.

If the table is larger than the recorder will read, the record says so instead: `skipped` is the
table's size, `limit` the most the recorder reads, and `states` is empty:

```json
{"k":"anim","t":12.296,"skipped":300000,"limit":262144,"states":[]}
```

### `kh`: kit part heat

```json
{"k":"kh","t":129.402,"o":[[7641,0.450,0],[7712,0.000,1]]}
```

`o` is a list of `[id, heat, state]`, one per soldier wearing a kit part with heat, written when
the state changes or the heat moves by 0.005 or more. In the three packs that is Secret Weapons'
jet pack (`GermanElite_RocketPack`, kit `GermanElite_JetPack`); a soldier without one never
appears.

| field | |
| --- | --- |
| `id` | network id of the soldier |
| `heat` | the part's heat, 0 cool to 1 overheated: what the HUD's jet pack bar shows, filling as it heats |
| `state` | 0 burning, 1 idling, 2 overheated, 3 off (masked off, or not yet lit), 4 waiting on its activater |

It is the part the game's own HUD reads: the first of the soldier's active kit parts that is not in
state 4 and has heat. The heat is this client's: the game runs the part's heat from its state for
every soldier it holds, the server sending the state, so a burn shows as state 0 and the heat
climbing. A soldier's last `kh` stands until his next one, and goes with him.

### `f`: a round fired

```json
{"k":"f","t":40.402,"id":3752,"pid":1,"w":"5inchL38Dual_GunBarrel_Dummy","p":[1713.93,74.42,1208.21],"d":[-0.746,0.013,-0.666]}
```

Every round the client simulates, for every player in range, bots included. One record per shot
the game fires; a multi-barrel salvo is one record. A weapon that does not fire (overheated,
between shots, empty) writes nothing, and nor do the medic's and engineer's tools.

| field | |
| --- | --- |
| `id` | network id of the object the weapon belongs to: the soldier on foot, the vehicle for a vehicle's gun. -1 for none. |
| `pid` | who fired, or -1 when the game names no player |
| `w` | the weapon's template name |
| `p` | where the round was fired from: the camera's position for a weapon aimed from the camera (hand weapons), otherwise the weapon's own |
| `d` | the direction it was fired in, a unit vector: that camera's or weapon's +Z axis |
| `local` | 1 when the recording player fired. Absent otherwise. |

A shot by another player depends on the server's trigger flags reaching this client, so a single
quick shot (a bolt action, a bazooka, a tank gun) can be missed. `roundStats` has the true count.
Another player's shots arrive a little after the server fired them, sometimes after the kill they
made.

### `hm`: hit mark

```json
{"k":"hm","t":215.600,"pid":24,"at":215.520}
```

The recording player's crosshair hit mark: one of his rounds hit something directly. `pid` is the
recording player. `at` is when the hit happened, worked out from how far the mark has faded at
`t`. A blast does not make a mark, and nor does hitting an empty vehicle; hitting a teammate does.
Two hits less than a sample apart, or within 50 ms of each other, make one record, and a frame
hitch can make a mark that was not there.

### `chat`: a line in the chat box

```json
{"k":"chat","t":37.774,"pid":4,"team":2,"text":"Player1 [allies]: f2 flag"}
```

Every line the chat box shows, and the only record of chat. `text` is the line as shown, with the
speaker's name in front as the game writes it, and a team message's side after the name. This
includes the recording player's own chat, which the server never sends back to him. `pid` is the
speaker, or -1 for a line no player said, such as a server message. `team` is the team whose colour the line is drawn in. Radio messages are not here; they
are `radio` events.

### `roster`: who is playing

```json
{"k":"roster","t":0.012,"p":[[0,1,0,"Player1",0],[24,2,0,"Player2",1]]}
```

Only in a file begun mid-round, after the held events. `p` is a list of
`[pid, team, ai, name, local]`: `team` his side as the file begins, `ai` 1 for a bot, `local` 1
for the recording player.

### `end`: the file closed

```json
{"k":"end","t":1476.612}
```

## `e`: events

Every event has `e`, its name. All but `fire` are the server's, listed with the engine's event id.
The event's fields follow its name. Where the game's event carries more than is listed here, the
rest is not recorded.

### The join

| `e` | event id | fields |
| --- | --- | --- |
| `serverInfo` | 0x1A | `mapId`, `mod` and `gameId`. The first event of every join. What `mapId` and `gameId` hold beyond their names is not known. |
| `serverName` | 0x1B | `name` |
| `challenge` | 0x14 | `mod`, the mod the server runs; `xpack`, the expansion pack, -1 for none |
| `sessionId` | 0x37 | `id`, this client's connection id, such as `_ClientID_24` |
| `mapList` | 0x13 | One entry of the server's map rotation; the whole list comes at the join, and a list sent again replaces it. `add` is 1 (what 0 would mean is not known), `map`, `mode` (`ctf`, `conquest`, `tdm`, `coop`, `objective`, or empty), `mod`. |
| `setLevel` | 0x36 | `level`, the level's path, such as `bf1942/levels/wake/`; `mode`, its game mode file, such as `conquest.con` |
| `gameRules` | 0x16 | `extViews` and `noseCam` (1 allowed), `crosshair` (1 the crosshair has its centre dot), `soldierFF` and `ticketRatio` (scale not known), `timeLimit` in seconds (0 none), `worldTime`. Sent at the join, at every round start, and whenever a rule changes. |
| `welcome` | 0x17 | One line of the server's welcome message. `id` is probably the line's place. `text` can run on past a `\u0000` into junk: cut it at the first `\u0000`. |
| `dbComplete` | 0x34 | The server has sent the client everything on the map. No fields. |
| `simStart` | 0x04 | `running`, always 1, and `worldTime`. Sent once per join, as the world finishes loading. |

### The round

| `e` | event id | fields |
| --- | --- | --- |
| `gameStatus` | 0x24 | `status`: 1 playing, 2 round over, 3 round starting (world time goes back to 0), 4 paused, 5 map over. Sent again every time a player joins. A file joined mid-round gets 1 with no 3 before it. After 2 or 5 the server tears the round down: deaths, exits and a `destroyObject` for every object. |
| `clock` | 0x29 | `timeLimit` and `worldTime` in whole seconds. Sent every 10 seconds. |
| `roundStats` | 0x30, 0x31, 0x32 | A player's tally for the round, at its end, sent to every client for every player: `stat` (`destroyed` vehicles destroyed, `fired` rounds fired, `hit` rounds that did damage), `pid`, and `rows`, up to 10 of `{tid, tmpl, n}`, by the template that did it (the vehicle when mounted, the weapon on foot). A player with more than 10 rows gets more than one event. |
| `special` | 0x27 | `action`: 0 the recording player was resupplied at a depot (every half second while he stands there); 1 a hit mark, which dedicated servers do not send this way (see `hm`); 2 seen during the join. |
| `vote` | 0x12 | `action` (0 started, 1 failed, 2 passed, 3 a vote cast), `type` (0 map, 1 kick, 2 team kick), `pid` who started it or voted, `target` (a player id, or for a map an index into `mapList`), `yes`, `required` |

### Players

| `e` | event id | fields |
| --- | --- | --- |
| `createPlayer` | 0x08 | `pid`, `name`, `team`, `ai` (1 for a bot), `netId` (the player's own network id), `vehNetId` (what he controls: his soldier if he has spawned, possibly 0), `camNetId` (his free camera), `kitNetId` (his kit). At the join the server sends one for every player already there, each followed by a `score` of kind 7. |
| `destroyPlayer` | 0x0C | `pid`, who left. His id goes to the next player to join. |
| `setTeam` | 0x39 | `pid`, `team` |
| `control` | 0x09 | `pid` now controls `netId`: his soldier when he spawns, his camera when he dies and when the round ends |
| `enterVehicle` | 0x0A | `pid` took the seat `netId`, the seat's id, not the vehicle's |
| `exitVehicle` | 0x0B | `pid` got out. The exits the server sends while tearing a round down are not recorded. |
| `pickupKit` | 0x23 | `pid` took the kit `netId`. A player who had already spawned at the join gets none: his kit is his `createPlayer` `kitNetId`. |
| `score` | 0x2A | `kind`, `pid`, `victim`, `weapon`, `bodypart`, and `weaponName` for a kill. See below. |
| `radio` | 0x3A | `pid`, `msg` the message (below), `global` (1 over the team radio, 0 shouted to anyone within about 70 m) |
| `hitFrom` | 0x3C | The recording player was hurt, or the vehicle he is in was hit. `dir`, the side it came from, below; `strength`, 255 × the damage ÷ his full hit points. |
| `fire` | | The recording player, `pid`, pressed fire, from his own input: one per press, not per round. `kind` 1 fire, 2 alternate fire alone (zooming in shows as 2); `weapon`, the template of what he controls, not the weapon; `pos` and `dir`, its position and +Z axis, `[0,0,0]` and `[0,0,1]` when he controls nothing. `f` has every round. |

`score` `kind`:

| `kind` | |
| --- | --- |
| 0 | captured a flag |
| 1 | attack: in conquest, every player who raises a flag from neutral; in CTF, took the flag |
| 2 | defence; in CTF, returned the flag |
| 3 | kill: `pid` killed `victim` |
| 4 | death of `pid` with no killer: a suicide, a fall, a crew lost with its vehicle. A team kill's victim also gets one. |
| 5 | death of `pid`, not announced |
| 6 | team kill: `pid` killed `victim` |
| 7 | `pid` spawned |
| 8 | objective |
| 9 | team kill on an objective |

For a kill or a team kill, `weapon` is the template id of what killed: the weapon on foot, the
vehicle when mounted, or -1 when the game names nothing (a bot's fire, a crash). `weaponName` is
its name, when there is one. For other kinds `victim` and `weapon` are not set and may hold any
number. What `bodypart` holds is not known.

`hitFrom` `dir`, in 45° sectors around the player, with "right" his +X side:

| `dir` | | `dir` | |
| --- | --- | --- | --- |
| 0 | ahead | 4 | behind |
| 1 | ahead right | 5 | behind left |
| 2 | right | 6 | left |
| 3 | behind right | 7 | ahead left |

`radio` `msg`. The ids are the game's; a mod can change the words and voices behind them.

| `msg` | |
| --- | --- |
| 1, 2 | roger, negative |
| 8 to 14 | requests: pickup, reinforcements, anti-tank, naval, air, artillery ready, APC |
| 15 to 21 | spotted: armour, infantry, unit, ship, submarine, aircraft, scout |
| 22 to 28 | control point 1 to 7 (whether it is "attack" or "defend" is the listener's) |
| 29 to 32 | shouted: defending, attacking, roger, negative |
| 36 to 49 | shouted: wait, fire, hold fire, fire in the hole, medic, take cover, bail out, cover me, hold position, you take the point, fall back, stick together, follow me, go |
| 51 to 54 | in a vehicle: get in, check your six, abandon ship, need repairs |
| 55 to 58 | CTF: go for the enemy flag, protect the flag, get the flag back, protect the flag carrier |
| 59 | artillery support |

### Objects

| `e` | event id | fields |
| --- | --- | --- |
| `createObject` | 0x07 | `tid`, `netId`, `tmpl`, `pos`, `rot` (`[yaw, pitch, roll]` in degrees). Sent for every object on the map, near or far, where it spawns. A vehicle that respawns comes again with a new `netId`. |
| `destroyObject` | 0x06 | `netId` was removed: a wreck about 10 seconds after it was destroyed, and every object when a round ends. It can name an object the file never saw. |
| `projPool` | 0x05 | A kit's pool of networked projectiles, made when a player spawns: `tid` and `tmpl` the projectile (a grenade, a land mine, an explosive pack, the binoculars' marker), `count` how many, and `netId` the first of their network ids, `netId` to `netId + count - 1`. Each is disabled until it is thrown or laid. |

### Raw

```
{"k":"e","t":<t>,"e":"raw","type":<event id>,"size":<bytes>,"raw":"<hex>"}
```

An event the recorder has no layout for. `type` is the event id, `size` the event's size in bytes
as the game defines it (including a 12-byte header), and `raw` the `size - 12` bytes after the
header, in hex. `size` is `null` when the game's size could not be read, and `raw` is then the
first 48 bytes, which may run past the event. Every such event is also written, in hex and as
text, to `logs/bf42++_unknown_events.log`, across all recordings.

The ones expected:

| `type` | event | |
| --- | --- | --- |
| 0x0E | SetTempCamera | a soldier's network id, when a destroyed vehicle moves his camera |
| 0x10 | UpdateRate | |
| 0x18 | ResetMap | |
| 0x1E | FlagHolder | CTF: 1 taken or 0 dropped, then the player's id. It does not say which flag. |
| 0x25 | ControlPointCaptured | not known |
| 0x3B | TriggerObjectSpawner | not known |

HUD text (0x01) and bf42++'s own static object events (0x1C, 0x1D) are not recorded.
