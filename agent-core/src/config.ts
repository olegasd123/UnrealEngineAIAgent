import { z } from "zod";

const ProviderSchema = z.enum(["openai", "gemini"]);

const PortSchema = z.preprocess(
  (value) => {
    if (value === undefined || value === null || value === "") {
      return 4317;
    }
    return Number(value);
  },
  z.number().int().min(1).max(65535)
);

const TemperatureSchema = z.preprocess(
  (value) => {
    if (value === undefined || value === null || value === "") {
      return 0.2;
    }
    return Number(value);
  },
  z.number().min(0).max(2)
);

const MaxTokensSchema = z.preprocess(
  (value) => {
    if (value === undefined || value === null || value === "") {
      return 1200;
    }
    return Number(value);
  },
  z.number().int().min(1)
);

const RawEnvSchema = z.object({
  AGENT_HOST: z.string().trim().min(1).default("127.0.0.1"),
  AGENT_PORT: PortSchema,
  AGENT_PROVIDER: ProviderSchema.default("openai"),
  AGENT_TASK_LOG_PATH: z.string().trim().min(1).default("data/task-log.jsonl"),

  OPENAI_API_KEY: z.string().trim().optional(),
  OPENAI_MODEL: z.string().trim().min(1).default("gpt-4.1-mini"),
  OPENAI_BASE_URL: z.string().trim().optional(),
  OPENAI_TEMPERATURE: TemperatureSchema,
  OPENAI_MAX_TOKENS: MaxTokensSchema,

  GEMINI_API_KEY: z.string().trim().optional(),
  GEMINI_MODEL: z.string().trim().min(1).default("gemini-2.0-flash"),
  GEMINI_BASE_URL: z.string().trim().optional(),
  GEMINI_TEMPERATURE: TemperatureSchema,
  GEMINI_MAX_TOKENS: MaxTokensSchema
});

function optionalNonEmpty(value: string | undefined): string | undefined {
  if (!value) {
    return undefined;
  }
  const trimmed = value.trim();
  return trimmed.length > 0 ? trimmed : undefined;
}

const env = RawEnvSchema.parse(process.env);

export type ProviderName = z.infer<typeof ProviderSchema>;

export interface ProviderRuntimeConfig {
  apiKey?: string;
  model: string;
  baseUrl?: string;
  temperature: number;
  maxTokens: number;
}

export const config = {
  host: env.AGENT_HOST,
  port: env.AGENT_PORT,
  provider: env.AGENT_PROVIDER as ProviderName,
  taskLogPath: env.AGENT_TASK_LOG_PATH,
  providers: {
    openai: {
      apiKey: optionalNonEmpty(env.OPENAI_API_KEY),
      model: env.OPENAI_MODEL,
      baseUrl: optionalNonEmpty(env.OPENAI_BASE_URL),
      temperature: env.OPENAI_TEMPERATURE,
      maxTokens: env.OPENAI_MAX_TOKENS
    },
    gemini: {
      apiKey: optionalNonEmpty(env.GEMINI_API_KEY),
      model: env.GEMINI_MODEL,
      baseUrl: optionalNonEmpty(env.GEMINI_BASE_URL),
      temperature: env.GEMINI_TEMPERATURE,
      maxTokens: env.GEMINI_MAX_TOKENS
    }
  }
};
