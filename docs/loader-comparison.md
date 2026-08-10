# Loaders Dreamcast : isoldr, dc-virtcd et dcload-ip, mécanisme par mécanisme

> Document de **référence croisée** sur les trois émulateurs de GD-ROM dont le
> source est disponible localement. Il sert à répondre à « comment font-ils, et
> où en sommes-nous ? » quand la redirection CDFS de dcload-ip (`dc-tool -i
> jeu.iso`, ou `dcload-ip-rs`) pose problème.
>
> Les colonnes isoldr et dc-virtcd sont de la **documentation d'un tiers** : elles
> décrivent du code qui ne bouge pas. La colonne dcload décrit **l'état courant du
> dépôt** et doit être relue à chaque fois qu'un mécanisme change.
>
> Les recommandations priorisées de la version d'origine (P1-P8, 2026-07-19) ont
> toutes été soit implémentées soit réfutées par la mesure ; elles ont été
> retirées. Ce qui reste ouvert vit en §8.
>
> **Sources**
> - isoldr : `/opt/toolchains/dc/kos/ds/firmware/isoldr/` et
>   `/opt/toolchains/dc/kos/ds/modules/isoldr/` (références `fichier:ligne`
>   vérifiées le 2026-07-19, arborescence et tailles re-vérifiées le 2026-08-07)
> - dc-virtcd : `/opt/toolchains/dc/dc-virtcd/src/` (idem)
> - dcload : ce dépôt, relu **le 2026-08-07**. En cas de conflit, le code gagne.

---

## 1. Vue d'ensemble

Les trois systèmes résolvent le même problème : faire croire à un jeu qu'un
GD-ROM est présent alors que les données viennent d'ailleurs. Le jeu parle au
lecteur via les syscalls BIOS `gdGdc*` (vecteur `0x8c0000bc`), et parfois via le
matériel directement (registres G1/ASIC).

| | isoldr | dc-virtcd | dcload-ip (état 2026-08-07) |
|---|---|---|---|
| Support | SD (SPI), IDE (G1 ATA), GD, réseau | réseau BBA (UDP 4781/4782) | réseau BBA (RTL8139) / LAN Adapter, UDP 53535 |
| Résidence | blob relogeable, 32 Ko max (`0x8ce00400` par défaut) | launcher jetable `0x8cf40000` + « skel » résident `0x8c008300` (zone IP.BIN) | ~45 Ko à `0x8cf00000`, **scindé** : noyau résident ≤ `0x8cf07400` + étage `.transient.*` abandonnable après `EXEC` |
| Exécution | coroutine (`gdcExitToGame`) ; vraies IRQ G1 sur GD/IDE seulement ; SD/net = polling pur | 100 % pollé, coopératif | pollé depuis les syscalls GD du jeu, plus une pompe d'interruption optionnelle (§2.3) |
| IRQ vers le jeu | GD/IDE : vraie IRQ G1 (hide/restore). SD/net : aucune, callback appelé directement | aucune | aucune créée ; les IRQ **du jeu** sont chaînées, jamais avalées ni acquittées |
| Vecteurs hookés | 6 (`0xbc`, `0xc0`, `0xb0`, `0xb4`, `0xb8`, `0xe0`) + patch RAM GDC `0x8c0010f0` | 1 (`0xbc`) | 1 (`0xbc`) + **stub 22 octets in-place dans le VBR du jeu** |
| Lectures concurrentes | 1 (coroutine, séquentielle) | jusqu'à 8 slots de commande | **4 slots** cmd 17 en vol, `ReqCmd` non bloquant |
| Contrôle de flux | néant (I/O locale) | structurel : pull d'1 Ko par aller-retour | fenêtre glissante 8 Ko sur acquittements `ABIN` non sollicités |
| CDDA | oui (émulation AICA complète) | non | non — mais les **états** (lecteur + subcode) restent cohérents |
| Hacks par jeu | oui (patch list, région, GPIO) | non | non ; seulement des réglages globaux (§6.3) |
| Instrumentation | trace UDP/SD optionnelle | échec bruyant (bordure PVR) | compteurs résidents permanents + bloc post-mortem survivant au reboot (§7) |

Configuration : isoldr lit une struct `isoldr_info_t` de 1024 octets préfixée au
binaire (`ds/include/isoldr.h:154-196`) — device, mode DMA/IRQ, `emu_async`
(secteurs par frame), heap, patches, TOC, région. virtcd n'a aucune config (le
serveur pilote tout). dcload passe le flag `cdfsredir` dans le champ `size` de
`CMD_EXECUTE` (bit 1), le reste étant des drapeaux de **compilation**
(`target-src/dcload/Makefile` : `LAUNCH_MACHINE_CLEANUP`, `CDFS_TRACE`,
les quatre `RX_*`).

---

## 2. Interruptions

### 2.1 isoldr : chaîner, pas posséder

isoldr n'installe **jamais** son propre VBR. Il patche quelques instructions
dans le VBR **du jeu** pour détourner le vecteur d'interruption, puis rebranche
le handler original.

- `loader/exception.c:64-132` `exception_init(vbr_addr)` : copie un
  `interrupt_sub_handler` juste avant `VBR+0x600`, NOP à `VBR+0x600`,
  `icache_flush_range`. L'adresse de retour dépend de l'OS détecté : Katana saute
  4 instructions, KOS utilise `vbr - 0x188`, WinCE lit la table du jeu à
  `vbr+0x68c`.
- `exception_vbr_ok()` (`exception.c:26-62`) **re-vérifie à chaque passage** que
  le patch est là : un jeu qui réinstalle son VBR est re-hooké.
- Le handler bas niveau (`exception-lowlevel.S:80-165`) sauve tout le contexte
  (banques r0-r7, sr, gbr, vbr, dbr, ssr, spc, mach, macl, pr) sur la pile
  interrompue. **La FPU n'est volontairement pas sauvée.**
- `_my_exception_finish` = `stc sgr,r15; rte` permet d'**avaler** une IRQ sans
  que le jeu la voie. UBC (0x1E0) et TRAP (0x160) ne lui sont jamais transmises.

**Routage ASIC** (`loader/include/asic.h`) : status `0xa05f6900` (NRM),
`0xa05f6904` (EXT), `0xa05f6908` (ERR), tous write-1-to-clear. Banques de masques
par niveau : `0xa05f6910` (IRQ13), `0xa05f6920` (IRQ11), `0xa05f6930` (IRQ9).
Bits : `ASIC_NRM_GD_DMA=0x4000`, `ASIC_NRM_AICA_DMA=0x8000`, `ASIC_EXT_GD_CMD=0x1`.
INTEVT : INT9=0x320, INT11=0x360, INT13=0x3A0.

**Le tour de magie « IRQ hide/restore »** (`loader/dev/ide/ide.c:344-439`) —
la réponse d'isoldr à « faire du DMA sans perturber le jeu » :

