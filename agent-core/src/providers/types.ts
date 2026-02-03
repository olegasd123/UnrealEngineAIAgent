export interface PlanInput {
  prompt: string;
  mode: "chat" | "agent";
  context: Record<string, unknown>;
}

export interface PlanOutput {
  summary: string;
  steps: string[];
}

export interface LlmProvider {
  name: "openai" | "gemini";
  planTask(input: PlanInput): Promise<PlanOutput>;
}

