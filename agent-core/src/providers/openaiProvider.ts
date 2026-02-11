import type { ProviderRuntimeConfig } from "../config.js";
import type { PlanOutput } from "../contracts.js";
import { buildPlanPrompt, parsePlanOutput } from "../planner/planJson.js";
import type { LlmProvider, PlanInput, TextReplyInput } from "./types.js";

function formatErrorBody(body: string): string {
  const max = 4000;
  if (body.length <= max) {
    return body;
  }
  return `${body.slice(0, max)}... [truncated ${body.length - max} chars]`;
}

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
        max_completion_tokens: this.runtime.maxTokens,
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
      throw new Error(`OpenAI request failed (${response.status}): ${formatErrorBody(body)}`);
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

  private async requestTextReply(input: TextReplyInput): Promise<string> {
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
        max_completion_tokens: this.runtime.maxTokens,
        messages: [
          {
            role: "system",
            content:
              "You are an Unreal Engine assistant. Reply in plain text. Keep answers clear, helpful, and safe. Use enough detail to answer well. Use markdown only for headings, bullet lists, bold, italic, inline code, and fenced code blocks. Do not use markdown tables. Do not output JSON."
          },
          {
            role: "user",
            content: input.prompt
          }
        ]
      })
    });

    if (!response.ok) {
      const body = await response.text();
      throw new Error(`OpenAI request failed (${response.status}): ${formatErrorBody(body)}`);
    }

    const data = (await response.json()) as {
      choices?: Array<{ message?: { content?: string } }>;
    };
    const content = data.choices?.[0]?.message?.content;
    if (!content || typeof content !== "string") {
      throw new Error("OpenAI response did not include a text reply.");
    }

    return content.trim();
  }

  async planTask(input: PlanInput): Promise<PlanOutput> {
    return this.requestPlan(input);
  }

  async respondText(input: TextReplyInput): Promise<string> {
    return this.requestTextReply(input);
  }
}
