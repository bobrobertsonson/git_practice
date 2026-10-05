#!/bin/sh
# Stands in for bin/sawblade-t3k in the editor tests. FAKE_T3K_MODE (inherited from the test process) picks a behaviour:
#   (unset)   whoami ok; login prints device_code and waits (the test cancels it)
#   approve   login prints device_code then logged_in (with the user fields) and exits 0
#   error     login prints device_code then {"error","code"} and exits 1; whoami prints an error line and exits 1
#   loggedout whoami prints {"error","code":"auth"} and exits 4 (not logged in)
DC='{"event": "device_code", "user_code": "ABCD-1234", "verification_uri": "https://www.tone3000.com/device", "verification_uri_complete": "https://www.tone3000.com/device?code=ABCD-1234", "expires_in": 600}'
case "$1" in
  --help) echo "usage: sawblade-t3k [-h] {login,whoami,pull,search,resolve} ..." ;;
  whoami)
    case "$FAKE_T3K_MODE" in
      loggedout) echo '{"error": "not logged in: run `sawblade-t3k login`", "code": "auth"}'; exit 4 ;;
      error) echo '{"error": "could not reach TONE3000", "code": "network"}'; exit 1 ;;
    esac
    echo '{"id": "u1", "username": "gatefan", "display_name": "Gate Fan", "token_file": "/tmp/none"}' ;;
  login)
    echo "$DC"
    case "$FAKE_T3K_MODE" in
      approve) echo '{"event": "logged_in", "username": "gatefan", "display_name": "Gate Fan", "id": "u1", "token_file": "/tmp/none"}'; exit 0 ;;
      error) echo '{"error": "the login code expired", "code": "auth"}'; exit 1 ;;
      loggedout) exit 4 ;;
    esac
    exec sleep 30
    ;;
  *) echo "unknown command: $1"; exit 2 ;;
esac
