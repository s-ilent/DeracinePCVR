#!/usr/bin/env bash
# gpu-lock.sh - machine-wide GPU benchmark lock for headless VR sessions.
#
# Same slot, same protocol as testslot.sh: the lock directory is
# _build/team/hmd-slot/lock, created with mkdir (atomic on the drvfs/NTFS backing store), so an
# old testslot.sh user and a headless harness user exclude each other without either script
# knowing about the other. testslot.sh is not modified; this script only adds what a headless
# benchmark needs: a quiet-machine audit (no other shadps4.exe, compiler or linker), a "kind"
# field so the log says whether the hold is for a headset or a headless run, and the same
# release-on-failure wrapper.
#
# State lives outside the worktrees, in _build/team/hmd-slot/:
#   lock/            present == slot HELD; contains meta + hb
#   queue/           FIFO waiter entries (name = <epoch_ns>-<owner>-<pid>)
#   archive/         lock dirs broken as stale, kept for audit
#   log              append-only audit log
#   USER-HOLD        present == the human reserved the machine; blocks everyone
#   STALE-POLICY     "any" (default) | "lead-only"
#
# Commands:
#   status | acquire | wait | run | heartbeat | release | reclaim | log
#
# Exit codes: 0 ok | 3 busy | 4 stale but not breakable | 5 user hold | 6 wait timeout
#             7 quiet check failed (another GPU/CPU benchmark process is running)
#             8 headless-only: --kind hmd refused while _build/team/hmd-slot/HEADLESS-ONLY exists
set -u

ROOT="${GPU_LOCK_ROOT:-${HMD_SLOT_ROOT:-/mnt/m/Development/Deracine-work/_build/team/hmd-slot}}"
LOCK="$ROOT/lock"
QUEUE="$ROOT/queue"
ARCH="$ROOT/archive"
LOGF="$ROOT/log"
USERHOLD="$ROOT/USER-HOLD"
POLICY="$ROOT/STALE-POLICY"
# User directive 2026-11-18 01:07: headset use is over. When this marker exists, a hold may only
# be taken for a headless run (--kind headless); an HMD/SteamVR/OpenXR hold is refused (exit 8),
# the same rule testslot.sh enforces on the reason text.
HEADLESSONLY="$ROOT/HEADLESS-ONLY"
TTL_DEFAULT="${GPU_LOCK_TTL:-${HMD_SLOT_TTL:-600}}"
POLL="${GPU_LOCK_POLL:-${HMD_SLOT_POLL:-20}}"
HB_EVERY="${GPU_LOCK_HEARTBEAT:-${HMD_SLOT_HEARTBEAT:-15}}"
QUIET_WAIT_DEFAULT="${GPU_LOCK_QUIET_WAIT:-0}"
TASKLIST="/mnt/c/Windows/System32/tasklist.exe"
# Processes that make a frame-rate measurement meaningless. Matched on the image name, lowercased.
QUIET_RE='^(shadps4|shadps4\.exe|ninja|ninja\.exe|clang-cl|clang-cl\.exe|link|link\.exe|msbuild|msbuild\.exe|renderdoccmd|renderdoccmd\.exe|qrenderdoc|qrenderdoc\.exe)$'

err()  { printf '%s\n' "$*" >&2; }
die()  { err "gpu-lock: $*"; exit 1; }
now()  { date +%s; }
iso()  { date -u +%Y-%m-%dT%H:%M:%SZ; }
ensure_root() { mkdir -p "$ROOT" "$QUEUE" "$ARCH" 2>/dev/null; }

logline() {
  ensure_root
  printf '%s %s\n' "$(iso)" "$*" >> "$LOGF" 2>/dev/null
  local sz
  sz=$(stat -c %s "$LOGF" 2>/dev/null || echo 0)
  if [ "$sz" -gt 2097152 ]; then
    tail -n 4000 "$LOGF" > "$LOGF.tmp" && mv "$LOGF.tmp" "$LOGF"
  fi
}

meta_get() {
  [ -f "$LOCK/meta" ] || return 0
  sed -n "s/^$1=//p" "$LOCK/meta" 2>/dev/null | head -1
}

