import type { LlmProvider, PlanInput, PlanOutput } from "./types.js";

export class MockProvider implements LlmProvider {
  public readonly name = "openai";

  async planTask(input: PlanInput): Promise<PlanOutput> {
    return {
      summary: `Draft plan for: ${input.prompt}`,
      steps: [
        "Collect scene context",
        "Build action list",
        "Wait for user approval in risky steps"
      ]
    };
  }
}

