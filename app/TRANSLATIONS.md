# Translations: notes for reviewers

Every user-visible string of vita-turbo-vpn is translated into the
Vita's 20 system languages, in the order of `SCE_SYSTEM_PARAM_LANG`.
None has been checked by a native speaker yet. This file lists, per
language, what a native speaker should look at first.

| Where | Strings | Source |
|---|---|---|
| App (`VPNT00001`) | 284 per language (302 ids; the rest are names, file names, symbols) | `src/strings_en.def` (English), `src/lang/<code>.def` |
| Settings page, quick menu, notification | 27 per language | `../tools/gen_i18n.py` (generates `../settings_app/i18n_table.h`) |

Codes: `ja` 0, `en` 1 (US), `fr` 2, `es` 3 (Spain), `de` 4, `it` 5, `nl` 6,
`pt` 7 (Portugal), `ru` 8, `ko` 9, `zht` 10 (Traditional, Taiwan usage),
`zhs` 11 (Simplified), `fi` 12, `sv` 13, `da` 14, `no` 15 (Bokmål), `pl` 16,
`ptbr` 17, `engb` 18, `tr` 19.

## Ground rules used everywhere

- Shared concepts reuse Wi-Fi Turbo's wording (Settings, Wi-Fi, quick menu,
  on/off, profile) and the Vita's own page names (Settings › Network). German
  says WLAN and uses du, as Wi-Fi Turbo does.
- The app and the Settings page / quick menu use the same words for the same
  states (Off, Connecting, Connected, Paused, Blocked, kill switch).
- Kept verbatim: VPN, DNS, IP, PSN, PS Vita, Vita,
  FTP, USB, MTU, `.conf`, `ur0:tai/config.txt`, `*KERNEL`, `*main`,
  `profiles/`. No VPN provider is named in any language. The app name
  "Vita Turbo VPN" is not translated.
- "Kill switch" stays in Latin letters in the European languages, Russian
  and Turkish (German writes "Kill Switch"); Japanese キルスイッチ, Korean
  킬 스위치, Chinese 斷網保護 / 断网保护 (descriptive, since there is no
  established term): **review first** in Chinese.
- Languages with several plural forms (ru, pl, fi, tr) use a "label: {0}"
  construction for counts so one form fits any number.
- QR import: the hint "Scan QR" is short because it sits in
  the Profiles footer beside a START key cap (Russian "Считать QR",
  "Сканировать QR" did not fit); QR stays "QR" everywhere except Simplified
  Chinese, which says 二维码. "Saved as {0}" / "Already saved as {0}" take a
  file name. The invalid-profile strings no longer name a key: the parser's
  own message (English, with the line and key) is shown under them.
- SceShell plugin missing: "The quick menu plugin is not
  loaded" under the Not installed status word, "install the plugin to
  connect" on the pipe, and S_MODULE_LOADED ("Loaded") on the kernel-module
  tile, separate from S_PLUGIN_LOADED so it can agree with "module" (Polish
  Załadowany, against Załadowana for the plugin). The plugin is named as in
  S_QUICK_MENU_PLUGIN. Polish S_STATE_NOT_INSTALLED "Brak modułu" now also
  stands over the plugin-missing line: check it still reads right.
- Lockdown (28 strings): the term is the Settings page's
  (`../tools/gen_i18n.py` SET_ROW_LOCKDOWN): Sperrmodus, Mode
  verrouillage, Modo bloqueo, Modalità blocco, Vergrendelmodus, Modo de
  bloqueio, Режим блокировки, ロックダウンモード, 잠금 모드, 鎖定模式 / 锁定模式,
  Lukitustila, Låsläge, Låsetilstand, Låsemodus, Tryb blokady, Kilit modu;
  shorter forms where space is tight (chip, tube, toggles). The recovery
  path in S_DLG_LD_TEXT uses each language's Settings › Network ›
  VPN Settings as in S_HELP_SET_TEXT. Shortened to fit: Russian
  Details hints "Блок. вкл." / "Блок. выкл." and S_HINT_LEAK_CHECK
  "Тест утечек" (was "Проверка утечек"; the screen title is unchanged);
  Japanese S_HELP_LOCKDOWN is terse (one line). **Review first**: the
  home tile pair (S_LOCKDOWN_HOWTO, S_LOCKDOWN_OFF_WHERE) and the dialog.
- Labels drawn in capitals are written in sentence case; the app capitalises
  them (Latin Extended-A and Cyrillic included; in Turkish i becomes İ except
  in the names Vita, Kill switch, Wi-Fi).
