# UE Plugin

This folder contains the Unreal Editor plugin code.

Target: Unreal Engine 5.7.

Settings path in UE:
- `Project Settings -> Plugins -> UE AI Agent`

Quick validation in editor tab:
- `Check Local Agent` for localhost health check
- provider key tools: `Save API Key`, `Remove API Key`, `Test Provider`, `Refresh Provider Status`
- `Plan With Selection` to parse prompt into planned actions (move/rotate)
- review and toggle action approvals
- `Run Agent Loop` to start core session orchestration (`/v1/session/start`)
- `Resume Agent Loop` to send approvals and continue (`/v1/session/approve` + `/v1/session/resume`)
- `Apply Planned Actions` to execute approved `scene.modifyActor`, `scene.createActor`, and `scene.deleteActor` actions with Undo support
