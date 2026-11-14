#!/bin/sh
# Fouille, step 0 — runs ON the Move (copied there by scripts/netcheck.sh).
# POSIX sh only (the Move may have busybox, no bash, no jq, no python).
# Writes only under /data/UserData/fouille-check/. Never /tmp.

DIR="${FOUILLE_CHECK_DIR:-/data/UserData/fouille-check}"
PROBE="$DIR/fouille_probe"
LOG="$DIR/netcheck.log"
mkdir -p "$DIR" || exit 1
: > "$LOG"

say()  { printf '%s\n' "$*" | tee -a "$LOG"; }
run()  { "$@" 2>&1 | tee -a "$LOG"; }
head_() { printf '\n== %s ==\n' "$*" | tee -a "$LOG"; }

head_ "1. system"
run uname -a
[ -f /etc/os-release ] && run cat /etc/os-release
run grep -E 'MemTotal|MemAvailable|SwapTotal' /proc/meminfo
say "cores: $(grep -c ^processor /proc/cpuinfo)"
run df -h /data
if command -v ldd >/dev/null 2>&1; then ldd --version 2>&1 | head -n 1 | tee -a "$LOG"
else for l in /lib/libc.so.6 /lib/aarch64-linux-gnu/libc.so.6; do [ -x "$l" ] && "$l" 2>&1 | head -n 1 | tee -a "$LOG"; done; fi
[ -d /data/UserData/schwung ] && say "schwung dir: present" || say "schwung dir: MISSING"
ls /data/UserData/schwung/modules 2>/dev/null | tee -a "$LOG"

head_ "2. tools"
for t in curl wget openssl busybox nslookup getent python3 ffmpeg; do
  p=$(command -v "$t" 2>/dev/null); say "$t: ${p:-none}"
done
if [ -x "$PROBE" ]; then run "$PROBE" curl; CURL_OK=$?
else say "fouille_probe missing at $PROBE"; CURL_OK=1; fi
CURL=""
for c in "$FOUILLE_CURL" /data/UserData/schwung/bin/curl /usr/bin/curl /bin/curl /usr/local/bin/curl; do
  if [ -n "$c" ] && [ -x "$c" ]; then CURL="$c"; break; fi
done
say "curl used below: ${CURL:-none}"

head_ "3. certificates"
for f in /etc/ssl/certs/ca-certificates.crt /etc/ssl/cert.pem /etc/pki/tls/certs/ca-bundle.crt; do
  [ -f "$f" ] && say "CA bundle: $f ($(grep -c 'BEGIN CERT' "$f") certs)"
done
[ -d /etc/ssl/certs ] && say "/etc/ssl/certs: $(ls /etc/ssl/certs | wc -l) entries"

head_ "4. dns"
for h in archive.org freesound.org de1.api.radio-browser.info; do
  if command -v getent >/dev/null 2>&1; then r=$(getent hosts "$h" | head -n 1)
  elif command -v nslookup >/dev/null 2>&1; then r=$(nslookup "$h" 2>&1 | grep -A1 -i name | tail -1)
  else r="(no resolver tool)"; fi
  say "$h -> ${r:-FAILED}"
done

if [ -z "$CURL" ]; then
  head_ "STOP: no curl on this Move. Steps 5-9 need it."
  say "log: $LOG"; exit 1
fi

get() { "$CURL" -sS -L --connect-timeout 8 --max-time 25 "$@"; }

head_ "5. https reachability (code, seconds)"
for u in https://archive.org/ https://freesound.org/apiv2/ https://de1.api.radio-browser.info/json/stats; do
  say "$u -> $(get -o /dev/null -w '%{http_code} %{time_total}' "$u" 2>&1)"
done

head_ "6. Radio Aporee on Internet Archive: recordings per place"
AQ='https://archive.org/advancedsearch.php?q=collection%3Aradio-aporee-maps+AND+'
for q in montreal plateau mile+end hochelaga verdun quebec saguenay chicoutimi jonquiere kamouraska; do
  n=$(get "${AQ}${q}&fl%5B%5D=identifier&rows=0&output=json" | grep -o '"numFound":[0-9]*' | cut -d: -f2)
  say "$q: ${n:-error}"
done

head_ "7. one Aporee MP3: download 256 KB + decode"
ID=$(get "${AQ}montreal&fl%5B%5D=identifier&rows=1&output=json" | grep -o '"identifier":"[^"]*"' | head -n 1 | cut -d'"' -f4)
say "item: ${ID:-none}"
if [ -n "$ID" ]; then
  F=$(get "https://archive.org/metadata/$ID/files" | grep -o '"name":"[^"]*\.mp3"' | head -n 1 | cut -d'"' -f4)
  say "file: ${F:-none}"
  if [ -n "$F" ]; then
    FE=$(printf '%s' "$F" | sed 's/ /%20/g')
    run "$PROBE" fetch "https://archive.org/download/$ID/$FE" 256
  fi
fi

head_ "8. Freesound"
say "no key: $(get -o /dev/null -w '%{http_code}' 'https://freesound.org/apiv2/search/text/?query=rain' 2>&1) (401 = reachable, key needed)"
KEY=""
for k in /data/UserData/schwung/modules/overtake/fouille/freesound.key \
         /data/UserData/schwung/modules/sound_generators/fouille/freesound.key \
         "$DIR/freesound.key"; do
  [ -f "$k" ] && { KEY=$(tr -d ' \r\n' < "$k"); say "key file: $k"; break; }
done
if [ -n "$KEY" ]; then
  # 3 km around the Plateau Mont-Royal, by GPS position, not by keyword.
  GEO='%7B%21geofilt%20sfield%3Dgeotag%20pt%3D45.5225%2C-73.5800%20d%3D3%7D'
  R=$(get "https://freesound.org/apiv2/search/text/?query=&filter=$GEO&fields=id,name,previews&page_size=5&token=$KEY")
  say "geotagged within 3 km of the Plateau: $(printf '%s' "$R" | grep -o '"count":[0-9]*' | head -n 1 | cut -d: -f2)"
  P=$(printf '%s' "$R" | grep -o '"preview-hq-mp3":"[^"]*"' | head -n 1 | cut -d'"' -f4)
  [ -n "$P" ] && run "$PROBE" fetch "$P" 512
else
  say "no freesound.key found: skipped the geotag search"
fi

head_ "9. web radios in Quebec (radio-browser.info, MP3 only)"
RB='https://de1.api.radio-browser.info/json/stations/search?countrycode=CA&codec=MP3&hidebroken=true&order=clickcount&reverse=true&limit=15&state='
J=$(get -A 'Fouille/0.3' "${RB}Quebec")
[ -z "$(printf '%s' "$J" | grep -o '"url_resolved"' | head -n 1)" ] && J=$(get -A 'Fouille/0.3' "${RB}Qu%C3%A9bec")
printf '%s' "$J" | grep -o '"name":"[^"]*"\|"url_resolved":"[^"]*"\|"bitrate":[0-9]*' \
  | cut -d: -f2- | awk '{printf "%s%s", $0, (NR%3 ? "  " : "\n")}' | tee -a "$LOG"
i=0
for u in $(printf '%s' "$J" | grep -o '"url_resolved":"[^"]*"' | cut -d'"' -f4 | head -n 3); do
  i=$((i+1)); say "-- stream $i: $u"
  run "$PROBE" stream "$u" 8
done

head_ "10. write test"
echo ok > "$DIR/w.test" && say "write to $DIR: ok" && rm -f "$DIR/w.test"

head_ "done"
say "log: $LOG"
