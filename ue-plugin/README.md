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
- `Run Agent Loop` to auto-execute low-risk actions with retry
- `Resume Agent Loop` to continue after approvals or pause
- `Apply Planned Actions` to execute approved `scene.modifyActor`, `scene.createActor`, and `scene.deleteActor` actions with Undo support
