#!/bin/bash
#
# Build one file that is the whole game, ready to run on another Mac.
#
# `make bundle` alone does not produce that.  It leaves out the game data, and
# the binary it copies in still loads SDL from /opt/homebrew -- a path that
# exists on this machine and on almost no other, so the app dies on launch with
# a dyld error and no window.  This script closes both holes: it vendors the
# SDL dylib into the bundle and rewrites the load command to point inside it,
# and it puts bin/data/ back where `make bundle`'s own `rm -rf bundle/` removed
# it.
#
# Then it proves the result rather than assuming it: the finished zip is
# unpacked into a scratch directory and the headless self-tests are run from
# *that* copy.  A package that does not pass is not written to bundles/.
#
#   tools/package.sh                 build, package, verify
#   tools/package.sh --no-data       engine only, no *.PAK (see INSTALL note)
#   tools/package.sh --no-build      package whatever is already in bin/
#   tools/package.sh --no-verify     skip the self-tests (do not, normally)
#   tools/package.sh --name=NAME     override the archive name
#
# The archive is named after the architecture it actually contains, and
# INSTALL.txt states that and the minimum macOS.  Which architecture that is
# comes from configure, not from here: Homebrew has no Intel bottles any more,
# so an x86_64 package needs an SDL2 built from source for x86_64 and
# --with-sdl2 pointed at its sdl2-config.  See "Shipping a build" in CLAUDE.md.
#
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DATA=1
BUILD=1
VERIFY=1
NAME=""

for arg in "$@"; do
	case "$arg" in
		--no-data)   DATA=0 ;;
		--no-build)  BUILD=0 ;;
		--no-verify) VERIFY=0 ;;
		--name=*)    NAME=${arg#*=} ;;
		-h|--help)   sed -n '3,25p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*)           echo "package: unknown argument $arg" >&2; exit 1 ;;
	esac
done

die() { echo "package: $*" >&2; exit 1; }
step() { echo; echo "=== $* ==="; }

[ "$(uname -s)" = "Darwin" ] || die "this script packages the macOS app; on another host use make bundle_gzip"

APP="$ROOT/bundle/OpenDUNE.app"
MACOS="$APP/Contents/MacOS"
RES="$APP/Contents/Resources"
FRAMEWORKS="$APP/Contents/Frameworks"

# ---------------------------------------------------------------- build -----
if [ "$BUILD" = 1 ]; then
	step "Building the game"
	make -C "$ROOT" -j8 >/dev/null || die "make failed"
fi
[ -x "$ROOT/bin/opendune" ] || die "bin/opendune is missing -- run without --no-build"

# The first field is what the game shows in its menu (revision plus branch); the
# last is the revision alone, which is what belongs in a file name.  Taking it as
# $NF rather than by number because one of the middle fields is empty.
REV=$("$ROOT/findversion.sh" | cut -f1)
SHORTREV=$("$ROOT/findversion.sh" | awk -F'\t' '{print $NF}')
[ -n "$REV" ] || REV=norev000
[ -n "$SHORTREV" ] || SHORTREV=norev000
case "$SHORTREV" in
	*M) echo "package: WARNING -- packaging a dirty tree ($REV); the receiving machine cannot reproduce this build" ;;
esac

RELAY_BIN=""
if command -v go >/dev/null 2>&1; then
	step "Building the relay"
	# GOARCH from the game, not from the host: an Intel package with an arm64
	# relay in it hands the other machine one binary it cannot run.
	RELAY_GOARCH=$(lipo -archs "$ROOT/bin/opendune" 2>/dev/null | tr ' ' '\n' | head -1)
	case "$RELAY_GOARCH" in
		x86_64) RELAY_GOARCH=amd64 ;;
		arm64)  RELAY_GOARCH=arm64 ;;
		*)      RELAY_GOARCH="" ;;
	esac
	(cd "$ROOT/tools/relay" && env GOOS=darwin ${RELAY_GOARCH:+GOARCH=$RELAY_GOARCH} go build -o "$ROOT/bin/relay" .) && RELAY_BIN="$ROOT/bin/relay"
	[ -n "$RELAY_BIN" ] || echo "package: relay build failed, packaging without it"
else
	echo "package: go not found, packaging without the relay"
fi

# ------------------------------------------------------------- bundle -------
# This wipes bundle/ wholesale, which is why the data copy comes after it and
# not before.
step "make bundle"
make -C "$ROOT" bundle >/dev/null || die "make bundle failed"
[ -d "$APP" ] || die "make bundle produced no $APP"