- Lengths were checked against the layout with `make shots-all` (no text
  runs out of its box or into other text in any language). Several strings
  were shortened to fit; they are listed below. Toasts and some error causes
  do not appear in the screenshots and were kept close to English length.
- Settings page and quick menu: Japanese ST_NOT_PROTECTED is 保護されていません
  while the app's big status word is the shorter 保護なし; the Settings hints
  say "the VPN app" literally. The quick-menu pause button is 400 px;
  the longest labels are Polish and Danish (22 characters): check on the
  console.

## Japanese (ja)
- S_STATE_NOT_PROTECTED is 保護なし (status word, 6-character limit), while the Settings page and quick menu say 保護されていません. Check that both read as the same state.
- ISP is プロバイダー throughout (S_IP_NOT_ISP プロバイダーとは別のアドレス, S_RES_ISP プロバイダー).
- DNS terms: lookups 名前解決, leak 漏れ (漏れチェック, DNS漏れ); split tunnel 分割トンネル rather than スプリットトンネル.
- Technical terms: handshake ハンドシェイク, endpoint 接続先 (not エンドポイント), keepalive キープアライブ, held back 保留.
- Shortened to fit: S_CAUSE_PORT drops "this network" (VPNのポートが遮断されている可能性); S_HINT_PAUSE 5分間停止; S_CAUSE_NO_ANSWER is a list (サーバー応答なし：鍵の誤り、サーバー停止、ポート遮断).
- Migrate is 移行 (Seamless シームレス); About is 情報.
- The leak-check paragraph breaks as …送られていま / す。: legal, but maybe rephrase.
- Plural pairs (_1/_N) use the same text.

## Korean (ko)
- ISP kept as ISP (not 인터넷 제공업체); "provider" in S_NO_PROFILES is 제공업체.
- Status words: 보호 안 됨, 연결 안 됨, 설치 안 됨; Traffic held is 트래픽 보류.
- Technical terms: handshake 핸드셰이크, endpoint 엔드포인트, keepalive 킵얼라이브, split tunnel 분할 터널, leak 유출.
- "Only {0}" is "{0} 전용" (also in S_SUB_ONLY): 192.168.1.0/24만 looked odd.
- S_LEAK_TEXT_LEAK starts 조회가… without "DNS", so 설정 › 네트워크 does not split across lines.
- Placeholders take 은(는) / 을(를) / (으)로 since the name or address is unknown.
- Kill switch hints are verbs (켜기 / 끄기); chips and toasts say 켜짐 / 꺼짐. Seamless is 끊김 없이; Migrate is 이동.
- Plural pairs (_1/_N) use the same text.

## German (de)
- Informal du and WLAN for Wi-Fi, as Wi-Fi Turbo; ISP is "Provider".
- "Kill Switch" with a capital S (German noun, and as on the Settings page); other languages write "Kill switch".
- Migrate is "Wechseln" ("Zu {0} wechseln", "Tunnel verlegen zu"): check it reads as moving the tunnel to another server.
- Timer labels "Verbunden seit" / "Versuch seit" / "Neuer Versuch in" / "Weiter in": "seit" before a duration is colloquial.
- Traffic held: status word "Angehalten", pipe node "Gehalten".
- How-to lines shortened to "Unter *KERNEL in ur0:tai/config.txt, dann Neustart".
- Handshake and Keepalive kept in English; endpoint "Endpunkt"; leak check "Leck-Test" (DNS-Leck / Kein Leck / Kein Test).

## Dutch (nl)
- Informal je, Wi-Fi kept, ISP "provider", quick menu "snelmenu" (Wi-Fi Turbo).
- Migrate "Migreren"; the footer hint is just "Naar {0}" to leave room for long names.
- Traffic held: status word "Tegengehouden" (at the size limit), pipe node "Vast".
- DNS lookups "opvragingen"; leak check "Lektest"; "Up for" is just "Verbonden".
- Handshake and Keepalive kept in English; endpoint "Eindpunt".
- Chip "klok niet ingesteld" is longer than the other chips; it fits but could be shorter.

