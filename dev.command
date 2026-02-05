#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
AGENT_CORE_DIR="$ROOT_DIR/agent-core"
PLUGIN_SRC_DIR="$ROOT_DIR/ue-plugin/UEAIAgent"
UE_PROJECT_PLUGINS_DIR="/Users/oleg/Dev/temp/AIAgentTPCpp/Plugins"
PLUGIN_DST_DIR="$UE_PROJECT_PLUGINS_DIR/UEAIAgent"
UE_ENGINE_DIR="/Users/Shared/Epic Games/UE_5.7"
UE_PROJECT_FILE="/Users/oleg/Dev/temp/AIAgentTPCpp/AIAgentTPCpp.uproject"
UE_PROJECT_NAME="$(basename "$UE_PROJECT_FILE" .uproject)"
PID_FILE="/tmp/ue-ai-agent-core.pid"
LOG_FILE="/tmp/ue-ai-agent-core.log"

print_usage() {
  cat <<'EOF'
Usage:
  ./dev.command start-agent
  ./dev.command stop-agent
  ./dev.command restart-agent
  ./dev.command deploy-plugin
  ./dev.command build-plugin
  ./dev.command setup

Commands:
  start-agent    Start agent-core in background.
  stop-agent     Stop running agent-core process.
  restart-agent  Restart agent-core process.
  deploy-plugin  Copy UEAIAgent plugin to UE project Plugins folder.
  build-plugin   Build UE project (compiles the plugin).
  setup          Restart agent-core, deploy plugin, and build it.
EOF
}

is_agent_running() {
  if [[ -f "$PID_FILE" ]]; then
    local pid
    pid="$(cat "$PID_FILE")"
    if [[ -n "${pid:-}" ]] && kill -0 "$pid" 2>/dev/null; then
      return 0
    fi
  fi
  return 1
}

start_agent() {
  if is_agent_running; then
    echo "agent-core is already running (PID: $(cat "$PID_FILE"))."
    return 0
  fi

  (
    cd "$AGENT_CORE_DIR"
    nohup npm run dev >"$LOG_FILE" 2>&1 &
    echo $! >"$PID_FILE"
  )

  echo "agent-core started (PID: $(cat "$PID_FILE"))."
  echo "Log: $LOG_FILE"
}

stop_agent() {
  if ! [[ -f "$PID_FILE" ]]; then
    echo "agent-core is not running."
    return 0
  fi

  local pid
  pid="$(cat "$PID_FILE")"
  if [[ -n "${pid:-}" ]] && kill -0 "$pid" 2>/dev/null; then
    kill "$pid" || true
    sleep 0.3
    if kill -0 "$pid" 2>/dev/null; then
      kill -9 "$pid" || true
    fi
    echo "agent-core stopped."
  else
    echo "agent-core PID file found, but process is not running."
  fi

  rm -f "$PID_FILE"
}

restart_agent() {
  stop_agent
  start_agent
}

deploy_plugin() {
  mkdir -p "$UE_PROJECT_PLUGINS_DIR"
  rsync -a --delete --exclude "Binaries/" --exclude "Intermediate/" "$PLUGIN_SRC_DIR/" "$PLUGIN_DST_DIR/"
  echo "Plugin deployed to: $PLUGIN_DST_DIR"
}

build_plugin() {
  if [[ ! -f "$UE_PROJECT_FILE" ]]; then
    echo "UE project not found: $UE_PROJECT_FILE"
    return 1
  fi

  local ubt="$UE_ENGINE_DIR/Engine/Build/BatchFiles/RunUBT.sh"
  if [[ ! -x "$ubt" ]]; then
    echo "UnrealBuildTool not found: $ubt"
    return 1
  fi

  local target="${UE_PROJECT_NAME}Editor"
  "$ubt" "$target" Mac Development -Project="$UE_PROJECT_FILE" -WaitMutex
  echo "Build done for target: $target"
}

main() {
  local cmd="${1:-}"
  case "$cmd" in
    start-agent) start_agent ;;
    stop-agent) stop_agent ;;
    restart-agent) restart_agent ;;
    deploy-plugin) deploy_plugin ;;
    build-plugin) build_plugin ;;
    setup)
      restart_agent
      deploy_plugin
      build_plugin
      ;;
    *)
      print_usage
      exit 1
      ;;
  esac
}

main "${1:-}"
