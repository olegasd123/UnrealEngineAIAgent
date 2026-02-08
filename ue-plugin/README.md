# UE Plugin

This folder contains the Unreal Editor plugin code.

Target: Unreal Engine 5.7.

Settings path in UE:
- `Project Settings -> Plugins -> UE AI Agent`

Quick validation in editor tab:
- Agent status is checked when tab opens and refreshed every 10 seconds
- open `Settings` to manage provider keys: `Save API Key`, `Remove API Key`, `Test Provider`, `Refresh Provider Status`
- provider selector supports `OpenAI`, `Gemini`, and `Local`
- main view:
  - mode selector near `Run`: `Chat` or `Agent`
  - prompt input is multi-line and grows up to 10 lines
  - selection summary shows selected actors and updates automatically
  - `Run` starts flow:
    - in chat mode: request `/v1/task/plan` and show parsed actions
    - in agent mode: start session orchestration with `/v1/session/start`
  - `Resume` continues session (`/v1/session/approve` + `/v1/session/resume`)
  - `Apply` executes approved actions with Undo support
  - `Approve Low Risk` marks low-risk planned actions as approved
  - `Reject All` unchecks all planned actions
  - planned actions are grouped by risk (`High`, `Medium`, `Low`) and each row has `Show details`
- history view:
  - `Refresh Chats` reloads chat list
  - `Archive Selected` archives active chat
  - `Show Archived` toggles archived list from backend
  - search box filters chats by title or id
  - left panel is chat list (sorted by last activity)
  - chat title supports inline rename (select chat, press `F2` or `Enter`)
  - right panel shows selected chat history entries (kind, route, timestamp, summary)
- keyboard shortcuts:
  - `Ctrl+Enter` / `Cmd+Enter`: run from main view
  - `Esc`: focus prompt input
