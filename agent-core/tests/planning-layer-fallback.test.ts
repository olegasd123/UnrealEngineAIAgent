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

const contextOnlyPlan: PlanOutput = {
  summary: "Collect current scene summary context.",
  steps: ["Retrieve scene summary"],
  actions: [{ command: "context.getSceneSummary", params: {}, risk: "low" }],
  goal: {
    id: "goal_context_scene",
    description: "Collect scene context.",
    priority: "low"
  },
  subgoals: [],
  checks: [],
  stopConditions: [{ type: "all_checks_passed" }, { type: "max_iterations", value: 1 }, { type: "user_denied" }]
};

class ContextOnlyProvider implements LlmProvider {
  public readonly name = "local";
  public readonly model = "test-model";
  public readonly hasApiKey = true;
  public readonly adapter = "stub" as const;

  async planTask(_input: PlanInput): Promise<PlanOutput> {
    return contextOnlyPlan;
  }

  async respondText(_input: TextReplyInput): Promise<string> {
    return "ok";
  }
}

const landscapeGeneratePlanMissingConstraints: PlanOutput = {
  summary: "Generate nature island.",
  steps: ["Generate landscape island"],
  actions: [
    {
      command: "landscape.generate",
      params: {
        target: "all",
        theme: "nature_island",
        detailLevel: "medium",
        useFullArea: true,
        mountainStyle: "hills"
      },
      risk: "medium"
    }
  ],
  goal: {
    id: "goal_landscape_island",
    description: "Generate nature island.",
    priority: "medium"
  },
  subgoals: [],
  checks: [],
  stopConditions: [{ type: "all_checks_passed" }, { type: "max_iterations", value: 1 }, { type: "user_denied" }]
};

class LandscapeGenerateMissingConstraintsProvider implements LlmProvider {
  public readonly name = "local";
  public readonly model = "test-model";
  public readonly hasApiKey = true;
  public readonly adapter = "stub" as const;

  async planTask(_input: PlanInput): Promise<PlanOutput> {
    return landscapeGeneratePlanMissingConstraints;
  }

  async respondText(_input: TextReplyInput): Promise<string> {
    return "ok";
  }
}

const pcgCreateGraphMissingTemplatePlan: PlanOutput = {
  summary: "Create runtime grass PCG graph.",
  steps: ["Create PCG graph"],
  actions: [
    {
      command: "pcg.createGraph",
      params: {
        assetPath: "/Game/PCG/RuntimeGrassGPU",
        overwrite: false
      },
      risk: "medium"
    }
  ],
  goal: {
    id: "goal_pcg_runtime_grass",
    description: "Create runtime grass PCG graph.",
    priority: "medium"
  },
  subgoals: [],
  checks: [],
  stopConditions: [{ type: "all_checks_passed" }, { type: "max_iterations", value: 1 }, { type: "user_denied" }]
};

class PcgCreateGraphMissingTemplateProvider implements LlmProvider {
  public readonly name = "local";
  public readonly model = "test-model";
  public readonly hasApiKey = true;
  public readonly adapter = "stub" as const;

  async planTask(_input: PlanInput): Promise<PlanOutput> {
    return pcgCreateGraphMissingTemplatePlan;
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

test("PlanningLayer uses local fallback when provider returns context-only plan for write intent", async () => {
  const provider = new ContextOnlyProvider();
  const planning = new PlanningLayer();
  const intent = new IntentLayer().normalize(makeRequest("Generate a moon surface across the full landscape"));

  const plan = await planning.buildPlan(intent, provider);
  assert.equal(plan.actions.length, 1);
  assert.equal(plan.actions[0]?.command, "landscape.generate");
  assert.match(plan.steps[0] ?? "", /context-only actions.*local fallback/i);
});

test("PlanningLayer enriches provider landscape.generate with parsed width and height ranges", async () => {
  const provider = new LandscapeGenerateMissingConstraintsProvider();
  const planning = new PlanningLayer();
  const intent = new IntentLayer().normalize(
    makeRequest("create a nature island with a hill, width 500-1000, height 100-500")
  );

  const plan = await planning.buildPlan(intent, provider);
  assert.equal(plan.actions.length, 1);
  assert.equal(plan.actions[0]?.command, "landscape.generate");
  if (plan.actions[0]?.command !== "landscape.generate") {
    return;
  }

  assert.equal(plan.actions[0].params.mountainStyle, "hills");
  assert.equal(plan.actions[0].params.mountainCount, 1);
  assert.equal(plan.actions[0].params.mountainWidthMin, 500);
  assert.equal(plan.actions[0].params.mountainWidthMax, 1000);
  assert.equal(plan.actions[0].params.maxHeight, 500);
  assert.match(plan.steps[0] ?? "", /Filled missing landscape\.generate constraints/i);
});

test("PlanningLayer enriches provider pcg.createGraph with inferred runtime grass template", async () => {
  const provider = new PcgCreateGraphMissingTemplateProvider();
  const planning = new PlanningLayer();
  const intent = new IntentLayer().normalize(
    makeRequest("create a new pcg runtime grass gpu form template")
  );

  const plan = await planning.buildPlan(intent, provider);
  assert.equal(plan.actions.length, 1);
  assert.equal(plan.actions[0]?.command, "pcg.createGraph");
  if (plan.actions[0]?.command !== "pcg.createGraph") {
    return;
  }

  assert.equal(plan.actions[0].params.assetPath, "/Game/PCG/RuntimeGrassGPU");
  assert.equal(
    plan.actions[0].params.templatePath,
    "/PCG/GraphTemplates/TPL_Showcase_RuntimeGrassGPU.TPL_Showcase_RuntimeGrassGPU"
  );
  assert.match(plan.steps[0] ?? "", /Filled missing pcg\.createGraph template/i);
});

test("PlanningLayer enriches provider pcg.createGraph for 'grass form template' wording", async () => {
  const provider: LlmProvider = {
    name: "local",
    model: "test-model",
    hasApiKey: true,
    adapter: "stub",
    async planTask(): Promise<PlanOutput> {
      return {
        summary: "Create a new PCG grass form template.",
        steps: ["Prepare PCG graph creation", "Execute create graph action"],
        actions: [
          {
            command: "pcg.createGraph",
            params: {
              assetPath: "/Game/PCG/GrassFormTemplate",
              overwrite: false
            },
            risk: "medium"
          }
        ],
        goal: {
          id: "goal_pcg_grass_template",
          description: "Create grass template graph.",
          priority: "medium"
        },
        subgoals: [],
        checks: [],
        stopConditions: [{ type: "all_checks_passed" }, { type: "max_iterations", value: 1 }, { type: "user_denied" }]
      };
    },
    async respondText(): Promise<string> {
      return "ok";
    }
  };

  const planning = new PlanningLayer();
  const intent = new IntentLayer().normalize(makeRequest("create a new pcg grass form template"));
  const plan = await planning.buildPlan(intent, provider);

  assert.equal(plan.actions.length, 1);
  assert.equal(plan.actions[0]?.command, "pcg.createGraph");
  if (plan.actions[0]?.command !== "pcg.createGraph") {
    return;
  }

  assert.equal(plan.actions[0].params.assetPath, "/Game/PCG/GrassFormTemplate");
  assert.equal(
    plan.actions[0].params.templatePath,
    "/PCG/GraphTemplates/TPL_Showcase_RuntimeGrassGPU.TPL_Showcase_RuntimeGrassGPU"
  );
  assert.match(plan.steps[0] ?? "", /Filled missing pcg\.createGraph template/i);
});