1. `g1_dma_has_irq_mask()` scanne les trois banques pour savoir sur quel niveau
   le jeu a câblé GD_DMA (les jeux Katana l'arment typiquement sur INT11).
2. `g1_dma_irq_hide()` route le bit vers la banque interne d'isoldr (INT9) : les
   transferts internes déclenchent son handler, invisibles pour le jeu.
3. `g1_dma_irq_restore()` remet le bit dans la banque du jeu : le prochain DMA —
   celui qui répond à la requête du jeu — lève la vraie IRQ chez lui.

Il ne **crée** donc jamais une IRQ, il **cache ou expose** des IRQ réelles.

**Sans IRQ de périphérique (SD, réseau)** — tout ce qui précède est compilé
`#if GD || IDE` uniquement. La réponse d'isoldr est triple :

1. **Complétion par polling pur** : la coroutine plus les `gdcExecServer` /
   `gdcGetCmdStat` du jeu suffisent. Les builds SD/net de base fonctionnent sans
   aucun hook VBR ni ASIC.
2. **Invocation directe du callback du jeu** (`syscalls.c:653-656`) : le jeu
   reçoit son « DMA end » sous forme d'appel de fonction.
3. **Parasitage des IRQ que le jeu a lui-même armées** (variantes `_cdda`/`_vmu`/
   `_full`) : VSYNC, Maple DMA, AICA DMA — pour pomper `CDDA_MainLoop()` et
   `apply_patch_list()`, **jamais** pour la complétion des données.

S'y ajoute la chorégraphie `ata_status` : le chemin stream pose `CMD_WAIT_IRQ` au
lancement d'un chunk puis bascule `WAIT_DRQ_0`/`WAIT_INTERNAL` — il mime la
machine d'état ATA que la lib Katana observe à travers `GetCmdStat`, sans
qu'aucune IRQ n'existe.

**`setup_machine()`** (`loader/utils.c:28-73`), avant de lancer le jeu :
`irq_disable()`, reset G2 (`0xffd80000`), zéroïsation de **toutes** les banques de
masques ASIC (`0xa05f6910..6954`), clear du pending (`*0xa05f6900=0xffffffff`,
`*0xa05f6908=0x9fffff`, `*0xa05f6904=0xf`), timing G1 `*0xa05f74a0=0x2001`.

### 2.2 dc-virtcd : aucune interruption, par conception

Zéro hook VBR, zéro IRQ, zéro ASIC.

- Tout le travail réseau se fait à l'intérieur des syscalls que le jeu appelle
  (`gdrom_mainloop`, `gdrom_check_command`, `proto.c:119-137`).
- Chaque handler C encadre son travail par `setimask(15)` / restauration : un jeu
  qui masque les interruptions n'a aucun effet sur le fonctionnement.
- Pacing optionnel sur le vblank par lecture de `0xa05f810c`.
- Limite assumée : les logiciels qui pilotent le lecteur en direct (NetBSD, la
  plupart des jeux WinCE) sont hors périmètre, comme tout jeu qui attend
  réellement l'IRQ de fin de G1 DMA.

### 2.3 dcload : aucun accrochage VBR, et c'est aussi le choix d'isoldr sur ce transport

**dcload n'accroche pas le VBR.** `go.s` installe sa propre table
(`exception.bin`, à `_stack`) juste avant de lancer le jeu, et s'arrête là. Le
vecteur d'interruption `VBR+0x600` est celui de dcload d'origine :

```
interrupt:
	nop        ! jamais un branchement en 1re instruction de vecteur
	rte
	 nop
```

Les interruptions sont donc **ignorées**. dcload ne tourne que depuis les
syscalls GD ; il n'a **aucun battement de cœur propre** pendant le jeu.

**Pourquoi, alors que §2.1 décrit le patch in-place d'isoldr.** Parce qu'isoldr
ne l'utilise pas non plus sur notre transport : `ENABLE_IRQ` est commenté dans
son `Makefile.cfg`, si bien que son loader `net` de base **ne compile même pas
`exception_init()`**. L'accrochage VBR n'y arrive qu'avec CDDA, MAPLE, UBC ou
GDB. Son chemin réseau est du polling pur plus l'invocation directe des
callbacks du jeu — ce que dcload fait depuis les syscalls GD.

**Ce qui a été essayé et retiré (2026-08-07).** dcload a porté pendant un temps
le swap complet de VBR, puis le patch in-place de 22 octets (`vbr_patch.c`), un
`interrupt_common` dans `exception.bin` et une pompe réseau
`dcload_bg_service()` sur VSYNC/TMU0. **Le tout n'a jamais tourné une seule
fois** sur la cible de référence : compteur d'entrées d'`interrupt_common`
(`VBR+0x7e8`) à **0 sur 250 146 syscalls GD**, `g_bg_ticks` et `g_bg_polls` à 0.

La cause est structurelle. Le patch commençait par

```c
stc vbr,r0 ; if (vbr == DCLOAD_VBR) return;   /* "phase loader" */
```

en supposant que notre VBR encore actif signifie que le jeu n'a pas pris la
main. **SA n'installe jamais de VBR** : il garde le nôtre et écrit ses propres
handlers *dedans*. Le test sortait donc à sa première ligne pour toujours — et
`interrupt_common`, publié à `VBR+0x604`, était de toute façon écrasé par le
handler que SA pose en `+0x600`.

isoldr ne fait pas cette erreur parce qu'il **n'a pas de modèle de phases** :
`exception_init(0)` patche `vbr()`, quel qu'il soit, et `exception_vbr_ok()`
re-vérifie par `memcmp`.

Suppression mesurée : **632 octets résidents** récupérés (marge jusqu'à
`_resident_end` passée de 76 à 708 octets) et **640 octets de pile**.

**Si on veut le réintroduire**, deux préalables : distinguer « phase loader » de
« le jeu a peuplé NOTRE table » (ou supprimer le modèle de phases comme isoldr),
et déplacer `interrupt_common` hors de la zone des vecteurs. `VBR+0x5f0` est
libre (22 `nop` mesurés) et conviendrait au stub.

**Le trou réservé `VBR+0x1c0..0x300`** (`exception.S:205-238`) est le corollaire de
la phase loader : SA n'installe pas de VBR mais **lit** celui qu'il hérite et
traite `VBR+0x200` comme son propre tableau de pointeurs de handlers
(`stc vbr,r14 ; mov.l @(0x200,r14),r14 ; tst r14,r14 ; bt .skip ; jsr @r14`).
Cette table atterrit donc *dans* `exception.bin`, et tout ce qui y vivrait serait
relu comme un pointeur de fonction puis **appelé** — mesuré : `jsr` dans un de nos
mots, SIGILL, « écran noir figé », avec 0 % de trous côté hôte. Zéro est le
remplissage idéal : le `tst/bt` du titre saute une entrée nulle. **Rien** de
dcload ne peut vivre entre `0x1c0` et `0x300`, pas même un literal.

**`launch_machine_cleanup()`** (`commands.c:451-493`, drapeau
`LAUNCH_MACHINE_CLEANUP`, défaut 1) est une copie fidèle de `setup_machine()`
d'isoldr, appelée juste avant `go()` **et seulement si `cdfsredir` est actif** :
arrêt des TMU, `TCOR0/TCNT0/TCOR1/TCNT1/TCOR2/TCNT2 = 0xffffffff`, `TCR1/TCR2 = 0`,
redémarrage de TMU0, zéroïsation des 13 registres de masque ASIC, lecture bidon de
`0xa05f709c`, clear du pending NRM/ERR/EXT, masque ERR `0x9fffff`, timing G1
`0xa05f74a0 = 0x2001`. **Ne jamais écrire `TCR0`** ici : isoldr ne le fait pas, et
une révision antérieure de dcload qui l'écrivait à 0 provoquait un écran noir
reproductible au lancement.

C'est sûr pour dcload parce que le BBA est pollé : `rtl8139.c`/`net.c` ne touchent
jamais les banques `0xa05f69xx` et ne dépendent d'aucune IRQ ASIC.

---

## 3. Lectures asynchrones

### 3.1 isoldr : coroutine + trois stratégies

**État global** : une seule struct `gd_state_t _GDS`
(`loader/include/syscalls.h:140-174`) — `cmd, status, ata_status, err, req_count,
requested, transfered, callback, param[4], lba, drv_stat, gdc{flags,mode,sec_size},
true_async`.

**La coroutine `gdcExitToGame`** (`loader/gdc_syscall.s:147-191`) : pendant un
transfert long, isoldr sauvegarde le frame de pile du jeu (30 slots, verrou =
l'octet mutex BIOS `0x8c00002d`) et retourne au jeu ; le prochain
`gdcExecServer`/`gdcGetCmdStat` **reprend le transfert où il en était**. C'est la
pompe fondamentale : la progression est cadencée par le polling naturel du jeu.

**Trois stratégies** (`data_transfer`, `loader/syscalls.c:508-566`) :

1. **True async DMA** (`:383-435`, IDE/GD) : `ReadSectors(..., callback)` lance le
   DMA matériel, puis boucle `gdcExitToGame(); poll(iso_fd)` en mettant à jour
   `transfered`.
2. **Emulated async** (`:439-502`, SD et mode recommandé en général) : découpe la
   requête en `emu_async` secteurs par frame, chaque chunk = `ReadSectors`
   bloquant + purge dcache + `gdcExitToGame()`. Le jeu voit PROCESSING avec
   `transfered` qui progresse.
3. **Single-shot** : 1 secteur, ou ≥100 secteurs sur SD.

**Rapport d'avancement** : `gdcGetCmdStat` (`syscalls.c:989-1057`) retourne
PROCESSING avec `status[2]=transfered` et `status[3]=ata_status`, puis COMPLETED
**une seule fois** (consume-on-read, retour à IDLE).

À noter : **le backend réseau d'isoldr est purement synchrone** (`fs/net/fs.c:141`,
pas de `_FS_ASYNC`) — même DreamShell n'a pas résolu l'async réseau.

### 3.2 dc-virtcd : pull par chunks de 1 Ko

Jamais plus de 1 Ko par datagramme, et **c'est le DC qui tire les données**.

- UDP ports 4781/4782, jusqu'à 8 slots de commandes en vol (`proto.c:12`), chaque
  slot = `[seq_id, slot, cmd|phase<<16, params…]`.
- `read_data` (`src/host/server/server.c:153-182`) : chaque requête retourne une
  **moitié de secteur** (1024 octets) écrite à `addr+(phase<<10)`. Si le read
  n'est pas fini, le serveur répond avec le code `phase<<16` ; le DC
  (`proto_got_packet`, `proto.c:90-117`) OR-e cette phase et **re-émet la même
  requête de slot**. Un read de N secteurs = 2N allers-retours.
- **Chaque chunk est donc acquitté implicitement** : aucun débordement de FIFO RX
  possible, quel que soit le débit.
- Retransmission : `RESEND_TIME=4`, `RESEND_COUNT=100`, backoff progressif
  (`resend_time = 4 × (100 - resend_count)`), échec définitif = statut 99. Mode
  congestion : si `num_resends > 20 × num_commands`, passage au pacing vblank.
- Les données atterrissent directement dans le buffer du jeu, sans staging.
- **cmd 17 (DMAREAD) est traité exactement comme le PIO** : memcpy et on prétend.

### 3.3 dcload : quatre slots, push acquitté, et « on ne rate jamais une lecture »

C'est ici que dcload a le plus divergé des deux références. Le modèle est un
**push depuis l'hôte avec fenêtre glissante**, pas un pull — et un pool de slots,
pas une coroutine.

**Le contrat** (`cdfs_syscalls.c:22-44`) : cmd 16 (PIOREAD) est synchrone ; cmd 17
(DMAREAD) est asynchrone. `gdGdcReqCmd` alloue un slot parmi `CDFS_NUM_SLOTS = 4`
(`syscalls.h:102`), émet `CMD_CDFSREAD` et **rend la main immédiatement** — c'est
une exigence dure, les lectures concurrentes doivent pouvoir se chevaucher.
L'hôte répond par `LBIN`/`PBIN`/`DBIN` écrits directement dans le buffer du jeu,
et `CMD_RETVAL` retire le slot. Le jeu apprend la complétion en pollant
`gdGdcGetCmdStat` / `gdGdcExecServer`.

**Où tourne dcload** : uniquement depuis ces syscalls. Aucune interruption
n'atteint notre table pendant qu'un titre Katana tourne (§2.3), donc `poll_once()`
est appelé depuis les handlers GD et de nulle part ailleurs. Et **un TX n'est sûr
qu'au sommet d'un syscall** : `pkt_buf` est unique et partagé, donc jamais
d'émission depuis `poll_once`. Se tromper là-dessus a un coût mesuré : le SH4
s'arrête net une seconde après les premières lectures de secteur, tous compteurs
gelés, jeu et dcload ensemble.

**Aucun DMA réel, aucune IRQ réelle** : les données arrivent par le réseau, donc
aucune rafale G1 ne se termine et `ASIC_NRM_GD_DMA` ne peut pas se lever —
`0xa05f6900` est write-1-to-**clear**, donc le logiciel ne peut pas l'armer (une
rafale de longueur nulle a été essayée : elle ne latch pas). La complétion est
rapportée purement par le statut que rend un poll, comme isoldr sur ses backends
SD et réseau.

