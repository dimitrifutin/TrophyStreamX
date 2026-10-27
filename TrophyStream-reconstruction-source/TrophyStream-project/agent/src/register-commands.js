import 'dotenv/config';
import { REST, Routes, SlashCommandBuilder } from 'discord.js';
if (!process.env.DISCORD_TOKEN || !process.env.DISCORD_CLIENT_ID) throw new Error('Configure DISCORD_TOKEN et DISCORD_CLIENT_ID dans .env');
const commands = [
  new SlashCommandBuilder().setName('trophystream-start').setDescription('Lance le suivi TrophyStream et tente de démarrer OBS.'),
  new SlashCommandBuilder().setName('trophystream-stop').setDescription('Arrête le suivi et masque l’overlay.'),
  new SlashCommandBuilder().setName('trophystream-status').setDescription('Affiche l’état du plugin et du compte PSN configuré.'),
  new SlashCommandBuilder().setName('trophystream-psn').setDescription('Affiche le lien local de configuration PSN.')
].map(c => c.toJSON());
const rest = new REST({ version: '10' }).setToken(process.env.DISCORD_TOKEN);
const route = process.env.DISCORD_GUILD_ID ? Routes.applicationGuildCommands(process.env.DISCORD_CLIENT_ID, process.env.DISCORD_GUILD_ID) : Routes.applicationCommands(process.env.DISCORD_CLIENT_ID);
await rest.put(route, { body: commands });
console.log('Commandes TrophyStream enregistrées.');
