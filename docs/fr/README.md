# Camdrop pour OBS — guide en français

**Utilisez des téléphones et des navigateurs comme caméras à faible latence dans OBS — sans appli,
en WebRTC, multicaméra.**

Camdrop est un plugin OBS qui ouvre un petit serveur HTTPS sur le PC. N'importe quel téléphone,
tablette ou ordinateur du même réseau ouvre l'adresse affichée (ou scanne le QR code), saisit un nom,
autorise caméra et micro, appuie sur **Démarrer** : la caméra devient une source OBS avec 100 à 200 ms
de latence.

## Installation

1. Téléchargez `camdrop-x.y.z-windows-x64.zip` dans les [releases](https://github.com/Wiibleyde/obs-camdrop/releases).
2. Extrayez-le dans `C:\ProgramData\obs-studio\plugins` (vous obtenez
   `...\plugins\camdrop\bin\64bit\camdrop.dll`).
3. Redémarrez OBS (version 32.2 ou plus récente).

## Démarrage rapide

1. **Outils → Camdrop** : cochez **Activer le serveur Camdrop**, cliquez sur **Appliquer**. Windows
   demande d'autoriser OBS sur le réseau : autorisez-le pour les réseaux privés.
2. Sur le téléphone, scannez le QR code (ou tapez l'adresse, ex. `https://192.168.1.20:7443`).
3. La première fois, le navigateur avertit que le certificat n'est pas reconnu (auto-signé) :
   *Paramètres avancés → Continuer*. Pour supprimer l'avertissement : [guide HTTPS](../https.md)
   (domaine perso + Let's Encrypt).
4. Saisissez le code d'accès et un nom (ex. `Scène`), autorisez la caméra, appuyez sur **Démarrer**.
5. Dans OBS, ajoutez une source **Caméra distante (Camdrop)** et choisissez `Scène`. Vous pouvez
   taper le nom avant que le téléphone se connecte.
6. **Docks → Camdrop** affiche toutes les caméras et leurs alertes (batterie, réseau, chauffe, page
   en arrière-plan, déconnexion).

## Réglages

**Plugin (Outils → Camdrop)** : serveur activé, port TCP (page + signalisation, 7443 par défaut),
plage de ports UDP pour WebRTC (51000–51100), code d'accès, nombre maximum de caméras (8),
certificat (auto-signé ou fichiers PEM importés, rechargés automatiquement au renouvellement),
résolution, fréquence d'images (30 ou 60) et débit maximum demandés, codec préféré (H.264 ou VP8),
décodage matériel, trames Opus de 10 ms.

**Source** : nom de la caméra, **buffer (ms)**, audio activé ou coupé. Le buffer absorbe les
variations du réseau : 40–60 ms sur un bon réseau, 80–150 ms sur un Wi-Fi chargé.

## Conseils

- PC en Ethernet, téléphone en Wi-Fi 5 GHz, économie d'énergie Wi-Fi désactivée.
- 4 à 6 téléphones en 1080p30 sur un Wi-Fi partagé ; jusqu'à 8 avec une borne Wi-Fi 6 dédiée reliée
  au PC en Ethernet.
- Gardez la page au premier plan, ne verrouillez pas le téléphone, branchez le chargeur. Le **mode
  économie** (écran noir) limite la chauffe.

## Dépannage

Voir la [FAQ](../faq.md) (en anglais) : pare-feu (autoriser TCP 7443 et UDP 51000–51100 sur les
réseaux privés), Wi-Fi invité ou isolation des clients, iOS, certificats.