**Contrôle de flux : `CMD_ACKBIN` (« ABIN »)** (`commands.c:153-270`). L'hôte
bursait toute la fenêtre `LBIN`, attendait un délai de « settle », puis sondait par
`DBIN` ce qui avait été perdu. Impossible pour du CDFS : une seule lecture SA de
32 Ko fait deux fois la taille de l'anneau RX de 16 Ko du BBA, et c'est **le jeu**
qui décide quand l'anneau est drainé. Mesuré sur ~1500 lectures : 62 % trouées,
~48 % des octets de charge utile réémis, 170 Kio/s.

Donc dcload publie désormais sa **frontière contiguë** spontanément, tous les
`BIN_ACK_STRIDE = 4096` octets, et l'hôte fait glisser une fenêtre
`ACK_WINDOW = 8 Ko` (`dcload-ip-rs/src/dispatch.rs:964`) contre elle. Détails qui
comptent :

- ce sont de **pures notifications** : rien n'attend un ABIN, et en perdre un ne
  coûte que le repli sur l'ancienne sonde DBIN ;
- l'émission est **différée** au sommet d'un syscall GD (`bin_ack_flush`), jamais
  faite depuis `cmd_partbin` qui tourne en contexte RX ; un seul emplacement en
  attente suffit et n'est pas lossy, la frontière étant cumulative ;
- un **tag de fenêtre** de 4 octets est ajouté en charge utile (`ACK_TAG`), parce
  qu'adresse et taille n'identifient pas une fenêtre : les titres réutilisent un
  seul buffer de staging pour de longues séries de lectures identiques, donc deux
  fenêtres consécutives produisent des acks **octet pour octet identiques**. Un
  traînard de la fenêtre N annonçait la fenêtre N+1 complète, l'hôte s'arrêtait,
  répondait `CMD_RETVAL`, et le titre exécutait un buffer jamais rempli. `PBIN`
  résout le même problème dans les bits hauts de son champ `size` ; `ABIN` ne peut
  pas, sa taille étant un vrai compte d'octets ;
- c'est **conditionné à l'hôte** : armé seulement pour un pair ayant annoncé
  ≥ 2.1.0 au handshake `VERS`. `dc-tool-ip` (2.0.x) et tout hôte antérieur ne
  doivent jamais voir un paquet inattendu, et gardent le burst aveugle.
- une lecture plus fine ne sert à rien : 1440 (un ack par charge utile) a été
  essayé, sans effet mesurable. L'hôte bloque sur `sent - acked >= ACK_WINDOW`
  où `acked` suit les octets que dcload a **effectivement copiés hors de l'anneau** :
  il attend notre cadence de drainage, et acquitter plus souvent ne drainerait pas
  plus vite.

**Comptabilité de frontière** (`commands.c:808-870`, et son jumeau
`cdfs_slot_mark_data_progress`). La frontière avance par morceaux de 1440 octets
(`CDFS_CHUNK_BYTES`), et un `win_mask` de 8 bits mémorise les morceaux arrivés
**au-delà** d'elle, bit 0 = le morceau immédiatement suivant. Sans lui, une arrivée
hors ordre n'était jamais créditée et le slot ne se retirait jamais. La fenêtre de
8 Ko ne laisse au plus ~6 morceaux en vol, donc 8 bits couvrent tout le désordre
possible. Règle non négociable : **re-baser le masque à chaque avancée** de la
frontière, absorption ou non — décaler seulement quand on absorbe fait *dériver*
la grille, et un bit dérivé finit par faire franchir à la frontière un morceau qui
n'est jamais arrivé, donc dcload acquitte des données qu'il n'a pas et l'hôte
arrête de réémettre un vrai trou. **Cette logique existe en deux endroits qui
DOIVENT s'accorder** (un pilote ce qu'on dit à l'hôte, l'autre quand la lecture du
jeu se termine) ; `scripts/frontier_fuzz.c` les fuzze contre une référence par
force brute — 0 sur-déclaration sur 200 k livraisons en ordre aléatoire.

**Attribution des slots : ambiguë par construction.** Les titres réutilisent un
buffer unique — SA émet des centaines de lectures de 2048 octets toutes vers
`0x0c694660` — donc « à quel slot appartient ce PBIN ? » n'a **aucune réponse** au
niveau d'un paquet : même adresse, même taille, tout pareil. Trois mécanismes
répondent, à trois niveaux :

1. **Liaison de fenêtre** (`cdfs_slot_bind_window`, `cdfs_syscalls.c:910-984`) :
   la question est décidable un étage plus haut. L'hôte sert les lectures CDFS
   **strictement une à la fois** (`dcload-ip-rs` déroule tout l'échange
   LBIN/PBIN/ABIN/DBIN dans son handler DC19 avant de regarder à nouveau la
   socket), donc toutes les charges utiles d'une fenêtre `LBIN` appartiennent à
   une seule lecture. On décide une fois, à `CMD_LOADBIN` : le plus ancien slot
   (par `queue_pos`) dans lequel la fenêtre tient, sauf si une fenêtre reprend
   *exactement* à la frontière d'un slot — auquel cas c'est une re-demande de
   celui-là et ça l'emporte.
2. **Crédit depuis la fenêtre** (`cdfs_slot_credit_window`,
   `cdfs_syscalls.c:1286-1356`) : une fenêtre que l'hôte déclare complète est la
   seule affirmation autoritaire du protocole — `[win_addr, win_addr+win_size)`
   est **entièrement présent en mémoire**. Tout slot vivant contenu dedans détient
   donc tous ses octets, peu importe à qui ils ont été crédités. Créditer
   plusieurs slots d'un coup n'est pas un fudge : quand deux slots partagent
   destination et taille, ce sont littéralement les mêmes octets.
3. **Retrait des orphelins** (`cdfs_retire_orphaned_complete`,
   `cdfs_syscalls.c:1229-1279`) : `CMD_RETVAL` est **un** datagramme UDP non
   acquitté et jamais réémis, et c'est la seule chose qui retire un slot. Perdre
   ce paquet-là bloque définitivement une lecture qui avait **réussi**. Après une
   grâce de `CDFS_RETV_GRACE_TICKS = 64` ticks, dcload conclut lui-même : ce n'est
   pas une supposition, le DC détient chaque octet demandé.

**Re-demande** (`cdfs_retry_silent_requests`, `cdfs_syscalls.c:722-823`) : la
seule récupération qu'une lecture CDFS possède, contre deux pannes — requête
perdue (aucun octet n'est jamais arrivé) et *queue* perdue (des octets sont
arrivés puis plus rien). Le critère unifié est l'**absence de progrès**, pas
l'absence de données. Trois garde-fous, chacun payé par une mesure :

- `cdfs_last_data_tick` : tant que **quoi que ce soit** arrive, personne n'est
  perdu — c'est juste en file d'attente. Vu d'un slot, « ma requête est perdue »
  et « ma requête est derrière trois autres » sont indistinguables, l'hôte servant
  une lecture à la fois ;
