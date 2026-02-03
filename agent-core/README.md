# Agent Core

Local service that plans and executes UE editor actions.

## Scope in scaffold

- health endpoint
- basic command endpoint
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