meta_write() { # token owner reason ttl kind
  local tmp="$ROOT/.meta.$$.$RANDOM"
  {
    printf 'token=%s\n' "$1"
    printf 'owner=%s\n' "$2"
    printf 'reason=%s\n' "$3"
    printf 'ttl=%s\n' "$4"
    printf 'kind=%s\n' "${5:-hmd}"
    printf 'pid=%s\n' "$$"
    printf 'host=%s\n' "$(hostname 2>/dev/null || echo unknown)"
    printf 'acquired=%s\n' "$(iso)"
  } > "$tmp" || return 1
  mv "$tmp" "$LOCK/meta" && touch "$LOCK/hb"
}

lock_age() {
  if [ -f "$LOCK/meta" ]; then
    local ref="$LOCK/hb"
    [ -e "$ref" ] || ref="$LOCK/meta"
    echo $(( $(now) - $(stat -c %Y "$ref" 2>/dev/null || now) ))
  else
    echo $(( $(now) - $(stat -c %Y "$LOCK" 2>/dev/null || now) ))
  fi
}

is_stale() {
  [ -d "$LOCK" ] || return 1
  local age ttl
  age=$(lock_age)
  ttl=$(meta_get ttl)
  [ -n "$ttl" ] || ttl=30            # half-created lock: short fuse
  [ "$age" -gt "$ttl" ]
}

stale_break_allowed() {
  local p="any"
  [ -f "$POLICY" ] && p=$(head -1 "$POLICY" 2>/dev/null | tr -d '[:space:]')
  case "$p" in
    lead-only) [ "${HMD_SLOT_LEAD:-0}" = "1" ] ;;
    *) return 0 ;;
  esac
}

break_stale() { # who -> 0 broken | 1 lost race | 2 actually live (restored)
  local who="$1" dest="$ARCH/$(date +%s%N)-$RANDOM"
  ensure_root
  [ -d "$LOCK" ] || return 1
  mv "$LOCK" "$dest" 2>/dev/null || return 1
  local age ttl
  age=$(( $(now) - $(stat -c %Y "$dest/hb" 2>/dev/null || stat -c %Y "$dest/meta" 2>/dev/null || stat -c %Y "$dest" 2>/dev/null || now) ))
  ttl=$(sed -n 's/^ttl=//p' "$dest/meta" 2>/dev/null | head -1)
  [ -n "$ttl" ] || ttl="$TTL_DEFAULT"
  if [ "$age" -le "$ttl" ]; then
    [ -d "$LOCK" ] || mv "$dest" "$LOCK" 2>/dev/null
    logline "BREAK_RACE who=$who age=${age}s ttl=${ttl}s"
    return 2
  fi
  logline "BREAK_STALE who=$who owner=$(sed -n 's/^owner=//p' "$dest/meta" 2>/dev/null) age=${age}s ttl=${ttl}s dir=$(basename "$dest")"
  return 0
}

break_force() { # who reason -> unconditional break (human-approved force)
  local who="$1" reason="$2" dest="$ARCH/force-$(date +%s%N)-$RANDOM"
  ensure_root
  [ -d "$LOCK" ] || return 1
  mv "$LOCK" "$dest" 2>/dev/null || return 1
  logline "BREAK_FORCE who=$who owner=$(sed -n 's/^owner=//p' "$dest/meta" 2>/dev/null) reason=$reason"
  return 0
}

# --- quiet-machine audit ----------------------------------------------------------------------
# A frame-rate number is only comparable to another if the machine was doing the same kind of
# work: another emulator, a parallel build (ninja/clang-cl/link/msbuild) or a RenderDoc capture
# steals GPU/CPU and makes the cell meaningless. The audit only *records and refuses*; it never
# stops a process it did not start.
quiet_busy() {
  timeout 10 "$TASKLIST" 2>/dev/null |
    awk 'NR>3 && NF>0 {print tolower($1)}' |
    grep -E "$QUIET_RE" | sort -u
}

quiet_list() {
  quiet_busy | tr '\n' ',' | sed 's/,$//'
}

