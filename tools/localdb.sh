#!/usr/bin/env bash
# Local Supabase for fast dashboard / SQL turnaround. Never touches the live
# project: the stack runs in Docker and is built from this repo's own SQL, in
# the order docs/supabase.md section 1 and whats_installed.sql give.
#
#   bash tools/localdb.sh up      start (first run pulls the images) + load
#   bash tools/localdb.sh load    rebuild the schema from supabase/*.sql
#   bash tools/localdb.sh seed    dummy BWL-001 + LDC-001 meals on D slot 1
#   bash tools/localdb.sh env     print what fleet_sim and the browser need
#   bash tools/localdb.sh down    stop the containers
#
# The workdir is tools/localdb/ (gitignored), not the repo's supabase/ folder:
# that folder is hand-run SQL, and `supabase init` there would mix a CLI
# project into it.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK="$REPO/tools/localdb"
export PATH="/c/Program Files/Docker/Docker/resources/bin:$PATH"
sb() { supabase --workdir "$WORK" "$@"; }

init() {
  [ -f "$WORK/supabase/config.toml" ] && return
  mkdir -p "$WORK"
  (cd "$WORK" && supabase init)
  # The dashboard signs in anonymously (docs/supabase.md section 5); off by
  # default locally, and with it off the dashboard reads nothing at all.
  sed -i -e 's/^project_id = .*/project_id = "bowlstack-local"/' \
         -e 's/^enable_anonymous_sign_ins = false/enable_anonymous_sign_ins = true/' \
         "$WORK/supabase/config.toml"
}

load() {
  # schema.sql drops and rebuilds, so a reload is just running this again.
  # cutover_buffers.sql is here for parity with live (every BWL is a buffer).
  for f in schema register_devices assign_devices seed_meal_mapping \
           migrate_bowl_weight apply_loadcell weekly_menu_and_offline \
           cutover_buffers migrate_statistics smoke_test; do
    echo "== $f.sql"
    # psql, not `supabase db query --local`: that sends a file as one prepared
    # statement, which refuses more than one command.
    docker exec -i supabase_db_bowlstack-local psql -X -q -v ON_ERROR_STOP=1 \
      "postgresql://postgres:postgres@127.0.0.1:5432/postgres" \
      < "$REPO/supabase/$f.sql" > "$WORK/$f.out" 2>&1 \
      || { tail -20 "$WORK/$f.out"; echo "FAILED at $f.sql"; exit 1; }
  done
  grep -h "VERDICT" "$WORK/smoke_test.out"
}

env_() {
  sb status -o env 2>/dev/null | grep -E '^(API_URL|ANON_KEY|STUDIO_URL)=' | tr -d '"' > "$WORK/.env"
  . "$WORK/.env"
  cat <<EOF
fleet_sim:  BOWLSTACK_SUPABASE_URL=$API_URL BOWLSTACK_ANON_KEY=$ANON_KEY python tools/fleet_sim.py
studio:     $STUDIO_URL
browser:    open the dashboard served locally, then in its console:
  localStorage.setItem('bowlstack.connection', JSON.stringify({url:'$API_URL',anonKey:'$ANON_KEY',autoLogin:'anonymous'})); location.reload()
  back to live: localStorage.removeItem('bowlstack.connection'); location.reload()
EOF
}

case "${1:-up}" in
  up)   init
        # ponytail: only what the dashboard and the devices use -- add a
        # service back here if a feature starts needing it.
        sb start -x realtime,storage-api,imgproxy,mailpit,edge-runtime,logflare,vector,supavisor
        load; env_ ;;
  load) load ;;
  # Dummy BWL-001 + LDC-001 history on D slot 1 for the Statistics page. It
  # refuses to run where those ids hold real readings (i.e. on live).
  seed) docker exec -i supabase_db_bowlstack-local psql -X -q -v ON_ERROR_STOP=1 \
          "postgresql://postgres:postgres@127.0.0.1:5432/postgres" \
          < "$REPO/tools/localdb_seed_slot1.sql" ;;
  env)  env_ ;;
  down) sb stop ;;
  *)    echo "usage: $0 [up|load|seed|env|down]"; exit 2 ;;
esac
