import type { PlanOutput, TaskRequest } from "../contracts.js";

export type PlanInput = TaskRequest;

export interface LlmProvider {
  name: "openai" | "gemini";
  planTask(input: PlanInput): Promise<PlanOutput>;
}
