import 'dotenv/config';
import express from 'express';
import { WebSocketServer } from 'ws';
import { Client, GatewayIntentBits, REST, Routes, SlashCommandBuilder, PermissionFlagsBits } from 'discord.js';
import { spawn } from 'node:child_process';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import { fileURLToPath } from 'node:url';
import { saveNpssoLocally, hasNpsso } from './security.js';
import { getLatestGameAndTrophies } from './psn.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const appDir = path.join(process.env.APPDATA || path.join(os.homedir(), 'AppData', 'Roaming'), 'TrophyStream');
const settingsFile = path.join(appDir, 'settings.json');
const PORT = 3020;
const WS_PORT = 3002;
const pollMs = 60_000;
let settings = { psnId: '', running: false };
let currentGame = null;
let previouslyEarned = new Set();
let pollTimer = null;
let obsProcess = null;
let lastError = '';
const peers = new Set();

await mkdir(appDir, { recursive: true });
try { settings = { ...settings, ...JSON.parse(await readFile(settingsFile, 'utf8')) }; } catch {}
async function saveSettings() { await writeFile(settingsFile, JSON.stringify(settings, null, 2), 'utf8'); }

function sendAll(payload) {
  const text = JSON.stringify(payload);
  for (const peer of peers) if (peer.readyState === 1) peer.send(text);
}
function escapeHtml(s='') { return String(s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c])); }

async function refreshFromPsn() {
  try {
    const game = await getLatestGameAndTrophies();
    const oldGameId = currentGame?.id;
    const wasInitialized = currentGame !== null;
    currentGame = game;
    lastError = '';
    const earnedNow = new Set(game.trophies.filter(t => t.earned).map(t => t.id));
    if (wasInitialized && oldGameId === game.id) {
      for (const t of game.trophies) {
        if (t.earned && t.id && !previouslyEarned.has(t.id)) {
          sendAll({ type: 'trophy', unlocked: game.unlocked, total: game.total, percent: game.percent,
            trophyName: t.name, trophyDetail: t.detail, trophy_type: t.type,
            trophyIconUrl: t.icon, duration: 10000 });
        }
      }
    }
    if (oldGameId !== game.id) {
      sendAll({ type: 'game-change', titleName: game.title, coverUrl: game.cover,
        npCommunicationId: game.id, npServiceName: game.serviceName || '' });
    }
    previouslyEarned = earnedNow;
    const remaining = game.trophies.filter(t => !t.earned).slice(0, 80).map(t => ({
      trophyName: t.name, trophyDetail: t.detail, trophy_type: t.type, trophyIconUrl: t.icon
    }));
    sendAll({ type: 'state', titleName: game.title, coverUrl: game.cover,
      npCommunicationId: game.id, npServiceName: game.serviceName || '',
      unlocked: game.unlocked, total: game.total, percent: game.percent, remaining });
    return game;
  } catch (err) {
    lastError = err?.message || String(err);
    console.error('[PSN]', lastError);
    throw err;
  }
}

function startPolling() {
  if (pollTimer) return;
  settings.running = true;
  sendAll({ type: 'connected' });
  refreshFromPsn().catch(() => {});
  pollTimer = setInterval(() => refreshFromPsn().catch(() => {}), pollMs);
  saveSettings();
}
function stopPolling() {
  settings.running = false;
  if (pollTimer) clearInterval(pollTimer);
  pollTimer = null;
  sendAll({ type: 'trophystream-stopped' });
  saveSettings();
}
function startObs() {
  if (obsProcess && !obsProcess.killed) return { ok: true, message: 'OBS semble déjà lancé.' };
  const obsExe = process.env.OBS_EXE || 'C:\\Program Files\\obs-studio\\bin\\64bit\\obs64.exe';
  try {
    obsProcess = spawn(obsExe, [], { detached: true, stdio: 'ignore', windowsHide: false });
    obsProcess.unref();
    return { ok: true, message: 'Commande de lancement OBS envoyée.' };
  } catch (e) {
    return { ok: false, message: `Impossible de lancer OBS (${obsExe}) : ${e.message}` };
  }
}

const web = express();
web.disable('x-powered-by');
web.use(express.urlencoded({ extended: false, limit: '8kb' }));
web.get('/', async (_req, res) => {
  const configured = await hasNpsso();
  res.type('html').send(`<!doctype html><html lang="fr"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>TrophyStream — configuration locale</title><style>body{font:16px system-ui;background:#101218;color:#f3f4f6;max-width:700px;margin:48px auto;padding:0 20px}main{background:#1a1d26;border:1px solid #303543;border-radius:16px;padding:28px}label{display:block;margin:18px 0 7px}input{box-sizing:border-box;width:100%;padding:12px;border-radius:8px;border:1px solid #43495a;background:#0f1117;color:white}button{margin-top:20px;padding:12px 16px;background:#dce5ff;color:#101218;border:0;border-radius:8px;font-weight:700;cursor:pointer}.muted{color:#b5bdce;font-size:14px}code{background:#0e1015;padding:3px 6px;border-radius:5px}.ok{color:#8ee2b4}</style><main><h1>TrophyStream</h1><p class="muted">Configuration locale — cette page écoute uniquement sur 127.0.0.1.</p><p class="${configured?'ok':'muted'}">NPSSO : ${configured?'enregistré chiffré avec DPAPI Windows':'pas encore configuré'}</p><form method="post" action="/save"><label for="psnId">PSN Online ID</label><input id="psnId" name="psnId" maxlength="64" value="${escapeHtml(settings.psnId)}" placeholder="MonPSNID"><label for="npsso">NPSSO (saisi uniquement ici)</label><input id="npsso" name="npsso" type="password" autocomplete="off" minlength="30" placeholder="Laisser vide pour conserver le jeton existant"><p class="muted">Le NPSSO est chiffré avec DPAPI pour ton compte Windows. Ne le poste jamais sur Discord et ne partage pas le fichier chiffré avec d’autres personnes.</p><button type="submit">Enregistrer localement</button></form><hr><p>État du suivi : <b>${settings.running?'actif':'arrêté'}</b></p><p class="muted">L’agent lit le jeu PSN le plus récemment mis à jour. Les données PSN ne permettent pas toujours de savoir quel jeu tourne en direct sur la console.</p></main></html>`);
});
web.post('/save', async (req, res) => {
  const psnId = String(req.body.psnId || '').trim().slice(0, 64);
  const npsso = String(req.body.npsso || '').trim();
  if (psnId && !/^[A-Za-z0-9_-]{3,16}$/.test(psnId)) return res.status(400).send('PSN ID invalide. Reviens en arrière et vérifie sa valeur.');
  if (npsso) {
    try { await saveNpssoLocally(npsso); } catch (e) { return res.status(400).send(`Impossible d’enregistrer le NPSSO : ${escapeHtml(e.message)}`); }
  }
  settings.psnId = psnId;
  await saveSettings();
  res.redirect('/?saved=1');
});
web.listen(PORT, '127.0.0.1', () => console.log(`[TrophyStream] Configuration locale : http://127.0.0.1:${PORT}`));

