# UE Plugin

This folder contains the Unreal Editor plugin code.

Target: Unreal Engine 5.7.

Settings path in UE:
- `Project Settings -> Plugins -> UE AI Agent`

Quick validation in editor tab:
- Agent status is checked when tab opens and refreshed every 10 seconds
- health text is shown only when check is not ok
- open `Settings` to manage provider keys: `Save API Key`, `Remove API Key`, `Test Provider`, `Refresh Provider Status`
- provider selector supports `OpenAI`, `Gemini`, and `Local`
- main view:
  - first row has `New Chat`, `Show Chats`/`Hide Chats`, and right-aligned `Settings`
  - `Show Chats`/`Hide Chats` state is saved and restored on next open
  - chat controls are on top: `Refresh`, `Archived`, and search by title
  - chat list is on top (sorted by last activity)
  - chat list height: min `3` rows, auto-grow, max rows from setting `Chat List Max Rows` (default `10`)
  - when list is larger than max rows, it is scrollable
  - each chat row shows `Title (relative time)` like `today`, `yesterday`, `2 days ago`, `last week`
  - switch active chat by selecting it in the list
  - chat row actions are right-aligned icon buttons with tooltips:
    - active chats: `Archive` (with confirm dialog)
    - archived chats: `Restore` and `Delete` (both with confirm dialog)
    - `Delete` removes chat permanently from DB
  - chat title supports inline rename (double click row, or select row and press `F2`/`Enter`)
  - `New Chat` click is ignored if current active chat is still empty
  - mode selector near `Run`: `Chat` or `Agent`
  - prompt input is multi-line and grows up to 10 lines
  - prompt input is hidden while `Run` is loading or approval UI is open (no empty gap)
  - selection summary shows selected actors and updates automatically
  - `Run` starts flow:
    - in chat mode: request `/v1/task/plan`, show planned actions, and set all actions unchecked
    - in agent mode: start session orchestration with `/v1/session/start`
  - chat list auto-refreshes after chat/agent updates when title can change from `New chat`
  - confirmation controls:
    - `Check all` / `Uncheck all` for planned actions
    - `Apply` executes checked actions with Undo support
    - `Cancel` is shown in chat mode and clears planned actions
    - `Resume` is shown in agent mode and continues the session (`/v1/session/approve` + `/v1/session/resume`)
    - `Reject` is shown in agent mode and rejects current action (with confirm dialog)
  - planned actions are grouped by risk (`High`, `Medium`, `Low`) and each row has `Show details`
- selected chat history entries are shown in the main view and loaded with no default entry cap (full history by default)
- keyboard shortcuts:
  - `Ctrl+Enter` / `Cmd+Enter`: run from main view
  - `F2` / `Enter`: rename selected chat
  - `Esc`: focus prompt input
