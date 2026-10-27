import {
  exchangeNpssoForAccessCode,
  exchangeAccessCodeForAuthTokens,
  exchangeRefreshTokenForAuthTokens,
  getUserTitles,
  getTitleTrophies,
  getUserTrophiesEarnedForTitle
} from 'psn-api';
import { loadNpssoLocally } from './security.js';

let authCache = null;
let authForSecret = null;

export async function getAuthorization() {
  const npsso = await loadNpssoLocally();
  if (!npsso) throw new Error('NPSSO non configuré. Ouvre http://127.0.0.1:3020/ dans ton navigateur.');
  const now = Date.now();
  if (authCache && authForSecret === npsso && authCache.expiresAt > now + 30_000) return authCache;
  if (authCache && authForSecret === npsso && authCache.refreshToken) {
    const refreshed = await exchangeRefreshTokenForAuthTokens(authCache.refreshToken);
    authCache = { ...refreshed, expiresAt: Date.now() + Number(refreshed.expiresIn || 3600) * 1000 };
    return authCache;
  }
  const accessCode = await exchangeNpssoForAccessCode(npsso);
  const authorization = await exchangeAccessCodeForAuthTokens(accessCode);
  authCache = { ...authorization, expiresAt: Date.now() + Number(authorization.expiresIn || 3600) * 1000 };
  authForSecret = npsso;
  return authCache;
}

function normalizeType(type) {
  const t = String(type || 'bronze').toLowerCase();
  return ['bronze', 'silver', 'gold', 'platinum'].includes(t) ? t : 'bronze';
}
function trophyKey(t) { return String(t.trophyId ?? t.trophyID ?? t.id ?? ''); }

export async function getLatestGameAndTrophies() {
  const auth = await getAuthorization();
  const titlesResponse = await getUserTitles(auth, 'me', { limit: 20 });
  const titles = titlesResponse?.trophyTitles || [];
  if (!titles.length) throw new Error('Aucun jeu PSN accessible. Vérifie les paramètres de confidentialité et la synchronisation des trophées.');
  // PSN returns titles ordered by most recent trophy update; this is NOT proof of the currently running console game.
  const title = titles[0];
  const communicationId = title.npCommunicationId;
  if (!communicationId) throw new Error('Le jeu le plus récent ne contient pas de npCommunicationId exploitable.');
  const isPs5 = String(title.trophyTitlePlatform || '').toLowerCase().includes('ps5');
  const serviceName = isPs5 ? '' : 'trophy';
  const options = isPs5 ? {} : { npServiceName: 'trophy' };
  const [meta, earnedResponse] = await Promise.all([
    getTitleTrophies(auth, communicationId, 'all', { ...options, limit: 800 }),
    getUserTrophiesEarnedForTitle(auth, 'me', communicationId, 'all', { ...options, limit: 800 })
  ]);
  const earnedById = new Map((earnedResponse?.trophies || []).map(t => [trophyKey(t), Boolean(t.earned)]));
  const trophies = (meta?.trophies || []).map(t => ({
    id: trophyKey(t),
    name: t.trophyName || t.trophyTitle || 'Trophée',
    detail: t.trophyDetail || t.trophyDescription || '',
    type: normalizeType(t.trophyType),
    icon: t.trophyIconUrl || t.trophyIconURL || '',
    earned: earnedById.get(trophyKey(t)) ?? false
  }));
  const total = trophies.length;
  const unlocked = trophies.filter(t => t.earned).length;
  return {
    id: communicationId,
    serviceName,
    title: title.trophyTitleName || title.titleName || 'Jeu PlayStation',
    cover: title.trophyTitleIconUrl || title.titleIconUrl || '',
    unlocked, total,
    percent: total ? (unlocked / total) * 100 : 0,
    trophies
  };
}
