# Boucle de debug flycast <-> dcload-ip <-> dcload-ip-rs

Reproduit la chaîne complète : build dcload → CDI → flycast (Windows) →
attache GDB (SH4 remote, port :3263) → upload/exec du jeu via le serveur
hôte Rust (`dcload-ip-rs`). Écrit pour qu'un agent LLM puisse boucler
dessus vite et sans gaspiller de tokens (pas de screenshot, pas de
chemin GDB en dur, une seule commande pour tout relancer).

## Prérequis one-shot (déjà faits, à ne refaire que si régressé)

- `ENABLE_GDB_SERVER=ON` dans le CMakeCache de flycast
  (`/mnt/c/Users/arnod/Code/flycast/build`). Sinon `:3263` n'écoutera
  jamais, quoi que dise `emu.cfg` (`Debug.GDBEnabled=yes` seul ne
  suffit pas — c'est un `#ifdef GDB_SERVER` côté C++). Reconfigurer :
  `cd /mnt/c/Users/arnod/Code/flycast && cmake -B build -DENABLE_GDB_SERVER=ON`
  puis rebuild côté Windows (Visual Studio/MSBuild) — l'utilisateur
  s'en charge, ça ne se scripte pas depuis WSL de façon fiable.
- `dcload-ip-rs` déjà compilé en Debug
  (`E:\Nextcloud\Projets\Dreamcast\dcload-ip-rs\target\debug\`).

## Boucle rapide

```sh
scripts/flycast-debug-loop.sh --game sa-pal
```

Ça build dcload (incrémental, rapide si rien n'a changé), régénère le
CDI, le copie côté Windows, tue les process flycast/dcload-ip-rs
existants, relance flycast, attend que `:3263` écoute, puis lance
`dcload-ip-rs` avec les mêmes arguments que la config Zed
`.zed/debug.json` du preset choisi.

Flags utiles :
- `--skip-build` — itération rapide quand seul le côté Rust/flycast a
  changé, pas dcload.
- `--no-rust` — juste build+flycast, pour inspecter dcload tout seul
  en GDB avant même de lancer un jeu.
- `--keep-running` — ne tue pas les process existants (pour attacher à
  une session déjà en cours).
- `--host <ip>` — force l'IP si le bail DHCP a changé (voir plus bas).
- `--game {sa-pal,sa-intl,sa-cdi,crazy-taxi,dreamshell}`.

Sortie exploitable : le script imprime `FLYCAST_PID=`, `RUST_PID=`,
`DC_IP=` — grep dessus plutôt que reparser tout le log.

## Temps de démarrage attendu

Mesuré sur cette machine, `--no-rust` :

| Chemin | Durée |
| ------ | ----- |
| Départ à froid (build + kill + relance flycast) | **~7 s** |
| `--skip-build --keep-running` (réutilise l'instance) | **~1,4 s** |
| flycast sans `ENABLE_GDB_SERVER` (cas dégradé) | ~31 s puis warning, borné |

Répartition du départ à froid : build+CDI 0,75 s, kill 1,1 s, lancement
+ détection PID 0,7 s, attente `:3263` + resume 0,55 s, **attente de la
première trame invité 3,6 s** (c'est dcload qui boote réellement dans
l'émulateur — irréductible), voisin 0,01 s.

Si vous voyez nettement plus, le suspect n°1 est le point ci-dessous.

## WSL en mode Mirrored : un port Windows fermé ne refuse pas, il avale

Cette machine a `networkingMode=Mirrored` + `firewall=true` dans
`C:\Users\arnod\.wslconfig`. Conséquence non évidente : depuis WSL,
`127.0.0.1` atteint bien la loopback Windows, **mais le pare-feu
Hyper-V jette les SYN vers un port Windows fermé au lieu de les
refuser**. Un port fermé coûte donc le timeout de connexion *complet*,
pas un `ECONNREFUSED` immédiat :

```sh
# port Windows fermé, vu depuis WSL
python3 -c "import socket; socket.create_connection(('127.0.0.1',3263), timeout=5)"
# -> timed out après 5,01 s   (et non "Connection refused" en ~0 ms)
```

C'était exactement la cause de la lenteur de démarrage historique : le
script relançait `flycast-resume.py` (timeout 5 s) jusqu'à 20 fois avec
`sleep 1`, **avant** d'attendre que `:3263` écoute. Chaque tentative
faite avant que le stub ait bindé son socket coûtait 5 s pleines, soit
jusqu'à 120 s de temps mort, suivies de 30 s de plus dans l'ancienne
étape 4.

Règles à respecter dans tout script de ce répertoire :

- **Jamais de timeout de connexion généreux depuis WSL vers un service
  Windows.** Préférer réessayer un timeout court : un stub qui écoute
  sur la loopback accepte en ~1 ms (mesuré : 0,021 s). `port_open()` et
  `flycast-resume.py` prennent tous deux un timeout en argument, court
  par défaut.
- **Attendre que le port soit ouvert avant de parler au stub**, pas
  l'inverse.
- **Sonder d'abord, dormir ensuite.** Le helper `poll_until` du script
  applique ça ; toutes les boucles d'attente dormaient auparavant avant
  leur première sonde, ce qui facturait du temps mort à des conditions
  souvent déjà vraies (l'entrée voisin, par exemple, est presque
  toujours déjà `Stale` et utilisable).

`scripts/dc-peek.py` garde un `timeout=5` : c'est un outil one-shot,
pas une boucle, donc il ne coûte au pire qu'un seul blocage — mais si
vous l'appelez en boucle un jour, passez-lui un timeout court.

## IP du DC émulé (DHCP) — sans screenshot

flycast avec `EmulateBBA=yes` ponte le BBA virtuel sur le vrai réseau
via Npcap ; le DC obtient donc une IP DHCP du routeur. En pratique,
observé stable : **192.168.1.130** pour le preset `sa-pal`,
**192.168.1.64** pour les autres (bail probablement lié à l'adresse
MAC fixe de l'émulation BBA côté flycast). Le script part de cette
hypothèse par défaut.

Si le bail a changé (le jeu ne boot pas, `dcload-ip-rs` reste bloqué
sur `Successfully connected...` ou timeout de version handshake) :
préférer un diff `arp -a` (Windows) avant/après lancement de flycast
plutôt qu'un screenshot — plus rapide et déterministe :

```sh
powershell.exe -NoProfile -Command "arp -a" > /tmp/arp_before.txt
# lancer flycast ici (ou via le script), laisser ~5s pour le DHCP
powershell.exe -NoProfile -Command "arp -a" > /tmp/arp_after.txt
diff /tmp/arp_before.txt /tmp/arp_after.txt
```

Un screenshot (`Add-Type System.Windows.Forms,System.Drawing` +
`CopyFromScreen`) reste possible en dernier recours (l'écran dcload
affiche l'IP en clair), mais coûte cher en tokens (image ~4 Mo) —
à réserver aux cas où l'ARP ne donne rien.

## Attacher GDB (MCP) — recette exacte

Le serveur GDB de flycast n'écoute que si dcload a fini de booter (le
script attend déjà ça). Séquence à utiliser avec les outils MCP
`mcp__gdb__*` :

1. **Sourcer l'environnement KOS n'est PAS nécessaire pour le tool
   MCP** (il tourne dans son propre process) — mais `gdb-multiarch`
   doit être précisé explicitement en `gdb_path`, sinon le tool prend
   `gdb` par défaut, qui sur ce système ne connaît pas l'architecture
   SH4 (`Undefined item: "sh4"`) et le start timeout.

   ```
   mcp__gdb__gdb_start_session(
     program="/opt/toolchains/dc/dcload-ip/target-src/dcload/dcload",
     gdb_path="gdb-multiarch",
     init_commands=["set arch sh4", "set endian little", "target remote localhost:3263"]
   )
   ```

   Charger le binaire `dcload` (pas le jeu) donne les symboles du
   loader/monitor — utile pour identifier où le CPU est bloqué même
   quand c'est le jeu qui tourne (adresses > 0x8c010000 restent sans
   symbole, c'est normal).

2. À la connexion, GDB **interrompt** systématiquement l'exécution
   (comportement du stub à l'attache). Il faut la relâcher tout de
   suite si on veut que dcload continue de servir le réseau pendant
   qu'on observe :

   ```
   mcp__gdb__gdb_continue(session_id=...)
   ```

3. Pour vérifier un hang (le serveur Rust n'a plus de trafic depuis
   longtemps, l'écran flycast semble figé) : **interrompre** puis lire
   l'état — beaucoup plus efficace en tokens qu'un screenshot :

   ```
   mcp__gdb__gdb_interrupt(session_id=...)      # révèle souvent un signal déjà en attente (ex: SIGILL)
   mcp__gdb__gdb_get_registers(session_id=...)  # reg 16=PC, 17=PR, 18=GBR, 19=VBR, 22=SR (numérotation SH4 de ce gdb)
   mcp__gdb__gdb_execute_command(session_id=..., command="info symbol <PC>")
   mcp__gdb__gdb_execute_command(session_id=..., command="info symbol <PR>")
   ```

   Puis `gdb_continue` pour reprendre si ce n'était qu'une pause
   d'inspection, ou creuser plus loin (`gdb_get_backtrace`,
   `disassemble`) si c'est un vrai crash.

4. Ne PAS relancer `gdb_start_session` à chaque itération de la boucle
   si le process flycast n'a pas redémarré — la session MCP reste
   valide tant que flycast tourne. Ne relancer que si `flycast.exe` a
   été tué/relancé (nouveau process = nouveau port TCP côté stub, la
   session GDB précédente est morte).

## Repères connus (utiles pour interpréter un hang)

- Numérotation registres SH4 de ce gdb-mcp : 0-15 = r0-r15, 16 = PC,
  17 = PR, 18 = GBR, 19 = VBR, 20 = MACH, 21 = MACL, 22 = SR.
- `go.s` (`target-src/dcload/go.s`) est le trampoline qui saute dans le
  jeu : r14 porte l'adresse d'entrée à travers le changement de banque
  SR, GBR est forcé à `0x8c000000`, VBR à `0x8c00f400`, SR à
  `0x500000f0` (RB=0, BL=1). Un PC qui atterrit sur une adresse basse
  du type `0xac0000xx` juste après l'`EXEC` indique presque toujours
  un mauvais saut dans ce trampoline ou dans le crt0 du jeu, pas un
  vrai crash applicatif.
- `dcload-ip-rs` logge `Upload complete, executing at 0x...` dès que
  l'`EXEC` part — l'absence de tout syscall `ReadSector`/`FSCommand`
  dans les ~10s qui suivent est le signal fiable d'un hang précoce
  (voir mémoire `sa-boot-init-findings` pour l'historique de ce genre
  de bug).
