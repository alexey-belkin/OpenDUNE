#!/bin/bash
#
# Start both sides of one networked match on this machine.
#
# Two GUI clients, two windows, one world: switch between them with Cmd-Tab and
# you are looking at the same match through two pairs of eyes.  They meet in a
# room on the public relay, so the packets take the real path over the internet
# rather than a pipe -- which is the point of testing it this way.
#
# Both clients write a log.  When the match ends the script says where, and the
# last thing it prints is whether the two logs agree: a networked match that
# desyncs is one where they stop agreeing, and that line is the answer.
#
#   tools/mpduel.sh                 two players, four units each, speed x2
#   tools/mpduel.sh --units=0       no starting squad
#   tools/mpduel.sh --seed=1234     a named map, to replay the same one
#   tools/mpduel.sh --room=foo      a named room, if two are running at once
#   tools/mpduel.sh --purity=1      also clamp drawing and input (costs ~40% of
#                                   the tick budget; only when hunting)
#   tools/mpduel.sh --dump=0        do not keep the rolling state snapshots
#
set -u

RELAY=146.103.110.160:31337
ROOM=""
SEED=""
UNITS=4
SPEED=2
HOUSES=ordos,harkonnen
SAMPLE=500
DUMP=1
PURITY=0
EXTRA=""

for arg in "$@"; do
	case "$arg" in
		--relay=*)  RELAY=${arg#*=} ;;
		--room=*)   ROOM=${arg#*=} ;;
		--seed=*)   SEED=${arg#*=} ;;
		--units=*)  UNITS=${arg#*=} ;;
		--speed=*)  SPEED=${arg#*=} ;;
		--houses=*) HOUSES=${arg#*=} ;;
		--sample=*) SAMPLE=${arg#*=} ;;
		--dump=*)   DUMP=${arg#*=} ;;
		--purity=*) PURITY=${arg#*=} ;;
		-h|--help)  sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*)          EXTRA="$EXTRA $arg" ;;
	esac
done

cd "$(dirname "$0")/.." || exit 1

GAME=bundle/OpenDUNE.app/Contents/MacOS/opendune
[ -x "$GAME" ] || GAME=bin/opendune
if [ ! -x "$GAME" ]; then
	echo "no build to run: make bundle (or make) first" >&2
	exit 1
fi

# A fresh room every time, so an abandoned client from a previous run cannot be
# mistaken for this run's opponent.
[ -n "$ROOM" ] || ROOM="duel$$"
[ -n "$SEED" ] || SEED=$$

LOGDIR=$(mktemp -d /tmp/mpduel.XXXXXX)

FLAGS=""
[ "$DUMP" = 1 ]   && FLAGS="$FLAGS --mp-desync-dump"
[ "$PURITY" = 1 ] && FLAGS="$FLAGS --sim-purity"

# The snapshots land beside the binary's working directory; sweep them into the
# log directory at the end so one run's evidence cannot be mistaken for the
# next one's.
rm -f mpdesync-s*-turn*.bin

echo "relay $RELAY   room $ROOM   seed $SEED   speed x$SPEED   units $UNITS"
echo "logs  $LOGDIR"
echo

for slot in 1 2; do
	"$GAME" --skirmish="$HOUSES" --human=1,2 \
		--speed="$SPEED" --mp-units="$UNITS" --mp-seed="$SEED" --mp-sample="$SAMPLE" \
		--mp-relay="$RELAY,$ROOM,$slot" $FLAGS $EXTRA \
		> "$LOGDIR/p$slot.log" 2>&1 &
	eval "PID$slot=$!"
	# The relay pairs the first two clients in a room; give slot 1 the head
	# start it would have had anyway, rather than racing the connect.
	[ "$slot" = 1 ] && sleep 2
done

echo "player 1 pid $PID1, player 2 pid $PID2 -- quit either window to end the match"
trap 'kill -9 $PID1 $PID2 2>/dev/null' INT TERM
wait $PID1 2>/dev/null
wait $PID2 2>/dev/null
kill -9 $PID1 $PID2 2>/dev/null

echo
mv -f mpdesync-s*-turn*.bin "$LOGDIR/" 2>/dev/null
a=$(grep -c '^mp-checksum' "$LOGDIR/p1.log" 2>/dev/null || echo 0)
b=$(grep -c '^mp-checksum' "$LOGDIR/p2.log" 2>/dev/null || echo 0)
echo "$a checksums from player 1, $b from player 2"
if [ "$a" -gt 0 ] && [ "$b" -gt 0 ]; then
	if diff <(grep '^mp-checksum' "$LOGDIR/p1.log") \
	        <(grep '^mp-checksum' "$LOGDIR/p2.log") > /dev/null 2>&1; then
		echo "the two players saw the same world throughout"
	else
		first=$(diff <(grep '^mp-checksum' "$LOGDIR/p1.log") \
		             <(grep '^mp-checksum' "$LOGDIR/p2.log") \
		        | grep '^<' | head -1 | awk '{print $3}')
		echo "the two players stopped agreeing at $first -- logs in $LOGDIR"
	fi
fi
grep -h 'DESYNC' "$LOGDIR"/p1.log "$LOGDIR"/p2.log 2>/dev/null | head -2
ls "$LOGDIR"/mpdesync-*.bin >/dev/null 2>&1 && \
	echo "state snapshots kept: $(ls "$LOGDIR"/mpdesync-*.bin | wc -l | tr -d ' ') files"
echo "$LOGDIR"
