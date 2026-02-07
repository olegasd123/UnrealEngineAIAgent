import type {
  SessionApproveRequest,
  SessionNextRequest,
  SessionResumeRequest,
  SessionStartRequest,
  TaskRequest
} from "../contracts.js";
import { ExecutionLayer } from "../executor/executionLayer.js";
import { IntentLayer } from "../intent/intentLayer.js";
import { PlanningLayer } from "../planner/planningLayer.js";
import type { LlmProvider } from "../providers/types.js";
import type { SessionDecision } from "../sessions/sessionStore.js";
import { ValidationLayer } from "../validator/validationLayer.js";

export class AgentService {
  constructor(
    private readonly intentLayer: IntentLayer,
    private readonly planningLayer: PlanningLayer,
    private readonly validationLayer: ValidationLayer,
    private readonly executionLayer: ExecutionLayer
  ) {}

  async planTask(input: TaskRequest, provider: LlmProvider) {
    const intent = this.intentLayer.normalize(input);
    const rawPlan = await this.planningLayer.buildPlan(intent, provider);
    return this.validationLayer.validatePlan(intent, rawPlan);
  }

  async startSession(input: SessionStartRequest, provider: LlmProvider): Promise<SessionDecision> {
    const { plan } = await this.planTask(input, provider);
    return this.executionLayer.startSession(input, plan);
  }

  next(input: SessionNextRequest): SessionDecision {
    return this.executionLayer.next(input);
  }

  approve(input: SessionApproveRequest): SessionDecision {
    return this.executionLayer.approve(input);
  }

  resume(input: SessionResumeRequest): SessionDecision {
    return this.executionLayer.resume(input);
  }
}
