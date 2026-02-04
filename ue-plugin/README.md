# UE Plugin

This folder contains the Unreal Editor plugin code.

Target: Unreal Engine 5.7.

Settings path in UE:
- `Project Settings -> Plugins -> UE AI Agent`

Quick validation in editor tab:
- `Check Local Agent` for localhost health check
- `Plan With Selection` to parse prompt into planned actions (move/rotate)
- review and toggle action approvals
- `Apply Planned Actions` to execute approved `scene.modifyActor`, `scene.createActor`, and `scene.deleteActor` actions with Undo support
