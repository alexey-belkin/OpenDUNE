# The OpenDUNE relay

A pipe between the two players of a multiplayer match. It forwards packets and
does nothing else: it never parses a command, holds no state a client could
disagree with, and decides nothing. Two clients that disagree about the world
find out from each other's checksums, not from here — which is what keeps a bug
in this program from becoming a bug in a match.

Why a relay at all, rather than a direct connection between the players: peer to
peer needs hole punching and still fails behind symmetric NAT, which is most home
routers and every mobile network. Both clients dialling **out** to one public
address works everywhere, and the room name doubles as the join code.

## Building

```bash
cd tools/relay
go build -o relay .
```

For the server, from any machine:

```bash
GOOS=linux GOARCH=amd64 go build -o relay-linux-amd64 .
```

It has no dependencies beyond the Go standard library, and the result is one
static binary with no configuration file.

## Running

```bash
./relay -listen :31337 -verbose
```

| Flag | Meaning |
|---|---|
| `-listen` | address to listen on, default `:31337` |
| `-verbose` | log joins, leaves and errors — one line each, no packet contents |
| `-lag` | hold every forwarded packet back this long (`-lag 150ms`), to emulate a ping when testing |

`-lag` is a testing tool. It belongs here rather than in the game because the
game must not be able to tell a slow relay from a slow opponent.

## Deploying

The relay keeps no data, so a restart costs nothing but the matches in progress,
and there is nothing to back up.

```ini
# /etc/systemd/system/opendune-relay.service
[Unit]
Description=OpenDUNE multiplayer relay
After=network.target

[Service]
ExecStart=/usr/local/bin/opendune-relay -listen :31337
Restart=always
RestartSec=2

# A dedicated transient user rather than `nobody`, which half the system
# already shares. The relay needs no files, no home and no privileges: it
# opens one socket and forwards bytes.
DynamicUser=yes
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
PrivateDevices=yes
RestrictAddressFamilies=AF_INET AF_INET6
MemoryMax=128M

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl enable --now opendune-relay
sudo ufw allow 31337/tcp        # or: firewall-cmd --add-port=31337/tcp --permanent
```

Traffic is negligible: one packet per player per turn, 7.5 per second at the
default turn length, a few dozen bytes each. A match is under a kilobyte per
second in both directions together, so the smallest VPS on offer is oversized for
several hundred simultaneous matches.

### Sharing a server with something else

The relay costs about ten megabytes of memory and no measurable CPU, so it fits
anywhere. What it is sensitive to is **being scheduled promptly**, and that is
worth saying out loud on a box that also runs something busy.

Every millisecond the relay spends waiting for a CPU is a millisecond added to
both players' ping, and the ping table above shows the margin is thinner than
the arithmetic suggests: at a 150 ms round trip the nominal budget at `D=2` is
266 ms, and it still stutters, because the slack has to absorb scheduling jitter.
On a shared one-core VPS, assume more jitter and use `D=3`.

If the neighbour is genuinely bursty, `Nice=-5` in the unit is enough — the relay
wants latency, not throughput, and it gives the CPU straight back.

Port choice matters more than it looks: some mobile and corporate networks allow
only well-known ports outbound, and a player behind one cannot reach a relay on
31337 at all. If that shows up, move the relay to a port such a network does let
through rather than debugging the game.

## The instance that exists

One is deployed on the VDSina VPS that also runs the LiteLLM proxy:
`146.103.110.160:31337`, as `opendune-relay.service`. It shares the box with
nginx (80, 443) and the proxy (localhost only), and takes a megabyte of memory.
A round trip through it, warm, is about 90 ms from Moscow — which is why the
default turn delay is 3 rather than 2.

```bash
systemctl status opendune-relay
journalctl -u opendune-relay -f
```

## Connecting a game to it

```bash
./opendune --skirmish=ordos,harkonnen --human=1,2 --mp-turnloop=20000,1000 --mp-relay=HOST:31337,room,1
./opendune --skirmish=ordos,harkonnen --human=1,2 --mp-turnloop=20000,1000 --mp-relay=HOST:31337,room,2
```

Both must name the same room and the same seed, and take different slots. There
is no lobby yet: this command line is the lobby.

## The wire protocol

Line-oriented and readable on purpose. When a match desyncs the packets are the
evidence, and being able to watch a room with `netcat` is worth more than the
bytes it costs.

```
client -> relay   JOIN <room> <slot>\n
relay  -> client  WELCOME <slot> <members>\n
relay  -> client  READY <members>\n           once the room is full
relay  -> client  LEFT <slot>\n               when somebody drops
client -> relay   PKT <length>\n<length bytes>
relay  -> client  PKT <slot> <length>\n<length bytes>
```

The slot in an outgoing `PKT` is filled in from the connection it arrived on,
never from what the sender claims: a client cannot speak for its opponent.

## What it deliberately does not do

* **No authentication and no encryption.** A room name is a shared secret and
  nothing more. Anyone who guesses one can join the match as the free slot; the
  worst they can do is desync it, which both players will see. Pick a room name
  that is not `test`.
* **No matchmaking, no persistence, no accounts.** Rooms exist while somebody is
  in them.
* **No more than two players per room**, matching v1 of the game.
