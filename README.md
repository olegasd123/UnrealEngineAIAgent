# Unreal Engine AI Agent

Local-first AI agent for Unreal Editor (UE 5.7).

## Current scaffold

- `ue-plugin/UEAIAgent`: Unreal Editor plugin (C++)
- `agent-core`: local orchestration service (TypeScript)
- `shared`: shared schemas and contracts
- local provider key management via Agent Core credentials endpoints
- agent session orchestration endpoints in Agent Core (`start/next/approve/resume`)

## Vision

User writes a task in the editor.  
Agent plans and applies changes in scene, landscape, and PCG with safe approvals.

## Next

1. Build plugin panel UI
2. Add localhost transport
3. Implement first scene edit tools
