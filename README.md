# Unreal Engine AI Agent

Local-first AI agent for Unreal Editor (UE 5.7).

## Overview

This project has three parts:

- `ue-plugin/UEAIAgent` - Unreal Editor plugin (C++)
- `agent-core` - local orchestration service (TypeScript/Node.js)
- `shared` - shared JSON schemas and contracts

The plugin sends task requests to Agent Core over localhost.  
Agent Core plans actions, applies safety checks, and returns decisions for chat mode or agent mode.

## V1 scope

- Unreal Engine target: `5.7`
- Platforms: `Windows` and `macOS`
- Providers: `OpenAI`, `Gemini`, and `Local` (OpenAI-compatible, for example LM Studio)
- Main domains:
  - scene and actor edits
  - landscape edits
  - PCG graph actions
- Session flow with approval gates:
  - `/v1/session/start`
  - `/v1/session/next`
  - `/v1/session/approve`
  - `/v1/session/resume`
- Token-efficient context strategy: use the `fetch-more` pattern (ask for more context only when needed)

## Landscape Features (Current)

Supported landscape actions:

- `landscape.sculpt` (bounded area, raise/lower, strength/falloff)
- `landscape.paintLayer` (bounded area, add/remove, strength/falloff)
- `landscape.generate` with themes:
  - `moon_surface`
  - `nature_island`

`landscape.generate` supports:

- area mode: full landscape or bounded `center/size`
- quality: `detailLevel` (`low|medium|high|cinematic`)
- reproducibility: `seed`
- height: `maxHeight`

Moon surface options:

- `moonProfile`: `ancient_heavily_cratered` (default)
- crater controls:
  - `craterCountMin` / `craterCountMax`
  - `craterWidthMin` / `craterWidthMax`

Nature island options:

- mountain controls:
  - `mountainCount`
  - `mountainStyle` (`sharp_peaks` default, `hills` when requested)
  - `mountainWidthMin` / `mountainWidthMax`

Default behavior for prompt `create a nature island`:

- mountains: random `1-3` with `sharp_peaks` style

If user asks for hills, use hill wording in prompt (example: `create a natural island with 2 hills, hill size between 1200 and 2000, at x 4000 y -1500`).

## Quick start

### 1) Prerequisites

- Unreal Engine `5.7`
- Node.js `20+`
- npm

### 2) Run Agent Core

```bash
cd agent-core
npm install
npm run generate:commands
npm run dev
```

Agent Core runs on `127.0.0.1:4317` by default.

### 3) Add plugin to your UE project

Copy this folder to your UE project:

- `ue-plugin/UEAIAgent` -> `<YourProject>/Plugins/UEAIAgent`

Then open Unreal Editor and open the plugin panel.

### 4) Optional helper script

You can use `./dev.command` from repo root for local workflow (`setup`, `start-agent`, `deploy-plugin`, `build-plugin`).

Note: this script contains machine-specific paths. Update them before use.

## Documentation

- Root architecture: `ARCHITECTURE_V1.md`
- Agent Core details: `agent-core/README.md`
- UE plugin details: `ue-plugin/README.md`
- Shared schemas: `shared/README.md`