quiet_audit() { # who phase -> 0 quiet | 1 busy (also writes a log line)
  local who="$1" phase="$2" list
  list="$(quiet_list)"
  if [ -n "$list" ]; then
    logline "QUIET who=$who phase=$phase busy=$list"
    printf '%s\n' "$list" >&2
    return 1
  fi
  logline "QUIET who=$who phase=$phase busy=none"
  return 0
}

wait_for_quiet() { # who timeout -> 0 quiet | 1 still busy
  local who="$1" timeout="${2:-0}" start list
  start=$(now)
  while :; do
    if quiet_audit "$who" "acquire"; then return 0; fi
    if [ "$timeout" -le 0 ]; then
      err "gpu-lock: machine is busy: $(quiet_list) (retry with --quiet-wait SECONDS to wait)"
      return 1
    fi
    if [ $(( $(now) - start )) -ge "$timeout" ]; then
      err "gpu-lock: still busy after ${timeout}s: $(quiet_list)"
      return 1
    fi
    err "gpu-lock: waiting for $(quiet_list) to finish ..."
    sleep 10
  done
}

# --- the slot ---------------------------------------------------------------------------------
acquire_once() { # owner reason ttl kind -> prints token on stdout
  local owner="$1" reason="$2" ttl="$3" kind="${4:-hmd}"
  ensure_root
  if [ -e "$HEADLESSONLY" ] && [ "$kind" != "headless" ]; then
    err "gpu-lock: the user ended headset use (2026-11-18 01:07)."
    err "gpu-lock: only headless runs may take the GPU now: SHADPS4_VR=1 SHADPS4_OPENXR=0 SHADPS4_VR_DEMO=0."
    err "gpu-lock: use --kind headless (and put 'headless' in --reason); see $HEADLESSONLY.md (exit 8)."
    logline "NOTE kind=$kind owner=$owner refused: HEADLESS-ONLY marker is present"
    return 8
  fi
  if [ -e "$USERHOLD" ]; then
    err "gpu-lock: user hold active: $(head -1 "$USERHOLD" 2>/dev/null)"
    return 5
  fi
  if [ -d "$LOCK" ]; then
    if ! is_stale; then
      err "gpu-lock: slot busy - $(meta_get owner) since $(sed -n 's/^acquired=//p' "$LOCK/meta" 2>/dev/null) (heartbeat $(lock_age)s ago)"
      return 3
    fi
    if ! stale_break_allowed; then
      err "gpu-lock: slot is stale but STALE-POLICY=lead-only"
      return 4
    fi
    break_stale "acquire:$owner" || true
    if [ -d "$LOCK" ] && ! is_stale; then
      err "gpu-lock: lost stale-break race to $(meta_get owner)"
      return 3
    fi
  fi
  mkdir "$LOCK" 2>/dev/null || return 3
  local token
  token="$(date +%s%N)-$RANDOM-$RANDOM"
  meta_write "$token" "$owner" "$reason" "$ttl" "$kind" || { rm -rf "$LOCK"; return 1; }
  logline "ACQUIRE owner=$owner token=$token ttl=${ttl}s kind=$kind reason=$reason"
  printf '%s\n' "$token"
  return 0
}

queue_add() { # owner reason -> path
  ensure_root
  local name
  name="$(date +%s%N)-$(printf '%s' "$1" | tr -c 'A-Za-z0-9_.-' '_')-$$"
  printf 'owner=%s\nreason=%s\nqueued=%s\n' "$1" "$2" "$(iso)" > "$QUEUE/$name"
  printf '%s\n' "$QUEUE/$name"
}

queue_head() { ls -1 "$QUEUE" 2>/dev/null | sort | head -1; }