const wss = new WebSocketServer({ host: '127.0.0.1', port: WS_PORT });
wss.on('connection', socket => {
  peers.add(socket);
  socket.on('close', () => peers.delete(socket));
  socket.on('error', () => peers.delete(socket));
  socket.send(JSON.stringify({ type: 'connected' }));
  if (currentGame) socket.send(JSON.stringify({ type: 'state', titleName: currentGame.title, coverUrl: currentGame.cover, npCommunicationId: currentGame.id, unlocked: currentGame.unlocked, total: currentGame.total, percent: currentGame.percent, remaining: currentGame.trophies.filter(t => !t.earned).slice(0,80).map(t => ({ trophyName:t.name,trophyDetail:t.detail,trophy_type:t.type,trophyIconUrl:t.icon })) }));
});
wss.on('listening', () => console.log(`[TrophyStream] WebSocket plugin : ws://127.0.0.1:${WS_PORT}`));
wss.on('error', e => console.error('[WebSocket]', e.message));

const commands = [
  new SlashCommandBuilder().setName('trophystream-start').setDescription('Lance le suivi TrophyStream et tente de démarrer OBS.'),
  new SlashCommandBuilder().setName('trophystream-stop').setDescription('Arrête le suivi et masque l’overlay.'),
  new SlashCommandBuilder().setName('trophystream-status').setDescription('Affiche l’état du plugin et du compte PSN configuré.'),
  new SlashCommandBuilder().setName('trophystream-psn').setDescription('Affiche le lien local pour configurer le PSN ID et le NPSSO.')
].map(c => c.toJSON());

if (process.env.DISCORD_TOKEN && process.env.DISCORD_CLIENT_ID) {
  const rest = new REST({ version: '10' }).setToken(process.env.DISCORD_TOKEN);
  try {
    if (process.env.DISCORD_GUILD_ID) await rest.put(Routes.applicationGuildCommands(process.env.DISCORD_CLIENT_ID, process.env.DISCORD_GUILD_ID), { body: commands });
    else await rest.put(Routes.applicationCommands(process.env.DISCORD_CLIENT_ID), { body: commands });
  } catch (e) { console.error('[Discord] Échec de publication des commandes :', e.message); }
  const client = new Client({ intents: [GatewayIntentBits.Guilds] });
  client.on('interactionCreate', async interaction => {
    if (!interaction.isChatInputCommand()) return;
    const allowed = process.env.DISCORD_ALLOWED_USER_ID;
    if (allowed && interaction.user.id !== allowed) return interaction.reply({ content: 'Commande réservée au propriétaire configuré.', ephemeral: true });
    if (interaction.commandName === 'trophystream-start') {
      const result = startObs(); startPolling();
      return interaction.reply({ content: `${result.message}\nSuivi TrophyStream activé. Si le plugin est déjà dans une scène OBS, l’overlay devrait se connecter.`, ephemeral: true });
    }
    if (interaction.commandName === 'trophystream-stop') { stopPolling(); return interaction.reply({ content: 'Suivi arrêté et overlay masqué.', ephemeral: true }); }
    if (interaction.commandName === 'trophystream-status') {
      return interaction.reply({ content: `Suivi : ${settings.running?'actif':'arrêté'}\nPSN ID : ${settings.psnId || 'non configuré'}\nNPSSO : ${await hasNpsso()?'enregistré localement':'manquant'}\nJeu PSN le plus récemment mis à jour : ${currentGame?.title || 'inconnu'}\nErreur : ${lastError || 'aucune'}`, ephemeral: true });
    }
    if (interaction.commandName === 'trophystream-psn') return interaction.reply({ content: 'Ouvre cette adresse sur le PC où tourne OBS : http://127.0.0.1:3020/ — le NPSSO ne doit pas être envoyé sur Discord.', ephemeral: true });
  });
  client.once('ready', () => console.log(`[Discord] Connecté comme ${client.user.tag}`));
  client.login(process.env.DISCORD_TOKEN).catch(e => console.error('[Discord] Connexion échouée :', e.message));
} else {
  console.warn('[Discord] Bot non configuré : copie .env.example vers .env et renseigne les identifiants Discord.');
}

if (settings.running) startPolling();
process.on('SIGINT', () => { stopPolling(); wss.close(); process.exit(0); });
