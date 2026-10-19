#!/usr/bin/env bash
# Writes the Grafana Cloud credentials Alloy relays with, outside the repository:
#   ~/.config/opensupcom/pyroscope-cloud.env   (mode 600)
# Run it yourself in a terminal: the key is read without echo and never passes
# through a shell history, a file in the tree, or an agent's transcript.
#
#   tools/pyroscope/cloud-login.sh [--url https://profiles-prod-NNN.grafana.net] [--user 123456]
#
# Then: docker compose --profile cloud up -d   and   opensupcom --pyroscope http://localhost:4041
set -euo pipefail
URL="" USER_ID=""
while [ $# -gt 0 ]; do
  case "$1" in
    --url) URL=$2; shift 2 ;;
    --user) USER_ID=$2; shift 2 ;;
    -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
    *) echo "cloud-login.sh: unknown option $1" >&2; exit 1 ;;
  esac
done
[ -t 0 ] || { echo "cloud-login.sh: run it in a terminal (the key is typed, not piped)" >&2; exit 1; }
[ -n "$URL" ] || read -r -p "Pyroscope URL (Grafana Cloud -> stack -> Pyroscope -> Send profiles): " URL
[ -n "$USER_ID" ] || read -r -p "Pyroscope user (the number shown there): " USER_ID
case "$URL" in https://*) ;; *) echo "cloud-login.sh: the URL must start with https://" >&2; exit 1 ;; esac
case "$USER_ID" in ''|*[!0-9]*) echo "cloud-login.sh: the user is a number" >&2; exit 1 ;; esac
read -r -s -p "Access-policy token (profiles:write scope; not echoed): " KEY; echo
case "$KEY" in glc_*) ;; *) echo "cloud-login.sh: a Grafana Cloud token starts with glc_" >&2; exit 1 ;; esac
DIR="$HOME/.config/opensupcom"; FILE="$DIR/pyroscope-cloud.env"
mkdir -p "$DIR"; chmod 700 "$DIR"
umask 077
printf 'PYROSCOPE_URL=%s\nPYROSCOPE_USER=%s\nPYROSCOPE_API_KEY=%s\n' "$URL" "$USER_ID" "$KEY" > "$FILE"
chmod 600 "$FILE"
echo "wrote $FILE (mode 600). Next: docker compose --profile cloud up -d"
