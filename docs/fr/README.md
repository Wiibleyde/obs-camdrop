# Camdrop pour OBS — guide en français

**Utilisez des téléphones et des navigateurs comme caméras à faible latence dans OBS — sans appli,
en WebRTC, multicaméra.**

Camdrop est un plugin OBS qui ouvre un petit serveur HTTPS sur le PC. N'importe quel téléphone,
tablette ou ordinateur du même réseau ouvre l'adresse affichée (ou scanne le QR code), saisit un nom,
autorise caméra et micro, appuie sur **Démarrer** : la caméra devient une source OBS avec 100 à 200 ms
de latence.

## Installation

OBS 32.2 ou plus récent. Téléchargez le fichier de votre système dans les
[releases](https://github.com/Wiibleyde/obs-camdrop/releases), puis redémarrez OBS.

- **Windows** : extrayez `camdrop-x.y.z-windows-x64.zip` dans `C:\ProgramData\obs-studio\plugins`
  (vous obtenez `...\plugins\camdrop\bin\64bit\camdrop.dll`).
- **macOS** (12 ou plus récent) : ouvrez `camdrop-x.y.z-macos-universal.pkg`. Le paquet n'est pas
  encore notarisé : si macOS le bloque, clic droit → *Ouvrir*.
- **Ubuntu 24.04** (OBS du PPA officiel) : `sudo apt install ./camdrop-x.y.z-x86_64-linux-gnu.deb`.
  Les autres distributions et la version Flatpak d'OBS demandent une compilation.

## Démarrage rapide

1. **Outils → Camdrop** : cochez **Activer le serveur Camdrop**, cliquez sur **Appliquer**. Windows
   demande d'autoriser OBS sur le réseau : autorisez-le pour les réseaux privés.
2. Sur le téléphone, scannez le QR code (ou tapez l'adresse, ex. `https://192.168.1.20:7443`).
3. La première fois, le navigateur avertit que le certificat n'est pas reconnu (auto-signé) :
   *Paramètres avancés → Continuer*. Pour supprimer l'avertissement : [guide HTTPS](../https.md)
   (domaine perso + Let's Encrypt).
4. Saisissez le code d'accès et un nom (ex. `Scène`), autorisez la caméra, appuyez sur **Démarrer**.
5. Une source **Camdrop – Scène** apparaît dans la scène actuelle. Vous pouvez aussi ajouter
   vous-même une source **Caméra distante (Camdrop)** et choisir `Scène` (même avant que le
   téléphone se connecte).
6. **Docks → Camdrop** affiche toutes les caméras et leurs alertes (batterie, réseau, chauffe, page
   en arrière-plan, déconnexion) et, sous *Réglages de la caméra*, leurs commandes à distance.

## Fonctions

- **Tally** : bord rouge sur le téléphone quand sa source est à l'antenne, vert quand elle est dans
  l'aperçu du mode studio.
- **Télécommande depuis OBS** (dock) : changer de caméra ou d'objectif, zoom, lampe, résolution,
  images/s, couper le micro ou la vidéo.
- **Nouvelles caméras ajoutées toutes seules** dans la scène actuelle (ajustées au canevas, masquées
  si vous êtes en direct).
- **Côté téléphone** : partage d'écran depuis un navigateur d'ordinateur, audio seul, choix du micro.
- **Rotation** par source (téléphone fixé de côté) et orientation demandée (paysage / portrait) avec
  un rappel sur le téléphone.
- **H.265 / HEVC** quand le navigateur le propose (Safari, Chrome avec encodeur matériel).
- **obs-websocket** : requêtes du vendor `camdrop` (`GetCameras`, `SetCameraControl`,
  `RequestKeyframe`, `Identify`, `Disconnect`) pour Stream Deck, Touch Portal ou des scripts ; voir
  le [README anglais](../../README.md#automation-obs-websocket).

## Réglages

**Plugin (Outils → Camdrop)** : serveur activé, port TCP (page + signalisation, 7443 par défaut),
plage de ports UDP pour WebRTC (51000–51100), code d'accès, nombre maximum de caméras (8),
certificat (auto-signé ou fichiers PEM importés, rechargés automatiquement au renouvellement),
résolution, fréquence d'images (30 ou 60) et débit maximum demandés, codec préféré (H.264, H.265
ou VP8), orientation (libre, paysage, portrait), décodage matériel, trames Opus de 10 ms. Onglet
*Studio* : tally sur les téléphones, sources automatiques pour les nouvelles caméras.

**Source** : nom de la caméra, **buffer (ms)**, audio activé ou coupé, rotation (0/90/180/270°). Le
buffer absorbe les variations du réseau : 40–60 ms sur un bon réseau, 80–150 ms sur un Wi-Fi chargé.

Quand on tourne le téléphone, l'image passe de paysage à portrait. Avec la zone de délimitation
*Mettre à l'échelle dans les limites* (celle des sources automatiques, et celle que pose *Ajuster à
l'écran*, Ctrl+F), la mise en page de la scène ne bouge pas.

## Conseils

- PC en Ethernet, téléphone en Wi-Fi 5 GHz, économie d'énergie Wi-Fi désactivée.
- 4 à 6 téléphones en 1080p30 sur un Wi-Fi partagé ; jusqu'à 8 avec une borne Wi-Fi 6 dédiée reliée
  au PC en Ethernet.
- Gardez la page au premier plan, ne verrouillez pas le téléphone, branchez le chargeur. Le **mode
  économie** (écran noir) limite la chauffe.

## Dépannage

Voir la [FAQ](../faq.md) (en anglais) : pare-feu (autoriser TCP 7443 et UDP 51000–51100 sur les
réseaux privés), Wi-Fi invité ou isolation des clients, iOS, certificats.
