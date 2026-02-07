# UE Plugin

This folder contains the Unreal Editor plugin code.

Target: Unreal Engine 5.7.

Settings path in UE:
- `Project Settings -> Plugins -> UE AI Agent`

Quick validation in editor tab:
- Agent status is checked when tab opens and refreshed every 10 seconds
- open `Settings` to manage provider keys: `Save API Key`, `Remove API Key`, `Test Provider`, `Refresh Provider Status`
- mode selector near `Run`: `Chat` or `Agent`
- chat selector and management:
  - `New Chat` to create chat
  - `Refresh Chats` to reload active chat list
  - `Archive Chat` to archive selected chat
  - chat history box shows asked/done timeline from Agent Core
- `Run` to run one flow:
  - in chat mode: request `/v1/task/plan` and show parsed actions
  - in agent mode: start session orchestration with `/v1/session/start`
- prompt input is multi-line and grows up to 10 lines
- `Resume` to send approvals and continue (`/v1/session/approve` + `/v1/session/resume`)
- `Apply` to execute approved `scene.modifyActor`, `scene.createActor`, and `scene.deleteActor` actions with Undo support
- buttons are state-based:
  - `Run` when there is no pending work
  - `Resume` when an agent session is waiting on next action
  - `Apply` when there are planned actions ready for manual apply
