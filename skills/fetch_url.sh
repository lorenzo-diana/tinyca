#!/bin/bash
# @name: fetch_url
# @description: Download a file from a URL into a destination folder using wget.
# @usage: skills/fetch_url.sh <url> <destination_folder>
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <url> <destination_folder>" >&2
    exit 2
fi

url="$1"
dest="$2"

mkdir -p "$dest"
wget --no-verbose --timeout=30 --tries=2 -P "$dest" "$url"
echo "saved to: $dest"
