# Fouille v3 — conception

> Document de travail, 2026-11-14. Remplace `fouille-design-v2.md` pour tout ce qui touche
> l'architecture, la surface et les sources. Le v2 reste la référence pour le moteur sonore
> (LPG à vactrol, enveloppe ternaire, Spectral Lock, Erosion, Echo Pool), dont presque tout
> est gardé.
>
> **Statut : à relire par Vincent.** Les sections marquées **[À DÉCIDER]** attendent une
> réponse ; une valeur proposée est donnée à chaque fois, pour qu'on puisse avancer si elle
> convient.

---

## 0. Ce qui est décidé (conversation du 2026-11-14)

| Question | Décision |
|---|---|
| Type de module | **Overtake** (prend tout le Move, comme Loopex). |
| Rôle du site | **Un seul code, deux usages** : une carte publique (github.io) pour explorer et écouter, et la même page servie par le Move, qui le pilote. |
| Sources | **Les quatre** : enregistrements de terrain, radios web en direct, cartes sonores du Québec, micro / entrée du Move. |
| Territoire | **Montréal en profondeur d'abord**, puis l'autogéographie (**Saguenay, Kamouraska**, Montréal), puis d'autres lieux et une **carte libre** (un point n'importe où, Fouille cherche autour). |
| Pads | **Partage à la Loopex** : 16 pads de gauche = des voix, 16 pads de droite = des gestes. |
| Moteur / hôte | **Séparés dès l'étape 2** : un moteur en C pur et portable (`engine/`), un hôte Move (`move/`), plus tard un hôte Pandore. Voir §16. |
| Pandore, « Ici » | **Les deux** : la géolocalisation par IP propose un point, vous le corrigez au montage sur la carte web. |
| Pandore, place dans *Impressions* | **Un firmware au choix** : une boîte existante peut démarrer en Fouille plutôt qu'en Phasma, Res ou Essaim. |
| Longueur d'une voix | **30 s stéréo** (réponse du 2026-11-14). |
| Gestes de droite | **À construire en jammant ensemble.** Orientés vers la transformation, l'adjacent, la mémoire, le passé, le futur, les possibles, la recontextualisation ; expressifs avec la pression des pads, comme Loopex. La liste du §9 n'est qu'un point de départ. |
| Radios | Proposition à écouter (toutes en MP3, trouvées sur radio-browser.info le 2026-11-14) : Montréal **CIBL 101,5** (à confirmer : l'entrée s'appelle « La Cible »), **CISM 89,3**, **CKUT 90,3** ; Québec **CKIA 88,3** ; Saguenay **CKAJ 92,5** ; Kamouraska **CHOX-FM 97,5** (La Pocatière). |
| Langue | **Français** seulement (écran et carte). |
| Noms | voix, ondes, usure, mémoire, Ici : **gardés**. |
| Site public | **Dans ce dépôt** (rendu public, github.io). |
| Aporee par position | **Vérifié** : chaque item porte `latitude`/`longitude`. L'API accepte une plage de latitude (pas une longitude négative) : on cherche par bande de latitude + `coverage:Canada`, puis on filtre la distance dans le module. |

## 1. L'intention

Fouille est **une voix du proche et du lointain**. Il fait entendre des lieux du Québec :
les vôtres, ceux de vos gens. Il les entend par ce qu'ils publient eux-mêmes : des
enregistrements de terrain déposés sous licence libre, des radios communautaires qui
émettent en ce moment, des cartes sonores faites par ceux qui y vivent, et le son de la pièce
où vous jouez. L'instrument ne compose pas ces voix : il les **capte, les retient, les laisse
s'user et les rend**. Le geste de Pennisi (*de-authoring*, la matière trouvée plutôt que
choisie) reste au centre ; ce qui change, c'est qu'elle n'est plus trouvée n'importe où. Elle
est trouvée **chez soi et chez les siens**.

Deux tensions donnent sa forme à l'interface :

- **proche / loin** : le micro (ici, maintenant) contre le réseau (là-bas, ou il y a dix ans) ;
- **vivant / retenu** : une radio qui parle en ce moment contre un fragment figé qui s'érode à
  chaque passage.