# --------------------------------------------------------------- data -------
if [ "$DATA" = 1 ]; then
	step "Game data"
	ls "$ROOT"/bin/data/*.PAK >/dev/null 2>&1 || die "no *.PAK in bin/data -- use --no-data to package without them"
	mkdir -p "$RES/data"
	cp -p "$ROOT"/bin/data/*.PAK "$RES/data/"
	[ -f "$ROOT/bin/data/DUNE.CFG" ] && cp -p "$ROOT/bin/data/DUNE.CFG" "$RES/data/"
	echo "    $(ls "$RES/data" | wc -l | tr -d ' ') files, $(du -sh "$RES/data" | cut -f1)"
fi

# ---------------------------------------------------------------- SDL -------
# The one thing that decides whether the app opens a window on a machine that
# is not this one.  otool tells us which dylib the linker actually chose; we
# copy that file and rewrite the load command to look next to the executable.
# -------------------------------------------------------------- settings ----
# The config digest is folded from the balance tables, and opendune.ini patches
# those tables -- so two players with different ini files ask the relay for
# different rooms and each waits out the timeout for somebody who is in the
# other one.  That is the design working (a disagreement cannot start a match),
# but it is a cruel way to find out that one machine has a settings file and
# the other does not.  Shipping the builder's effective ini inside the package
# makes the digest a property of the package: both sides get the same tables by
# unpacking the same zip.
#
# Beside the app rather than inside it -- that is the fourth place Load_IniFile
# looks, and the only one a person can see.  A copy in
# ~/Library/Application Support/OpenDUNE still wins over it, which is why the
# one copied here is the one this machine actually plays with.
step "Settings"
INI=""
for candidate in "$HOME/Library/Application Support/OpenDUNE/opendune.ini" "$ROOT/bin/opendune.ini"; do
	if [ -f "$candidate" ]; then INI="$candidate"; break; fi
done
if [ -n "$INI" ]; then
	cp -p "$INI" "$ROOT/bundle/opendune.ini"
	chmod u+w "$ROOT/bundle/opendune.ini"
	echo "    $(basename "$(dirname "$INI")")/opendune.ini -> beside the app"
else
	echo "    no opendune.ini on this machine, so the package plays the defaults"
fi

step "Vendoring SDL"
SDL=$(otool -L "$MACOS/opendune" | awk '/libSDL2/ {print $1; exit}')
if [ -n "$SDL" ] && [ "${SDL#@}" = "$SDL" ]; then
	[ -f "$SDL" ] || die "the binary wants $SDL and it is not there"
	mkdir -p "$FRAMEWORKS"
	cp -p "$SDL" "$FRAMEWORKS/"
	SDLNAME=$(basename "$SDL")
	chmod u+w "$FRAMEWORKS/$SDLNAME"
	install_name_tool -id "@executable_path/../Frameworks/$SDLNAME" "$FRAMEWORKS/$SDLNAME"
	install_name_tool -change "$SDL" "@executable_path/../Frameworks/$SDLNAME" "$MACOS/opendune"
	# install_name_tool invalidates the signature the linker left, and arm64
	# refuses an image whose signature does not match -- the Signing step below
	# puts one back, on the dylib and on the bundle together.
	echo "    $SDLNAME -> Contents/Frameworks"
else
	echo "    nothing to vendor (SDL is already relative or statically linked)"
fi

# What the thing actually is, asked of the binary rather than assumed.  The
# script used to say "arm64" in the archive name and in INSTALL.txt no matter
# what it had built, which is a lie in the one place a person will read it.
ARCH=$(lipo -archs "$MACOS/opendune" | tr ' ' '-')
MINOS=$(otool -l "$MACOS/opendune" |
        awk '/LC_BUILD_VERSION/ {b=1} /LC_VERSION_MIN_MACOSX/ {v=1}
             b && /minos/ {print $2; exit} v && /version/ {print $2; exit}')
[ -n "$MINOS" ] || MINOS="unknown"

# Anything else outside /usr/lib and /System is a hole this script has not
# plugged, and the other machine will find it instead of us.
LEFT=$(otool -L "$MACOS/opendune" | tail -n +2 | awk '{print $1}' |
       grep -v '^/usr/lib/' | grep -v '^/System/' | grep -v '^@executable_path/' || true)
[ -z "$LEFT" ] || die "still linked against paths outside the bundle:
$LEFT"

if [ -n "$RELAY_BIN" ]; then
	cp -p "$RELAY_BIN" "$ROOT/bundle/relay"
fi

# ------------------------------------------------------------- readme -------
# ---------------------------------------------------------- Signature -------
# An unsigned app does not open on a machine that did not build it.  This was
# done inside the vendoring branch only, so the statically linked Intel package
# went out unsigned every single time -- and the person on the other end saw a
# refusal rather than a game.  It has to be its own step, after the data is in
# and the load command is rewritten, because both of those invalidate whatever
# signature came before them.
#
# Ad-hoc is not a developer identity and does not make the app trusted: the
# receiving machine still has to be told to allow it (INSTALL.txt says how).
# What it does buy is a code identity and a sealed resource directory, which is
# the difference between "allow this developer?" and "the app is damaged".
step "Signing"
codesign --force --deep --sign - "$APP" || die "could not sign the bundle"
codesign --verify --deep --strict "$APP" || die "the signature does not verify"
echo "    ad-hoc, sealed$( [ -n "$SDL" ] && [ "${SDL#@}" = "$SDL" ] && echo " with the vendored SDL" )"

step "Instructions"
cat > "$ROOT/bundle/INSTALL.txt" <<EOF
OpenDUNE $REV — сборка от $(date '+%Y-%m-%d')
========================================================================

Что это
-------
Форк OpenDUNE с сетевой игрой 1-на-1. Внутри всё, что нужно для запуска:
движок, SDL и $( [ "$DATA" = 1 ] && echo "игровые данные Dune II" || echo "НЕ входят игровые данные Dune II" ).
Архитектура: $ARCH. Минимальная версия macOS: $MINOS.
$( [ "$ARCH" = "arm64" ] && echo "Это сборка для Apple Silicon; на процессоре Intel она не запустится." || echo "Это сборка для Intel; на Apple Silicon она пойдёт через Rosetta." )

Как запустить
-------------
1. Скопируйте OpenDUNE.app куда угодно, например в /Applications.

2. Снимите карантин — иначе macOS откажется запускать приложение,
   пришедшее с другой машины и не подписанное разработчиком:

       xattr -dr com.apple.quarantine /путь/к/OpenDUNE.app

   Без этого шага будет «файл повреждён» или «не удаётся проверить».

3. Запустите обычным двойным щелчком.
EOF

if [ "$DATA" = 0 ]; then
	cat >> "$ROOT/bundle/INSTALL.txt" <<'EOF'

4. Положите файлы оригинальной Dune II (*.PAK и DUNE.CFG) в
   OpenDUNE.app/Contents/Resources/data/ — без них игра не стартует.
   Показать содержимое пакета: правый щелчок по .app → «Показать
   содержимое пакета».
EOF
fi

cat >> "$ROOT/bundle/INSTALL.txt" <<'EOF'

Настройка баланса
-----------------
opendune.ini (лежит рядом с приложением) — настройки, с которыми собран
этот пакет. НЕ УДАЛЯЙТЕ его и не меняйте в одиночку: он задаёт таблицы
баланса и дерево технологий, а оба игрока обязаны играть на одних и тех
же. Расходятся настройки — расходятся комнаты на релее, и вы просто не
найдёте друг друга («второй игрок так и не подключился»).

Проверить, что настройки совпали, можно не начиная матч:

    OpenDUNE.app/Contents/MacOS/opendune --config-hash

Первая строка — config-hash. У обоих игроков она должна быть одинаковой;
строки под ней показывают, чем именно расходитесь, если нет.

Внимание: копия в ~/Library/Application Support/OpenDUNE/ имеет
приоритет над файлом рядом с приложением. Если вы там что-то держите,
либо уберите её, либо положите туда ровно этот же файл.

opendune.ini.sample — аннотированный шаблон со значениями по умолчанию.
Он НЕ равен opendune.ini рядом с ним: переименовав шаблон, вы получите
другой config-hash. Что значат ключи — в README.txt, раздел «Combat
class balance».

Сетевая игра
------------
relay (лежит рядом) — сервер-посредник: клиенты не соединяются напрямую,
оба подключаются к нему и называют одну комнату. Достаточно одного
запущенного релея на двоих, на любой машине, видимой обоим:

    ./relay -listen 0.0.0.0:31337

Что внутри
----------
    OpenDUNE.app        игра
    relay               сервер-посредник для сетевой игры
    opendune.ini        настройки, с которыми собран пакет
    opendune.ini.sample шаблон настроек со значениями по умолчанию
    README.txt          полное описание, включая боевой баланс
    COPYING             лицензия (GPL v2)
    enhancement.txt     отличия от оригинальной Dune II
EOF

# ---------------------------------------------------------------- zip -------
step "Archive"
[ -n "$NAME" ] || NAME="opendune-$SHORTREV-$(date '+%Y%m%d')-macos-$ARCH"
BUNDLES="$ROOT/bundles"
mkdir -p "$BUNDLES"
ZIP="$BUNDLES/$NAME.zip"
rm -f "$ZIP"

# ditto, not zip: it is the only one that carries an .app's symlinks, extended
# attributes and permissions through a round trip unchanged.  It also names the
# archive's top folder after the directory it is given, so the payload is staged
# under the release name rather than under "bundle".
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
ditto "$ROOT/bundle" "$STAGE/$NAME" || die "staging failed"
# Clear the extended attributes cp leaves behind (provenance, quarantine).  They
# carry nothing the receiving machine wants, and with --sequesterRsrc every one
# of them becomes a second entry under __MACOSX/ -- which doubles the file count
# of the archive with 163-byte stubs.  The code signature lives inside the
# Mach-O, not in an xattr, so this does not disturb it.
xattr -cr "$STAGE/$NAME" 2>/dev/null
(cd "$STAGE" && ditto -c -k --sequesterRsrc --keepParent "$NAME" "$ZIP") || die "ditto failed"
shasum -a 256 "$ZIP" | sed "s|$BUNDLES/||" > "$ZIP.sha256"

echo "    $ZIP"
echo "    $(du -h "$ZIP" | cut -f1)"

# -------------------------------------------------------------- verify ------
# Unpack what we are about to hand over and run it, rather than testing the
# tree it came from.  Everything above -- the data copy, the SDL rewrite, the
# zip round trip -- is between the two, and each of them has its own way of
# going wrong.
if [ "$VERIFY" = 1 ]; then
	step "Verifying the archive"
	TMP=$(mktemp -d)
	trap 'rm -rf "$STAGE" "$TMP"' EXIT
	ditto -x -k "$ZIP" "$TMP" || die "the archive does not unpack"

	BIN="$TMP/$NAME/OpenDUNE.app/Contents/MacOS/opendune"
	[ -x "$BIN" ] || die "no executable inside the archive"

	# The configuration the package will play on, against the one this machine
	# plays on.  A difference here is exactly the "the other player never
	# joined" that cannot be diagnosed from inside the lobby: the digest is
	# folded from the balance tables, so an ini that did not travel puts the two
	# players in different rooms.  The package is asked with HOME pointed
	# somewhere empty, which is what a machine that has never run this has.
	mkdir -p "$TMP/nohome"
	printf '    %-32s' "config-hash"
	MINE=$(SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$MACOS/opendune" --config-hash 2>/dev/null |
	       awk '/^config-hash:/ {print $2; exit}')
	THEIRS=$(HOME="$TMP/nohome" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$BIN" --config-hash 2>/dev/null |
	         awk '/^config-hash:/ {print $2; exit}')
	if [ -n "$MINE" ] && [ "$MINE" = "$THEIRS" ]; then
		echo "ok ($MINE)"
	else
		echo "FAILED"
		echo "      this machine plays $MINE, the package plays $THEIRS" >&2
		rm -f "$ZIP" "$ZIP.sha256"
		die "the package would not play this machine's configuration; archive removed"
	fi

	# The signature decides whether the app opens at all, and the zip round
	# trip is the last thing that could have broken it.  Asked of the unpacked
	# copy for that reason.
	printf '    %-32s' "signature"
	if codesign --verify --deep --strict "$TMP/$NAME/OpenDUNE.app" 2>/dev/null; then
		echo "ok"
	else
		echo "FAILED"
		codesign --verify --deep --strict "$TMP/$NAME/OpenDUNE.app" 2>&1 | tail -5 >&2
		rm -f "$ZIP" "$ZIP.sha256"
		die "the package is not signed; archive removed"
	fi

	otool -L "$BIN" | tail -n +2 | awk '{print $1}' |
		grep -v '^/usr/lib/' | grep -v '^/System/' | grep -v '^@executable_path/' |
		grep . && die "the unpacked binary still points outside the bundle"

	if [ "$DATA" = 1 ]; then
		for t in "--combat-balance-self-test" "--build-rules-self-test" "--move-rules-self-test" \
		         "--pathfinder-self-test" "--build-queue-self-test" "--ownership-self-test" \
		         "--lobby-self-test" "--selection-self-test" "--build-list-self-test" \
		         "--deviator-self-test" "--harvester-self-test" "--mp-replay=20000,500" \
		         "--mp-modal=20000,4000"; do
			printf '    %-32s' "$t"
			if SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$BIN" --skirmish=ordos,harkonnen $t >"$TMP/log" 2>&1; then
				echo "ok"
			else
				echo "FAILED"
				tail -20 "$TMP/log" >&2
				rm -f "$ZIP" "$ZIP.sha256"
				die "the package does not pass its own tests; archive removed"
			fi
		done
	else
		echo "    no data in the package, so nothing to run against"
	fi
fi

step "Done"
echo "$ZIP"
