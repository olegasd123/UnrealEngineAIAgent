import type { LlmProvider, PlanInput, PlanOutput } from "./types.js";

export class MockProvider implements LlmProvider {
  public readonly name: "openai" | "gemini";

  constructor(name: "openai" | "gemini") {
    this.name = name;
  }

  async planTask(input: PlanInput): Promise<PlanOutput> {
    const selection = Array.isArray((input.context as { selection?: unknown }).selection)
      ? ((input.context as { selection: unknown[] }).selection as unknown[])
      : [];

    return {
      summary: `Draft plan for: ${input.prompt} (selected: ${selection.length})`,
      steps: [
        "Collect scene context",
        "Build action list",
        "Wait for user approval in risky steps"
      ]
    };
  }
}