## 2. Ce qui change depuis la v0.2

| | v0.2 (aujourd'hui) | v3 (proposé) |
|---|---|---|
| Module | `sound_generator`, pages de knobs dessinées par l'hôte | `overtake` : `ui.js` dessine l'écran, allume les pads, lit tous les boutons |
| Mémoire | 8 emplacements de 5 s (mono, voir §3) | 16 voix, chacune une bande stéréo en RAM (30 s proposé, §11) |
| Lieux | 16 quartiers de Montréal = 16 recherches par mots-clés, codés en dur | un **atlas** (fichier JSON) de lieux avec coordonnées, rayon, mots, radios ; édité par la carte web, relu à chaud |
| Sources | Freesound (texte) + Radio Aporee (mots-clés) | Aporee + Freesound **par position GPS** + radios en direct + cartes locales + micro |
| Jeu | les notes MIDI cherchent l'emplacement le plus proche en descripteurs | les pads de gauche **sont** des voix ancrées à des lieux ; les pads de droite sont des gestes ; les descripteurs servent la dérive et la carte |
| Hors ligne | bruit synthétique | la **mémoire** : les fragments déjà captés, gardés sur disque avec leurs crédits |

## 3. Audit de la v0.2 (ce que le code fait vraiment)

Lu ligne à ligne le 2026-11-14 :

- **Jamais vérifié sur le Move.** Le code appelle `curl` à `/data/UserData/schwung/bin/curl`.
  Rien ne prouve que ce fichier existe, ni que le `curl` du Move gère HTTPS. → **Étape 0**.
- **Branché :** le thread de fond (Freesound + Aporee + quartiers), minimp3, les descripteurs
  (centroïde, intensité, bruit), l'enveloppe ternaire, le LPG, les trois modes de Hold, la
  dérive brownienne, l'Echo Pool (simple écho 0,5 s), une érosion (un passe-bas fixe).
- **Pas branché :** **Lock** (Spectral Lock n'existe pas dans `render_block`) ; **Depth**
  (stocké, jamais utilisé dans les requêtes) ; le compander ; l'Echo Pool comme source jouable ;
  l'érosion comme *cycle de vie* (elle ne vieillit rien, c'est un filtre global).
- **Bogues à ne pas reporter :**
  1. *Échange de tampon sous une voix qui joue* : la voix relit `active_buf` à chaque bloc ;
     quand une nouvelle récolte arrive sur son emplacement, elle saute d'un son à l'autre au
     milieu d'une note (clic + mauvais son).
  2. `length` est écrit avant l'échange de tampon : un bloc peut lire la nouvelle longueur
     sur l'ancien tampon.
  3. Le MP3 est gardé à **sa** fréquence d'échantillonnage et joué comme du 44,1 kHz : un
     fichier à 48 kHz sonne 1,5 demi-ton trop bas. Le commentaire « 22 kHz » est faux : les
     5 s réelles viennent de 220 500 échantillons à 44,1 kHz.
  4. Journal écrit dans `/tmp` (à proscrire, toute la famille de modules l'interdit).
  5. Parseur JSON par `strstr` : il prend le premier `.mp3` venu, qui peut appartenir à un
     autre champ.

Ce qui se garde tel quel ou presque : `vactrol_process`, `drift_process`, la logique de
l'enveloppe ternaire et des modes de Hold, minimp3, `compute_descriptors` (à refaire avec une
vraie FFT, cf. Concat).

## 4. Vue d'ensemble

```
          ┌──────────────── ATLAS (lieux.json, UserLibrary/Fouille) ───────────────┐
          │ territoires → villes → quartiers → points ; mots ; radios ; itinéraires │
          └───────▲───────────────────────────────────────────────┬────────────────┘
       écrit par  │                                               │ relu à chaud (2 s)
 ┌────────────────┴─────────┐                         ┌───────────▼─────────────────────────┐
 │  CARTE WEB (même code)   │  /files (manager)       │  TRAVAILLEURS (cœurs 0-2, jamais 3)  │
 │  · github.io : explorer, ├────────────────────────►│  · récolte : Aporee, Freesound GPS,  │
 │    écouter dans le       │  statut.json ◄──────────┤    cartes locales → MP3 → PCM 44,1k  │
 │    navigateur            │                         │  · ondes : 1-2 radios en continu     │
 │  · sur le Move : piloter │                         │    → anneau de 60 s par radio        │
 └──────────────────────────┘                         │  · disque : mémoire, sessions        │
                                                      └───────────┬─────────────────────────┘
                                                                  │ échange atomique, à la frontière d'une note
    micro / entrée ───────────────────────────────────┐          │
                                                      ▼          ▼
                              ┌────────── 16 VOIX (pads de gauche) ──────────┐
                              │ bande stéréo · ancrée à un lieu ou une radio  │
                              │ enveloppe ternaire → LPG vactrol → Lock →     │
                              │ usure → dérive → pan / volume / envois        │
                              └──────────────────────┬───────────────────────┘
                                                     ▼
                     GESTES (pads de droite, pression) → Echo Pool → compander → limiteur → sortie
```

## 5. Les sources

Chaque source est un « chercheur » qui, pour un lieu donné, rend un fragment audio et sa
**fiche** (titre, auteur, licence, URL, date, coordonnées). La fiche suit le fragment partout :
écran, mémoire, site. C'est la condition pour respecter les licences CC, et c'est aussi ce qui
rend les voix à leurs auteurs.

### 5.1 Enregistrements de terrain

- **Radio Aporee** (collection `radio-aporee-maps` d'Internet Archive, sans clé). Aujourd'hui
  interrogée par mots-clés. À l'étape 0, on compte ce qu'elle contient pour chaque lieu ; si
  les métadonnées des items portent des coordonnées, on cherchera par position. **[À vérifier
  sur un vrai résultat]**
- **Freesound par position** : l'API accepte un filtre géographique (`{!geofilt sfield=geotag
  pt=lat,lon d=km}`), donc « tout ce qui a été enregistré à moins de 2 km du marché Jean-Talon »
  devient une requête directe. Il faut une clé (gratuite) ; limites de l'API à respecter
  (requêtes par minute et par jour, à lire dans leurs conditions).
- **Freesound par mots** : repli quand le rayon est vide (le Kamouraska aura peu de sons
  géolocalisés).

### 5.2 Radios web en direct

- **Annuaire** : radio-browser.info, une base communautaire et gratuite de radios web, avec une
  API JSON (pays, province, codec, débit). Elle sert à **proposer** des stations ; la liste
  jouable est celle de l'atlas, choisie par vous (une radio mal référencée ne doit pas arriver
  dans un concert par hasard).
- **Formats** : v1 = **MP3 sur HTTP/Icecast** seulement (minimp3 les décode). Les flux
  **AAC/HLS** (Radio-Canada, une partie des radios commerciales) sont exclus : il faudrait un
  décodeur AAC, lourd et à la licence compliquée.
- **Moteur « ondes »** : un travailleur par radio active (2 au plus) lit le flux par
  `curl -N` dans un **anneau de 60 s** en RAM. Une voix peut :
  - **capter** : copier une fenêtre de l'anneau dans sa bande (elle devient un fragment
    retenu, qui s'use) ;
  - **écouter** : lire l'anneau en direct, avec 1 à 2 s de retard de sécurité (c'est la radio
    qui joue à travers le LPG et les gestes).
- **Syntonisation** : entre deux stations de l'atlas, un passage par un souffle synthétisé
  (l'idée de la radio de RECORDA), pour que changer de station soit un geste et pas une coupure.
- **Panne** : flux coupé ou wifi tombé → la voix continue sur ce qui reste dans l'anneau, puis
  s'éteint dans un fondu ; le travailleur réessaie avec un délai croissant.

### 5.3 Cartes sonores du Québec

Adaptateur par source, **une à la fois, et seulement après avoir lu sa licence et vérifié qu'on
peut lire ses fichiers par programme**. Pistes à examiner (rien n'est confirmé) : la Montréal
Sound Map, les archives sonores publiques, les collections de radios communautaires. Si une
source n'a pas d'API, la carte web peut quand même la présenter par des liens, sans qu'elle
entre dans l'instrument.

### 5.4 Le proche : micro et entrée

`audio_in` comme Loopex : l'entrée ligne / micro du Move (et plus tard les pistes Move, via le
même mécanisme que l'InSrc de Loopex). Une voix peut **capter la pièce** comme elle capte une
radio. Le micro est la seule source qui ne vient pas d'ailleurs : sur l'écran et sur la carte,
il est le lieu « Ici ».

## 6. L'atlas : lieux, concepts, itinéraires

Un fichier JSON, `UserLibrary/Fouille/lieux.json` (hors du dossier du module, pour survivre aux
réinstallations), écrit par la carte web, relu par le Move toutes les 2 s comme le `.concat` de
Concat (on garde l'ancien si le nouveau est mal formé, et on le dit à l'écran).

```json
{
  "fouille": 1,
  "lieux": [
    {
      "id": "mtl-hochelaga",
      "nom": "Hochelaga",
      "parent": "mtl",
      "lat": 45.5446, "lon": -73.5436, "rayon_m": 1500,
      "mots": ["rails", "port", "usine", "ruelle"],
      "texte": "Ce que vous voulez dire du lieu : la carte l'affiche.",
      "sources": { "aporee": true, "freesound": true, "radios": ["cibl"] },
      "couleur": "#c4683a"
    }
  ],
  "radios": [
    { "id": "cibl", "nom": "CIBL 101,5", "url": "https://…/stream.mp3", "lieu": "mtl" }
  ],
  "itineraires": [
    { "id": "retour", "nom": "Retour au Saguenay", "etapes": ["mtl-plateau", "kamouraska", "saguenay-chicoutimi"], "duree_s": 600 }
  ]
}
```

- **Hiérarchie** : territoire → ville → quartier → point. Montréal d'abord : les limites des
  quartiers viennent des données ouvertes de la Ville (licence à confirmer avant de les
  embarquer, je crois CC BY 4.0).
- **« Concepts » d'un quartier** : les `mots` et le `texte`. Ils servent à la recherche (repli
  par mots) et à la carte, qui les affiche comme une couche poétique. C'est là que vous écrivez
  ce qu'un lieu est *pour vous*.
- **Itinéraires** : une suite de lieux et une durée. La voix ou tout l'instrument se déplace le
  long de l'itinéraire (le Flow de la v2 devient un voyage, et plus seulement un intervalle).
- **Carte libre** (plus tard) : un lieu sans nom, juste `lat`/`lon`/`rayon_m`, posé d'un clic.

**[À DÉCIDER] Atlas livré avec le module** : proposé, un atlas de départ avec les quartiers de
Montréal (coordonnées + limites), puis Saguenay et Kamouraska, et aucune radio tant que vous ne
les avez pas choisies et écoutées.

## 7. La mémoire

Chaque fragment capté (terrain, radio, micro) peut être gardé sur disque :
`UserLibrary/Fouille/memoire/<lieu>/<horodatage>.wav` + un `.json` (la fiche). Ce qui permet :

- de **jouer hors ligne** (scène sans wifi : l'instrument joue ce qu'il a déjà récolté) ;
- de **garder les crédits** (écran « Crédits », export pour une pièce publiée) ;
- de **revenir** à un lieu : la mémoire d'un lieu est son archive personnelle.

Plafond de disque réglable, les fragments les plus anciens et jamais figés partent en premier.

**[À DÉCIDER] Radios dans la mémoire** : un fragment de radio est une émission protégée.
Proposé : on peut le garder sur le Move (usage privé, jeu), mais la carte publique ne le diffuse
jamais (elle ne donne que le lien de la station).

## 8. Les 16 voix (pads de gauche)

Une voix = une bande stéréo en RAM + un **ancrage** (un lieu, une radio, ou Ici) + ses réglages.
C'est la piste de Loopex, mais la matière vient du lieu au lieu de l'enregistrement.

**États** : Vide → Fouille (récolte en cours, LED qui respire) → Prête → Joue → Figée → Usée.

**Gestes proposés** (calqués sur Loopex pour que les mains s'y retrouvent) :

| Geste | Effet |
|---|---|
| tap | joue / arrête (l'enveloppe ternaire et le LPG font l'attaque et la chute) |
| tenir | le Hold de la v2 : Freeze / Drift / Granulate selon le réglage |
| vélocité / pression | strike du LPG (plus fort = plus brillant), dispersion de la position |
| Shift + tap | **fouiller encore** : nouveau fragment du même lieu, remplacé à la fin de la note (corrige le bogue 1 de §3) |
| double tap | **figer** / libérer : une voix figée n'est plus jamais remplacée par la récolte automatique |
| Capture + pad | capter dans cette voix ce que joue la radio active, ou la pièce si c'est Ici |
| tenir (long) | effacer (Undo restaure) |
| Copy + pad + pad | cloner une voix |

**Pages de réglages de la voix choisie** (jog / flèches, 8 knobs par page) :

| Page | Knobs |
|---|---|
| **Lieu** | lieu · source · rayon · profondeur (brillance cherchée) · flux (renouvellement) · fidélité (rester au lieu ↔ dériver vers les voisins) · mots · volume |
| **Geste** | rise · hold · fall · strike · dispersion · départ · longueur · sens |
| **Matière** | vitesse · grain · lock · usure · dérive · ton · saturation · réverse |
| **Espace** | pan · largeur · envoi A · envoi B · pool · tête 2 · tête 3 · volume |

**Usure** (l'érosion de la v2 devenue un cycle de vie) : chaque passage use la voix un peu, à
la manière de la Tape Wear de Loopex (perte d'aigus, niveau, craquements), et une nouvelle
récolte la rend neuve. Une voix figée s'use sans être remplacée : elle finit en souffle.

## 9. Les 16 gestes (pads de droite, sensibles à la pression)

Proposition à jouer et à corriger, inspirée des punch-in de Loopex et des modules Interfera :

| | | | |
|---|---|---|---|
| **Dérive** : chaque voix glisse vers un lieu voisin | **Syntonie** : balaie les radios de l'atlas, la pression = la vitesse | **Gel** : fige le son de toutes les voix | **Pluie** : grains dispersés sur toutes les voix |
| **Écho** : l'Echo Pool remonte comme source | **Lock** : Spectral Lock fort sur le mix | **Effacement** : usure accélérée tant qu'on tient | **Souffle** : frappe le LPG de toutes les voix |
| **Retour** : saute au lieu d'origine de l'itinéraire | **Rafale** : répétitions courtes (stutter) | **Inverse** | **Ralenti** : vitesse ÷ 2 en glissant |
| **Foule** : toutes les voix du même quartier à la fois | **Ici** : ouvre le micro dans le mix | **Éloignement** : passe-haut + réverbe, la voix part au loin | **Approche** : l'inverse |

Comme dans Loopex : jusqu'à 5 gestes en série, Shift + pad = verrouiller, Undo + pad =
réinitialiser.

## 10. Écran, boutons, menus

- **En-tête** : nom du lieu de la voix choisie, état réseau (wifi, récoltes en cours), heure.
- **Bande de 16 cellules** sous l'en-tête (pleine = joue, soulignée = a de la matière, comme
  Loopex).
- **Vues** (au jog) : **Voix** (forme d'onde, tête de lecture, fiche courte) · **Carte** (les
  lieux comme des points sur 128 × 64, la voix choisie surlignée) · **Ondes** (le cadran radio,
  l'anneau qui se remplit) · **Crédits** (auteur, licence de ce qui joue).
- **Menus** (boutons de piste 1-4, comme Loopex) : Atlas · Sources · Ondes · Réglages. ≡ →
  Sessions. Back seul = suspendre (les voix continuent), sortie complète = Shift + Volume + Jog.

## 11. Mémoire vive, fils d'exécution, contraintes

| Élément | Taille | Note |
|---|---|---|
| 16 voix × 30 s × stéréo × int16 | ≈ 85 Mo | **[À DÉCIDER]** 30 s proposé ; Loopex prend 127 Mo pour 45 s. Échelle de repli si `calloc` échoue. |
| 16 tampons d'arrivée (pour l'échange sans clic) | 16 × 30 s mono float au pire, ou un seul partagé | un seul tampon d'arrivée partagé suffit (une récolte à la fois) : ≈ 5 Mo |
| Anneaux radio : 2 × 60 s stéréo int16 | ≈ 21 Mo | |
| Echo Pool, gestes, divers | ≈ 5 Mo | |
| **Total** | **≈ 115 Mo** | à comparer au MemAvailable mesuré à l'étape 0 |

- **Audio** (`render_block`) : aucune allocation, aucun fichier, aucun verrou, aucun journal.
  Les nouvelles récoltes ne remplacent une bande **qu'entre deux notes** (la voix garde son
  tampon tant qu'elle joue).
- **Travailleurs** : `SCHED_OTHER`, cœurs 0-2 (le 3 est celui du SPI) : récolte, ondes (1 par
  radio), disque. Tous joints dans `destroy_instance`, avec un arrêt borné (un `curl` bloqué
  ne doit jamais geler la sortie du module : on tue son processus).
- **Réseau** : `curl` par `popen`, comme aujourd'hui, si l'étape 0 confirme qu'il est là et
  parle HTTPS. Sinon, deux pistes : embarquer un `curl` statique ARM64 dans le module, ou
  compiler un client HTTPS minimal (BearSSL, licence MIT) dans le `dsp.so`. **[Dépend de
  l'étape 0]**
- **Clé Freesound** : `UserLibrary/Fouille/freesound.key` (hors du dossier du module).

## 12. La carte web

Une application statique (HTML / JS, sans étape de construction si possible), un seul code :

- **Ouverte n'importe où** (github.io) : la carte du Québec, les lieux et leurs textes, les
  sons géolocalisés d'Aporee et de Freesound jouables dans le navigateur, les radios
  écoutables, les itinéraires dessinés. C'est l'atlas sonore public.
- **Ouverte depuis le Move** (servie par schwung-manager, comme le Corpus Builder de Concat) :
  en plus, elle **écrit l'atlas** sur le Move (routes `/files` du manager, vérifiées pour
  Concat) et **lit l'état** de l'instrument dans un `statut.json` que le module écrit chaque
  seconde (qui joue, quoi, d'où). On y prépare un concert : on choisit les lieux, on écoute les
  radios avant de les retenir, on compose des itinéraires.
- **Fond de carte** : à choisir pour sa licence et ses conditions d'usage (tuiles
  OpenStreetMap avec attribution, ou un fond vectoriel libre). **[À vérifier]**
- **À vérifier dans le navigateur** : que les fichiers d'Internet Archive, les aperçus de
  Freesound et les flux radio se laissent lire par une page d'un autre domaine (CORS). Lire un
  flux dans une balise `<audio>` marche presque toujours ; l'analyser demande le CORS.

## 13. Droits et égards

- Les fragments CC gardent leur fiche ; l'écran Crédits et l'export la rendent visible.
- Les radios : jouées en direct comme matière (le geste d'*Imaginary Landscape*). Publier un
  enregistrement d'une pièce qui en contient est la responsabilité de l'artiste ; Fouille aide
  en gardant la trace de ce qui a été capté, et quand.
- Les voix des gens : ce sont des voix publiques (des émissions, des dépôts volontaires), mais
  ce sont des voix. L'instrument les montre avec leur nom, il ne les rend pas anonymes.

## 14. Étapes

| Étape | Contenu | Se teste par |
|---|---|---|
| **0** | `fouille_probe` + `netcheck.sh` (fait) : curl, HTTPS, Aporee, Freesound GPS, radios MP3, décodage | un journal rapporté du Move |
| **1** | ce document | votre relecture |
| **2** | moteur portable `engine/` (testé hors ligne : rendu WAV + vérifications) + hôte Overtake (`ui.js` + `dsp.so`), moteur v0.2 porté, 16 voix, bogues de §3 corrigés, gestes de gauche | sur le Move : pads, écran, son sans réseau (mémoire / synthèse) |
| **3** | atlas : format, relecture à chaud, Montréal (quartiers, coordonnées), Freesound par GPS, Aporee | une voix ancrée au Plateau qui récolte au Plateau |
| **4** | carte web : version publique, puis version Move (écrit l'atlas, lit le statut) | la page ouverte sur github.io et depuis le Move |
| **5** | moteur ondes : radios MP3, anneau, capter / écouter, syntonisation | une radio montréalaise captée dans une voix |
| **6** | mémoire sur disque + crédits, micro (Ici), gestes de droite | un concert sans wifi |
| **7** | Saguenay, Kamouraska, puis carte libre et cartes sonores locales | |

## 16. Vers Pandore et *Impressions*

Décidé le 2026-11-14 : après le Move, Fouille doit pouvoir tourner sur Pandore, sur la même
base que Res, Phasma et Essaim, comme **un firmware au choix** d'une boîte de l'œuvre. Dans
une logique in situ, il fait entendre **ce que l'internet connaît de l'environnement autour
de la salle**, à côté de ce que le micro entend dedans.

**Moteur ≠ hôte, dès l'étape 2** (la règle de Res, Phasma, Essaim et RECORDA) :

```
engine/   C99 pur, aucune dépendance (libm, pthreads). Voix, LPG, gestes, atlas, mémoire,
          travailleurs réseau (curl par popen). Fréquence et taille de bloc en paramètre :
          44,1 kHz / 128 sur le Move, 48 kHz sur Pandore. Interface en événements de contrôle
          normalisés (0..1), comme le MCU de Pandore.
tests/    rendu WAV + vérifications sur des RELATIONS (méthode audioapp), sans réseau
          (des fragments de test sur disque).
move/     dsp.so (move_plugin_init_v2) + ui.js Overtake : les 32 pads, les pages, l'écran.
pandore/  (plus tard) miniaudio duplex, le panneau web, file SPSC, net/coord.c et
          net/audionet.c copiés tels quels de Phasma, config TOML relue à chaud.
```

**Deux visages, un moteur.** La profondeur du Move (16 voix, 16 gestes, pages) ne passe pas
telle quelle sur le panneau de Pandore (K1-K4, deux pads XY, fader, inclinaison, tapes sur
les côtés, encodeur, capteur de présence). La version Pandore est **simplexifiée** : elle se
joue en 20 secondes. À concevoir à l'étape Pandore ; une première idée : le pad gauche =
proche ↔ loin (rayon autour d'Ici) × présent ↔ retenu (radio en direct ↔ mémoire), le pad
droit = les gestes, le capteur de présence = un visiteur qui s'approche fait venir les voix
les plus proches.

**« Ici »** : à l'installation, la géolocalisation par IP propose un point (souvent faux de
quelques km : c'est la ville du fournisseur), vous le corrigez sur la carte web. L'atlas
garde ce lieu ; les sons géolocalisés, les radios locales et les cartes sonores autour de lui
deviennent la matière principale.

**Dans l'écologie** : Fouille prend la niche de **la voix d'ailleurs** sur le bus de
coordination (le régulateur d'énergie lui laisse la place des voix parlées et des textures
lointaines, et le rythme et le grave à Phasma).

**Concat et Loopex sur Pandore** : à décider plus tard, séparément. Fouille sert de premier
cas pour la séparation moteur / hôte d'un module Move.

## 15. Questions ouvertes

1. **Longueur d'une voix** : 30 s stéréo (≈ 85 Mo pour 16) ? Plus court laisse de la place aux
   radios ; plus long fait des voix qui racontent.
2. **Gestes de droite** (§9) : la liste vous parle-t-elle ? Lesquels manquent, lesquels sont de
   trop ?
3. **Radios** : lesquelles comptent pour vous (Montréal, Saguenay, Kamouraska) ? Je ne les
   choisis pas à votre place.
4. **Langue de l'interface** (écran du Move et carte) : français seulement, ou bilingue ?
5. **Le nom des choses** : « voix », « ondes », « usure », « mémoire », « Ici »… à garder,
   changer ?
6. **Le site public** : sous `filliformes.github.io/fouille` (le dépôt `fouille-move` devient
   public ?) ou un dépôt séparé comme `recorda-manual` ?
