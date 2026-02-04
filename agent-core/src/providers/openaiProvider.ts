import type { ProviderRuntimeConfig } from "../config.js";
import type { PlanOutput } from "../contracts.js";
import { buildPlanPrompt, parsePlanOutput } from "./planJson.js";
import { buildRuleBasedPlan } from "./ruleBasedPlanner.js";
import type { LlmProvider, PlanInput } from "./types.js";

export class OpenAiProvider implements LlmProvider {
  public readonly name = "openai";
  public readonly model: string;
  public readonly hasApiKey: boolean;
  public readonly adapter: "stub" | "live";

  constructor(private readonly runtime: ProviderRuntimeConfig) {
    this.model = runtime.model;
    this.hasApiKey = Boolean(runtime.apiKey);
    this.adapter = this.hasApiKey ? "live" : "stub";
  }

  private getBaseUrl(): string {
    return this.runtime.baseUrl?.replace(/\/+$/, "") ?? "https://api.openai.com/v1";
  }

  private async requestPlan(input: PlanInput): Promise<PlanOutput> {
    if (!this.runtime.apiKey) {
      throw new Error("OPENAI_API_KEY is missing.");
    }

    const response = await fetch(`${this.getBaseUrl()}/chat/completions`, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        Authorization: `Bearer ${this.runtime.apiKey}`
      },
      body: JSON.stringify({
        model: this.runtime.model,
        temperature: this.runtime.temperature,
        max_tokens: this.runtime.maxTokens,
        response_format: { type: "json_object" },
        messages: [
          {
            role: "system",
            content: "You are a planning assistant for Unreal Editor actions."
          },
          {
            role: "user",
            content: buildPlanPrompt(input)
          }
        ]
      })
    });

    if (!response.ok) {
      const body = await response.text();
      throw new Error(`OpenAI request failed (${response.status}): ${body.slice(0, 250)}`);
    }

    const data = (await response.json()) as {
      choices?: Array<{ message?: { content?: string } }>;
    };
    const content = data.choices?.[0]?.message?.content;
    if (!content || typeof content !== "string") {
      throw new Error("OpenAI response did not include a JSON plan.");
    }

    return parsePlanOutput(content);
  }

  async planTask(input: PlanInput): Promise<PlanOutput> {
    if (!this.hasApiKey) {
      const basePlan = buildRuleBasedPlan(input);
      return {
        ...basePlan,
        steps: ["OPENAI_API_KEY is missing. Using local parser fallback.", ...basePlan.steps]
      };
    }

    try {
      return await this.requestPlan(input);
    } catch (error) {
      const reason = error instanceof Error ? error.message : "Unknown OpenAI error";
      const fallback = buildRuleBasedPlan(input);
      return {
        ...fallback,
        steps: [`OpenAI call failed. Using local fallback. Reason: ${reason}`, ...fallback.steps]
      };
    }
  }
}
