# ESP-Claw — New Capabilities: MCP Server, MCP Bridge & SSH Console

This document describes the capabilities added to `edge_agent` in this development cycle:

1. **`cap_mcp_server`** — a Model Context Protocol (MCP) server, configurable from the web UI.
2. **`cap_mcp_bridge`** — exposes `claw_cap` capabilities to any connected MCP client.
3. **`cap_ssh`** — a network-reachable SSH console, configurable from the web UI.

Available in three languages: [English](#english) · [العربية](#العربية) · [Français](#français)

---

## English

### 1. MCP Server (`cap_mcp_server`)

Starts an MCP-protocol HTTP server on the device, advertised over mDNS, so any MCP client (an AI
agent, an IDE integration, an inspector tool) can discover and talk to it.

**Enable:** Kconfig `CONFIG_APP_CLAW_CAP_MCP_SERVER` (default `y`).

**Configure from the web UI:** *System Settings → MCP Server*
| Field | Meaning | Default |
|---|---|---|
| Enable MCP server | Starts the server at boot | on |
| mDNS hostname | Advertised as `<hostname>.local` | `esp-claw` |
| Instance name | Human-readable mDNS instance name | `ESP-Claw` |
| HTTP endpoint path | e.g. `mcp` → `http://<host>:<port>/mcp` | `mcp` |
| Server port | MCP HTTP port | `18791` |
| Control port | Internal `esp_http_server` control port | `18792` |

Changing any field requires a device restart to apply.

**Security note:** this server has **no authentication of its own**. Anyone who can reach the
device's IP and port can open an MCP session. Only expose it on a trusted network (LAN, or a VPN
tunnel — see §3.4 below). The actual capability-level authorization is enforced by the MCP Bridge
(next section), not by the server itself.

### 2. MCP Bridge (`cap_mcp_bridge`)

The MCP server above starts empty — it speaks the protocol but has no tools. `cap_mcp_bridge`
registers two generic tools so any MCP client can reach the device's existing `claw_cap`
capabilities (the same capabilities the LLM agent and the serial console use):

- **`claw_list`** — no arguments. Returns the list of capabilities reachable via `claw_call`
  (name + description each).
- **`claw_call`** — `{ "name": "<capability>", "args": "<json>" }`. Invokes that capability and
  returns its output as text.

**Why only two tools, not one MCP tool per capability:** the MCP SDK used here has no per-tool
context/userdata in its callback signature, so a true 1:1 mapping would need a large table of
near-identical trampoline functions. Two generic tools, forwarding to `claw_cap`'s own dispatch,
cover the same ground with far less code.

**Security model — read this before exposing the MCP server on any network:** because the MCP
server has no login of its own, both `claw_list` and `claw_call` run with the same trust level as
an **untrusted, remote MQTT command** (`CLAW_CAP_CALLER_SUB_AGENT` — the least-privileged caller
class). This means:
- Ordinary tools (status queries, `get_current_time`, etc.) work normally.
- Anything marked `ROOT_AGENT_ONLY` or `RESTRICTED` (`ssh_configure`, `ota_update`,
  `wireguard_configure`, `mqtt_configure`, …) is **rejected** with *"not exposed to the LLM"* —
  exactly the same rule already enforced for inbound MQTT commands, applied here to close the same
  class of problem before it could ever be reopened.

### 3. SSH Console (`cap_ssh`)

A small, network-reachable SSH server (port 22) built on wolfSSH, for direct interactive access to
the device's capabilities from a real SSH client.

