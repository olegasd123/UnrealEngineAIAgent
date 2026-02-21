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
  assert.equal(plan.summary, "Move selected actor back 100 units along X.");
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
        command: "scene.modifyActor",
        params: { target: "selection", deltaLocation: { x: 10, y: 0, z: 0 } },
        risk: "medium"
      }
    ],
    goal: { id: "g1", description: "d", priority: "medium" },
    subgoals: [],
    checks: [],
    stopConditions: [{ type: "all_checks_passed" }]
  };

  const validated = validator.validatePlan(intent, candidate);
  assert.equal(validated.plan.actions[0]?.risk, "low");
});

test("Rule planner builds context.getSelection for selection info request", () => {
  const request: TaskRequest = {
    prompt: "what is selected right now?",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  assert.equal(plan.summary, "Collect current selection context.");
  const action = plan.actions[0];
  assert.equal(action.command, "context.getSelection");
  if (action.command !== "context.getSelection") {
    return;
  }
  assert.deepEqual(action.params, {});
  assert.equal(action.risk, "low");
});

test("Rule planner builds context.getSceneSummary for scene summary request", () => {
  const request: TaskRequest = {
    prompt: "show scene summary",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  assert.equal(plan.summary, "Collect current scene summary context.");
  const action = plan.actions[0];
  assert.equal(action.command, "context.getSceneSummary");
  if (action.command !== "context.getSceneSummary") {
    return;
  }
  assert.deepEqual(action.params, {});
  assert.equal(action.risk, "low");
});

test("Rule planner builds editor.undo action for undo prompt", () => {
  const request: TaskRequest = {
    prompt: "undo",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  assert.equal(plan.summary, "Undo last editor action.");
  const action = plan.actions[0];
  assert.equal(action.command, "editor.undo");
  if (action.command !== "editor.undo") {
    return;
  }
  assert.deepEqual(action.params, {});
  assert.equal(action.risk, "low");
});

test("Rule planner builds editor.redo action for redo prompt", () => {
  const request: TaskRequest = {
    prompt: "redo",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  assert.equal(plan.summary, "Redo last editor action.");
  const action = plan.actions[0];
  assert.equal(action.command, "editor.redo");
  if (action.command !== "editor.redo") {
    return;
  }
  assert.deepEqual(action.params, {});
  assert.equal(action.risk, "low");
});

test("Rule planner builds landscape.generate for moon surface prompt", () => {
  const request: TaskRequest = {
    prompt: "create moon surface using all available space on the landscape",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "landscape.generate");
  if (action.command !== "landscape.generate") {
    return;
  }
  assert.equal(action.params.theme, "moon_surface");
  assert.equal(action.params.moonProfile, "moon_surface");
  assert.equal(action.params.detailLevel, "high");
  assert.equal(action.params.mountainCount, 6);
  assert.equal(action.params.useFullArea, true);
  assert.equal(action.risk, "medium");
});

test("Rule planner respects explicit moon detail level hints", () => {
  const request: TaskRequest = {
    prompt: "create realistic moon surface with cinematic detail on the full landscape",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "landscape.generate");
  if (action.command !== "landscape.generate") {
    return;
  }
  assert.equal(action.params.theme, "moon_surface");
  assert.equal(action.params.detailLevel, "cinematic");
  assert.equal(action.params.mountainCount, 8);
});

test("Rule planner parses crater min/max constraints for moon surface prompt", () => {
  const request: TaskRequest = {
    prompt: "create moon surface with min crater count 20 max crater count 60, min crater width 400 max crater width 1800, max height 7000",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "landscape.generate");
  if (action.command !== "landscape.generate") {
    return;
  }
  assert.equal(action.params.theme, "moon_surface");
  assert.equal(action.params.craterCountMin, 20);
  assert.equal(action.params.craterCountMax, 60);
  assert.equal(action.params.craterWidthMin, 400);
  assert.equal(action.params.craterWidthMax, 1800);
  assert.equal(action.params.maxHeight, 7000);
});

test("Rule planner parses singular crater wording for strict count and width constraints", () => {
  const request: TaskRequest = {
    prompt:
      "Generate an ancient heavily cratered lunar field across the full landscape with crater count between 2 and 5, crater width between 300 and 10000",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "landscape.generate");
  if (action.command !== "landscape.generate") {
    return;
  }

  assert.equal(action.params.theme, "moon_surface");
  assert.equal(action.params.moonProfile, "moon_surface");
  assert.equal(action.params.craterCountMin, 2);
  assert.equal(action.params.craterCountMax, 5);
  assert.equal(action.params.craterWidthMin, 300);
  assert.equal(action.params.craterWidthMax, 10000);
});

test("Rule planner infers ancient heavily cratered moon profile from descriptive prompt", () => {
  const request: TaskRequest = {
    prompt:
      "Generate an ancient heavily cratered lunar field with overlapping impact craters, regolith, ejecta patterns, and terraces inside a large crater.",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "landscape.generate");
  if (action.command !== "landscape.generate") {
    return;
  }

  assert.equal(action.params.theme, "moon_surface");
  assert.equal(action.params.moonProfile, "moon_surface");
  assert.equal(action.params.detailLevel, "high");
  assert.equal(action.params.craterCountMin, 140);
  assert.equal(action.params.craterCountMax, 340);
});

test("Rule planner builds landscape.generate for nature island prompt with constraints", () => {
  const request: TaskRequest = {
    prompt: "create a nature island with 2 mountains, max height of 5000",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "landscape.generate");
  if (action.command !== "landscape.generate") {
    return;
  }
  assert.equal(action.params.theme, "nature_island");
  assert.equal(action.params.mountainStyle, "sharp_peaks");
  assert.equal(action.params.mountainCount, 2);
  assert.equal(action.params.maxHeight, 5000);
  assert.equal(action.params.useFullArea, true);
  assert.equal(action.risk, "medium");
});

test("Rule planner parses plain nature island prompt", () => {
  const request: TaskRequest = {
    prompt: "create a nature island",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "landscape.generate");
  if (action.command !== "landscape.generate") {
    return;
  }
  assert.equal(action.params.theme, "nature_island");
  assert.equal(action.params.mountainStyle, "sharp_peaks");
  assert.equal(action.params.mountainCount, undefined);
});

test("Rule planner parses nature island mountain size constraints", () => {
  const request: TaskRequest = {
    prompt:
      "create a nature island with 3 mountains, mountain width between 1200 and 3800",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "landscape.generate");
  if (action.command !== "landscape.generate") {
    return;
  }
  assert.equal(action.params.theme, "nature_island");
  assert.equal(action.params.mountainStyle, "sharp_peaks");
  assert.equal(action.params.mountainCount, 3);
  assert.equal(action.params.mountainWidthMin, 1200);
  assert.equal(action.params.mountainWidthMax, 3800);
});

test("Rule planner parses hills style and applies bounded area from hill position", () => {
  const request: TaskRequest = {
    prompt: "create a natural island with 2 hills, hill size between 1200 and 2000, at x 4000 y -1500",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "landscape.generate");
  if (action.command !== "landscape.generate") {
    return;
  }

  assert.equal(action.params.theme, "nature_island");
  assert.equal(action.params.mountainStyle, "hills");
  assert.equal(action.params.mountainCount, 2);
  assert.equal(action.params.mountainWidthMin, 1200);
  assert.equal(action.params.mountainWidthMax, 2000);
  assert.equal(action.params.useFullArea, false);
  assert.deepEqual(action.params.center, { x: 4000, y: -1500 });
  assert.deepEqual(action.params.size, { x: 6000, y: 6000 });
});

test("Rule planner parses singular hill with width and height ranges", () => {
  const request: TaskRequest = {
    prompt: "create a nature island with a hill, width 500-1000, height 100-500",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "landscape.generate");
  if (action.command !== "landscape.generate") {
    return;
  }

  assert.equal(action.params.theme, "nature_island");
  assert.equal(action.params.mountainStyle, "hills");
  assert.equal(action.params.mountainCount, 1);
  assert.equal(action.params.mountainWidthMin, 500);
  assert.equal(action.params.mountainWidthMax, 1000);
  assert.equal(action.params.maxHeight, 500);
  assert.equal(action.params.useFullArea, true);
});

test("Validation normalizes context action risk to low", () => {
  const request: TaskRequest = {
    prompt: "show selection info",
    mode: "agent",
    context: {}
  };
  const intent = new IntentLayer().normalize(request);
  const validator = new ValidationLayer();

  const candidate = {
    summary: "selection info",
    steps: ["step"],
    actions: [{ command: "context.getSelection", params: {}, risk: "medium" }],
    goal: { id: "g1", description: "d", priority: "medium" },
    subgoals: [],
    checks: [],
    stopConditions: [{ type: "all_checks_passed" }]
  };

  const validated = validator.validatePlan(intent, candidate);
  assert.equal(validated.plan.actions[0]?.risk, "low");
});

test("Validation normalizes context-only summary text", () => {
  const request: TaskRequest = {
    prompt: "what is selected right now?",
    mode: "agent",
    context: {}
  };
  const intent = new IntentLayer().normalize(request);
  const validator = new ValidationLayer();

  const candidate = {
    summary: "Retrieve current selection context.",
    steps: ["step"],
    actions: [{ command: "context.getSelection", params: {}, risk: "low" }],
    goal: { id: "g1", description: "d", priority: "medium" },
    subgoals: [],
    checks: [],
    stopConditions: [{ type: "all_checks_passed" }]
  };

  const validated = validator.validatePlan(intent, candidate);
  assert.equal(validated.plan.summary, "Collect current selection context.");
});

test("Validation adds internal transaction for multi-action agent plan", () => {
  const request: TaskRequest = {
    prompt: "move and rotate selected actor",
    mode: "agent",
    context: {}
  };
  const intent = new IntentLayer().normalize(request);
  const validator = new ValidationLayer();

  const candidate = {
    summary: "move and rotate",
    steps: ["step"],
    actions: [
      {
        command: "scene.modifyActor",
        params: { target: "selection", deltaLocation: { x: 10, y: 0, z: 0 } },
        risk: "low"
      },
      {
        command: "scene.modifyActor",
        params: { target: "selection", deltaRotation: { pitch: 0, yaw: 15, roll: 0 } },
        risk: "low"
      }
    ],
    goal: { id: "g1", description: "d", priority: "medium" },
    subgoals: [],
    checks: [],
    stopConditions: [{ type: "all_checks_passed" }]
  };

  const validated = validator.validatePlan(intent, candidate);
  assert.equal(validated.plan.actions[0]?.command, "session.beginTransaction");
  assert.equal(validated.plan.actions[validated.plan.actions.length - 1]?.command, "session.commitTransaction");
});

test("Validation does not add internal transaction for multi-action chat plan", () => {
  const request: TaskRequest = {
    prompt: "move and rotate selected actor",
    mode: "chat",
    context: {}
  };
  const intent = new IntentLayer().normalize(request);
  const validator = new ValidationLayer();

  const candidate = {
    summary: "move and rotate",
    steps: ["step"],
    actions: [
      {
        command: "scene.modifyActor",
        params: { target: "selection", deltaLocation: { x: 10, y: 0, z: 0 } },
        risk: "low"
      },
      {
        command: "scene.modifyActor",
        params: { target: "selection", deltaRotation: { pitch: 0, yaw: 15, roll: 0 } },
        risk: "low"
      }
    ],
    goal: { id: "g1", description: "d", priority: "medium" },
    subgoals: [],
    checks: [],
    stopConditions: [{ type: "all_checks_passed" }]
  };

  const validated = validator.validatePlan(intent, candidate);
  assert.equal(validated.plan.actions[0]?.command, "scene.modifyActor");
  assert.equal(validated.plan.actions[validated.plan.actions.length - 1]?.command, "scene.modifyActor");
});

test("Rule planner parses directional light intensity action", () => {
  const request: TaskRequest = {
    prompt: "set directional light intensity to 8.5",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "scene.setDirectionalLightIntensity");
  if (action.command !== "scene.setDirectionalLightIntensity") {
    return;
  }
  assert.equal(action.params.intensity, 8.5);
});

test("Rule planner parses fog density action", () => {
  const request: TaskRequest = {
    prompt: "set fog density to 0.03",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "scene.setFogDensity");
  if (action.command !== "scene.setFogDensity") {
    return;
  }
  assert.equal(action.params.density, 0.03);
});

test("Rule planner parses exposure compensation action", () => {
  const request: TaskRequest = {
    prompt: "set exposure compensation to -1.2",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "scene.setPostProcessExposureCompensation");
  if (action.command !== "scene.setPostProcessExposureCompensation") {
    return;
  }
  assert.equal(action.params.exposureCompensation, -1.2);
});

test("Rule planner parses landscape sculpt action", () => {
  const request: TaskRequest = {
    prompt: "sculpt terrain at x 1200 y -300 size 1800 strength 30% falloff 20%",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "landscape.sculpt");
  if (action.command !== "landscape.sculpt") {
    return;
  }
  assert.equal(action.params.center.x, 1200);
  assert.equal(action.params.center.y, -300);
  assert.equal(action.params.size.x, 1800);
  assert.equal(action.params.size.y, 1800);
  assert.equal(action.params.strength, 0.3);
  assert.equal(action.params.falloff, 0.2);
  assert.equal(action.params.mode, "raise");
});

test("Rule planner parses landscape paint layer action", () => {
  const request: TaskRequest = {
    prompt: "paint layer Grass at x 0 y 0 brush 900 strength 0.6 falloff 0.4",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "landscape.paintLayer");
  if (action.command !== "landscape.paintLayer") {
    return;
  }
  assert.equal(action.params.layerName, "Grass");
  assert.equal(action.params.center.x, 0);
  assert.equal(action.params.center.y, 0);
  assert.equal(action.params.size.x, 900);
  assert.equal(action.params.size.y, 900);
  assert.equal(action.params.strength, 0.6);
  assert.equal(action.params.falloff, 0.4);
  assert.equal(action.params.mode, "add");
});

test("Rule planner parses mixed sculpt+paint prompt with separate values", () => {
  const request: TaskRequest = {
    prompt: "sculpt terrain at x 1200 y -300 size 1800 strength 30% and paint layer Grass at x 0 y 0 brush 900 strength 0.6",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 2);

  const sculpt = plan.actions.find((action) => action.command === "landscape.sculpt");
  assert.ok(sculpt);
  if (!sculpt || sculpt.command !== "landscape.sculpt") {
    return;
  }
  assert.equal(sculpt.params.center.x, 1200);
  assert.equal(sculpt.params.center.y, -300);
  assert.equal(sculpt.params.size.x, 1800);
  assert.equal(sculpt.params.size.y, 1800);
  assert.equal(sculpt.params.strength, 0.3);

  const paint = plan.actions.find((action) => action.command === "landscape.paintLayer");
  assert.ok(paint);
  if (!paint || paint.command !== "landscape.paintLayer") {
    return;
  }
  assert.equal(paint.params.layerName, "Grass");
  assert.equal(paint.params.center.x, 0);
  assert.equal(paint.params.center.y, 0);
  assert.equal(paint.params.size.x, 900);
  assert.equal(paint.params.size.y, 900);
  assert.equal(paint.params.strength, 0.6);
});

test("Rule planner builds pcg.createGraph for PCG graph creation prompt", () => {
  const request: TaskRequest = {
    prompt: "create a pcg graph named ForestScatter",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "pcg.createGraph");
  if (action.command !== "pcg.createGraph") {
    return;
  }
  assert.equal(action.params.assetPath, "/Game/PCG/ForestScatter");
  assert.equal(action.params.overwrite, false);
});

test("Rule planner builds pcg.createGraph with template path", () => {
  const request: TaskRequest = {
    prompt: "create a pcg graph named GrassRuntime from template TPL_Showcase_RuntimeGrassGPU",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "pcg.createGraph");
  if (action.command !== "pcg.createGraph") {
    return;
  }
  assert.equal(action.params.assetPath, "/Game/PCG/GrassRuntime");
  assert.equal(action.params.templatePath, "TPL_Showcase_RuntimeGrassGPU");
  assert.equal(action.params.overwrite, false);
});

test("Rule planner keeps destination graph when template has full /Game path", () => {
  const request: TaskRequest = {
    prompt:
      "create a pcg graph named GrassRuntime from template /Game/PCG/Templates/TPL_Showcase_RuntimeGrassGPU.TPL_Showcase_RuntimeGrassGPU",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "pcg.createGraph");
  if (action.command !== "pcg.createGraph") {
    return;
  }
  assert.equal(action.params.assetPath, "/Game/PCG/GrassRuntime");
  assert.equal(
    action.params.templatePath,
    "/Game/PCG/Templates/TPL_Showcase_RuntimeGrassGPU.TPL_Showcase_RuntimeGrassGPU"
  );
});

test("Rule planner infers runtime grass gpu template from short prompt", () => {
  const request: TaskRequest = {
    prompt: "create a new pcg runtime grass gpu",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "pcg.createGraph");
  if (action.command !== "pcg.createGraph") {
    return;
  }
  assert.equal(action.params.assetPath, "/Game/PCG/RuntimeGrassGPU");
  assert.equal(
    action.params.templatePath,
    "/PCG/GraphTemplates/TPL_Showcase_RuntimeGrassGPU.TPL_Showcase_RuntimeGrassGPU"
  );
});

test("Rule planner infers runtime grass template from 'grass form template' prompt", () => {
  const request: TaskRequest = {
    prompt: "create a new pcg grass form template",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "pcg.createGraph");
  if (action.command !== "pcg.createGraph") {
    return;
  }
  assert.equal(
    action.params.templatePath,
    "/PCG/GraphTemplates/TPL_Showcase_RuntimeGrassGPU.TPL_Showcase_RuntimeGrassGPU"
  );
});

test("Rule planner infers runtime grass template from built-in template phrasing", () => {
  const request: TaskRequest = {
    prompt: "create a new pcg grass using built-in template",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "pcg.createGraph");
  if (action.command !== "pcg.createGraph") {
    return;
  }
  assert.equal(
    action.params.templatePath,
    "/PCG/GraphTemplates/TPL_Showcase_RuntimeGrassGPU.TPL_Showcase_RuntimeGrassGPU"
  );
});

test("Rule planner builds pcg.setKeyParameters for PCG parameter prompt", () => {
  const request: TaskRequest = {
    prompt:
      "set pcg graph /Game/PCG/ForestScatter key parameters: points per square meter 0.35 looseness 0.4 offset min x -80 y -80 z 0 offset max x 80 y 80 z 20",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "pcg.setKeyParameters");
  if (action.command !== "pcg.setKeyParameters") {
    return;
  }
  assert.equal(action.params.graphPath, "/Game/PCG/ForestScatter");
  assert.equal(action.params.surfacePointsPerSquaredMeter, 0.35);
  assert.equal(action.params.surfaceLooseness, 0.4);
  assert.deepEqual(action.params.transformOffsetMin, { x: -80, y: -80, z: 0 });
  assert.deepEqual(action.params.transformOffsetMax, { x: 80, y: 80, z: 20 });
});

test("Rule planner builds pcg.placeOnLandscape for selected graph with full landscape", () => {
  const request: TaskRequest = {
    prompt: "put selected pcg on full landscape size",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "pcg.placeOnLandscape");
  if (action.command !== "pcg.placeOnLandscape") {
    return;
  }
  assert.equal(action.params.graphSource, "selected");
  assert.equal(action.params.placementMode, "full");
  assert.equal(action.params.target, "selection");
});

test("Rule planner builds pcg.placeOnLandscape for last graph in landscape center", () => {
  const request: TaskRequest = {
    prompt: "put the newly created pcg on the landscape center with size 3000",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "pcg.placeOnLandscape");
  if (action.command !== "pcg.placeOnLandscape") {
    return;
  }
  assert.equal(action.params.graphSource, "last");
  assert.equal(action.params.placementMode, "center");
  assert.deepEqual(action.params.size, { x: 3000, y: 3000 });
});

test("Rule planner treats 'put it on the landscape' as referential PCG placement", () => {
  const request: TaskRequest = {
    prompt: "put it on the landscape",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "pcg.placeOnLandscape");
  if (action.command !== "pcg.placeOnLandscape") {
    return;
  }
  assert.equal(action.params.graphSource, "last");
  assert.equal(action.params.placementMode, "center");
});
