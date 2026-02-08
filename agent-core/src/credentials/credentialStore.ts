import { execFile } from "node:child_process";
import { mkdir, readFile, rm, writeFile } from "node:fs/promises";
import { homedir } from "node:os";
import { dirname, join } from "node:path";
import { promisify } from "node:util";

import type { ProviderName } from "../config.js";

const execFileAsync = promisify(execFile);

function toProviderKey(provider: ProviderName): string {
  return provider;
}

function getServiceName(provider: ProviderName): string {
  return `ue-ai-agent/${toProviderKey(provider)}`;
}

function getFallbackSecretFile(provider: ProviderName): string {
  return join(homedir(), ".ueaiagent", "secrets", `${toProviderKey(provider)}.txt`);
}

function getWindowsSecretFile(provider: ProviderName): string {
  const base = process.env.APPDATA?.trim() || join(homedir(), "AppData", "Roaming");
  return join(base, "UEAIAgent", "secrets", `${toProviderKey(provider)}.txt`);
}

async function setFileSecret(filePath: string, value: string): Promise<void> {
  await mkdir(dirname(filePath), { recursive: true });
  await writeFile(filePath, `${value}\n`, { encoding: "utf8", mode: 0o600 });
}

async function getFileSecret(filePath: string): Promise<string | undefined> {
  try {
    const content = await readFile(filePath, "utf8");
    const trimmed = content.trim();
    return trimmed.length > 0 ? trimmed : undefined;
  } catch (error) {
    const code = (error as NodeJS.ErrnoException).code;
    if (code === "ENOENT") {
      return undefined;
    }
    throw error;
  }
}

async function deleteFileSecret(filePath: string): Promise<void> {
  try {
    await rm(filePath, { force: true });
  } catch (error) {
    const code = (error as NodeJS.ErrnoException).code;
    if (code !== "ENOENT") {
      throw error;
    }
  }
}

export class CredentialStore {
  public async set(provider: ProviderName, apiKey: string): Promise<void> {
    const value = apiKey.trim();
    if (!value) {
      throw new Error("API key is empty.");
    }

    if (process.platform === "darwin") {
      await this.setMacos(provider, value);
      return;
    }

    if (process.platform === "win32") {
      await this.setWindows(provider, value);
      return;
    }

    await setFileSecret(getFallbackSecretFile(provider), value);
  }

  public async get(provider: ProviderName): Promise<string | undefined> {
    if (process.platform === "darwin") {
      return this.getMacos(provider);
    }

    if (process.platform === "win32") {
      return this.getWindows(provider);
    }

    return getFileSecret(getFallbackSecretFile(provider));
  }

  public async delete(provider: ProviderName): Promise<void> {
    if (process.platform === "darwin") {
      await this.deleteMacos(provider);
      return;
    }

    if (process.platform === "win32") {
      await this.deleteWindows(provider);
      return;
    }

    await deleteFileSecret(getFallbackSecretFile(provider));
  }

  private async setMacos(provider: ProviderName, apiKey: string): Promise<void> {
    const service = getServiceName(provider);
    const account = process.env.USER || "default";
    await execFileAsync("security", [
      "add-generic-password",
      "-a",
      account,
      "-s",
      service,
      "-w",
      apiKey,
      "-U"
    ]);
  }

  private async getMacos(provider: ProviderName): Promise<string | undefined> {
    const service = getServiceName(provider);
    try {
      const { stdout } = await execFileAsync("security", [
        "find-generic-password",
        "-s",
        service,
        "-w"
      ]);
      const value = stdout.trim();
      return value.length > 0 ? value : undefined;
    } catch (error) {
      const stderr = (error as { stderr?: string }).stderr ?? "";
      if (/could not be found/i.test(stderr)) {
        return undefined;
      }
      throw new Error(`Could not read key from macOS Keychain: ${stderr || "Unknown error"}`);
    }
  }

  private async deleteMacos(provider: ProviderName): Promise<void> {
    const service = getServiceName(provider);
    try {
      await execFileAsync("security", ["delete-generic-password", "-s", service]);
    } catch (error) {
      const stderr = (error as { stderr?: string }).stderr ?? "";
      if (/could not be found/i.test(stderr)) {
        return;
      }
      throw new Error(`Could not delete key from macOS Keychain: ${stderr || "Unknown error"}`);
    }
  }

  private async setWindows(provider: ProviderName, apiKey: string): Promise<void> {
    const filePath = getWindowsSecretFile(provider);
    await mkdir(dirname(filePath), { recursive: true });
    const escapedKey = apiKey.replace(/'/g, "''");
    const escapedPath = filePath.replace(/'/g, "''");
    const script = [
      `$secure = ConvertTo-SecureString '${escapedKey}' -AsPlainText -Force`,
      "$encrypted = ConvertFrom-SecureString $secure",
      `Set-Content -Path '${escapedPath}' -Value $encrypted -NoNewline`
    ].join("; ");
    await execFileAsync("powershell", ["-NoProfile", "-NonInteractive", "-Command", script]);
  }

  private async getWindows(provider: ProviderName): Promise<string | undefined> {
    const filePath = getWindowsSecretFile(provider);
    const escapedPath = filePath.replace(/'/g, "''");
    const script = [
      `if (!(Test-Path '${escapedPath}')) { return }`,
      `$encrypted = Get-Content -Path '${escapedPath}' -Raw`,
      "if ([string]::IsNullOrWhiteSpace($encrypted)) { return }",
      "$secure = ConvertTo-SecureString $encrypted",
      "$bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)",
      "$plain = [Runtime.InteropServices.Marshal]::PtrToStringAuto($bstr)",
      "Write-Output $plain"
    ].join("; ");

    const { stdout } = await execFileAsync("powershell", [
      "-NoProfile",
      "-NonInteractive",
      "-Command",
      script
    ]);
    const value = stdout.trim();
    return value.length > 0 ? value : undefined;
  }

  private async deleteWindows(provider: ProviderName): Promise<void> {
    await deleteFileSecret(getWindowsSecretFile(provider));
  }
}