queue_expire() {
  local f age
  [ -d "$QUEUE" ] || return 0
  for f in "$QUEUE"/*; do
    [ -e "$f" ] || continue
    age=$(( $(now) - $(stat -c %Y "$f") ))
    if [ "$age" -gt $((POLL * 3)) ]; then
      logline "WAIT_EXPIRE $(basename "$f") age=${age}s"
      rm -f "$f"
    fi
  done
}

wait_for_slot() { # owner reason timeout ttl kind -> prints token
  local owner="$1" reason="$2" timeout="$3" ttl="$4" kind="${5:-hmd}"
  local entry head start out rc
  entry="$(queue_add "$owner" "$reason")"
  start=$(now)
  while :; do
    queue_expire
    head=$(queue_head)
    if [ -z "$head" ] || [ "$head" = "$(basename "$entry")" ]; then
      out="$(acquire_once "$owner" "$reason" "$ttl" "$kind")"; rc=$?
      if [ "$rc" -eq 0 ]; then
        rm -f "$entry"
        logline "WAIT_ACQUIRED owner=$owner waited=$(( $(now) - start ))s"
        printf '%s\n' "$out"
        return 0
      fi
      if [ "$rc" -eq 5 ] || [ "$rc" -eq 4 ] || [ "$rc" -eq 8 ]; then
        rm -f "$entry"
        return "$rc"
      fi
    fi
    if [ $(( $(now) - start )) -ge "$timeout" ]; then
      rm -f "$entry"
      logline "WAIT_TIMEOUT owner=$owner waited=$(( $(now) - start ))s"
      err "gpu-lock: wait timed out after ${timeout}s"
      return 6
    fi
    sleep "$POLL"
    touch "$entry" 2>/dev/null
  done
}

release_token() { # token (empty only for a user-owned lock or forced cleanup)
  local token="$1" cur owner kind
  [ -d "$LOCK" ] || return 0
  cur="$(meta_get token)"
  owner="$(meta_get owner)"
  kind="$(meta_get kind)"
  if [ -z "$token" ] && [ "$owner" != "user" ] && [ "${HMD_SLOT_FORCE_OK:-0}" != "1" ]; then
    err "gpu-lock: release needs --token (or HMD_SLOT_FORCE_OK=1 for a forced cleanup)"
    return 1
  fi
  if [ -n "$token" ] && [ "$cur" != "$token" ]; then
    err "gpu-lock: token mismatch - holder token ${cur:-none}; refusing to release"
    return 1
  fi
  rm -rf "$LOCK"
  logline "RELEASE owner=$owner token=$cur kind=$kind"
}

status_cmd() {
  ensure_root
  local owner reason token ttl age state orph f n=0
  if [ -d "$LOCK" ]; then
    owner=$(meta_get owner); reason=$(meta_get reason); token=$(meta_get token); ttl=$(meta_get ttl)
    age=$(lock_age); [ -n "$ttl" ] || ttl="$TTL_DEFAULT"
    if [ "$age" -gt "$ttl" ]; then state="STALE (ttl exceeded)"; else state="fresh"; fi
    printf 'SLOT: HELD by %s (%s)\n' "${owner:-unknown}" "$(meta_get kind)"
    printf '  reason:    %s\n' "${reason:-}"
    printf '  token:     %s\n' "${token:-unknown}"
    printf '  acquired:  %s\n' "$(sed -n 's/^acquired=//p' "$LOCK/meta" 2>/dev/null)"
    printf '  heartbeat: %ss ago, ttl %ss -> %s\n' "$age" "$ttl" "$state"
  else
    printf 'SLOT: free\n'
  fi

  for f in "$QUEUE"/*; do
    [ -e "$f" ] || continue
    n=$((n + 1))
    local qr
    qr="$(sed -n 's/^reason=//p' "$f" 2>/dev/null | head -1)"
    [ -n "$qr" ] || qr='(no reason given)'
    printf '  %d. %s (%ss) %s\n' "$n" "$(sed -n 's/^owner=//p' "$f" | head -1)" \
      "$(( $(now) - $(stat -c %Y "$f") ))" "$qr"
  done
  if [ "$n" -eq 0 ]; then printf 'QUEUE: empty\n'; else printf 'QUEUE: %d waiting\n' "$n"; fi

  printf 'POLICY: stale locks breakable by %s\n' "$( [ -f "$POLICY" ] && head -1 "$POLICY" || echo any )"
  if [ -e "$USERHOLD" ]; then
    printf 'USER HOLD: yes (%s)\n' "$(sed -n '2p' "$USERHOLD" 2>/dev/null)"
  else
    printf 'USER HOLD: no\n'
  fi

  local quiet
  quiet="$(quiet_list)"
  if [ -n "$quiet" ]; then printf 'QUIET: no - %s\n' "$quiet"; else printf 'QUIET: yes\n'; fi

  orph=$(timeout 5 "$TASKLIST" 2>/dev/null | grep -i 'shadps4' | head -3)
  if [ -n "$orph" ] && [ ! -d "$LOCK" ]; then
    printf 'WARNING: shadps4.exe is running but the slot is FREE (orphan from an earlier run):\n%s\n' "$orph"
  fi

  printf 'LOG (last 8):\n'
  tail -n 8 "$LOGF" 2>/dev/null | sed 's/^/  /' || true
}

cmd_acquire() {
  local owner="" reason="" ttl="$TTL_DEFAULT" kind="hmd" require_quiet=0 quiet_wait="$QUIET_WAIT_DEFAULT"
  while [ $# -gt 0 ]; do
    case "$1" in
      --owner)       owner="${2:-}"; shift ;;
      --reason)      reason="${2:-}"; shift ;;
      --ttl)         ttl="${2:-}"; shift ;;
      --kind)        kind="${2:-}"; shift ;;
      --require-quiet) require_quiet=1 ;;
      --quiet-wait)  require_quiet=1; quiet_wait="${2:-0}"; shift ;;
      *) die "acquire: unknown argument '$1'" ;;
    esac
    shift
  done
  [ -n "$owner" ] || die "acquire: --owner is required"
  if [ "$require_quiet" = "1" ]; then
    wait_for_quiet "$owner" "$quiet_wait" || exit 7
  fi
  local token rc
  token="$(acquire_once "$owner" "$reason" "$ttl" "$kind")"; rc=$?
  if [ "$rc" -ne 0 ]; then exit "$rc"; fi
  if [ "$require_quiet" = "1" ] && ! quiet_audit "$owner" "after-acquire"; then
    release_token "$token" >/dev/null 2>&1
    exit 7
  fi
  printf '%s\n' "$token"
}

cmd_wait() {
  local owner="" reason="" timeout=900 ttl="$TTL_DEFAULT" kind="hmd" require_quiet=0 quiet_wait="$QUIET_WAIT_DEFAULT"
  while [ $# -gt 0 ]; do
    case "$1" in
      --owner)       owner="${2:-}"; shift ;;
      --reason)      reason="${2:-}"; shift ;;
      --timeout)     timeout="${2:-}"; shift ;;
      --ttl)         ttl="${2:-}"; shift ;;
      --kind)        kind="${2:-}"; shift ;;
      --require-quiet) require_quiet=1 ;;
      --quiet-wait)  require_quiet=1; quiet_wait="${2:-0}"; shift ;;
      *) die "wait: unknown argument '$1'" ;;
    esac
    shift
  done
  [ -n "$owner" ] || die "wait: --owner is required"
  if [ "$require_quiet" = "1" ]; then
    wait_for_quiet "$owner" "$quiet_wait" || exit 7
  fi
  local token rc
  token="$(wait_for_slot "$owner" "$reason" "$timeout" "$ttl" "$kind")"; rc=$?
  if [ "$rc" -ne 0 ]; then exit "$rc"; fi
  if [ "$require_quiet" = "1" ] && ! quiet_audit "$owner" "after-acquire"; then
    release_token "$token" >/dev/null 2>&1
    exit 7
  fi
  printf '%s\n' "$token"
}

cmd_heartbeat() {
  local token=""
  while [ $# -gt 0 ]; do
    case "$1" in --token) token="${2:-}"; shift ;; *) die "heartbeat: unknown argument '$1'" ;; esac
    shift
  done
  [ -d "$LOCK" ] || die "heartbeat: slot is not held"
  if [ -n "$token" ] && [ "$(meta_get token)" != "$token" ]; then
    die "heartbeat: token mismatch"
  fi
  touch "$LOCK/hb"
  printf 'heartbeat ok (%s)\n' "$(iso)"
}

cmd_release() {
  local token=""
  while [ $# -gt 0 ]; do
    case "$1" in --token) token="${2:-}"; shift ;; *) die "release: unknown argument '$1'" ;; esac
    shift
  done
  release_token "$token"
}

cmd_reclaim() {
  local force=0 reason="" as="${HMD_SLOT_AGENT:-unknown}"
  while [ $# -gt 0 ]; do
    case "$1" in
      --force)  force=1 ;;
      --reason) reason="${2:-}"; shift ;;
      --as)     as="${2:-}"; shift ;;
      *) die "reclaim: unknown argument '$1'" ;;
    esac
    shift
  done
  [ -d "$LOCK" ] || { printf 'slot already free\n'; return 0; }
  local owner
  owner=$(meta_get owner)
  if [ "$owner" = "user" ]; then
    die "slot is reserved by the user; use 'user-clear' if that is intended"
  fi
  if [ "$force" = "0" ]; then
    if ! is_stale; then
      err "gpu-lock: slot is live (heartbeat $(lock_age)s ago); not reclaiming without --force"
      return 3
    fi
    if ! stale_break_allowed; then
      err "gpu-lock: STALE-POLICY=lead-only - a Lead must run this with HMD_SLOT_LEAD=1"
      return 4
    fi
    break_stale "reclaim:$as"
  else
    [ -n "$reason" ] || die "reclaim --force needs --reason"
    [ "${HMD_SLOT_FORCE_OK:-0}" = "1" ] || die "reclaim --force needs HMD_SLOT_FORCE_OK=1 (human approved)"
    break_force "force:$as" "$reason"
  fi
}

cmd_run() {
  local owner="" reason="" aq=900 ttl="$TTL_DEFAULT" kind="hmd" require_quiet=0 quiet_wait="$QUIET_WAIT_DEFAULT"
  while [ $# -gt 0 ]; do
    case "$1" in
      --owner)       owner="${2:-}"; shift ;;
      --reason)      reason="${2:-}"; shift ;;
      --timeout)     aq="${2:-}"; shift ;;
      --ttl)         ttl="${2:-}"; shift ;;
      --kind)        kind="${2:-}"; shift ;;
      --require-quiet) require_quiet=1 ;;
      --quiet-wait)  require_quiet=1; quiet_wait="${2:-0}"; shift ;;
      --) shift; break ;;
      *) die "run: unknown argument '$1' (put the command after --)" ;;
    esac
    shift
  done
  [ -n "$owner" ] || die "run: --owner is required"
  [ $# -gt 0 ] || die "run: no command given (put the command after --)"

  if [ "$require_quiet" = "1" ]; then
    wait_for_quiet "$owner" "$quiet_wait" || exit 7
  fi

  local token rc every
  token="$(wait_for_slot "$owner" "$reason" "$aq" "$ttl" "$kind")"; rc=$?
  if [ "$rc" -ne 0 ]; then exit "$rc"; fi
  if [ "$require_quiet" = "1" ] && ! quiet_audit "$owner" "after-acquire"; then
    release_token "$token" >/dev/null 2>&1
    exit 7
  fi
  every="$HB_EVERY"
  [ "$every" -ge 2 ] || every=2
  if [ "$ttl" -ge 4 ] && [ "$every" -gt $((ttl / 2)) ]; then every=$((ttl / 2)); fi
  TOKEN="$token"
  HB_PID=""
  hb_stop() { [ -n "$HB_PID" ] && kill "$HB_PID" 2>/dev/null; HB_PID=""; }
  on_exit() {
    local code=$?
    trap - EXIT INT TERM
    hb_stop
    release_token "$TOKEN" >/dev/null 2>&1
    exit "$code"
  }
  on_int() { trap - EXIT INT TERM; hb_stop; release_token "$TOKEN" >/dev/null 2>&1; exit 130; }
  trap on_exit EXIT
  trap on_int INT TERM
  (
    while :; do
      sleep "$every"
      [ -d "$LOCK" ] || exit 0
      [ "$(meta_get token)" = "$TOKEN" ] || exit 0
      touch "$LOCK/hb"
    done
  ) &
  HB_PID=$!
  # The wrapper tells the child which hold it is inside, so a harness never acquires twice.
  # WSL does not share environment variables with Windows children (pwsh.exe) unless they are
  # named in WSLENV, so a headless harness started through this wrapper would otherwise see no
  # token and try to acquire a slot it already owns.
  export GPU_LOCK_TOKEN="$TOKEN"
  export GPU_LOCK_OWNER="$owner"
  case ":${WSLENV:-}:" in
    *:GPU_LOCK_TOKEN:*) ;;
    *) export WSLENV="${WSLENV:+$WSLENV:}GPU_LOCK_TOKEN:GPU_LOCK_OWNER" ;;
  esac
  err "gpu-lock: holding slot as $owner (kind $kind, token $TOKEN); running: $*"
  "$@"
  rc=$?
  hb_stop
  release_token "$TOKEN" >/dev/null 2>&1
  trap - EXIT INT TERM
  exit "$rc"
}

cmd_user_hold() {
  local reason="${1:-human reserved the machine}"
  shift || true
  ensure_root
  printf '%s\n%s\n' "$(iso)" "$reason" > "$USERHOLD"
  logline "USER_HOLD reason=$reason"
  printf 'user hold active: %s\n' "$reason"
}

cmd_user_clear() {
  if [ -e "$USERHOLD" ]; then rm -f "$USERHOLD"; logline "USER_CLEAR"; printf 'user hold cleared\n'; else printf 'no user hold\n'; fi
}

cmd_log() {
  local n="${1:-20}"
  tail -n "$n" "$LOGF" 2>/dev/null || true
}

usage() {
  cat <<'EOF'
gpu-lock.sh - machine-wide GPU benchmark lock (same slot as testslot.sh)

Usage:
  gpu-lock.sh status
  gpu-lock.sh acquire      --owner NAME [--reason TEXT] [--ttl SECONDS] [--kind hmd|headless]
                           [--require-quiet] [--quiet-wait SECONDS]
  gpu-lock.sh wait         --owner NAME [--reason TEXT] [--timeout SECONDS] [--ttl SECONDS]
                           [--kind hmd|headless] [--require-quiet] [--quiet-wait SECONDS]
  gpu-lock.sh run          --owner NAME [--reason TEXT] [--timeout SECONDS] [--ttl SECONDS]
                           [--kind hmd|headless] [--require-quiet] [--quiet-wait SECONDS]
                           -- COMMAND...
  gpu-lock.sh heartbeat    --token TOKEN
  gpu-lock.sh release      --token TOKEN
  gpu-lock.sh reclaim      [--as NAME] [--reason TEXT] [--force]
  gpu-lock.sh user-hold    [REASON]
  gpu-lock.sh user-clear
  gpu-lock.sh log          [N]

Slot state: /mnt/m/Development/Deracine-work/_build/team/hmd-slot/
Quiet check: another shadps4, ninja, clang-cl, link, msbuild or RenderDoc process makes a
frame-rate cell meaningless; --require-quiet refuses (exit 7) instead of measuring through it.
Exit codes: 0 ok | 3 busy | 4 stale-not-breakable | 5 user hold | 6 wait timeout | 7 quiet
            8 headless-only: --kind hmd refused while the HEADLESS-ONLY marker exists
EOF
}

case "${1:-help}" in
  status)     shift; status_cmd "$@" ;;
  acquire)    shift; cmd_acquire "$@" ;;
  wait)       shift; cmd_wait "$@" ;;
  run)        shift; cmd_run "$@" ;;
  heartbeat)  shift; cmd_heartbeat "$@" ;;
  release)    shift; cmd_release "$@" ;;
  reclaim)    shift; cmd_reclaim "$@" ;;
  user-hold)  shift; cmd_user_hold "$@" ;;
  user-clear) shift; cmd_user_clear "$@" ;;
  log)        shift; cmd_log "$@" ;;
  help|-h|--help) usage ;;
  *) usage; exit 1 ;;
esac
