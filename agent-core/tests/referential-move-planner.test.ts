import assert from "node:assert/strict";
import test from "node:test";

import { buildRuleBasedPlan } from "../src/planner/ruleBasedPlanner.js";
import { IntentLayer } from "../src/intent/intentLayer.js";
import { ValidationLayer } from "../src/validator/validationLayer.js";
import type { TaskRequest } from "../src/contracts.js";

test("Rule planner parses 'move it back to 100' and uses remembered selection byName", () => {
  const request: TaskRequest = {
    prompt: "move it back to 100",
    mode: "chat",
    context: {
      selectionNames: ["MyActor"]
    }
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "scene.modifyActor");
  if (action.command !== "scene.modifyActor") {
    return;
  }
  assert.equal(action.params.target, "byName");
  assert.deepEqual(action.params.actorNames, ["MyActor"]);
  assert.deepEqual(action.params.deltaLocation, { x: -100, y: 0, z: 0 });
});

test("Validation rewrites selection target to byName when context has remembered selection", () => {
  const request: TaskRequest = {
    prompt: "move it on x 10",
    mode: "chat",
    context: {
      selectionNames: ["ActorFromMemory"]
    }
  };
  const intent = new IntentLayer().normalize(request);
  const validator = new ValidationLayer();

  const candidate = {
    summary: "move",
    steps: ["step"],
    actions: [
      {
        command: "scene.modifyActor",
        params: { target: "selection", deltaLocation: { x: 10, y: 0, z: 0 } },
        risk: "low"
      }
    ],
    goal: { id: "g1", description: "d", priority: "medium" },
    subgoals: [],
    checks: [],
    stopConditions: [{ type: "all_checks_passed" }]
  };

  const validated = validator.validatePlan(intent, candidate);
  const action = validated.plan.actions[0];
  assert.equal(action.command, "scene.modifyActor");
  if (action.command !== "scene.modifyActor") {
    return;
  }
  assert.equal(action.params.target, "byName");
  assert.deepEqual(action.params.actorNames, ["ActorFromMemory"]);
});

test("Validation normalizes safe action risk to low", () => {
  const request: TaskRequest = {
    prompt: "move it on x 10",
    mode: "agent",
    context: {
      selectionNames: ["ActorFromMemory"]
    }
  };
  const intent = new IntentLayer().normalize(request);
  const validator = new ValidationLayer();

  const candidate = {
    summary: "move",
    steps: ["step"],
    actions: [
      {
        command: "session.beginTransaction",
        params: { description: "tx" },
        risk: "medium"
      },
      {
        command: "scene.modifyActor",
        params: { target: "selection", deltaLocation: { x: 10, y: 0, z: 0 } },
        risk: "medium"
      },
      {
        command: "session.commitTransaction",
        params: {},
        risk: "high"
      }
    ],
    goal: { id: "g1", description: "d", priority: "medium" },
    subgoals: [],
    checks: [],
    stopConditions: [{ type: "all_checks_passed" }]
  };

  const validated = validator.validatePlan(intent, candidate);
  assert.equal(validated.plan.actions[0]?.risk, "low");
  assert.equal(validated.plan.actions[1]?.risk, "low");
  assert.equal(validated.plan.actions[2]?.risk, "low");
});
