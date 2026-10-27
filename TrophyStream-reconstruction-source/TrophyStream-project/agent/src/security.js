import { spawn } from 'node:child_process';
import { mkdir, readFile, writeFile, chmod } from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';

const appDir = path.join(process.env.APPDATA || path.join(os.homedir(), 'AppData', 'Roaming'), 'TrophyStream');
const secretFile = path.join(appDir, 'npsso.dpapi');

function powershell(script, input = '') {
  return new Promise((resolve, reject) => {
    const child = spawn('powershell.exe', ['-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-Command', script], { windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'] });
    let stdout = '', stderr = '';
    child.stdout.setEncoding('utf8'); child.stderr.setEncoding('utf8');
    child.stdout.on('data', d => stdout += d); child.stderr.on('data', d => stderr += d);
    child.on('error', reject);
    child.on('close', code => code === 0 ? resolve(stdout.trim()) : reject(new Error(stderr.trim() || `PowerShell exited ${code}`)));
    child.stdin.end(input, 'utf8');
  });
}

export async function saveNpssoLocally(npsso) {
  if (!/^[A-Za-z0-9_-]{30,200}$/.test(npsso)) throw new Error('Le NPSSO ne semble pas valide. Vérifie la valeur copiée depuis Sony.');
  await mkdir(appDir, { recursive: true });
  // The secret is sent over stdin, never embedded in the PowerShell command line.
  const encrypted = await powershell(
    "$s=[Console]::In.ReadToEnd(); $sec=ConvertTo-SecureString $s -AsPlainText -Force; ConvertFrom-SecureString $sec",
    npsso
  );
  await writeFile(secretFile, encrypted + '\n', { encoding: 'utf8', mode: 0o600 });
  return true;
}

export async function loadNpssoLocally() {
  let encrypted;
  try { encrypted = (await readFile(secretFile, 'utf8')).trim(); } catch { return null; }
  if (!encrypted) return null;
  // ConvertFrom-SecureString uses Windows DPAPI user scope by default.
  return powershell(
    "$e=[Console]::In.ReadToEnd().Trim(); $sec=ConvertTo-SecureString $e; $b=[Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec); try {[Runtime.InteropServices.Marshal]::PtrToStringBSTR($b)} finally {[Runtime.InteropServices.Marshal]::ZeroFreeBSTR($b)}",
    encrypted
  );
}

export async function hasNpsso() {
  try { return Boolean((await readFile(secretFile, 'utf8')).trim()); } catch { return false; }
}
