import { dirname, resolve } from "node:path";

function pad2(value: number): string {
  return value.toString().padStart(2, "0");
}

function formatDateForFile(date: Date): string {
  return `${date.getFullYear()}${pad2(date.getMonth() + 1)}${pad2(date.getDate())}`;
}

export function resolveLogDirectory(pathOrDirectory: string): string {
  const absolute = resolve(process.cwd(), pathOrDirectory);
  if (absolute.toLowerCase().endsWith(".jsonl")) {
    return dirname(absolute);
  }
  return absolute;
}

export function buildDailyLogPath(
  logDirectory: string,
  kind: "task" | "session",
  date = new Date()
): string {
  return resolve(logDirectory, `${formatDateForFile(date)}-${kind}-log.jsonl`);
}