**Enable:** Kconfig `CONFIG_APP_CLAW_CAP_SSH` (default `n` — opt-in, pulls in a crypto/SSH
dependency) **and** `CONFIG_ESP_ENABLE_WOLFSSH` (wolfSSL's own Kconfig option, also required).

#### 3.1 Configure from the web UI

*System Settings → SSH*
| Field | Meaning |
|---|---|
| Enable SSH server | Starts the server at boot |
| Host private key (base64 DER) | The device's own SSH host identity key |
| Authorized client public key | The one client allowed to connect |

Generate a host key and convert it to the format this field expects:
```bash
ssh-keygen -m PEM -t ecdsa -f hostkey -N ""
openssl ec -in hostkey -outform DER | base64 -w0
```
Generate a client key and paste its `.pub` line into "Authorized client public key":
```bash
ssh-keygen -t ecdsa -b 256 -f clientkey -N ""
```

**Key type constraint: RSA or ECDSA only — not ed25519.** This wolfSSH build defines `HAVE_ED25519`
for the underlying crypto library but not the three extra macros wolfSSH itself needs to accept
ed25519 for *user authentication* (`WOLFSSL_ED25519_STREAMING_VERIFY`, `HAVE_ED25519_KEY_IMPORT`,
`HAVE_ED25519_KEY_EXPORT`). An ed25519 client key is silently rejected before authentication is
ever attempted. Use `-t ecdsa` (as above) or `-t rsa`.

Changing a key on an **already-running** server needs a restart to take effect.

#### 3.2 Authentication and trust model

- **Public-key authentication only** — no passwords.
- Authentication requires proving possession of the private key matching the one authorized public
  key. Only after that handshake succeeds does the shell become reachable.
- Inside the shell, commands run at the **same trust level as physical serial-console access**
  (`CLAW_CAP_CALLER_CONSOLE`) — this bypasses the `RESTRICTED`/`ROOT_AGENT_ONLY` gate entirely.
  That is intentional and safe *because* reaching the shell already required a successful
  public-key handshake — unlike the MCP bridge above, which has no such gate and is therefore
  restricted to sub-agent-level trust instead.

#### 3.3 Using the console

Not a proxy for the full serial REPL — a small purpose-built shell:
```
help                       List available commands
list                       List all claw_cap capabilities (name, group, description)
groups                     List capability groups and their state
call <name> <json>         Invoke a capability, e.g.: call get_current_time {}
exit  (or: quit)           Close the session cleanly
```
Example session:
```bash
ssh -i clientkey <user>@<device-ip>
> call get_current_time {}
2026-09-18 00:02:07 CET (Friday)
> exit
Goodbye.
```

#### 3.4 Remote access over a VPN (Tailscale or WireGuard)

The SSH server listens on **all interfaces** (`0.0.0.0`), so it becomes reachable through any
active tunnel automatically — no extra device-side configuration is needed once a VPN is up:

- **Tailscale gateway mode** (`cap_vpn`, mode `tailscale-gateway`): the device stays a plain LAN
  client; a Linux subnet-router elsewhere on the LAN, joined to your tailnet, advertises the LAN's
  subnet. Two steps outside this device: (1) on that gateway machine, run
  `tailscale up --advertise-routes=<lan-subnet>/24`; (2) in the Tailscale admin console, approve
  that subnet route for the gateway machine. Once your remote client accepts routes on the same
  tailnet, `ssh -i clientkey <user>@<device-lan-ip>` works from anywhere.
- **WireGuard mode** (`cap_vpn`, mode `wireguard`): the device runs its own on-device tunnel and
  gets a tunnel IP directly; SSH to that tunnel IP instead of the LAN IP, once `wireguard_configure`
  is set up with a real peer/server and `vpn_connect` brings the tunnel up.

Neither path needs new code — both VPN modes were already implemented before this cycle; this
section only documents that SSH rides on top of them for free.

---

## العربية

### 1. خادم MCP (`cap_mcp_server`)

يشغّل خادم HTTP يتحدث بروتوكول MCP على الجهاز، ويُعلن عنه عبر mDNS، بحيث يستطيع أي عميل MCP (وكيل
ذكاء اصطناعي، تكامل IDE، أداة فحص) اكتشافه والتواصل معه.

**التفعيل:** خيار Kconfig باسم `CONFIG_APP_CLAW_CAP_MCP_SERVER` (مفعّل افتراضيًا `y`).

**الإعداد من واجهة الويب:** *إعدادات النظام ← MCP Server*
| الحقل | المعنى | القيمة الافتراضية |
|---|---|---|
| تفعيل خادم MCP | يشغّل الخادم عند الإقلاع | مفعّل |
| اسم مضيف mDNS | يُعلن عنه كـ `<hostname>.local` | `esp-claw` |
| اسم النسخة | اسم mDNS قابل للقراءة البشرية | `ESP-Claw` |
| مسار HTTP | مثلاً `mcp` ← `http://<host>:<port>/mcp` | `mcp` |
| منفذ الخادم | منفذ HTTP الخاص بـ MCP | `18791` |
| منفذ التحكم | منفذ التحكم الداخلي لـ `esp_http_server` | `18792` |

تغيير أي حقل يتطلب إعادة تشغيل الجهاز ليصبح ساري المفعول.

**ملاحظة أمنية:** هذا الخادم **بلا أي مصادقة خاصة به**. أي جهة تستطيع الوصول إلى عنوان ومنفذ الجهاز
يمكنها فتح جلسة MCP. لا تعرّضه إلا على شبكة موثوقة (شبكة محلية، أو عبر نفق VPN — انظر §3.4 أدناه).
التفويض الفعلي على مستوى القدرات (capabilities) يُفرض عبر جسر MCP (القسم التالي)، وليس عبر الخادم
نفسه.

### 2. جسر MCP (`cap_mcp_bridge`)

خادم MCP أعلاه يبدأ فارغًا — يتحدث البروتوكول لكن بلا أي أدوات. يسجّل `cap_mcp_bridge` أداتين
عامّتين حتى يستطيع أي عميل MCP الوصول إلى قدرات `claw_cap` الموجودة في الجهاز (نفس القدرات التي
يستخدمها وكيل LLM وكونسول التسلسل):

- **`claw_list`** — بلا معطيات. يرجع قائمة القدرات القابلة للاستدعاء عبر `claw_call` (الاسم
  والوصف لكل منها).
- **`claw_call`** — `{ "name": "<اسم القدرة>", "args": "<json>" }`. يستدعي تلك القدرة ويرجع
  مخرجاتها كنص.

**لماذا أداتان فقط وليست أداة MCP لكل قدرة:** مكتبة MCP المستخدمة هنا لا تحمل أي سياق/بيانات خاصة
بكل أداة داخل توقيع دالة الاستدعاء (callback)، فربط كل قدرة بأداة MCP خاصة بها كان سيتطلب جدولًا
ضخمًا من دوال "تحويل" شبه متطابقة. أداتان عامّتان، تُحيلان إلى نظام التوزيع (dispatch) الخاص بـ
`claw_cap` نفسه، تغطيان نفس الوظيفة بكود أقل بكثير.

**نموذج الأمان — اقرأ هذا قبل تعريض خادم MCP على أي شبكة:** بما أن خادم MCP بلا أي تسجيل دخول خاص
به، فإن كلًا من `claw_list` و`claw_call` يعملان بنفس مستوى الثقة الممنوح **لأمر MQTT وارد غير
موثوق** (`CLAW_CAP_CALLER_SUB_AGENT` — أقل فئة صلاحية). يعني هذا:
- الأدوات العادية (استعلامات الحالة، `get_current_time`، إلخ) تعمل بشكل طبيعي.
- أي أداة موسومة بـ `ROOT_AGENT_ONLY` أو `RESTRICTED` (مثل `ssh_configure`، `ota_update`،
  `wireguard_configure`، `mqtt_configure`، ...) **تُرفض** برسالة *"not exposed to the LLM"* — تمامًا
  نفس القاعدة المُطبّقة أصلاً على أوامر MQTT الواردة، طُبّقت هنا لإغلاق نفس فئة المشكلة قبل أن تُفتح
  من جديد.

### 3. كونسول SSH (`cap_ssh`)

خادم SSH صغير قابل للوصول عبر الشبكة (المنفذ 22) مبني على مكتبة wolfSSH، للوصول التفاعلي المباشر
إلى قدرات الجهاز من عميل SSH حقيقي.

**التفعيل:** خيار Kconfig باسم `CONFIG_APP_CLAW_CAP_SSH` (معطّل افتراضيًا `n` — اختياري، يُدخل
تبعية تشفير/SSH جديدة) **و**أيضًا `CONFIG_ESP_ENABLE_WOLFSSH` (خيار Kconfig خاص بـ wolfSSL، مطلوب
أيضًا).

#### 3.1 الإعداد من واجهة الويب

*إعدادات النظام ← SSH*
| الحقل | المعنى |
|---|---|
| تفعيل خادم SSH | يشغّل الخادم عند الإقلاع |
| المفتاح الخاص للمضيف (base64 DER) | مفتاح هوية SSH الخاص بالجهاز نفسه |
| المفتاح العام للعميل المُصرَّح به | العميل الوحيد المسموح له بالاتصال |

توليد مفتاح المضيف وتحويله للصيغة التي يتوقعها هذا الحقل:
```bash
ssh-keygen -m PEM -t ecdsa -f hostkey -N ""
openssl ec -in hostkey -outform DER | base64 -w0
```
توليد مفتاح العميل ولصق سطر `.pub` الخاص به في حقل "المفتاح العام المُصرَّح به":
```bash
ssh-keygen -t ecdsa -b 256 -f clientkey -N ""
```

**قيد نوع المفتاح: RSA أو ECDSA فقط — وليس ed25519.** هذا البناء من wolfSSH يُعرّف `HAVE_ED25519`
لمكتبة التشفير الأساسية، لكن ليس الماكروهات الثلاثة الإضافية التي تحتاجها wolfSSH نفسها لقبول
ed25519 في *مصادقة المستخدم* (`WOLFSSL_ED25519_STREAMING_VERIFY`، `HAVE_ED25519_KEY_IMPORT`،
`HAVE_ED25519_KEY_EXPORT`). أي مفتاح عميل من نوع ed25519 يُرفض بصمت قبل حتى محاولة المصادقة.
استخدم `-t ecdsa` (كما أعلاه) أو `-t rsa`.

تغيير مفتاح على خادم **يعمل بالفعل** يتطلب إعادة تشغيل ليصبح ساري المفعول.

#### 3.2 نموذج المصادقة والثقة

- **مصادقة بالمفتاح العام فقط** — بلا كلمات مرور.
- المصادقة تتطلب إثبات امتلاك المفتاح الخاص المطابق للمفتاح العام المُصرَّح به الوحيد. فقط بعد نجاح
  تلك المصافحة يصبح الـ shell قابلاً للوصول.
- داخل الـ shell، الأوامر تعمل بنفس **مستوى الثقة الممنوح للوصول الفعلي عبر المنفذ التسلسلي**
  (`CLAW_CAP_CALLER_CONSOLE`) — يتجاوز هذا حاجز `RESTRICTED`/`ROOT_AGENT_ONLY` كليًا. هذا مقصود
  وآمن *لأن* الوصول إلى الـ shell تطلّب أصلاً مصافحة ناجحة بالمفتاح العام — بخلاف جسر MCP أعلاه الذي
  لا يملك هذا الحاجز، ولذلك اقتصر على ثقة مستوى sub-agent بدلاً من ذلك.

#### 3.3 استخدام الكونسول

ليس بديلاً عن الكونسول التسلسلي الكامل — بل shell صغير مُخصَّص:
```
help                       عرض الأوامر المتاحة
list                       عرض كل قدرات claw_cap (الاسم، المجموعة، الوصف)
groups                     عرض مجموعات القدرات وحالتها
call <name> <json>         استدعاء قدرة، مثال: call get_current_time {}
exit  (أو: quit)           إغلاق الجلسة بشكل نظيف
```
مثال جلسة:
```bash
ssh -i clientkey <user>@<device-ip>
> call get_current_time {}
2026-09-18 00:02:07 CET (Friday)
> exit
Goodbye.
```

#### 3.4 الوصول عن بُعد عبر VPN (Tailscale أو WireGuard)

خادم SSH يستمع على **كل الواجهات** (`0.0.0.0`)، فيصبح متاحًا تلقائيًا عبر أي نفق فعّال — بلا أي
إعداد إضافي على الجهاز بمجرد تشغيل VPN:

- **وضع بوابة Tailscale** (`cap_vpn`، الوضع `tailscale-gateway`): يبقى الجهاز عميل شبكة محلية عادي؛
  و subnet-router على نفس الشبكة المحلية، منضم إلى شبكة tailnet الخاصة بك، يُعلن عن الشبكة الفرعية
  المحلية. خطوتان خارج هذا الجهاز: (1) على جهاز البوابة ذاك، شغّل
  `tailscale up --advertise-routes=<lan-subnet>/24`؛ (2) في لوحة تحكم Tailscale، اعتمد ذلك المسار
  الفرعي لجهاز البوابة. بمجرد أن يقبل عميلك البعيد المسارات على نفس tailnet، يعمل
  `ssh -i clientkey <user>@<device-lan-ip>` من أي مكان.
- **وضع WireGuard** (`cap_vpn`، الوضع `wireguard`): يشغّل الجهاز نفقه الخاص به على الجهاز مباشرة
  ويحصل على IP نفق مباشرة؛ اتصل بـ SSH على IP النفق ذاك بدلاً من IP الشبكة المحلية، بمجرد إعداد
  `wireguard_configure` بخادم/نظير حقيقي وتفعيل النفق عبر `vpn_connect`.

كلا المسارين لا يحتاجان أي كود جديد — كلا وضعي VPN كانا مُنفَّذين بالفعل قبل هذه الدورة؛ هذا القسم
يوثّق فقط أن SSH يستفيد منهما مباشرة دون أي عمل إضافي.

---

## Français

### 1. Serveur MCP (`cap_mcp_server`)

Démarre un serveur HTTP parlant le protocole MCP (Model Context Protocol) sur l'appareil, annoncé
via mDNS, afin que tout client MCP (agent IA, intégration IDE, outil d'inspection) puisse le
découvrir et communiquer avec lui.

**Activation :** option Kconfig `CONFIG_APP_CLAW_CAP_MCP_SERVER` (activée par défaut, `y`).

**Configuration depuis l'interface web :** *Paramètres système → MCP Server*
| Champ | Signification | Valeur par défaut |
|---|---|---|
| Activer le serveur MCP | Démarre le serveur au boot | activé |
| Nom d'hôte mDNS | Annoncé comme `<hostname>.local` | `esp-claw` |
| Nom d'instance | Nom mDNS lisible | `ESP-Claw` |
| Chemin du point de terminaison HTTP | ex. `mcp` → `http://<host>:<port>/mcp` | `mcp` |
| Port du serveur | Port HTTP du serveur MCP | `18791` |
| Port de contrôle | Port de contrôle interne `esp_http_server` | `18792` |

Modifier un champ nécessite un redémarrage de l'appareil pour être appliqué.

**Remarque de sécurité :** ce serveur **n'a aucune authentification propre**. Quiconque peut
atteindre l'adresse IP et le port de l'appareil peut ouvrir une session MCP. Ne l'exposez que sur
un réseau de confiance (LAN, ou un tunnel VPN — voir §3.4 ci-dessous). L'autorisation réelle au
niveau des capacités est appliquée par le pont MCP (section suivante), pas par le serveur
lui-même.

### 2. Pont MCP (`cap_mcp_bridge`)

Le serveur MCP ci-dessus démarre vide — il parle le protocole mais n'a aucun outil.
`cap_mcp_bridge` enregistre deux outils génériques afin que tout client MCP puisse accéder aux
capacités `claw_cap` existantes de l'appareil (les mêmes capacités utilisées par l'agent LLM et la
console série) :

- **`claw_list`** — sans argument. Retourne la liste des capacités accessibles via `claw_call`
  (nom + description pour chacune).
- **`claw_call`** — `{ "name": "<capacité>", "args": "<json>" }`. Invoque cette capacité et
  retourne sa sortie sous forme de texte.

**Pourquoi seulement deux outils, et non un outil MCP par capacité :** le SDK MCP utilisé ici ne
transporte aucun contexte/donnée propre à chaque outil dans la signature de son callback ; une
correspondance stricte 1 pour 1 aurait nécessité une grande table de fonctions « trampoline »
quasi identiques. Deux outils génériques, redirigeant vers le mécanisme de dispatch propre à
`claw_cap`, couvrent le même terrain avec beaucoup moins de code.

**Modèle de sécurité — à lire avant d'exposer le serveur MCP sur un réseau :** comme le serveur MCP
n'a pas de connexion propre, `claw_list` et `claw_call` s'exécutent tous deux avec le même niveau
de confiance qu'une **commande MQTT distante non fiable** (`CLAW_CAP_CALLER_SUB_AGENT` — la classe
d'appelant la moins privilégiée). Cela signifie :
- Les outils ordinaires (requêtes de statut, `get_current_time`, etc.) fonctionnent normalement.
- Tout ce qui est marqué `ROOT_AGENT_ONLY` ou `RESTRICTED` (`ssh_configure`, `ota_update`,
  `wireguard_configure`, `mqtt_configure`, …) est **rejeté** avec *« not exposed to the LLM »* —
  exactement la même règle déjà appliquée aux commandes MQTT entrantes, reprise ici pour fermer la
  même classe de problème avant qu'elle ne puisse être rouverte.

### 3. Console SSH (`cap_ssh`)

Un petit serveur SSH accessible sur le réseau (port 22), construit sur wolfSSH, pour un accès
interactif direct aux capacités de l'appareil depuis un vrai client SSH.

**Activation :** option Kconfig `CONFIG_APP_CLAW_CAP_SSH` (désactivée par défaut, `n` — opt-in,
ajoute une dépendance crypto/SSH) **et** `CONFIG_ESP_ENABLE_WOLFSSH` (propre option Kconfig de
wolfSSL, également requise).

#### 3.1 Configuration depuis l'interface web

*Paramètres système → SSH*
| Champ | Signification |
|---|---|
| Activer le serveur SSH | Démarre le serveur au boot |
| Clé privée hôte (DER en base64) | La propre clé d'identité SSH de l'appareil |
| Clé publique client autorisée | Le seul client autorisé à se connecter |

Générer une clé hôte et la convertir au format attendu par ce champ :
```bash
ssh-keygen -m PEM -t ecdsa -f hostkey -N ""
openssl ec -in hostkey -outform DER | base64 -w0
```
Générer une clé client et coller sa ligne `.pub` dans « Clé publique client autorisée » :
```bash
ssh-keygen -t ecdsa -b 256 -f clientkey -N ""
```

**Contrainte de type de clé : RSA ou ECDSA uniquement — pas ed25519.** Cette version de wolfSSH
définit `HAVE_ED25519` pour la bibliothèque crypto sous-jacente, mais pas les trois macros
supplémentaires dont wolfSSH lui-même a besoin pour accepter ed25519 en *authentification
utilisateur* (`WOLFSSL_ED25519_STREAMING_VERIFY`, `HAVE_ED25519_KEY_IMPORT`,
`HAVE_ED25519_KEY_EXPORT`). Une clé client ed25519 est silencieusement rejetée avant même toute
tentative d'authentification. Utilisez `-t ecdsa` (comme ci-dessus) ou `-t rsa`.

Changer une clé sur un serveur **déjà en cours d'exécution** nécessite un redémarrage pour prendre
effet.

#### 3.2 Modèle d'authentification et de confiance

- **Authentification par clé publique uniquement** — pas de mots de passe.
- L'authentification exige de prouver la possession de la clé privée correspondant à l'unique clé
  publique autorisée. Ce n'est qu'après le succès de cette poignée de main que le shell devient
  accessible.
- À l'intérieur du shell, les commandes s'exécutent avec le **même niveau de confiance qu'un accès
  physique à la console série** (`CLAW_CAP_CALLER_CONSOLE`) — cela contourne entièrement la barrière
  `RESTRICTED`/`ROOT_AGENT_ONLY`. C'est intentionnel et sûr *parce que* atteindre le shell a déjà
  nécessité une poignée de main par clé publique réussie — contrairement au pont MCP ci-dessus, qui
  n'a pas une telle barrière et se limite donc à une confiance de niveau sub-agent.

#### 3.3 Utilisation de la console

Ce n'est pas un proxy vers le REPL série complet — un petit shell dédié :
```
help                       Lister les commandes disponibles
list                       Lister toutes les capacités claw_cap (nom, groupe, description)
groups                     Lister les groupes de capacités et leur état
call <name> <json>         Invoquer une capacité, ex. : call get_current_time {}
exit  (ou : quit)          Fermer la session proprement
```
Exemple de session :
```bash
ssh -i clientkey <user>@<device-ip>
> call get_current_time {}
2026-09-18 00:02:07 CET (Friday)
> exit
Goodbye.
```

#### 3.4 Accès distant via VPN (Tailscale ou WireGuard)

Le serveur SSH écoute sur **toutes les interfaces** (`0.0.0.0`), il devient donc automatiquement
accessible via tout tunnel actif — sans configuration supplémentaire côté appareil une fois le VPN
en place :

- **Mode passerelle Tailscale** (`cap_vpn`, mode `tailscale-gateway`) : l'appareil reste un simple
  client LAN ; un routeur de sous-réseau Linux ailleurs sur le LAN, rejoint à votre tailnet,
  annonce le sous-réseau du LAN. Deux étapes en dehors de cet appareil : (1) sur cette machine
  passerelle, exécuter `tailscale up --advertise-routes=<lan-subnet>/24` ; (2) dans la console
  d'administration Tailscale, approuver cette route de sous-réseau pour la machine passerelle. Une
  fois que votre client distant accepte les routes sur le même tailnet,
  `ssh -i clientkey <user>@<device-lan-ip>` fonctionne depuis n'importe où.
- **Mode WireGuard** (`cap_vpn`, mode `wireguard`) : l'appareil exécute son propre tunnel
  directement sur l'appareil et obtient une IP de tunnel ; connectez-vous en SSH à cette IP de
  tunnel plutôt qu'à l'IP LAN, une fois `wireguard_configure` renseigné avec un vrai pair/serveur
  et le tunnel activé via `vpn_connect`.

Aucun des deux chemins ne nécessite de nouveau code — les deux modes VPN étaient déjà implémentés
avant ce cycle ; cette section documente seulement que SSH en profite gratuitement.