## Polish (pl)
- Informal imperative as Wi-Fi Turbo; ISP "dostawca"; quick menu "szybkie menu".
- Counts use "label: {0}" ("pakiety: {0}", "Tylko sieci: {0}", "Znaleziono: {0}") to avoid plural forms; the _1 strings are singular.
- Chips abbreviated to fit: "kill switch wył." / "wł.", "brak szyb. menu", "zegar nieustaw.": check.
- "Wstrzymano" (paused) and "Ruch wstrzymany" (traffic held) share a verb; the Settings page has the same overlap.
- S_STATE_NOT_INSTALLED is "Brak modułu" (no module), not literally "not installed".
- Handshaking is "Uzgadnianie" in the tube and state, but "Handshake" in the row label and "handshake {0} s temu".
- Endpoint "Punkt końcowy", Keepalive kept, "split tunnelingiem" in one note, kernel module "Moduł jądra"; Migrate "Przenieś".
- "Powtórz test" for Run again (not "Uruchom ponownie", which reads as reboot).

## Russian (ru)
- Formal вы. ISP провайдер; handshake рукопожатие; endpoint Конечная точка; Keepalive kept in Latin.
- Split tunnel "Сплит-туннель" (status word; "Раздельный туннель" does not fit) and "сплит-туннелем" in the DNS note.
- Counts use the "сетей: {0}" form ("сетей в туннеле: {1}", "пакетов: {0}", "Пропущено строк: {0}"); the _1 forms are "{0} строка", "Пропущена {0} строка".
- Rescan is "Обновить" ("Пересканировать" too long); toast "Профили перечитаны".
- S_LEAK_CANT_CHECK is "Сбой проверки" (check failed): "Не удаётся проверить" is too long for the status word.
- Shortened: S_KS_OFF_NOTE "Включите, чтобы блокировать трафик без VPN" (original: while the tunnel is down); the module and plugin how-to lines.
- S_HELD_BACK_NOTE "Отключение или пауза пустят трафик напрямую"; S_TRYING_FOR "Пытаемся уже"; S_UP_FOR "Время работы"; S_HINT_CONNECT_ANYWAY "Всё же подключить".
- Display faces without Cyrillic hand the whole Russian text to the system font, so Latin words (VITA, Wi-Fi) match the Cyrillic beside them.

## Chinese, Traditional (zht, Taiwan usage)
- ISP 網路業者 (alternatives ISP, 電信業者).
- Handshake 交握; endpoint 端點; keepalive 保持連線 (can read as "stay connected").
- Split tunnel 分割隧道, matching 隧道 elsewhere; Taiwanese Windows says 分割通道.
- Kill switch 斷網保護 (descriptive; other apps keep "Kill Switch"); chips abbreviated 斷網保護關/開, 無快速選單.
- Kernel module 核心模組; quick-menu plugin 快速選單外掛程式.
- Traffic held 流量已攔住 / 已攔住, reusing 攔住 from the Settings page.

## Chinese, Simplified (zhs)
- ISP 运营商; handshake 握手; keepalive 保持连接 (保活 possible); split tunnel 分离隧道 (拆分隧道 also common).
- Memory card 记忆卡 as Wi-Fi Turbo; Sony mainland material often says 存储卡.
- Kill switch 断网保护; Blocked 已阻止; held 拦住.
- Own URL 自定义网址; generic 通用.

## Swedish (sv)
- ISP "internetleverantör" in sentences, "leverantör" elsewhere.
- S_HINT_NET_SETTINGS is just "Nätverk" ("Nätverksinställningar" too long).
- Handshake "handskakning" / "handskakar"; endpoint "Slutpunkt"; keepalive in English; split tunnel "delad tunnel".
- Leak check "Läcktest"; Rescan "Sök igen"; Migrate "Flytta"; Seamless "Sömlöst"; S_STATE_HELD "Trafik kvarhållen".

## Danish (da)
- ISP "internetudbyder" / "udbyder"; On/Off "Til" / "Fra".
- Handshake kept in English; endpoint "Endepunkt"; sleep "dvale"; Seamless "Problemfrit".
- Clash wording "Konflikt med Wi-Fi"; check the definite form "Vitaens dato".

## Norwegian Bokmål (no)
- Handshake "Håndtrykk" (a reviewer may prefer the English term).
- S_ROW_BLOCKED / S_ST_BLOCKED shortened to "Blokkert, kobler til", which could read as plain "connecting".
- ISP "internettleverandør" / "leverandøren din"; clock chip uses colloquial "klokka ikke satt"; leak check "Lekkasjetest".