- un budget dur de `CDFS_REQ_RETRY_MAX = 6` re-demandes **consécutives
  infructueuses** par lecture (tout progrès crédité le remet à zéro), parce que
  toute borne temporelle a échoué ici : PMCR rend 0 pendant le jeu, et une échéance
  comptée en syscalls GD s'effondre exactement quand il faut — le taux passe de
  ~120/s en boucle idle à ~54000/s en attente active, **un facteur 454**. Mesuré
  avec une échéance mal calibrée : 73 lectures du jeu devenues 1590 re-demandes et
  ~58 Mo de trafic dupliqué, écran noir pendant que l'hôte écoulait l'arriéré ;
- on redemande **à partir de la frontière**, pas depuis le début. Mesuré : deux
  slots bloqués avec `win_mask == 0xff` (saturé) et une frontière alignée sur un
  morceau qui n'avait pas bougé après trois re-demandes complètes. Perdre trois
  fois de suite exactement le morceau de frontière n'est pas de la perte de
  paquets, c'est **le blocage de la fenêtre ABIN** : elle fait 8 Ko soit ~5,7
  morceaux, notre ack rapporte la frontière contiguë, donc dès que le morceau de
  frontière est perdu l'hôte remplit la fenêtre avec les huit suivants puis
  s'arrête, en attente d'un ack qui ne peut plus venir. Repartir de 0 ne casse pas
  ça (l'hôte recale sur la même fenêtre avant d'atteindre le trou) ; repartir *au*
  trou, oui.

**« Ne jamais échouer une lecture en retard »** (`gdGdcG1DmaEnd`,
`cdfs_syscalls.c:2674-2708`). C'est la divergence la plus importante avec les deux
références, et elle est empirique : SA **ne gère pas** l'erreur de lecture — il
continue avec le buffer tel quel et l'**exécute**. Pris en flagrant délit deux
fois : PC dans un buffer de zéros à `0x8ceb0000`, et PC à `0x8c0996dc` dans une
région d'instructions réelles entrelacées de mots `0x0000`. Donc rapporter
l'erreur ne protège pas le titre, ça le tue. Le slot est laissé en vol,
`gdGdcGetCmdStat` continue à répondre PROCESSING avec le vrai compte d'octets, et
la récupération de l'hôte a un nombre illimité de chances supplémentaires. Le
budget de drainage devient « combien de temps aider avant de rendre la main », pas
une échéance avec un verdict attaché — et il est borné **en nombre de
`poll_once()`** (`CDFS_DMAEND_STALL_POLLS = 40000` sur un *stall*,
`CDFS_DMAEND_MAX_POLLS = 400000` en plafond absolu), jamais en temps, puisqu'il
n'y a pas d'horloge utilisable.

**Cadence de drainage vs autorisation de livrer** (`gdGdcExecServer`,
`cdfs_syscalls.c:2241-2296`). Deux décisions distinctes, et les confondre a coûté
des trous. La porte `acknowledged` gouverne s'il faut **livrer** : polluer un
buffer que le jeu n'attend pas encore faisait crasher les lectures de boot. Mais
l'anneau RX est partagé, fini (16 Ko) et rempli par l'hôte à sa cadence : refuser
de drainer le fait déborder et le NIC jette les trames. Mesuré le 2026-08-05 :
ExecServer représente 49,8 % de tous les syscalls GD à ~3300/s, et `rtl_bb_rx()`
n'était entré que ~125 fois/s — un drainage toutes les 8 ms contre un anneau qui se
remplit en ~1,3 ms au débit du câble. Donc quand la porte est fermée mais qu'une
lecture est en vol, on sert quand même l'anneau avec un petit budget
(`poll_once(4)`) : ça change **à quelle fréquence on regarde**, pas ce qu'on fait
de ce qu'on trouve.

**Rapport d'avancement au jeu** (`gdGdcGetCmdStat`, `cdfs_syscalls.c:2346-2509`),
sémantique isoldr adoptée :

- le **handle est monotone et jamais recyclé** (`gdCmdId++`). L'ancien bouclage
  1..8 faisait collisionner une nouvelle commande avec l'entrée d'historique
  COMPLETED d'une précédente de même id : le premier `GetCmdStat` du jeu voyait le
  COMPLETED périmé, son wrapper le traduisait en « fini », et il arrêtait de poller
  avant l'arrivée des données. C'était la cause racine du gel sur la grosse lecture
  SDRV de Sonic Adventure ;
- IDLE(0) une fois un statut terminal consommé, PROCESSING(1) avec
  `status[2] = bytes_received` réel, terminal rapporté **exactement une fois** puis
  retour à IDLE (consume-on-read) ;
- `status[3] = 0` (`CMD_WAIT_INTERNAL`) et **jamais** `CMD_WAIT_IRQ` : isoldr ne
  pose ce dernier que quand une vraie IRQ G1 va se lever, et dcload n'en lève
  jamais aucune. Ne jamais dire à un jeu d'attendre une IRQ qu'il ne peut pas
  recevoir ;
- handle inconnu : FAILED(-1) avec `status[0] = 5` (`CMD_ERR_ILLEGALREQUEST`),
  comme isoldr.

**Callback** (`cdfs_fire_pending_callback`) : le substitut d'IRQ d'isoldr —
appeler directement le callback enregistré — existe, mais il est **latché** par
`cdfs_slot_retire_next()` (contexte RX, où appeler du code du jeu est interdit) et
consommé depuis le sommet de `GetCmdStat`/`ExecServer`/`GetDrvStat`. SA n'appelle
jamais `gdGdcSetPioCallback`, donc ce chemin est inerte pour lui ; il existe pour
la compatibilité future. Dans `gdGdcG1DmaEnd` le callback est invoqué **en
dernier**, après que tout l'état est posté, parce qu'il appartient au jeu et peut
réentrer les syscalls GD.

---

## 4. Cohabitation avec le jeu

### 4.1 Placement mémoire

