# Agent Core

Local service that plans and executes UE editor actions.

## Scope in scaffold

- health endpoint
- task planning endpoint (`/v1/task/plan`)
- simple prompt parser for move/rotate actions (example: `move +250 on x and rotate yaw +45`)
- provider adapter interface

## Run

```bash
npm install
npm run dev
```

Optional environment:
- `AGENT_HOST` (default `127.0.0.1`)
- `AGENT_PORT` (default `4317`)
- `AGENT_PROVIDER` (`openai` or `gemini`, default `openai`)
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

## Provider behavior

- If API key exists for selected provider, Agent Core makes a real provider call.
- If key is missing or call fails, Agent Core falls back to local rule-based planning.
