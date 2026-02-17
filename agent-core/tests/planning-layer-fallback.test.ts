import assert from "node:assert/strict";
import test from "node:test";

import type { PlanOutput, TaskRequest } from "../src/contracts.js";
import { IntentLayer } from "../src/intent/intentLayer.js";
import { PlanningLayer } from "../src/planner/planningLayer.js";
import type { LlmProvider, PlanInput, TextReplyInput } from "../src/providers/types.js";

const noActionPlan: PlanOutput = {
  summary: "No actionable intent detected.",
  steps: ["Verify lack of explicit action request"],
  actions: [],
  goal: {
    id: "goal_no_action",
    description: "No actions to perform.",
    priority: "low"
  },
  subgoals: [],
  checks: [],
  stopConditions: [{ type: "all_checks_passed" }, { type: "max_iterations", value: 1 }, { type: "user_denied" }]
};

class NoActionProvider implements LlmProvider {
  public readonly name = "local";
  public readonly model = "test-model";
  public readonly hasApiKey = true;
  public readonly adapter = "stub" as const;

  async planTask(_input: PlanInput): Promise<PlanOutput> {
    return noActionPlan;
  }

  async respondText(_input: TextReplyInput): Promise<string> {
    return "ok";
  }
}

function makeRequest(prompt: string): TaskRequest {
  return {
    prompt,
    mode: "chat",
    context: {}
  };
}

test("PlanningLayer uses local fallback when provider returns no actions for write intent", async () => {
  const provider = new NoActionProvider();
  const planning = new PlanningLayer();
  const intent = new IntentLayer().normalize(makeRequest("sculpt terrain at x 1200 y -300 size 1800 strength 30%"));

  const plan = await planning.buildPlan(intent, provider);
  assert.ok(plan.actions.length > 0);
  assert.equal(plan.actions[0]?.command, "landscape.sculpt");
  assert.match(plan.steps[0] ?? "", /Using local fallback/i);
});

test("PlanningLayer uses local fallback for undo prompt when provider returns no actions", async () => {
  const provider = new NoActionProvider();
  const planning = new PlanningLayer();
  const intent = new IntentLayer().normalize(makeRequest("undo"));

  const plan = await planning.buildPlan(intent, provider);
  assert.equal(plan.actions.length, 1);
  assert.equal(plan.actions[0]?.command, "editor.undo");
  assert.match(plan.steps[0] ?? "", /Using local fallback/i);
});

test("PlanningLayer uses local fallback for redo prompt when provider returns no actions", async () => {
  const provider = new NoActionProvider();
  const planning = new PlanningLayer();
  const intent = new IntentLayer().normalize(makeRequest("redo"));

  const plan = await planning.buildPlan(intent, provider);
  assert.equal(plan.actions.length, 1);
  assert.equal(plan.actions[0]?.command, "editor.redo");
  assert.match(plan.steps[0] ?? "", /Using local fallback/i);
});

test("PlanningLayer uses local fallback for landscape generate prompt when provider returns no actions", async () => {
  const provider = new NoActionProvider();
  const planning = new PlanningLayer();
  const intent = new IntentLayer().normalize(makeRequest("create moon surface using all available space on the landscape"));

  const plan = await planning.buildPlan(intent, provider);
  assert.equal(plan.actions.length, 1);
  assert.equal(plan.actions[0]?.command, "landscape.generate");
  assert.match(plan.steps[0] ?? "", /Using local fallback/i);
});

test("PlanningLayer keeps provider no-action result for non-write prompt", async () => {
  const provider = new NoActionProvider();
  const planning = new PlanningLayer();
  const intent = new IntentLayer().normalize(makeRequest("what is selected right now?"));

  const plan = await planning.buildPlan(intent, provider);
  assert.equal(plan.actions.length, 0);
  assert.equal(plan.summary, "No actionable intent detected.");
});
