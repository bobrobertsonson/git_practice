#!/bin/sh
# Stands in for bin/sawblade-t3k in the editor tests.
case "$1" in
  --help) echo "usage: sawblade-t3k [-h] {login,whoami,pull,search,resolve} ..." ;;
  whoami) echo '{"username": "gatefan", "display_name": "Gate Fan", "id": "u1", "token_file": "/tmp/none"}' ;;
  login)
    echo '{"event": "device_code", "user_code": "ABCD-1234", "verification_uri": "https://www.tone3000.com/device", "verification_uri_complete": "https://www.tone3000.com/device?code=ABCD-1234", "expires_in": 600}'
    exec sleep 30
    ;;
  *) echo "unknown command: $1"; exit 2 ;;
esac