| | isoldr | dc-virtcd | dcload |
|---|---|---|---|
| Zone résidente | relogeable : `0x8ce00400` (défaut), LOW `0x8c004000`, MIN `0x8c000100`, HIGH `0x8cfe8000` | `0x8c008300`, dans la fenêtre IP.BIN `0x8c008000–0x8c010000` que les jeux ne réutilisent pas | `0x8cf00000`–`0x8cf07400` (assert `_resident_end`) |
| Étage jetable | non (un seul blob) | launcher `0x8cf40000`, abandonné après lancement | sections `.transient.{text,data,bss}` au-dessus de `_resident_end`, dans le même blob |
| Relocation | `patch_loader_addr()` re-base les literal pools (`modules/isoldr/module.c:430-464`) | aucune (adresses fixes) | aucune : adresses fixes, ~11 fichiers à éditer (checklist dans l'en-tête de `dcload.x`) |
| Si le jeu recouvre la zone | `Load_BootBin` détecte le conflit et lit le binaire **en deux morceaux autour du loader** (`loader/utils.c:121-169`) ; `restore_syscalls()` restaure la zone BIOS depuis la ROM | impossible par construction | attendu et toléré **au-dessus** de `_resident_end` ; refusé en dessous par `dcload_owns_range()` |
| Budget | `ISOLDR_MAX_MEM_USAGE = 32768` | skel = quelques Ko | 46 Ko de région totale ; `exception.bin` (3 Ko) à `0x8cf0b400` ; bloc post-mortem à `0x8cf0c000` |

**Pourquoi `0x8cf00000`** (en-tête de `dcload.x`) : la base se glisse entre deux
dangers, tous deux appris en s'y mettant. En bas, SA émet des DMAREAD uniques de
1 312 768 octets vers `0x0cd00000`, couvrant jusqu'à `0x0ce40800` — ce qui a évincé
la base d'origine `0x8ce00000`. En haut, SA installe **sa propre pile** haut en
RAM (sommet mesuré entre `0x8cfe1560` et `0x8cfe16a0`) et la fait descendre ; à la
base `0x8cfe0000`, une imbrication galopante du dispatcher d'interruption du titre
est descendue jusqu'à nous et a détruit les 5792 premiers octets de `.text`.
`0x8cf00000` laisse 766 Ko au-dessus du danger bas et 874 Ko de marge de pile en
dessous du danger haut. C'est aussi pourquoi la fiche de compatibilité de
DreamShell place isoldr en LOW pour ce titre plutôt qu'à une de ses adresses hautes
habituelles.

**`dcload_owns_range()`** (`commands.c:739-753`) est le garde-fou que la frontière
résidente n'est *pas* : aucune écriture demandée par l'hôte ne peut atterrir dans
`0x0cf00000..0x0cf0c000` (comparaison sur l'adresse physique 29 bits, donc valable
en P1/P2/physique). Une seule telle écriture est irrécupérable et silencieuse.
Mesuré (2026-08-05, GDB sur une instance bloquée) : `cdfs_slots[0].handle` valant
`0x8c607ee4` — impossible venant de notre allocateur — et `g_rxh[12..15]`
contenant les mêmes mots étrangers dont un ASCII `"PVRT"`, la magie d'une texture
PowerVR. C'est du **contenu de fichier écrit à la mauvaise adresse**. Conséquence
permanente : `cdfs_slot_find_free()` ne considère libre que `handle == 0`, donc ce
slot était perdu pour la session et le pool passait silencieusement de 4 à 3 — ce
que « le jeu devient de plus en plus lent » signifie réellement. Le compteur est
`g_selfwrite_refused` ; toute valeur non nulle est un bug **sur le fil**, pas un
réglage à monter.

**Le zéro-BSS en deux plages** (`dcload-crt0.s`) est le piège du split : crt0 doit
zéroïser `[__bss_start, _resident_end)` **et** `[__transient_bss_start, _end)`, et
rien d'autre. Zéroïser `[_edata, _end)` d'un bloc effaçait `.transient.text` et
`.transient.data`, donc `main()` lui-même avant que le `jsr @r0` de crt0 puisse
l'appeler.

**La pile** : `_stack = 0x8cf0b400`, qui est aussi le VBR remis au jeu et la base
d'`exception.bin`. L'assert `(_stack - _end) > 1024` a été relevé de 64 à 1024 le
2026-08-06 parce que « juste mais sûr » n'était ni l'un ni l'autre : à 240 octets
dcload crashait au **premier** paquet d'un upload, à 368 il tournait — le seuil de
64 laissait donc passer des builds qui ne peuvent pas fonctionner. Et la panne est
silencieuse, pas une faute : la pile descend directement dans `.transient.bss`,
dont l'essentiel est `bin_info_map`, le tableau que l'upload utilise à cet instant
même. Si l'assert saute, **ne pas le baisser** : prendre les octets sur
`BIN_INFO_MAP_SIZE`, adjacent à la pile.

### 4.2 Pile, registres, cache

- **isoldr** tourne sur la pile du jeu ; sauve tous les registres y compris les
  banques, mais **jamais la FPU** (choix délibéré). `mmu_disable()/mmu_restore()`
  encadrent les accès bruts (les jeux WinCE activent la MMU).
  `dcache_purge_range`/`icache_flush_range` autour de chaque auto-modification et
  de chaque chunk DMA.
- **virtcd** : pas de pile privée, ABI SH4 standard, `setimask(15)` pendant l'I/O.
  Une **unique invalidation totale du cache** (CCR bit CCI `0x800`, exécutée
  depuis P2) juste avant de sauter dans le jeu ; les anneaux RX/TX sont toujours
  accédés via le miroir non caché P2. Aucun `ocbi/ocbwb` par read : les données
  étant écrites par le CPU, le cache est cohérent par nature.
- **dcload** : les syscalls GD tournent sur la pile du **jeu** (très serrée,
  d'où le budget de §4.1) ; `exception.S` garde une pile de dump séparée à
  `0x8d000000` (à ce moment-là on a récupéré la machine). Il n'y a plus de
  handler d'interruption du tout, donc plus de sauvegarde FPU en contexte
  d'interruption (§2.3). Côté cache, la purge est **par paquet et bornée aux
  lignes touchées** (`CacheBlockPurge` sur `[cmd_addr & ~31, +size arrondi]`,
  `commands.c:872-893`), avec écrêtage sur la fin de l'ouverture RAM de 16 Mo pour
  n'importe quel alias de segment SH4 ; purge plutôt que write-back, pour ne pas
  bagarrer avec le cache du jeu. `dcload_icache_invalidate()` pulse CCR.ICI
  **depuis P2** après un patch de VBR, comme `disable_cache()` — CCR ne doit pas
  être écrit depuis un contexte de fetch caché pendant que l'état de l'IC change.
  Règle payée par un gel : **jamais de lecture 64 bits (`fmov.d`) sur la fenêtre
  GAPS/G2**, ça fige le SH4.

### 4.3 État machine au lancement

| Registre | isoldr `boot_stub` (`startup.s:113-177`) | virtcd `launch.s:48-53` | dcload `go.s` |
|---|---|---|---|
| SR | `0x700000f0` (RB=1) | `0x700000f0` (RB=1) | **`0x500000f0` (RB=0)** |
| VBR | `0x8c00f400` | `0x8c00f400` | `0x8cf0b400` (sa propre table) |
| SP | `0x8c00f400` | `0x8c00f400` | `0x8cfe0000` |
| FPSCR | `0x40001` | `0x40001` | `0x40001` |
| GBR | `0x8c000000` | — | `0x8c000000` |
| TMU0 | amorcé + démarré comme le BIOS | — | amorcé + démarré (prescaler 2, TCNT0/TCOR0 = `0xffffffff`, pas de TUNIE) |
| Divers | zéroïse les GPR, flush cache CCR `0x909` | zéroïse r0-r13, invalide le cache, zéroïse `0xac00fc00` | zéroïse r0-r13, cache désactivé avant `go()`, entrée portée dans r14 (non banké) |

Trois écarts de dcload, chacun documenté sur place dans `go.s` :

- **SR = `0x500000f0`, RB=0.** Valeur historique connue bonne : SA v1.003 bootait
  et lisait avec RB=0 ; la basculer à `0x700000f0` « pour coller à isoldr » a
  regressé le boot en un gel déterministe **avant le premier syscall GD**. isoldr
  utilise RB=1 dans son propre contexte, mais notre chemin d'entrée (et le crt0 de
  SA) attendent RB=0 ici. Corollaire : l'entrée du jeu est portée dans **r14** avant
  la bascule de SR, parce que r4-r7 sont bankés.
- **VBR = la table de dcload, pas `0x8c000000`.** Essayé et annulé : corruption
  visible sur le logo Sonic Team, crash après le fondu, BSS de dcload relue comme
  des données arbitraires, magie post-mortem détruite, trous hôte remontés de 0 % à
  8 %. Deux raisons plausibles, non vérifiées : toute exception du jeu vectorerait
  dans le handler BIOS à `0x8c000100` (on perd le contrôle à la première faute), et
  `dcload_vbr_patch_check()` écrirait son stub de 22 octets à `0x8c0005f0`, dans la
  zone système BIOS. C'est ce choix qui **impose** le trou réservé de §2.3.
- **La pile remise au jeu, `0x8cfe0000`**, deux règles apprises à la dure : elle ne
  doit pas être **dans** dcload (le titre y descendait, r15 mesuré à seulement 2764
  octets au-dessus de `_resident_end` pendant un gel — c'est aussi pourquoi marteler
  Start rendait les gels plus probables : chaînes d'appel plus profondes, pile plus
  basse), et elle ne doit pas **égaler** la base de dcload : « ça descend, donc ça ne
  nous touche pas » est faux, l'ABI SH4 passe les 5e arguments et suivants dans le
  frame de l'appelant à `@(0,r15)`, donc un appel écrit **à** r15 — le premier tel
  appel du titre a écrasé nos deux premiers mots, sentinelle `0xdeadbeef` incluse.
  Ne pas la baisser non plus : `0x8cef0000` a été essayé, le titre resettait
  instantanément, avant le logo Sonic Team.

Et une convergence : l'amorçage de TMU0 est copié d'isoldr (`main.c:37-39`,
« Pre-initialize a timer as do it the BIOS »). SA en a besoin comme **source de
temps libre**, pas comme interruption : Ghidra sur `FUN_8c641e40` montre
`_DAT_8c8a31c4 = TCNT0` lu directement, et c'est la fonction qui se bloque — ses
boucles d'attente sont bornées par un compteur que rien dans le corps n'avance
(la pompe qu'elle appelle est littéralement `rts; nop`), donc avec TCNT0 gelé le
timeout ne se déclenche jamais. dcload n'utilise aucun canal TMU pour lui-même,
donc aucun conflit.

---

## 5. Hooking des syscalls BIOS

### 5.1 Points d'interception

| Vecteur (P2) | Rôle | isoldr | virtcd | dcload |
|---|---|---|---|---|
| `0xac0000bc` | GDC « misc » / entrée principale `gdGdc*` | oui | oui | oui |
| `0xac0000c0` | GDC vecteur 2 (dispatch réel GD-ROM) | oui | non | non |
| `0xac0000b0` / `b4` / `b8` | sysinfo / font / flashrom | oui (mode « all ») | non | non |
| `0xac0000e0` | menu système (reboot, chk-disc) | oui | non | non — **et ça se voit** : SA appelle `(*0x8c0000e0)(1)` pour abandonner vers le menu BIOS, donc son renoncement est invisible et inarrêtable côté dcload |
| RAM GDC `0x8c0010f0` | entrée du driver GDC en RAM | oui (`gdc_syscall_patch()`) | non | non |
| VBR du jeu | vecteur d'interruption `+0x600` | oui (patch in-place) | non | **oui** (patch in-place, §2.3) |

Le binaire optionnel `syscalls.bin` d'isoldr (linké à `0x8c000000`) va plus loin :
il **reconstruit toute la structure logicielle BIOS en RAM** avec des `.org` (id
Dreamcast à `0x68`, table de vecteurs `0xb0..0xe0`, entrée GDC réimplémentée à
`0x10f0`, tables de 18 fonctions GDC et 48 commandes SPI).

### 5.2 Discrimination jeu/BIOS et survie du hook

Convention découverte par Marcus Comstedt : **r6 == 0 → appel `gdGdc*` du jeu ;
r6 != 0 → super-fonction BIOS** (dont la réinitialisation des vecteurs syscall).

- virtcd (`sub.s:64-74`) : si r6 != 0, appelle le vecteur original **puis réécrit
  son hook** dans `0x8c0000bc` — c'est ainsi qu'il survit à un jeu qui appelle
  « restore syscall vectors » pendant son init.
- isoldr : équivalent, plus la re-vérification périodique du patch VBR et
  `restore_syscalls()` depuis la ROM.
- **dcload a supprimé son chemin `r6 != 0`** (`cdfs_redir.s:102-112`) et adopté le
  modèle isoldr : **émuler chaque appel GD, ne jamais forwarder au BIOS physique**,
  quel que soit r6. L'ancien chemin (héritage virtcd) invoquait le handler GD du
  BIOS, qui **reset physiquement le lecteur vide** (reset audible) puis bloquait le
  boot de SA en attente d'un drive-ready qui ne vient jamais. Il n'y a ici ni disque
  ni image BIOS GD chargée, donc il n'y a rien vers quoi forwarder.
- Contrepartie : dcload ne réinstalle donc **pas** son hook après un « restore
  syscall vectors » du jeu, puisqu'il ne voit plus passer cet appel comme un cas
  distinct. Non observé comme problème sur SA ; à garder en tête pour un autre titre.

Bornage des indices : virtcd accepte 0..16, isoldr borne à 18 fonctions, dcload
accepte **0..17 (18 entrées)** — table `gd_first_k` dans `cdfs_redir.s:249-286`,
test `mov #17,r1 ; cmp/hs r0,r1 ; bf badsyscall`. Hors table :
`cdfs_badsyscall_trace(id)` puis retour −1.

Le dispatcher de dcload fait par ailleurs trois choses qu'aucune des deux
références ne fait (`cdfs_redir.s:122-217`) :

1. il latche le **PR du jeu** (les wrappers du jeu finissent en `jmp @r0`, pas
   `jsr`, donc PR nomme encore l'appelant) dans le mot post-mortem `0x8cf0c030`,
   ainsi que `*(game_sp)` — le retour du *thunk* de vtable, c'est-à-dire le
   sous-système qui attend réellement. Nécessaire parce que SA atteint les
   syscalls GD par vtable (`0x8c66d1a0`/`0x8c66d1c0`) et que l'analyse statique
   n'y résout aucun appelant ;
2. il pose le **garde `cdfs_in_syscall` en premier**, avant les sondes de trace :
   celles-ci vidangent le tampon sur le réseau, et un tick d'échantillonnage TMU1
   (priorité 15, qui perçait l'IMASK=6 du jeu) tombant dans cette vidange faisait
   démarrer un second TX réentrant — mort de la machine ;
3. il appelle `dcload_vbr_hook_dispatch()` à **chaque** syscall GD, ce qui est le
   `exception_vbr_ok()` d'isoldr.

### 5.3 Couverture des commandes GDC

| Cmd | Nom | isoldr | virtcd | dcload |
|---|---|---|---|---|
| 16 | PIOREAD | oui, 3 stratégies | oui | oui, **synchrone** (`bb->loop(0)`, timeout 20 s) |
| 17 | DMAREAD | oui, 3 stratégies | oui (identique au PIO) | oui, **asynchrone**, pool de 4 slots (§3.3) |
| 18/19 | GETTOC | TOC falsifiée dual-density | TOC synthétisée par le serveur | **19 seulement** : déléguée à l'hôte (`CMD_CDFSTOC`), écrite directement en RAM cible ; 18 tombe dans le défaut |
| 20 | PLAY_TRACKS | oui | non | pas d'audio, mais `drv_stat → PLAYING` |
| 21 | PLAY_SECTORS | oui | non | idem |
| 22 / 33 | PAUSE / STOP | oui (réel ou stub) | non | `drv_stat → PAUSED` |
| 23 | RELEASE | oui | non | `drv_stat → PLAYING` |
| 24 | INIT | oui | oui | oui, no-op — **et surtout ne réinitialise pas le NIC** (§6.3) |
| 28 | DMAREAD_STREAM | oui | non | non (défaut) |
| 30/31 | REQ_MODE / SET_MODE | stub COMPLETED | non | défaut → COMPLETED |
| 34 | GETSCD (subcode) | fabriqué (`get_scd`, `syscalls.c:247-322`) | err 32 | **non traité** : tombe dans le défaut → COMPLETED sans données (voir §8) |
| 35 | GETSES | fabriqué | err 32 | session unique fabriquée, 6 octets, leadout `0x0861B4` (modèle `get_session_info`) |
| 40 | GET_VERS | oui | `"GDC Version 1.10 1999-03-31"` | même chaîne, 28 octets (charge utile virtcd) |
| 2 / 4 | CHECK_LICENSE / REQ_SPI | acceptées, no-op « pass » | err 32 | défaut → COMPLETED |
| inconnue (0..47) | | **COMPLETED** (`syscalls.c:968-972`) | erreur 32 | **COMPLETED** (modèle isoldr adopté) |
| hors 0..47 | | — | rejet | rejet immédiat, retourne **0** (pas de handle) — modèle virtcd |

**La divergence clé est maintenant partagée** : isoldr répond COMPLETED à toute
commande inconnue — le pari que « dire oui » bloque moins de jeux que « dire
erreur ». dcload a adopté ce défaut (`cdfs_syscalls.c:2204-2223`), en gardant le
handle valide pour que le wrapper de statut du jeu voie « fini » plutôt que de
réessayer.

Un piège corrigé au passage, qui mérite d'être connu : GETSES était câblé sur le
**cas 20**, qui est `PLAY_TRACKS`. Pour PLAY_TRACKS, `param[2]` est un compte de
répétitions, pas un pointeur — donc `buf = (unsigned char *)param[2]` transformait
un petit entier en adresse et y écrivait six octets. Tout titre qui lance de la
musique CD, ce que SA fait dans ses menus, prenait ce chemin.

Et une leçon de modèle : dcload ne peut pas jouer d'audio, mais « ne peut pas »
n'est pas « peut ignorer ». Le défaut force-complétait les commandes audio en
laissant `gd_drv_stat` à PAUSED ; un titre qui lance sa musique puis attend que le
lecteur rapporte PLAYING attend alors pour toujours, ayant été informé du succès.
La réponse d'isoldr sans `HAVE_CDDA` (`gdcMainLoop`, branche `#else`) est de garder
l'état du lecteur **cohérent avec ce que le titre a demandé** même sans produire de
son ; c'est ce qui est implémenté.

### 5.4 Statut lecteur

- **isoldr** : `drv_stat` dynamique (PLAYING pendant un read, PAUSED ensuite),
  `drv_media = CD_GDROM/CD_CDROM_XA`, simulation complète eject/insert pour le
  multi-disc (`CD_STATUS_OPEN`, `CMD_ERR_UNITATTENTION`). L'IGR est testé dans
  `GetDrvStat` : appelé en boucle par tous les jeux, c'est un bon point d'ancrage
  pour du code périodique.
- **virtcd** : constant `{status=1 (PAUSED), disc_type=0x80 (GD-ROM)}`.
- **dcload** : modèle isoldr, latché explicitement — `gd_drv_stat` passe à
  `CD_STATUS_PLAYING(3)` quand une lecture (cmd 16/17) est mise en file et
  redescend à `CD_STATUS_PAUSED(1)` quand ses données sont toutes arrivées ; les
  deux transitions sont posées aux points d'émission plutôt que calculées à
  l'appel depuis `cdfs_slot_in_use()`, ce qui supprime un échantillonnage fragile.
  Un titre qui tourne sur `GetDrvStat` après un chargement attend exactement le
  front 3 → 1, et l'échantillon est pris **avant** le drainage pour que l'appel qui
  termine la lecture rapporte encore PLAYING et que le suivant rapporte PAUSED.
  `param[1] = 0x80` (GD-ROM), comme virtcd.

  **Mais le retour `r0` vaut 0, pas 1.** Vérité terrain (Ghidra sur
  `1ST_READ.BIN`) : `r0` de `GetDrvStat` a exactement un consommateur, le slot de
  vtable du driver GD à `0x8c64513c`, qui calcule
  `iVar4 = (gdGdcGetDrvStat() == 1) ? 0 : 1`, consommé par l'init de volume gdFs
  `FUN_8c5fff78` comme `if (iVar4 != 0) { ...succès... } return -2;`. La boucle de
  boot `FUN_8c010180` répète cet init jusqu'à 8 fois, chaque `-2` décompte un essai,
  et à 0 elle appelle `(*0x8c0000e0)(1)` = retour au menu BIOS. Le chemin de succès
  **exige** donc `r0 != 1`, ce que rend le vrai BIOS. Une mauvaise lecture antérieure
  de cette vtable nous faisait retourner 1, donc `iVar4 == 0`, donc `-2`, donc les 8
  abandons observés vers le menu BIOS avant la moindre lecture.

Comme isoldr, dcload profite du fait que `GetDrvStat` est pollé en boucle : c'est
un des points où le réseau est relancé (`poll_once(32)` si une lecture est en vol,
sinon `poll_once(8)`).

### 5.5 La sécurité GD-ROM

Les deux références la contournent par construction : le serveur virtcd descramble
1ST_READ.BIN (PRNG MIL-CD `seed = seed*2109+9273 & 0x7fff`, seed initial =
taille & 0xffff, auto-détecté via la chaîne device d'IP.BIN : `"CD-ROM"`/`"MIL CD"`
= scramblé, `"GD-ROM"` = non) et saute directement à `0x8c010000`. isoldr fait de
même et accepte en plus CHECK_LICENSE/REQ_SPI comme no-op.

dcload ne rencontre pas le problème sous cette forme : le binaire du jeu arrive
déjà déscramblé par l'hôte via l'upload `LBIN`/`PBIN`/`DBIN` ordinaire, et
`CMD_EXECUTE` saute à l'adresse fournie. CHECK_LICENSE/REQ_SPI sont couvertes par
le défaut COMPLETED.

---

## 6. Catalogue des hacks

### 6.1 isoldr — c'est ce qui lui donne sa compatibilité

**Patches mémoire par jeu** : `IsoInfo->patch_addr[2]/patch_value[2]`, deux
couples adresse/valeur appliqués **à chaque frame** par `apply_patch_list()`
(`utils.c:476-489`) — défait les jeux qui réécrivent périodiquement une valeur.
`patch_memory()/search_memory()` scannent le binaire pour une clé 32 bits, donc
les patches survivent aux déplacements de code entre versions.

**Boot** : descrambler KOS embarqué, appliqué si `src[1] != 0xD0`.
Patches IP.BIN (`Load_IPBin`, `utils.c:270-298`) : pointeur bootstrap-2 à
`+0x032c`, opcodes forcés `0x5113/0x000b/NOP` à `+0x10d8/+0x140a/+0x140c`.
Spoof région (`setup_region()`) : réécrit `00000/00110/00211`, mappe Corée→Japon
et Australie→Europe ; `flashrom_read` émulée sert région et sysid depuis la RAM.
Patch GPIO câble (`patch_memory(0xff800030, …)`) pour empêcher les jeux Katana de
détecter le câble réseau et changer de comportement.

**VMU émulé via breakpoint UBC** : canal A armé en break-on-write sur le registre
Maple DMA `0xa05f6c18` (`maple.c:456-478`) — isoldr intercepte la frame Maple
avant émission et injecte les réponses d'un fichier `.vmd`. Le même crochet lit
l'état des boutons.

**IGR (in-game reset)** : bouton GPIO ou combo manette → `shutdown_machine()`
(MMUCR=0, CCR=0x929, banques ASIC nettoyées, bit G1 `0xa05f8044` effacé) et
relance de `DS_CORE.BIN`.

**CDDA** (`cdda.c`, 37 Ko) : streaming PCM double-buffer vers les canaux AICA
62/63, position simulée par TMU avec constantes de calibration par révision Holly
VA0/VA1 ; transferts au choix DMA / DMA_BLOCKED (WinCE) / store queues / PIO ;
dé-entrelacement L/R en asm ; **détection de corruption de canal AICA et
auto-migration** (des jeux écrasent les canaux 62/63) ; verrouillage du bus G2
(attente FIFO + registres DMA-suspend `0xa05f783c/785c/787c`).

**TOC GD-ROM dual-density falsifiée** quand `track_lba[0] == 45150` : pistes 1/2
en basse densité avec leadout `0x01001A2C`, entrées haute densité masquées
(`GetTOC`, `syscalls.c:134-180`).

**Compatibilité dcload** : la macro `dclsc` désactive les IRQ et attend le
sémaphore G1 `0xa05f688c & 0x20` avant chaque syscall dcload
(`loader/dcload-syscall.s`). Noter que dcload teste `0x30` (les deux bits).

### 6.2 dc-virtcd — peu de hacks, mais très malins

- **Descramble côté hôte** avec mémoïsation de seed par chunk de 2 Mo, et
  auto-détection scramble/pas scramble via la chaîne device d'IP.BIN.
- **Synthèse d'un ISO9660 complet à la volée** (`directory.c` fabrique PVD,
  arborescence, place la piste de données à `BASE_SECTOR=45150`) : servir un
  simple dossier du PC comme si c'était un disque.
- TOC de trois sources : ISO brut (2 pistes + leadout, avec correction d'offset de
  session calculée depuis le PVD), NRG (chunks Nero `CUEX/DAOX`), répertoire
  synthétique.
- **Auto-IP depuis la flash** : le launcher lit l'IP configurée de la console via
  le vrai syscall flash `0x8c0000b8` (partitions Broadband-Passport puis Quake III).
- **Échec bruyant plutôt que silencieux** : syscall non implémenté → diagnostic
  réseau (`800+n°`) au serveur et **bordure PVR clignotante** rouge/verte pour
  toujours. Le développeur voit immédiatement quel syscall manque.
- Jukebox : plusieurs images servies, le DC choisit par index (« insertion de
  disque » à distance) ; état par client dans un trie radix sur l'IPv4.

### 6.3 dcload — pas de hacks par jeu, mais une liste de règles payées cher

Il n'y a **aucune** patch list, aucun spoof de région, aucun contournement par
titre. Ce que dcload a accumulé à la place, ce sont des invariants globaux, chacun
issu d'une panne mesurée. Les plus contre-intuitifs :

- **`gdGdcInitSystem` ne réinitialise volontairement pas le NIC/PHY**
  (`cdfs_syscalls.c:2900-2911`). Un `rtl_init()` complet — ce que fait
  `ether_setup` de virtcd — relance l'auto-négociation PHY (~2-3 s de coupure de
  lien) et démonte l'anneau TX alors que l'hôte croit encore la connexion dcload
  vivante. virtcd s'en sort parce que son `gdrom_init` enchaîne sur un handshake
  (`send_command_packet(991)` + `wait_command_packet`) qui re-synchronise les
  comms ; dcload n'a pas d'équivalent, et re-initier ici a fait passer les lectures
  CDFS de 27 abouties à **0** (l'hôte ne reçoit plus jamais de CDFSREAD après la
  coupure). Le NIC est déjà debout depuis la phase d'upload.
- **ARP : une ARP gratuite ne suffit pas** (`net.c:29-110`). Mesuré sur Windows 11 :
  une ARP non sollicitée dont la cible est notre propre adresse ne peuple pas le
  cache voisin. Une **requête ARP pour l'adresse de l'HÔTE**, si. dcload envoie donc
  aussi cela, et la keepalive part du sommet d'un syscall GD (même invariant que
  l'ack ABIN : `in_syscall` posé, `pkt_buf` intact), jamais depuis `poll_once`.
- **Le crt0 doit installer le VBR au boot.** Avec la base d'origine, le VBR du BIOS
  pointait par accident exactement sur `exception.bin`, donc ça marchait par
  chance. Ce n'est plus le cas, et sans cette installation toute exception devient
  une boucle de reset.
- **La sentinelle console suit la base.** Le mot `dcloadmagic` à base+4 est écrit
  par `cmd_execute` via l'alias P2 ; il est resté à l'adresse pré-relocation
  `0xace00004` à travers deux déménagements, ce qui désactivait silencieusement la
  bascule console **et** déposait quatre octets parasites dans ce qui est
  aujourd'hui la mémoire du jeu.
- **`RX_DIRECT` est à 0 sur mesure, pas par prudence.** Faire atterrir la charge
  utile d'un PBIN directement depuis l'anneau RX économise une copie réelle
  (~1440 octets RAM-à-RAM par trame), mais découper la lecture en « en-tête,
  valider, charge utile » ouvre une course contre le NIC qui continue d'écrire
  l'anneau. Mesuré sous flycast avec SA en streaming : 392 trames sur 9718 (4 %)
  avec un checksum UDP invalide alors que `g_ip_cksum_bad` restait à 0 — en-têtes
  intacts, charges utiles écrasées. Trous passés de 0 % à 10 %.
- **`RX_FRAME_COMPLETE_CHECK` est un test d'égalité, délibérément.** Arrêter le
  drainage quand `cur_rx == CBR` est la seule façon de savoir que l'anneau est vide :
  `RxBufEmpty` ne peut pas le dire, puisqu'on publie `CAPR = cur_rx - 16` et que la
  puce croit donc toujours 16 octets non lus. Une variante comparant la distance
  modulaire `(CBR - cur_rx)` n'a jamais déclenché une seule fois (0 report contre 13
  resyncs) : cette soustraction rend un nombre énorme précisément quand on est en
  avance.
- **`RX_GATE_ON_ROK` ne garde rien**, malgré son nom : il démasque les bits de
  statut RX dans `RT_INTRMASK`, et c'est la partie porteuse — le masque décide si
  ces bits sont observables du tout. Avec le masque à 0, le chemin ROK **paraissait**
  mort. Combiné à l'élargissement du drainage ExecServer, ça a fait passer
  `g_rtl_rx_resyncs` de 32 à 0 et les lectures CDFS de 73 à 790+. Et ses compteurs
  ne touchent **pas** `RT_INTRSTATUS` : deux propriétaires d'un même registre de
  statut, c'est la forme de « l'hôte réémet à l'infini, le DC reçoit 0 octet ».
- **`CDFS_TRACE` est à 0 par défaut** et doit y rester hors debug : la trace tourne
  à chaque syscall GD, un titre en attente en émet des milliers par seconde, et la
  vidange UDP périodique concurrence les données de secteur sur le même lien —
  mesuré ~60 Ko/s avec contre ~161 Ko/s sans, plus la marge résidente que ça coûte.

---

## 7. Instrumentation — le domaine où dcload est allé le plus loin

Ni isoldr ni virtcd n'ont d'équivalent (virtcd a une bordure PVR clignotante,
isoldr une trace optionnelle). dcload embarque des **compteurs résidents
toujours compilés**, lus hors bande pendant que la machine tourne, parce qu'un gel
sous CDFS est indistinguable de l'extérieur : le jeu continue d'émettre des
syscalls GD, aucun slot n'est en vol, l'anneau est sain et l'hôte n'a rien à
répondre.

- **Ce que le jeu demande vraiment** : histogramme `g_gd_idx_counts[48]` par index
  de syscall, et anneau `g_gd_seq[16]` des derniers couples (index, premier
  paramètre). C'est ce qui identifie la condition sur laquelle un titre tourne en
  rond, verbatim. Indices : 0 ReqCmd, 1 GetCmdStat, 2 ExecServer, 3 InitSystem,
  4 GetDrvStat.
- **Qui nous poll** : `g_gd_caller[18][2]` — PR du jeu et retour du thunk de
  vtable, par index. Nécessaire parce que l'analyse statique ne résout aucun
  appelant à travers une vtable.
- **Le dernier échange de statut** : `g_gd_stat_handle`, `g_gd_stat_ret`,
  `g_gd_stat_out[4]`. Un jeu qui n'avance plus en pollant un handle pour lequel on
  répond IDLE(0) a perdu sa complétion.
- **Le bloc post-mortem** à `PM_BASE = 0x8cf0c000` : premier octet que ni le
  zéro-fill de `loader.s` ni `exception.bin` ne touchent, donc **il survit à un
  reboot**. Contient le compte de boots, les échecs, l'historique des commandes, et
  le PR du jeu au dernier syscall GD (`0x8cf0c030`).
- **Les compteurs de pannes survécues**, chacun avec une signification précise
  documentée sur place : `g_selfwrite_refused` (écriture hôte dans notre image
  refusée — bug sur le fil), `g_cdfs_slots_reclaimed` (table de slots corrompue et
  survécue), `g_cdfs_credit_lost` (charge utile écrite mais créditée à personne),
  `g_cdfs_window_bound` / `g_cdfs_window_credited` (les réparations d'attribution
  de §3.3 en action), `g_cdfs_retv_orphans` (RETV perdus et conclus),
  `g_cdfs_dmaend_incomplete` (lectures laissées finir au lieu d'être échouées),
  `g_cdfs_req_retries`, les compteurs RX de `rtl8139.c`.
- **L'outillage** : `scripts/dc-peek.py <symbole|0xaddr>` résout les symboles depuis
  l'ELF dcload et lit la mémoire invitée par paquets GDB bruts ;
  `scripts/dc-sample.py` échantillonne une table de compteurs à cadence fixe et
  affiche les deltas ; `scripts/dc-freeze.py` prend un instantané complet pendant un
  gel ; `scripts/frontier_fuzz.c` fuzze la comptabilité de frontière ;
  `scripts/check-resident-invariant.sh` vérifie la frontière résidente.

Deux avertissements qui valent autant que les outils. **Ne jamais coder en dur
l'adresse d'un compteur** dans un script : ajouter un compteur décale `.data` et
les valeurs deviennent du non-sens plausible. Et **vérifier qu'un garde garde
encore** : `check-resident-invariant.sh` avait un préfixe d'adresse figé et a
silencieusement délivré un bulletin de santé impeccable pendant des semaines après
une relocation.

---

## 8. Ce qui reste ouvert

Points relevés à la relecture du 2026-08-07, sans verdict :

- **Commande 34 (GETSCD) non traitée.** `gd_cdda_stat` est maintenu à jour par les
  commandes audio (§5.3) et le commentaire de `cdfs_syscalls.c:2175` dit qu'il est
  « rapporté par la commande subcode (34) » — mais il n'existe aucun `case 34` : la
  commande tombe dans le défaut, qui répond COMPLETED **sans écrire de données**.
  `gd_cdda_stat` n'est donc jamais lu. Un titre qui poll le subcode voit un
  buffer non écrit avec un statut de succès. isoldr fabrique la réponse
  (`get_scd`, `syscalls.c:247-322`) ; c'est le modèle à copier si un titre s'y
  bloque.
- **Le vecteur `0xe0` n'est pas hooké.** SA appelle `(*0x8c0000e0)(1)` pour
  abandonner vers le menu BIOS après 8 échecs d'init de volume (§5.4). Le hooker
  donnerait à la fois un point d'observation (« le titre vient de renoncer, voici
  quand ») et un point d'intervention. isoldr le hooke.
- **La cible de chaînage en phase loader.** `go.s` écrit dans `0x8cf0bbf0` le VBR du
  BIOS **verbatim** (`stc vbr,r2`), alors que le commentaire de
  `exception.S:630` décrit ce slot comme contenant « `game_vbr+0x608`, ou
  `BIOS vbr+0x600` ». Les deux ne s'accordent pas sur l'offset. Sans conséquence
  observée — les interruptions sont masquées en phase loader — mais l'un des deux
  est faux et il vaut mieux savoir lequel avant qu'une interruption y passe.
- **Pas de réinstallation du hook après un « restore syscall vectors »** (§5.2),
  conséquence de la suppression du chemin `r6 != 0`. Non observé sur SA.
- **Le pump d'interruption est inerte sur la seule cible mesurée** (§2.3). Aucun
  titre ne l'a encore exercé, donc c'est du code non validé en conditions réelles.
- **`RX_HEADER_STABLE_RESYNC` reste à 0** et, avec 0, le streak est calculé mais
  jamais lu : `g_rx_header_stable_resyncs` reste à 0. Un build par défaut ne peut
  donc pas dire si l'activer aiderait.

---

## 9. Ce qu'il faut en retenir

1. **Aucune IRQ n'est nécessaire pour les données.** virtcd boote des jeux
   commerciaux en 100 % pollé ; les builds SD/net d'isoldr aussi ; dcload aussi, et
   sur sa cible de référence la pompe d'interruption ne s'exécute littéralement
   jamais. Les IRQ qu'isoldr utilise alors sont **celles du jeu** (VSYNC/Maple) et
   servent uniquement au CDDA, au VMU et aux patches.
2. **Le substitut d'IRQ est l'invocation directe du callback** que le jeu a
   enregistré (`G1DmaEnd`, `SetPioCallback`) — présent chez les trois, mais chez
   dcload il faut le latcher et ne le tirer que depuis le contexte syscall.
3. **Rendre la main pendant un transfert** : `gdcExitToGame` chez isoldr, polling
   naturel chez virtcd, `ReqCmd` non bloquant + pool de slots chez dcload. Bloquer
   dans un syscall fait dérailler les watchdogs du jeu et l'empêche de pomper son et
   Maple.
4. **Le contrôle de flux doit être structurel**, pas correctif : chez virtcd
   l'overflow de FIFO RX est impossible par construction (pull d'1 Ko par
   aller-retour) ; chez dcload c'est la fenêtre glissante de 8 Ko contre les acks
   ABIN, et le reste des trous vient de la **cadence de drainage**, qui est une
   question distincte de l'autorisation de livrer.
5. **Mentir avec aplomb** : COMPLETED par défaut, TOC/subcode/flash/région
   fabriqués, et surtout garder l'état **cohérent avec ce que le titre a demandé**
   même quand on ne peut pas le servir. **Chaîner sans posséder** : VBR patché
   in-place, masques ASIC manipulés plutôt que le status, aucune IRQ avalée.
6. **Ne jamais rendre un buffer à moitié rempli.** C'est la leçon propre à dcload,
   et elle n'était pas dans le document d'origine : un titre commercial n'a pas
   forcément de chemin de récupération sur erreur de lecture — SA exécute ce qu'il a.
   Attendre est récupérable, corrompre ne l'est pas.
7. **Aucune borne temporelle n'est fiable pendant qu'un jeu tourne.** PMCR rend 0,
   le taux de syscalls GD varie d'un facteur 454. Compter des itérations, ou tenir
   un budget dur — les deux résistent à une horloge cassée, une échéance non.
