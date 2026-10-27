# TrophyStream — projet natif OBS + bot Discord local

Cette livraison est un **projet source de reconstruction**, basé sur `TrophyStream-improved.c`. Ce n'est pas encore un installateur compilé : le plugin C doit être compilé avec les dépendances de développement OBS pour produire `trophy-stream.dll`, puis Inno Setup peut générer l'installateur.

## Contenu

- `plugin/trophy-stream.c` : source native OBS conservant l'overlay existant et ses animations.
- `plugin/CMakeLists.txt` : cible de compilation Windows x64, avec libobs.
- `agent/` : agent local Node.js, serveur WebSocket sur `127.0.0.1:3002`, configuration locale sur `127.0.0.1:3020`, commandes Discord et accès PSN via `psn-api`.
- `scripts/build-plugin.ps1` : vérifications et commande de compilation, à configurer avec le SDK/dépendances OBS.
- `installer/TrophyStream.iss` : script Inno Setup, qui attend le DLL compilé sous `release/trophy-stream/bin/64bit/`.

## Pré-requis

- Windows 10/11 x64.
- OBS Studio x64 déjà installé.
- Node.js 20 ou plus récent.
- Visual Studio 2022 avec outils C/C++ et CMake, plus les dépendances de développement OBS pour compiler le plugin.
- Inno Setup 6 pour générer le programme d'installation.
- Une application/bot Discord créé dans le portail développeur Discord.

## 1. Configurer l'agent local

1. Ouvre `agent` dans un terminal.
2. Copie `.env.example` en `.env`.
3. Renseigne `DISCORD_TOKEN`, `DISCORD_CLIENT_ID`, et idéalement `DISCORD_ALLOWED_USER_ID`. Le token Discord reste local et ne doit jamais être publié.
4. Facultatif : renseigne `DISCORD_GUILD_ID` pour enregistrer les commandes immédiatement dans un serveur de test.
5. Exécute `npm install` puis `npm start`.
6. Ouvre `http://127.0.0.1:3020/` sur ce même PC. Renseigne ton PSN Online ID et colle le NPSSO dans ce formulaire local uniquement.
7. Sur Discord, utilise `/trophystream-start`, `/trophystream-stop`, `/trophystream-status` ou `/trophystream-psn`.

Le NPSSO est enregistré via DPAPI Windows (chiffrement lié à l'utilisateur Windows) dans `%APPDATA%\TrophyStream\npsso.dpapi`. Il n'est ni placé dans `.env`, ni enregistré dans le fichier de paramètres en clair. Un NPSSO reste un secret sensible ; révoque/renouvelle ta session Sony si tu penses qu'il a été exposé.

## 2. Compiler le plugin OBS

Le plugin utilise des API natives OBS et Windows. La compilation doit se faire sous Windows avec un environnement de développement OBS compatible. Configure `OBS_CMAKE_PREFIX_PATH` vers le dossier qui contient `libobsConfig.cmake`, puis lance PowerShell depuis le dossier du projet :

```powershell
$env:OBS_CMAKE_PREFIX_PATH = 'C:\chemin\vers\les-dependances-obs'
.\scripts\build-plugin.ps1
```

Le script échoue volontairement si le SDK/dossier de dépendances n'est pas configuré. OBS installé seul ne fournit pas nécessairement les fichiers CMake nécessaires au développement de plugins.

## 3. Générer l'installeur

Après compilation et vérification de `release\trophy-stream\bin\64bit\trophy-stream.dll` :

1. Ouvre `installer/TrophyStream.iss` dans Inno Setup 6.
2. Compile le script pour créer `TrophyStream-Setup.exe`.
3. Ferme OBS avant l'installation pour éviter qu'il garde le module chargé.
4. L'installateur place le plugin dans `%ProgramData%\obs-studio\plugins\trophy-stream\` et copie l'agent dans Program Files.

Le script d'installation copie le code source de l'agent et lance `npm install` si npm est disponible. Vérifie la sortie de l'installeur si l'installation des dépendances échoue.

## Limites connues — à lire avant de streamer

1. **Détection du jeu :** l'API PSN utilisée fournit une liste de jeux classés selon la dernière mise à jour de trophées. Ce n'est pas une preuve que le jeu tourne en direct sur la PS4/PS5. Le jeu peut donc être inexact jusqu'à une mise à jour PSN, et certains profils/jeux privés peuvent ne pas être visibles.
2. **NPSSO :** la bibliothèque et les points d'API PSN peuvent changer ou être limités par Sony. Le flux d'authentification doit être validé avec un compte réel après installation.
3. **Discord :** l'agent et OBS doivent tourner sur le même PC. Le bot distant ne peut pas démarrer un OBS qui n'est pas joignable ; ici le bot s'exécute localement comme demandé.
4. **Compilation/tests :** ce projet n'a pas été compilé sous Windows dans cet environnement et n'a pas été validé en session OBS/PSN réelle. Ne considère pas le DLL/installeur comme validé tant que ces tests ne sont pas passés.
5. **Arrêt :** l'agent garde le serveur local actif pour que le plugin puisse se reconnecter. Arrêter le suivi masque l'overlay, mais ne ferme pas automatiquement OBS.

## Format des messages vers le plugin

Le backend envoie `game-change`, `state`, `trophy`, `connected` et `trophystream-stopped`, avec les champs attendus par la source native : `titleName`, `coverUrl`, `npCommunicationId`, `npServiceName`, `unlocked`, `total`, `percent`, `remaining`, `trophyName`, `trophyDetail`, `trophy_type`, `trophyIconUrl` et `duration`.
