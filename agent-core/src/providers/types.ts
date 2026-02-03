export interface PlanInput {
  prompt: string;
  mode: "chat" | "agent";
  context: Record<string, unknown>;
}

export interface SceneModifyActorAction {
  command: "scene.modifyActor";
  params: {
    target: "selection";
    deltaLocation: {
      x: number;
      y: number;
      z: number;
    };
  };
  risk: "low" | "medium" | "high";
}

export type PlanAction = SceneModifyActorAction;

export interface PlanOutput {
  summary: string;
  steps: string[];
  actions?: PlanAction[];
}

export interface LlmProvider {
  name: "openai" | "gemini";
  planTask(input: PlanInput): Promise<PlanOutput>;
}
