# Agent Core

Local service that plans and executes UE editor actions.

## Runtime layers

Request flow is split into 4 layers:
- Intent Layer: normalize user request and detect goal type.
- Planning Layer: build action plan with selected provider.
- Validation Layer: validate plan schema and basic intent/plan alignment.
- Execution Layer: manage session state and approval flow.

## Scope in scaffold

- health endpoint
- task planning endpoint (`/v1/task/plan`)
- agent session endpoints (`/v1/session/start|next|approve|resume`)
- simple prompt parser for move/rotate/scale + component visibility + tag actions (example: `move +250 on x and rotate yaw +45`)
- provider adapter interface

## Run

```bash
npm install
npm run dev
```

## Generated command list

The allowed tool commands are generated from the shared JSON schema. Re-generate after any change to:
- `./shared/schemas/ue-tool-command.schema.json`

```bash
npm run generate:commands
```

Optional environment:
- `AGENT_HOST` (default `127.0.0.1`)
- `AGENT_PORT` (default `4317`)
- `AGENT_PROVIDER` (`openai` or `gemini`, default `openai`)
- `AGENT_TASK_LOG_PATH` (default `data`; used as log directory, legacy file path still works)
- `OPENAI_API_KEY` (optional in stub mode)
- `OPENAI_MODEL` (default `gpt-4.1-mini`)
- `OPENAI_BASE_URL` (optional)
- `OPENAI_TEMPERATURE` (default `0.2`)
- `OPENAI_MAX_TOKENS` (default `1200`)
- `GEMINI_API_KEY` (optional in stub mode)
- `GEMINI_MODEL` (default `gemini-2.0-flash`)
- `GEMINI_BASE_URL` (optional)
- `GEMINI_TEMPERATURE` (default `0.2`)
- `GEMINI_MAX_TOKENS` (default `1200`)
- `AGENT_POLICY_MAX_CREATE_COUNT` (default `50`)
- `AGENT_POLICY_MAX_DUPLICATE_COUNT` (default `10`)
- `AGENT_POLICY_MAX_TARGET_NAMES` (default `50`)
- `AGENT_POLICY_MAX_DELETE_BY_NAME_COUNT` (default `20`)
- `AGENT_POLICY_SELECTION_TARGET_ESTIMATE` (default `5`)
- `AGENT_POLICY_MAX_SESSION_CHANGE_UNITS` (default `120`)

## Credential management

- Provider keys can be managed over local endpoints:
  - `GET /v1/providers/status`
  - `POST /v1/credentials/set`
  - `POST /v1/credentials/delete`
  - `POST /v1/credentials/test`
- macOS stores keys in Keychain.
- Windows stores keys in user-protected encrypted file (DPAPI via PowerShell).
- Linux/other platforms store keys in `~/.ueaiagent/secrets` (local file fallback).
- `OPENAI_API_KEY` and `GEMINI_API_KEY` env vars still work and have priority.

## Provider behavior

- If API key exists for selected provider, Agent Core makes a real provider call.
- If key is missing or call fails, Agent Core falls back to local rule-based planning.

## Task log store

- Each `/v1/task/plan` request is appended to local JSONL log file.
- Success and error entries include `requestId`, timestamp, provider info, and duration.
- Read logs: `GET /v1/task/logs?limit=50` (max limit is 50, default is 50).

## Session log store

- Each session endpoint call is appended to local JSONL log file:
  - `POST /v1/session/start`
  - `POST /v1/session/next`
  - `POST /v1/session/approve`
  - `POST /v1/session/resume`
- Success and error entries include `requestId`, timestamp, route, request sample/data, decision (on success), and duration.
- Read logs: `GET /v1/session/logs?limit=50` (max limit is 50, default is 50).

## Log file naming

- Log files are daily and use this format:
  - `yyyyMMdd-task-log.jsonl`
  - `yyyyMMdd-session-log.jsonl`
- Example: `20261129-session-log.jsonl`

## Agent session orchestration

- `POST /v1/session/start`: create a session from prompt/context and return first decision.
- `POST /v1/session/next`: report execution result and get the next decision.
- `POST /v1/session/approve`: approve/reject current pending action.
- `POST /v1/session/resume`: request current next decision without sending result.
- Decisions include statuses:
  - `ready_to_execute`
  - `awaiting_approval`
  - `completed`
  - `failed`

## Unreal plugin migration (strict context schema)

`TaskRequest.context` is now strict. Unknown keys are rejected.

Allowed top-level context keys:
- `selection`
- `mapName`, `levelName`, `gameStyle`, `timeOfDay`, `weather`
- `materialStyle`
- `qualityTier`, `targetFps`, `maxDrawCalls`
- `manualStop`
- `environment`, `lighting`, `materials`, `performance`, `assets`

If your plugin sends custom fields, move them to allowed keys or remove them.

Example: `/v1/task/plan` payload

```json
{
  "prompt": "Tune lighting for open-world survival mood",
  "mode": "agent",
  "context": {
    "selection": ["SM_Rock_01", "SM_Tree_05"],
    "mapName": "OpenWorld_P",
    "gameStyle": "open_world_survival",
    "timeOfDay": "dusk",
    "environment": {
      "weather": "overcast"
    },
    "lighting": {
      "hasDirectionalLight": true,
      "hasSkyLight": true,
      "hasFog": true,
      "exposureCompensation": -0.3,
      "directionalLightIntensity": 6.5,
      "skyLightIntensity": 1.0,
      "fogDensity": 0.02
    },
    "materials": {
      "styleHint": "wet, cold, worn",
      "targetMaterialPaths": [
        "/Game/Materials/M_Rock_Wet.M_Rock_Wet",
        "/Game/Materials/M_Mud.M_Mud"
      ],
      "selectedMaterialCount": 2
    },
    "performance": {
      "qualityTier": "high",
      "targetFps": 60,
      "maxDrawCalls": 4000
    },
    "assets": {
      "materialPaths": ["/Game/Materials/M_Rock_Wet.M_Rock_Wet"],
      "meshPaths": ["/Game/Props/SM_Crate.SM_Crate"]
    }
  }
}
```

Example: `/v1/session/start` payload

```json
{
  "prompt": "Adapt selected materials to stylized fantasy look",
  "mode": "agent",
  "maxRetries": 2,
  "context": {
    "selection": ["SM_House_02"],
    "levelName": "Village_L",
    "materialStyle": "stylized_fantasy",
    "materials": {
      "styleHint": "painted, soft roughness, bright accents",
      "targetMaterialPaths": ["/Game/Materials/M_House_Stylized.M_House_Stylized"]
    },
    "manualStop": false
  }
}
```

Example: invalid context (will fail)

```json
{
  "prompt": "Move selected actors +100 on X",
  "context": {
    "selection": ["Actor_1"],
    "biome": "forest"
  }
}
```

`biome` is not an allowed key. Use `environment.gameStyle`, `weather`, or another supported field.