## Finnish (fi)
- Number-independent forms: "vain verkot, määrä: {1}", "Vain verkot: {0}", "paketteja: {0}", "Muistikortilta löytyi {0}", "Siirrä: {0}", "Nyt: {0}".
- How-to lines shortened hard: "Lisää ur0:tai/config.txt:n *KERNEL:iin, käynnistä uudelleen".
- ISP as "liittymä" (your connection): "Ei internetliittymäsi osoite".
- Pause hint "Keskeytä 5 min", state "Tauolla" (as the quick menu); handshake "Kättely"; endpoint "Päätepiste"; split tunnel "jaettu tunneli".
- S_STATE_HELD "Liikenne pidossa"; S_UP_FOR "Yhteydessä".

## French (fr)
- Vous; ISP is FAI.
- Chips "sans kill switch" / "avec kill switch" (fit 16 characters); hints "Activer kill switch" / "Couper kill switch" without article.
- Module how-to lines in the infinitive: "À placer sous *KERNEL dans ur0:tai/config.txt et redémarrer".
- Handshake and Endpoint kept in English ("Handshake en cours"); split tunnel "Tunnel partiel"; kernel module "module noyau".
- Stalled state "sans réponse" (not "bloqué", which is Blocked); attempt line "tentative {0}, sans réponse".
- The app's status word is "Connexion"; the Settings page says "Connexion en cours".

## Italian (it)
- Tu; ISP kept; VPN feminine.
- Chips "kill switch: sì" / "kill switch: no", "no menu rapido".
- Traffic held "Traffico fermo"; held node "Trattenuto"; leak "perdita" (Perdita DNS, Test perdite); "Split tunnel" kept in English.
- Handshake, endpoint, keepalive kept in English; routes "Rotte"; "modulo kernel".
- "niente" for none (agrees with "Ignorate"); About is "Info"; reconnect option "Riconnessione".

## Spanish, Spain (es)
- Tú, "la VPN", ISP kept.
- Chips "sin kill switch" / "con kill switch", "LAN directa", "hora sin ajustar".
- "Tráfico retenido" is long for a status word (it shrinks to fit); Not connected "Desconectado".
- Leak check "Prueba de fugas"; can't check "Sin resultado" (a guess); split tunnel "Túnel dividido".
- Timers "Tiempo activo" (Up for), "Intentando" (Trying for); Rescan "Buscar de nuevo".
- Handshake, Endpoint, Keepalive kept in English; "ninguno" for both Last error and Ignored.

## Portuguese, Portugal (pt)
- ISP "operador"; chips "com kill switch" / "sem kill switch", "sem data/hora".
- On/Off "Ativado" / "Desativado" (as the Settings page); Not connected "Desligado".
- "Esta Vita" treats the console as feminine.
- Handshake and Endpoint kept in English; handshaking "A negociar"; split tunnel "Túnel dividido"; leak "fuga" ("Teste de fugas").
- Shortened: S_PAUSED_NOTE "Para Wi-Fi de hotel e jogos que rejeitam VPN", S_BLOCKED_WHY "A VPN estava ativa ao entrar em espera", Details "IP do túnel" / "IP do Wi-Fi".
- Long hints that fit: "Desativar kill switch", "Definições de rede".

## Portuguese, Brazil (ptbr)
- ISP "provedor"; leak "vazamento" ("Teste de vazamento", "via Wi-Fi · vaza").
- "Este Vita" treats the console as masculine, as Brazilian usage does.
- On/Off "Ligado" / "Desligado"; hints "Ligar kill switch" / "Desligar kill switch".
- Abbreviated: network settings hint "Config. de rede"; Rescan "Buscar de novo".
- "Criptografado"; handshaking "Negociando"; own URL "URL própria" (pt says "URL próprio").
- "A PSN segue conectada" is a free rendering of "PSN stays signed in".

## Turkish (tr)
- Formal siz (as the Settings page); ISP "İSS".
- Timer labels are loose: "Bağlantı süresi" (Up for), "Sonraki deneme" (Retry in), "Deneme süresi" (Trying for), "Devama kalan" (Resumes in, the weakest).
- Handshake "el sıkışma" / "El sıkışılıyor"; endpoint "Uç nokta"; keepalive kept; split tunnel "Bölünmüş tünel".
- Shortened: S_HELD_BACK_NOTE "Doğrudan çıkmak için kesin ya da duraklatın", S_HINT_MIGRATE_TO "Taşı: {0}", S_DNS_NETWORK "ağınki".
- In capitals INTERNET becomes İNTERNET (Turkish rule) while VITA and KILL SWITCH keep the plain I: check this reads right.

## English, United Kingdom (engb)
- The English text is already British in spelling, so `engb.def` is empty and shows the English (US) table.
