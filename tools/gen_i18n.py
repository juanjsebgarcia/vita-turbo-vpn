#!/usr/bin/env python3
"""gen_i18n.py - writes settings_app/i18n_ids.h (the string ids) and
settings_app/i18n_table.h (the strings) for the Settings page and the quick
menu, from the table below.

    python3 tools/gen_i18n.py        (from the repository root)
    python3 tools/gen_i18n.py --widths   (also lists the quick-menu widths)

Columns follow the Vita system-language index (enum vtw_lang_id). English is
authoritative; every language is filled, and check() refuses a table with a
missing language or different placeholders. (A language with no entry would
get the English text, marked "English copy, to translate".)

Placeholders: %s and %u, filled in order by vtw_format() (user/i18n_fmt.c). Keep
the same placeholders, in the same order, in every language.
"""
import os
import sys

LANGS = ["ja", "en", "fr", "es", "de", "it", "nl", "pt", "ru", "ko", "zht", "zhs",
         "fi", "sv", "da", "no", "pl", "ptbr", "engb", "tr"]

ORDER = []
T = {}


def s(id_, **kw):
    assert id_ not in T, id_
    assert "en" in kw, id_
    kw.setdefault("engb", kw["en"])   # British English: same text unless given
    ORDER.append(id_)
    T[id_] = kw


# The quick menu's own strings come first in the table (and the ids), so its
# build (VTW_I18N_QM_ONLY) keeps only those rows and does not carry the
# Settings page's text into SceShell: every QM_* id, plus the ids the
# status code reads on the quick-menu path (status.c: the "(test build)"
# mark). Same scheme as Wi-Fi Turbo's WQ_I18N_QM_ONLY.
QM_EXTRA = ("ST_TEST_BUILD",)


def is_qm(id_):
    return id_.startswith("QM_") or id_ in QM_EXTRA


def same(id_, text):
    """A name kept verbatim in every language."""
    s(id_, **{l: text for l in LANGS})


# Settings -> Network -> VPN Settings. Page and app names follow
# the Vita's own Settings wording and Wi-Fi Turbo's translations (German says
# WLAN, the informal du, as Wi-Fi Turbo does). Notes for reviewers:
# ../app/TRANSLATIONS.md.
s("SET_TITLE", en="VPN Settings",
  ja="VPN設定", fr="Paramètres VPN", es="Ajustes de VPN",
  de="VPN-Einstellungen", it="Impostazioni VPN", nl="VPN-instellingen",
  pt="Definições de VPN", ru="Настройки VPN", ko="VPN 설정",
  zht="VPN 設定", zhs="VPN 设置", fi="VPN-asetukset",
  sv="VPN-inställningar", da="VPN-indstillinger", no="VPN-innstillinger",
  pl="Ustawienia VPN", ptbr="Configurações de VPN", tr="VPN Ayarları")
s("SET_ROW_ON", en="VPN Enabled",
  ja="VPNを有効にする", fr="VPN activé", es="VPN activada", de="VPN aktiviert",
  it="VPN attiva", nl="VPN ingeschakeld", pt="VPN ativada", ru="VPN включён",
  ko="VPN 사용", zht="啟用 VPN", zhs="启用 VPN", fi="VPN käytössä", sv="VPN på",
  da="VPN slået til", no="VPN på", pl="VPN włączony", ptbr="VPN ativada", tr="VPN etkin")
s("SET_HINT_ON", en="Connects or disconnects the profile you last used in the "
  "VPN app. Profiles, migrate and the checks are in the app.",
  ja="VPNアプリで最後に使ったプロファイルに接続、または切断します。"
     "プロファイル、移行、チェックはアプリで行います。",
  fr="Connecte ou déconnecte le dernier profil utilisé dans l'app VPN. "
     "Les profils, la migration et les vérifications sont dans l'app.",
  es="Conecta o desconecta el último perfil que usaste en la app VPN. "
     "Los perfiles, la migración y las comprobaciones están en la app.",
  de="Verbindet oder trennt das zuletzt in der VPN-App verwendete Profil. "
     "Profile, Wechsel und Prüfungen findest du in der App.",
  it="Connette o disconnette l'ultimo profilo usato nell'app VPN. "
     "Profili, migrazione e controlli sono nell'app.",
  nl="Verbindt of verbreekt het profiel dat je het laatst in de VPN-app hebt gebruikt. "
     "Profielen, migreren en de controles vind je in de app.",
  pt="Liga ou desliga o último perfil usado na aplicação VPN. "
     "Os perfis, a migração e as verificações estão na aplicação.",
  ru="Подключает или отключает профиль, который вы последним использовали в приложении "
     "VPN. Профили, перенос и проверки находятся в приложении.",
  ko="VPN 앱에서 마지막으로 사용한 프로필을 연결하거나 연결을 끊습니다. "
     "프로필, 이전, 점검은 앱에서 할 수 있습니다.",
  zht="連線或中斷你在 VPN 應用程式中最後使用的設定檔。設定檔、遷移與檢查都在應用程式中。",
  zhs="连接或断开你在 VPN 应用中最后使用的配置文件。配置文件、迁移和检查都在应用中。",
  fi="Yhdistää tai katkaisee VPN-sovelluksessa viimeksi käyttämäsi profiilin. "
     "Profiilit, siirto ja tarkistukset ovat sovelluksessa.",
  sv="Ansluter eller kopplar från profilen du senast använde i VPN-appen. "
     "Profiler, flytt och kontroller finns i appen.",
  da="Forbinder eller afbryder den profil, du sidst brugte i VPN-appen. "
     "Profiler, flytning og tjek finder du i appen.",
  no="Kobler til eller fra profilen du sist brukte i VPN-appen. "
     "Profiler, flytting og kontroller finner du i appen.",
  pl="Łączy lub rozłącza profil ostatnio używany w aplikacji VPN. "
     "Profile, przenoszenie i testy są w aplikacji.",
  ptbr="Conecta ou desconecta o último perfil que você usou no app VPN. "
       "Perfis, migração e verificações ficam no app.",
  tr="VPN uygulamasında en son kullandığınız profile bağlanır veya bağlantıyı keser. "
     "Profiller, taşıma ve denetimler uygulamadadır.")
s("SET_HINT_PROFILE", en="The profile can only be changed in the VPN app.",
  ja="プロファイルはVPNアプリでのみ変更できます。",
  fr="Le profil ne peut être changé que dans l'app VPN.",
  es="El perfil solo se puede cambiar en la app VPN.",
  de="Das Profil lässt sich nur in der VPN-App ändern.",
  it="Il profilo si può cambiare solo nell'app VPN.",
  nl="Het profiel kan alleen in de VPN-app worden gewijzigd.",
  pt="O perfil só pode ser alterado na aplicação VPN.",
  ru="Профиль можно сменить только в приложении VPN.",
  ko="프로필은 VPN 앱에서만 바꿀 수 있습니다.",
  zht="設定檔只能在 VPN 應用程式中變更。",
  zhs="配置文件只能在 VPN 应用中更改。",
  fi="Profiilin voi vaihtaa vain VPN-sovelluksessa.",
  sv="Profilen kan bara ändras i VPN-appen.",
  da="Profilen kan kun ændres i VPN-appen.",
  no="Profilen kan bare endres i VPN-appen.",
  pl="Profil można zmienić tylko w aplikacji VPN.",
  ptbr="O perfil só pode ser alterado no app VPN.",
  tr="Profil yalnızca VPN uygulamasında değiştirilebilir.")
s("SET_HINT_STATUS", en="The tunnel's state when this page was opened. Go back "
  "and open the page again to refresh it.",
  ja="このページを開いたときのトンネルの状態です。更新するには、戻ってからもう一度このページを開いてください。",
  fr="État du tunnel à l'ouverture de cette page. Revenez en arrière et rouvrez la page pour l'actualiser.",
  es="Estado del túnel al abrir esta página. Vuelve atrás y ábrela de nuevo para actualizarlo.",
  de="Der Tunnelstatus beim Öffnen dieser Seite. Geh zurück und öffne die Seite erneut, um ihn zu aktualisieren.",
  it="Lo stato del tunnel all'apertura di questa pagina. Torna indietro e riaprila per aggiornarlo.",
  nl="De status van de tunnel toen deze pagina werd geopend. Ga terug en open de pagina opnieuw om te vernieuwen.",
  pt="O estado do túnel quando esta página foi aberta. Volte atrás e abra a página de novo para o atualizar.",
  ru="Состояние туннеля на момент открытия страницы. Чтобы обновить его, вернитесь назад и откройте страницу снова.",
  ko="이 페이지를 열었을 때의 터널 상태입니다. 새로 고치려면 뒤로 갔다가 페이지를 다시 여세요.",
  zht="開啟此頁面時的隧道狀態。返回後再次開啟此頁面即可更新。",
  zhs="打开此页面时的隧道状态。返回后再次打开此页面即可刷新。",
  fi="Tunnelin tila, kun tämä sivu avattiin. Päivitä se palaamalla takaisin ja avaamalla sivu uudelleen.",
  sv="Tunnelns status när sidan öppnades. Gå tillbaka och öppna sidan igen för att uppdatera den.",
  da="Tunnelens status, da siden blev åbnet. Gå tilbage, og åbn siden igen for at opdatere den.",
  no="Tunnelens status da siden ble åpnet. Gå tilbake og åpne siden igjen for å oppdatere den.",
  pl="Stan tunelu w chwili otwarcia tej strony. Aby go odświeżyć, wróć i otwórz stronę ponownie.",
  ptbr="O estado do túnel quando esta página foi aberta. Volte e abra a página de novo para atualizar.",
  tr="Bu sayfa açıldığındaki tünel durumu. Yenilemek için geri dönüp sayfayı yeniden açın.")

# Settings rows for the status-bar padlock and the HUD.
# The HUD rows reuse Wi-Fi Turbo's wording and translations (its "HUD
# position", corner names and "In-game HUD").
s("SET_ROW_PADLOCK", en="Status bar padlock",
  ja="ステータスバーの鍵アイコン", fr="Cadenas dans la barre d'état", es="Candado en la barra de estado",
  de="Schloss in der Statusleiste", it="Lucchetto nella barra di stato", nl="Hangslot in de statusbalk",
  pt="Cadeado na barra de estado", ru="Замок в строке состояния", ko="상태 표시줄 자물쇠",
  zht="狀態列鎖頭圖示", zhs="状态栏锁图标", fi="Riippulukko tilarivillä", sv="Hänglås i statusfältet",
  da="Hængelås i statuslinjen", no="Hengelås i statuslinjen", pl="Kłódka na pasku stanu",
  ptbr="Cadeado na barra de status", tr="Durum çubuğunda kilit")
s("SET_HINT_PADLOCK", en="Shows a padlock in the middle of the status bar at the top of the screen "
  "while the VPN is connected, in red while traffic is blocked (Lockdown Mode with the VPN off, or "
  "until the VPN reconnects). Never shown over games.",
  ja="VPNの接続中、画面上部のステータスバー中央に鍵アイコンを表示します。通信がブロックされている間"
     "（VPNがオフの状態のロックダウンモード、またはVPNの再接続まで）は赤で表示します。ゲームの上には表示されません。",
  fr="Affiche un cadenas au milieu de la barre d'état en haut de l'écran quand le VPN est connecté, "
     "en rouge quand le trafic est bloqué (Mode verrouillage avec le VPN désactivé, ou jusqu'à la "
     "reconnexion du VPN). Jamais affiché pendant un jeu.",
  es="Muestra un candado en el centro de la barra de estado, en la parte superior de la pantalla, mientras "
     "la VPN está conectada, en rojo mientras el tráfico está bloqueado (Modo bloqueo con la VPN "
     "desactivada, o hasta que la VPN se reconecte). Nunca aparece sobre los juegos.",
  de="Zeigt ein Schloss in der Mitte der Statusleiste oben am Bildschirm, solange das VPN verbunden ist, "
     "in Rot, solange der Datenverkehr gesperrt ist (Sperrmodus bei ausgeschaltetem VPN oder bis zur "
     "erneuten VPN-Verbindung). Nie über Spielen.",
  it="Mostra un lucchetto al centro della barra di stato in alto sullo schermo mentre la VPN è connessa, "
     "in rosso mentre il traffico è bloccato (Modalità blocco con la VPN spenta, o finché la VPN non si "
     "riconnette). Mai sopra i giochi.",
  nl="Toont een hangslot midden in de statusbalk boven in het scherm zolang de VPN verbonden is, in rood "
     "zolang verkeer geblokkeerd is (Vergrendelmodus met de VPN uit, of tot de VPN opnieuw verbinding "
     "maakt). Nooit boven games.",
  pt="Mostra um cadeado no centro da barra de estado no topo do ecrã enquanto a VPN está ligada, a "
     "vermelho enquanto o tráfego está bloqueado (Modo de bloqueio com a VPN desligada, ou até a VPN "
     "voltar a ligar). Nunca sobre os jogos.",
  ru="Показывает замок в середине строки состояния вверху экрана, пока VPN подключён, и красный замок, "
     "пока трафик заблокирован (Режим блокировки при выключенном VPN или до переподключения VPN). "
     "Поверх игр не отображается.",
  ko="VPN이 연결되어 있는 동안 화면 위쪽 상태 표시줄 가운데에 자물쇠를 표시하고, 트래픽이 차단된 "
     "동안(VPN이 꺼진 상태의 잠금 모드 또는 VPN이 다시 연결될 때까지)에는 빨간색으로 표시합니다. "
     "게임 위에는 표시되지 않습니다.",
  zht="VPN 連線時，在畫面上方狀態列的中央顯示鎖頭；流量被封鎖時（VPN 關閉時的鎖定模式，或在 VPN "
      "重新連線前）顯示為紅色。不會顯示在遊戲畫面上。",
  zhs="VPN 连接时，在屏幕上方状态栏的中间显示锁图标；流量被阻止时（VPN 关闭时的锁定模式，或在 VPN "
      "重新连接前）显示为红色。不会显示在游戏画面上。",
  fi="Näyttää riippulukon näytön yläreunan tilarivin keskellä, kun VPN on yhdistetty, ja punaisena, "
     "kun liikenne on estetty (Lukitustila VPN:n ollessa pois päältä tai kunnes VPN yhdistää "
     "uudelleen). Ei koskaan pelien päällä.",
  sv="Visar ett hänglås mitt i statusfältet högst upp på skärmen när VPN är anslutet, i rött när "
     "trafiken är blockerad (Låsläge med VPN avstängt, eller tills VPN ansluter igen). Visas aldrig "
     "över spel.",
  da="Viser en hængelås midt i statuslinjen øverst på skærmen, mens VPN er forbundet, i rødt, mens "
     "trafikken er blokeret (Låsetilstand med VPN slået fra, eller indtil VPN forbinder igen). Vises "
     "aldrig over spil.",
  no="Viser en hengelås midt i statuslinjen øverst på skjermen mens VPN er tilkoblet, i rødt mens "
     "trafikken er blokkert (Låsemodus med VPN av, eller til VPN kobler til igjen). Vises aldri over "
     "spill.",
  pl="Pokazuje kłódkę na środku paska stanu u góry ekranu, gdy VPN jest połączony, na czerwono, gdy "
     "ruch jest zablokowany (Tryb blokady przy wyłączonym VPN lub do ponownego połączenia VPN). "
     "Nigdy nad grami.",
  ptbr="Mostra um cadeado no centro da barra de status no alto da tela enquanto a VPN está conectada, "
       "em vermelho enquanto o tráfego está bloqueado (Modo de bloqueio com a VPN desligada, ou até a "
       "VPN reconectar). Nunca sobre os jogos.",
  tr="VPN bağlıyken ekranın üstündeki durum çubuğunun ortasında bir kilit gösterir; trafik "
     "engellendiğinde (VPN kapalıyken Kilit modu ya da VPN yeniden bağlanana kadar) kilit kırmızı "
     "olur. Oyunların üzerinde gösterilmez.")
s("SET_ROW_HUD", en="VPN HUD overlay",
  ja="VPN HUDオーバーレイ", fr="Superposition HUD VPN", es="Superposición HUD VPN", de="VPN-HUD-Overlay",
  it="Overlay HUD VPN", nl="VPN HUD-overlay", pt="Sobreposição HUD VPN", ru="Наложение VPN HUD",
  ko="VPN HUD 오버레이", zht="VPN HUD 疊加顯示", zhs="VPN HUD 叠加显示", fi="VPN HUD -peittokuva",
  sv="VPN HUD-överlägg", da="VPN HUD-overlay", no="VPN HUD-overlegg", pl="Nakładka HUD VPN",
  ptbr="Sobreposição HUD VPN", tr="VPN HUD katmanı")
# The three values are Wi-Fi Turbo's words.
s("SET_ITEM_HUD_OFF", en="Off",
  ja="オフ", fr="Désactivé", es="Desactivado", de="Aus", it="Disattivato", nl="Uit",
  pt="Desligado", ru="Выкл.", ko="끔", zht="關閉", zhs="关闭", fi="Pois",
  sv="Av", da="Fra", no="Av", pl="Wyłączony", ptbr="Desligado", tr="Kapalı")
s("SET_ITEM_HUD_COMPACT", en="Compact",
  ja="コンパクト", fr="Compact", es="Compacto", de="Kompakt", it="Compatto", nl="Compact",
  pt="Compacto", ru="Компактно", ko="간단히", zht="精簡", zhs="精简", fi="Tiivis",
  sv="Kompakt", da="Kompakt", no="Kompakt", pl="Kompaktowy", ptbr="Compacto", tr="Kompakt")
s("SET_ITEM_HUD_EXTENDED", en="Extended",
  ja="拡張", fr="Étendu", es="Extendido", de="Erweitert", it="Esteso", nl="Uitgebreid",
  pt="Alargado", ru="Расширенно", ko="자세히", zht="詳細", zhs="详细", fi="Laaja",
  sv="Utökad", da="Udvidet", no="Utvidet", pl="Rozszerzony", ptbr="Estendido", tr="Genişletilmiş")
s("SET_HINT_HUD", en="Shows the VPN state over the screen. Compact: one line with the state. "
  "Extended: adds the tunnel address and the exit address from the app's last check.",
  ja="VPNの状態を画面上に表示します。コンパクト：状態を1行で表示。拡張：トンネルのアドレスと、"
     "アプリで最後に確認した出口アドレスも表示。",
  fr="Affiche l'état du VPN à l'écran. Compact : une ligne avec l'état. Étendu : ajoute l'adresse du "
     "tunnel et l'adresse de sortie de la dernière vérification de l'app.",
  es="Muestra el estado de la VPN en pantalla. Compacto: una línea con el estado. Extendido: añade la "
     "dirección del túnel y la dirección de salida de la última comprobación de la app.",
  de="Zeigt den VPN-Status auf dem Bildschirm. Kompakt: eine Zeile mit dem Status. Erweitert: "
     "zusätzlich die Tunneladresse und die Ausgangsadresse aus der letzten Prüfung in der App.",
  it="Mostra lo stato della VPN sullo schermo. Compatto: una riga con lo stato. Esteso: aggiunge "
     "l'indirizzo del tunnel e l'indirizzo di uscita dell'ultimo controllo dell'app.",
  nl="Toont de VPN-status op het scherm. Compact: één regel met de status. Uitgebreid: voegt het "
     "tunneladres en het uitgangsadres van de laatste controle in de app toe.",
  pt="Mostra o estado da VPN no ecrã. Compacto: uma linha com o estado. Alargado: acrescenta o "
     "endereço do túnel e o endereço de saída da última verificação da aplicação.",
  ru="Показывает состояние VPN на экране. Компактно: одна строка с состоянием. Расширенно: добавляет "
     "адрес туннеля и адрес выхода из последней проверки в приложении.",
  ko="화면에 VPN 상태를 표시합니다. 간단히: 상태를 한 줄로 표시. 자세히: 터널 주소와 앱의 마지막 "
     "점검에서 확인한 출구 주소를 추가로 표시.",
  zht="在畫面上顯示 VPN 狀態。精簡：以一行顯示狀態。詳細：另外顯示隧道位址，以及應用程式上次檢查得到的出口位址。",
  zhs="在屏幕上显示 VPN 状态。精简：以一行显示状态。详细：另外显示隧道地址，以及应用上次检查得到的出口地址。",
  fi="Näyttää VPN:n tilan näytöllä. Tiivis: yksi rivi tilasta. Laaja: lisää tunnelin osoitteen ja "
     "sovelluksen viimeisimmän tarkistuksen ulostulo-osoitteen.",
  sv="Visar VPN-status på skärmen. Kompakt: en rad med status. Utökad: lägger till tunneladressen och "
     "utgångsadressen från appens senaste kontroll.",
  da="Viser VPN-status på skærmen. Kompakt: én linje med status. Udvidet: tilføjer tunneladressen og "
     "udgangsadressen fra appens seneste tjek.",
  no="Viser VPN-status på skjermen. Kompakt: én linje med status. Utvidet: legger til tunneladressen og "
     "utgangsadressen fra appens siste sjekk.",
  pl="Pokazuje stan VPN na ekranie. Kompaktowy: jeden wiersz ze stanem. Rozszerzony: dodaje adres "
     "tunelu i adres wyjściowy z ostatniego sprawdzenia w aplikacji.",
  ptbr="Mostra o estado da VPN na tela. Compacto: uma linha com o estado. Estendido: adiciona o "
       "endereço do túnel e o endereço de saída da última verificação do aplicativo.",
  tr="VPN durumunu ekranda gösterir. Kompakt: durum tek satırda. Genişletilmiş: tünel adresini ve "
     "uygulamanın son kontrolündeki çıkış adresini ekler.")
s("SET_ROW_HUDPOS", en="HUD position",
  ja="HUDの位置", fr="Position du HUD", es="Posición del HUD", de="HUD-Position", it="Posizione HUD",
  nl="HUD-positie", pt="Posição do HUD", ru="Положение HUD", ko="HUD 위치", zht="HUD 位置",
  zhs="HUD 位置", fi="HUD:n sijainti", sv="HUD-position", da="HUD-placering", no="HUD-plassering",
  pl="Położenie HUD", ptbr="Posição do HUD", tr="HUD konumu")
s("SET_HINT_HUDPOS", en="Which corner of the screen the HUD sits in.",
  ja="HUDを表示する画面の隅。", fr="Le coin de l'écran où s'affiche le HUD.",
  es="En qué esquina de la pantalla aparece el HUD.", de="In welcher Bildschirmecke das HUD erscheint.",
  it="In quale angolo dello schermo appare l'HUD.", nl="In welke hoek van het scherm de HUD staat.",
  pt="Em que canto do ecrã aparece o HUD.", ru="В каком углу экрана отображается HUD.",
  ko="HUD가 표시될 화면 모서리.", zht="HUD 顯示在螢幕的哪個角落。", zhs="HUD 显示在屏幕的哪个角落。",
  fi="Missä näytön kulmassa HUD näkyy.", sv="I vilket hörn av skärmen HUD:en visas.",
  da="Hvilket hjørne af skærmen HUD'en vises i.", no="Hvilket hjørne av skjermen HUD-en vises i.",
  pl="W którym rogu ekranu pojawia się HUD.", ptbr="Em qual canto da tela o HUD aparece.",
  tr="HUD'nin ekranın hangi köşesinde görüneceği.")
s("SET_POS_TL", en="Top left",
  ja="左上", fr="En haut à gauche", es="Arriba a la izquierda", de="Oben links", it="In alto a sinistra",
  nl="Linksboven", pt="Canto superior esquerdo", ru="Слева сверху", ko="왼쪽 위", zht="左上", zhs="左上",
  fi="Ylävasen", sv="Uppe till vänster", da="Øverst til venstre", no="Øverst til venstre",
  pl="Lewy górny", ptbr="Canto superior esquerdo", tr="Sol üst")
s("SET_POS_TR", en="Top right",
  ja="右上", fr="En haut à droite", es="Arriba a la derecha", de="Oben rechts", it="In alto a destra",
  nl="Rechtsboven", pt="Canto superior direito", ru="Справа сверху", ko="오른쪽 위", zht="右上", zhs="右上",
  fi="Yläoikea", sv="Uppe till höger", da="Øverst til højre", no="Øverst til høyre",
  pl="Prawy górny", ptbr="Canto superior direito", tr="Sağ üst")
s("SET_POS_BL", en="Bottom left",
  ja="左下", fr="En bas à gauche", es="Abajo a la izquierda", de="Unten links", it="In basso a sinistra",
  nl="Linksonder", pt="Canto inferior esquerdo", ru="Слева снизу", ko="왼쪽 아래", zht="左下", zhs="左下",
  fi="Alavasen", sv="Nere till vänster", da="Nederst til venstre", no="Nederst til venstre",
  pl="Lewy dolny", ptbr="Canto inferior esquerdo", tr="Sol alt")
s("SET_POS_BR", en="Bottom right",
  ja="右下", fr="En bas à droite", es="Abajo a la derecha", de="Unten rechts", it="In basso a destra",
  nl="Rechtsonder", pt="Canto inferior direito", ru="Справа снизу", ko="오른쪽 아래", zht="右下", zhs="右下",
  fi="Alaoikea", sv="Nere till höger", da="Nederst til højre", no="Nederst til høyre",
  pl="Prawy dolny", ptbr="Canto inferior direito", tr="Sağ alt")
s("SET_ROW_HUDGAME", en="In-game HUD",
  ja="ゲーム中のHUD", fr="HUD en jeu", es="HUD durante el juego", de="HUD im Spiel", it="HUD in gioco",
  nl="HUD tijdens spelen", pt="HUD durante o jogo", ru="HUD в играх", ko="게임 중 HUD",
  zht="遊戲中顯示 HUD", zhs="游戏中显示 HUD", fi="HUD pelin aikana", sv="HUD i spel", da="HUD i spil",
  no="HUD i spill", pl="HUD w grach", ptbr="HUD durante o jogo", tr="Oyun içi HUD")
s("SET_HINT_HUDGAME", en="Also show the HUD while a game or app is running. When off, it only "
  "shows on the Home screen, in Settings and in the quick menu (press the PS button).",
  ja="ゲームやアプリの実行中もHUDを表示します。オフにすると、ホーム画面、設定、クイックメニュー（PSボタン）でのみ表示されます。",
  fr="Affiche aussi le HUD pendant un jeu ou une application. Désactivé, il n'apparaît que sur l'écran d'accueil, dans les Paramètres et dans le menu rapide (touche PS).",
  es="Muestra también el HUD mientras se ejecuta un juego o una aplicación. Si está desactivado, solo aparece en la pantalla de inicio, en Ajustes y en el menú rápido (botón PS).",
  de="Zeigt das HUD auch während eines Spiels oder einer App. Wenn aus, erscheint es nur auf dem Startbildschirm, in den Einstellungen und im Schnellmenü (PS-Taste).",
  it="Mostra l'HUD anche durante un gioco o un'applicazione. Se disattivato, appare solo nella schermata Home, nelle Impostazioni e nel menu rapido (tasto PS).",
  nl="Toont de HUD ook tijdens een game of app. Als dit uit staat, verschijnt de HUD alleen op het startscherm, in Instellingen en in het snelmenu (PS-toets).",
  pt="Mostra também o HUD durante um jogo ou aplicação. Desativado, só aparece no ecrã inicial, nas Definições e no menu rápido (botão PS).",
  ru="Показывать HUD и во время игры или приложения. Если выключено, HUD виден только на главном экране, в настройках и в быстром меню (кнопка PS).",
  ko="게임이나 앱 실행 중에도 HUD를 표시합니다. 끄면 홈 화면, 설정, 퀵 메뉴(PS 버튼)에서만 표시됩니다.",
  zht="執行遊戲或應用程式時也顯示 HUD。關閉時，只在主畫面、設定及快速選單（PS 按鈕）中顯示。",
  zhs="运行游戏或应用程序时也显示 HUD。关闭时，仅在主屏幕、设置和快捷菜单（PS 按钮）中显示。",
  fi="Näyttää HUD:n myös pelin tai sovelluksen aikana. Pois päältä se näkyy vain aloitusnäytössä, asetuksissa ja pikavalikossa (PS-näppäin).",
  sv="Visar HUD:en även när ett spel eller en app körs. Av: den visas bara på hemskärmen, i Inställningar och i snabbmenyn (PS-knappen).",
  da="Viser også HUD'en, mens et spil eller en app kører. Fra: den vises kun på startskærmen, i Indstillinger og i hurtigmenuen (PS-knappen).",
  no="Viser også HUD-en mens et spill eller en app kjører. Av: den vises bare på startskjermen, i Innstillinger og i hurtigmenyen (PS-knappen).",
  pl="Pokazuje HUD także podczas gry lub aplikacji. Gdy wyłączone, HUD jest widoczny tylko na ekranie głównym, w Ustawieniach i w szybkim menu (przycisk PS).",
  ptbr="Mostra o HUD também durante um jogo ou aplicativo. Desligado, ele só aparece na tela inicial, em Configurações e no menu rápido (botão PS).",
  tr="HUD'yi bir oyun veya uygulama çalışırken de gösterir. Kapalıyken yalnızca ana ekranda, Ayarlar'da ve hızlı menüde (PS düğmesi) görünür.")

# Lockdown mode: only tunnel traffic may
# leave; offline whenever the tunnel is off; kept across restarts.
s("SET_ROW_LOCKDOWN", en="Lockdown Mode",
  ja="ロックダウンモード", fr="Mode verrouillage", es="Modo bloqueo", de="Sperrmodus",
  it="Modalità blocco", nl="Vergrendelmodus", pt="Modo de bloqueio", ru="Режим блокировки",
  ko="잠금 모드", zht="鎖定模式", zhs="锁定模式", fi="Lukitustila", sv="Låsläge",
  da="Låsetilstand", no="Låsemodus", pl="Tryb blokady", ptbr="Modo de bloqueio", tr="Kilit modu")
s("SET_HINT_LOCKDOWN", en="Only traffic through the tunnel may leave the system. Whenever the VPN is "
  "off, the system is offline (the local network stays reachable if \"Local network\" is set to \"Direct\" in the "
  "app). Pause is not available. Kept after a restart. To undo it, turn it off here, or hold the L "
  "button while the system starts.",
  ja="トンネル経由の通信だけが外に出られます。VPNがオフのときは常にオフラインになります（アプリで"
     "「ローカルネットワーク」を「直接」にしていれば、ローカルネットワークには接続できます）。一時停止は使えません。"
     "再起動後も保持されます。解除するには、ここでオフにするか、起動中にLボタンを押し続けてください。",
  fr="Seul le trafic du tunnel peut sortir du système. Quand le VPN est désactivé, le système est hors "
     "ligne (le réseau local reste accessible si « Réseau local » est réglé sur « Direct » dans l'app). "
     "La pause n'est pas disponible. Conservé après un redémarrage. Pour l'annuler, désactivez-le ici "
     "ou maintenez la touche L pendant le démarrage.",
  es="Solo el tráfico del túnel puede salir del sistema. Cuando la VPN está desactivada, el sistema "
     "queda sin conexión (la red local sigue accesible si «Red local» está en «Directa» en la app). "
     "La pausa no está disponible. Se mantiene tras reiniciar. Para deshacerlo, desactívalo aquí o "
     "mantén pulsado el botón L mientras se inicia el sistema.",
  de="Nur Datenverkehr durch den Tunnel darf das System verlassen. Ist das VPN aus, ist das System "
     "offline (das lokale Netzwerk bleibt erreichbar, wenn „Lokales Netzwerk“ in der App auf "
     "„Direkt“ steht). Pausieren ist nicht möglich. Bleibt nach einem Neustart erhalten. Zum Aufheben schalte es "
     "hier aus oder halte beim Start die L-Taste gedrückt.",
  it="Può uscire dal sistema solo il traffico del tunnel. Quando la VPN è disattivata, il sistema è "
     "offline (la rete locale resta raggiungibile se «Rete locale» è impostata su «Diretta» nell'app). La pausa "
     "non è disponibile. Resta attiva dopo il riavvio. Per annullarla, disattivala qui o tieni premuto "
     "il tasto L durante l'avvio.",
  nl="Alleen verkeer door de tunnel mag het systeem verlaten. Als de VPN uit staat, is het systeem "
     "offline (het lokale netwerk blijft bereikbaar als 'Lokaal netwerk' in de app op 'Direct' staat). "
     "Pauzeren is niet mogelijk. Blijft na een herstart behouden. Zet het hier uit of houd de L-toets "
     "ingedrukt tijdens het opstarten om het op te heffen.",
  pt="Só o tráfego do túnel pode sair do sistema. Com a VPN desativada, o sistema fica sem ligação "
     "(a rede local continua acessível se «Rede local» estiver em «Direta» na aplicação). A pausa "
     "não está disponível. Mantém-se após reiniciar. Para anular, desative-o aqui ou mantenha premido o "
     "botão L durante o arranque.",
  ru="Наружу может выходить только трафик туннеля. Когда VPN выключен, система остаётся без сети "
     "(локальная сеть доступна, если в приложении для «Локальная сеть» выбрано «Напрямую»). Пауза "
     "недоступна. Сохраняется после перезагрузки. Чтобы отменить, выключите здесь или удерживайте "
     "кнопку L при запуске системы.",
  ko="터널을 거치는 트래픽만 시스템 밖으로 나갈 수 있습니다. VPN이 꺼져 있으면 시스템은 항상 "
     "오프라인입니다(앱에서 '로컬 네트워크'가 '직접'으로 설정되어 있으면 로컬 네트워크는 사용할 수 있습니다). "
     "일시 정지는 사용할 수 없습니다. 재시작 후에도 유지됩니다. 해제하려면 여기에서 끄거나 시스템이 "
     "시작되는 동안 L 버튼을 누르고 있으세요.",
  zht="只有經由隧道的流量可以送出系統。VPN 關閉時，系統一律離線（若在應用程式中將「區域網路」設為「直連」，"
      "仍可連到區域網路）。無法暫停。重新啟動後仍會保留。若要解除，請在這裡關閉，或在系統啟動時按住 L 按鈕。",
  zhs="只有经过隧道的流量可以发出系统。VPN 关闭时，系统始终离线（如果在应用中将“局域网”设为“直连”，"
      "仍可访问局域网）。无法暂停。重启后仍会保留。要解除，请在此处关闭，或在系统启动时按住 L 按钮。",
  fi="Vain tunnelin kautta kulkeva liikenne pääsee ulos järjestelmästä. Kun VPN on pois päältä, "
     "järjestelmä on offline-tilassa (lähiverkko on käytettävissä, jos sovelluksessa ”Lähiverkko” on "
     "asetettu arvoon ”Suoraan”). Tauko ei ole käytettävissä. Säilyy uudelleenkäynnistyksen jälkeen. Kumoa se "
     "kytkemällä se tästä pois tai pitämällä L-näppäintä painettuna järjestelmän käynnistyessä.",
  sv="Bara trafik genom tunneln får lämna systemet. När VPN är av är systemet offline (det lokala "
     "nätverket nås fortfarande om ”Lokalt nätverk” är inställt på ”Direkt” i appen). Paus går inte att använda. "
     "Behålls efter en omstart. Stäng av det här, eller håll ned L-knappen medan systemet startar, för "
     "att häva det.",
  da="Kun trafik gennem tunnelen må forlade systemet. Når VPN er slået fra, er systemet offline (det "
     "lokale netværk kan stadig nås, hvis ”Lokalt netværk” er sat til ”Direkte” i appen). Pause er ikke "
     "tilgængelig. Bevares efter en genstart. Slå det fra her, eller hold L-knappen nede, mens systemet "
     "starter, for at ophæve det.",
  no="Bare trafikk gjennom tunnelen får forlate systemet. Når VPN er av, er systemet frakoblet (det "
     "lokale nettverket kan fortsatt nås hvis «Lokalt nettverk» er satt til «Direkte» i appen). Pause er ikke "
     "tilgjengelig. Beholdes etter omstart. Slå det av her, eller hold inne L-knappen mens systemet "
     "starter, for å oppheve det.",
  pl="Wychodzić z systemu może tylko ruch przez tunel. Gdy VPN jest wyłączony, system jest offline "
     "(sieć lokalna pozostaje dostępna, jeśli w aplikacji „Sieć lokalna” jest ustawiona na „Bezpośrednio”). "
     "Wstrzymanie jest niedostępne. Ustawienie zostaje po ponownym uruchomieniu. Aby je cofnąć, wyłącz "
     "je tutaj lub przytrzymaj przycisk L podczas uruchamiania systemu.",
  ptbr="Só o tráfego do túnel pode sair do sistema. Com a VPN desligada, o sistema fica sem conexão "
       "(a rede local continua acessível se \"Rede local\" estiver em \"Direta\" no aplicativo). A "
       "pausa não está disponível. Continua valendo após reiniciar. Para desfazer, desligue aqui ou "
       "mantenha o botão L pressionado enquanto o sistema inicia.",
  tr="Sistemden yalnızca tünelden geçen trafik çıkabilir. VPN kapalıyken sistem çevrimdışıdır "
     "(uygulamada “Yerel ağ” “Doğrudan” olarak ayarlıysa yerel ağa erişilebilir). Duraklatma kullanılamaz. Yeniden "
     "başlatmadan sonra da korunur. Geri almak için burada kapatın veya sistem açılırken L düğmesini "
     "basılı tutun.")

# Faster sending: the plugin's sending thread on a
# game core instead of the system core.
s("SET_ROW_FASTSEND", en="Faster Sending",
  ja="高速送信", fr="Envoi plus rapide", es="Envío más rápido", de="Schnelleres Senden",
  it="Invio più rapido", nl="Sneller verzenden", pt="Envio mais rápido", ru="Быстрая отправка",
  ko="빠른 전송", zht="加速傳送", zhs="加速发送", fi="Nopeampi lähetys", sv="Snabbare sändning",
  da="Hurtigere afsendelse", no="Raskere sending", pl="Szybsze wysyłanie", ptbr="Envio mais rápido",
  tr="Daha hızlı gönderme")
s("SET_HINT_FASTSEND", en="[EXPERIMENTAL] Sends tunnel traffic on a free processor core among those games use, instead of "
  "sharing the system core. Lowers and steadies the delay of what you send, such as controller input "
  "when streaming a game, and adds a little speed. Takes a small share of that core from the game "
  "while sending. Changing it restarts the system at once, so save your game first. Off by default.",
  ja="［試験的］トンネルの送信を、システム用コアではなくゲームが使うプロセッサーコアの1つで行います。ゲーム配信時の"
     "コントローラー入力など、送信の遅延が小さく安定し、速度も少し上がります。送信中はそのコアの処理時間を"
     "ゲームから少しだけ使います。切り替えるとすぐにシステムが再起動するので、先にゲームをセーブしてください。初期設定はオフです。",
  fr="[EXPÉRIMENTAL] Envoie le trafic du tunnel sur l'un des cœurs du processeur utilisés par les jeux, au lieu de "
     "partager le cœur du système. Réduit et stabilise le délai de ce que vous envoyez, comme les "
     "commandes de la manette en streaming de jeu, et ajoute un peu de débit. Prend une petite part de "
     "ce cœur au jeu pendant l'envoi. Le modifier redémarre aussitôt le système : sauvegardez d'abord votre partie. Désactivé par défaut.",
  es="[EXPERIMENTAL] Envía el tráfico del túnel en uno de los núcleos del procesador que usan los juegos, en lugar "
     "de compartir el núcleo del sistema. Reduce y estabiliza el retardo de lo que envías, como los "
     "controles al transmitir un juego, y añade algo de velocidad. Toma una pequeña parte de ese núcleo "
     "al juego mientras envía. Cambiarlo reinicia el sistema al instante, así que guarda antes la partida. Desactivado de forma predeterminada.",
  de="[EXPERIMENTELL] Sendet den Tunnelverkehr auf einem der Prozessorkerne, die Spiele nutzen, statt den Systemkern "
     "zu teilen. Senkt und glättet die Verzögerung beim Senden, etwa der Controllereingaben beim "
     "Spiele-Streaming, und bringt etwas mehr Tempo. Nimmt dem Spiel beim Senden einen kleinen Teil "
     "dieses Kerns. Eine Änderung startet das System sofort neu, speichere also vorher dein Spiel. Standardmäßig aus.",
  it="[SPERIMENTALE] Invia il traffico del tunnel su uno dei core del processore usati dai giochi, invece di "
     "condividere il core di sistema. Riduce e stabilizza il ritardo di ciò che invii, come i comandi "
     "del controller in streaming di gioco, e aggiunge un po' di velocità. Durante l'invio toglie al "
     "gioco una piccola parte di quel core. Cambiarlo riavvia subito il sistema, quindi salva prima la partita. Disattivato per impostazione predefinita.",
  nl="[EXPERIMENTEEL] Verstuurt tunnelverkeer op een van de processorkernen die games gebruiken, in plaats van de "
     "systeemkern te delen. Verlaagt en stabiliseert de vertraging van wat je verstuurt, zoals "
     "controllerinvoer bij het streamen van een game, en geeft iets meer snelheid. Neemt tijdens het "
     "versturen een klein deel van die kern van de game. Wijzigen herstart het systeem meteen, dus sla eerst je game op. Standaard uit.",
  pt="[EXPERIMENTAL] Envia o tráfego do túnel num dos núcleos do processador usados pelos jogos, em vez de partilhar "
     "o núcleo do sistema. Reduz e estabiliza o atraso do que envia, como os comandos ao transmitir um "
     "jogo, e acrescenta alguma velocidade. Retira ao jogo uma pequena parte desse núcleo durante o "
     "envio. Alterá-lo reinicia logo o sistema, por isso guarde primeiro o jogo. Desativado por predefinição.",
  ru="[ЭКСПЕРИМЕНТАЛЬНО] Отправляет трафик туннеля на одном из ядер процессора, которые используют игры, а не на "
     "системном ядре. Уменьшает и выравнивает задержку отправки, например ввода с контроллера при "
     "стриминге игры, и немного прибавляет скорости. Во время отправки забирает у игры небольшую долю "
     "этого ядра. Изменение сразу перезапускает систему, поэтому сначала сохраните игру. По умолчанию выключено.",
  ko="[실험적] 터널 송신을 시스템 코어 대신 게임이 쓰는 프로세서 코어 중 하나에서 처리합니다. 게임 스트리밍 중 "
     "컨트롤러 입력처럼 보내는 데이터의 지연이 줄고 안정되며 속도도 조금 오릅니다. 송신하는 동안 그 "
     "코어의 시간을 게임에서 조금 가져옵니다. 변경하면 시스템이 바로 다시 시작되므로 먼저 게임을 저장하세요. 기본값은 꺼짐입니다.",
  zht="［實驗性］改在遊戲使用的處理器核心之一傳送隧道流量，而不是與系統核心共用。可降低並穩定傳送延遲（例如串流遊戲時的"
      "控制器輸入），速度也略有提升。傳送時會從遊戲取用該核心的一小部分時間。變更後系統會立即重新啟動，請先儲存遊戲。預設為關閉。",
  zhs="［实验性］改在游戏使用的处理器核心之一发送隧道流量，而不是与系统核心共用。可降低并稳定发送延迟（例如串流游戏时的"
      "控制器输入），速度也略有提升。发送时会从游戏占用该核心的一小部分时间。更改后系统会立即重启，请先保存游戏。默认关闭。",
  fi="[KOKEELLINEN] Lähettää tunnelin liikenteen yhdellä pelien käyttämistä suoritinytimistä järjestelmäytimen "
     "jakamisen sijaan. Pienentää ja tasoittaa lähetyksen viivettä, kuten ohjaimen syötteitä pelejä "
     "striimatessa, ja lisää hieman nopeutta. Vie lähetyksen aikana pelistä pienen osan ytimen ajasta. "
     "Muutos käynnistää järjestelmän heti uudelleen, joten tallenna peli ensin. Oletuksena pois.",
  sv="[EXPERIMENTELL] Skickar tunneltrafiken på en av de processorkärnor som spel använder, i stället för att dela "
     "systemkärnan. Sänker och jämnar ut fördröjningen för det du skickar, som handkontrollens "
     "inmatning när du strömmar ett spel, och ger lite mer fart. Tar en liten del av den kärnan från "
     "spelet medan det skickar. En ändring startar om systemet direkt, så spara spelet först. Av som standard.",
  da="[EKSPERIMENTEL] Sender tunneltrafikken på en af de processorkerner, som spil bruger, i stedet for at dele "
     "systemkernen. Sænker og udjævner forsinkelsen på det, du sender, fx controllerinput, når du "
     "streamer et spil, og giver lidt mere fart. Tager en lille del af kernen fra spillet, mens der "
     "sendes. En ændring genstarter systemet med det samme, så gem dit spil først. Slået fra som standard.",
  no="[EKSPERIMENTELL] Sender tunneltrafikken på en av prosessorkjernene spill bruker, i stedet for å dele "
     "systemkjernen. Senker og jevner ut forsinkelsen på det du sender, for eksempel kontrollerinndata "
     "når du strømmer et spill, og gir litt mer fart. Tar en liten del av kjernen fra spillet mens det "
     "sendes. En endring starter systemet på nytt med en gang, så lagre spillet først. Av som standard.",
  pl="[EKSPERYMENTALNE] Wysyła ruch tunelu na jednym z rdzeni procesora używanych przez gry, zamiast dzielić rdzeń "
     "systemowy. Zmniejsza i wyrównuje opóźnienie tego, co wysyłasz, np. sterowania podczas "
     "strumieniowania gry, i dodaje nieco szybkości. Podczas wysyłania zabiera grze niewielką część "
     "tego rdzenia. Zmiana od razu uruchamia system ponownie, więc najpierw zapisz grę. Domyślnie wyłączone.",
  ptbr="[EXPERIMENTAL] Envia o tráfego do túnel em um dos núcleos do processador usados pelos jogos, em vez de "
       "dividir o núcleo do sistema. Reduz e estabiliza o atraso do que você envia, como os comandos ao "
       "transmitir um jogo, e aumenta um pouco a velocidade. Tira do jogo uma pequena parte desse "
       "núcleo durante o envio. Alterar reinicia o sistema na hora, então salve o jogo antes. Desativado por padrão.",
  tr="[DENEYSEL] Tünel trafiğini sistem çekirdeğini paylaşmak yerine oyunların kullandığı işlemci "
     "çekirdeklerinden birinde gönderir. Oyun yayınlarken kumanda girişi gibi gönderdiklerinizin "
     "gecikmesini azaltır ve dengeler, biraz da hız katar. Gönderirken o çekirdeğin küçük bir kısmını "
     "oyundan alır. Değiştirmek sistemi hemen yeniden başlatır, bu yüzden önce oyununuzu kaydedin. Varsayılan olarak kapalı.")

# Tunnel state words; shared by Settings and the quick menu.
# The same words as the app's (../app/src/lang/).
s("ST_OFF", en="Off",
  ja="オフ", fr="Désactivé", es="Desactivado", de="Aus", it="Disattivato", nl="Uit",
  pt="Desligado", ru="Выкл.", ko="끔", zht="關閉", zhs="关闭", fi="Pois",
  sv="Av", da="Fra", no="Av", pl="Wyłączony", ptbr="Desligado", tr="Kapalı")
s("ST_CONNECTING", en="Connecting",
  ja="接続中", fr="Connexion en cours", es="Conectando", de="Verbinden", it="Connessione",
  nl="Verbinden", pt="A ligar", ru="Подключение", ko="연결 중", zht="連線中", zhs="正在连接",
  fi="Yhdistetään", sv="Ansluter", da="Forbinder", no="Kobler til", pl="Łączenie",
  ptbr="Conectando", tr="Bağlanıyor")
s("ST_CONNECTED", en="Connected",
  ja="接続済み", fr="Connecté", es="Conectado", de="Verbunden", it="Connesso", nl="Verbonden",
  pt="Ligado", ru="Подключено", ko="연결됨", zht="已連線", zhs="已连接", fi="Yhdistetty",
  sv="Ansluten", da="Forbundet", no="Tilkoblet", pl="Połączono", ptbr="Conectado", tr="Bağlı")
s("ST_NOT_PROTECTED", en="Not protected",
  ja="保護されていません", fr="Non protégé", es="Sin protección", de="Nicht geschützt",
  it="Non protetto", nl="Niet beschermd", pt="Sem proteção", ru="Не защищено",
  ko="보호되지 않음", zht="未受保護", zhs="未受保护", fi="Ei suojattu", sv="Inte skyddad",
  da="Ikke beskyttet", no="Ikke beskyttet", pl="Brak ochrony", ptbr="Sem proteção", tr="Korunmuyor")
s("ST_KILL_HOLDING", en="Kill switch holding traffic",
  ja="キルスイッチが通信を保留中", fr="Kill switch : trafic retenu",
  es="Kill switch reteniendo el tráfico", de="Kill Switch hält Datenverkehr zurück",
  it="Kill switch: traffico trattenuto", nl="Kill switch houdt verkeer tegen",
  pt="Kill switch a reter o tráfego", ru="Kill switch удерживает трафик",
  ko="킬 스위치가 트래픽 보류 중", zht="斷網保護正在攔住流量", zhs="断网保护正在拦住流量",
  fi="Kill switch pidättää liikenteen", sv="Kill switch håller kvar trafiken",
  da="Kill switch holder trafikken tilbage", no="Kill switch holder trafikken tilbake",
  pl="Kill switch wstrzymuje ruch", ptbr="Kill switch retendo o tráfego",
  tr="Kill switch trafiği tutuyor")
s("ST_BLOCKED", en="Blocked: reconnecting VPN",
  ja="ブロック中：VPNを再接続しています", fr="Bloqué : reconnexion du VPN",
  es="Bloqueado: reconectando la VPN", de="Blockiert: VPN wird neu verbunden",
  it="Bloccato: riconnessione VPN", nl="Geblokkeerd: VPN maakt opnieuw verbinding",
  pt="Bloqueado: a religar a VPN", ru="Заблокировано: переподключение VPN",
  ko="차단됨: VPN 재연결 중", zht="已封鎖：正在重新連線 VPN", zhs="已阻止：正在重新连接 VPN",
  fi="Estetty: VPN yhdistetään uudelleen", sv="Blockerad: återansluter VPN",
  da="Blokeret: genforbinder VPN", no="Blokkert: kobler til VPN igjen",
  pl="Zablokowano: ponowne łączenie VPN", ptbr="Bloqueado: reconectando a VPN",
  tr="Engellendi: VPN yeniden bağlanıyor")
s("ST_LOCKDOWN", en="Lockdown: traffic blocked",
  ja="ロックダウン：通信をブロック中", fr="Verrouillage : trafic bloqué",
  es="Bloqueo: tráfico bloqueado", de="Sperrmodus: Verkehr blockiert",
  it="Blocco: traffico bloccato", nl="Vergrendeld: verkeer geblokkeerd",
  pt="Bloqueio: tráfego bloqueado", ru="Блокировка: трафик закрыт",
  ko="잠금: 트래픽 차단됨", zht="鎖定：流量已封鎖", zhs="锁定：流量已阻止",
  fi="Lukitus: liikenne estetty", sv="Låst: trafiken blockeras",
  da="Låst: trafikken blokeres", no="Låst: trafikken blokkeres",
  pl="Blokada: ruch zablokowany", ptbr="Bloqueio: tráfego bloqueado",
  tr="Kilit: trafik engellendi")
s("ST_PAUSED", en="Paused",
  ja="一時停止中", fr="En pause", es="En pausa", de="Pausiert", it="In pausa", nl="Gepauzeerd",
  pt="Em pausa", ru="Приостановлено", ko="일시 정지됨", zht="已暫停", zhs="已暂停",
  fi="Tauolla", sv="Pausad", da="På pause", no="Pauset", pl="Wstrzymano", ptbr="Pausado",
  tr="Duraklatıldı")
s("ST_PAUSED_LEFT", en="Paused, %u min left",
  ja="一時停止中（残り%u分）", fr="En pause, encore %u min", es="En pausa, quedan %u min",
  de="Pausiert, noch %u Min.", it="In pausa, ancora %u min", nl="Gepauzeerd, nog %u min",
  pt="Em pausa, faltam %u min", ru="Приостановлено, осталось %u мин",
  ko="일시 정지됨, %u분 남음", zht="已暫停，剩餘 %u 分鐘", zhs="已暂停，剩余 %u 分钟",
  fi="Tauolla, %u min jäljellä", sv="Pausad, %u min kvar", da="På pause, %u min tilbage",
  no="Pauset, %u min igjen", pl="Wstrzymano, zostało %u min", ptbr="Pausado, faltam %u min",
  tr="Duraklatıldı, %u dk kaldı")
s("ST_NO_MODULE", en="VPN module not loaded",
  ja="VPNモジュールが読み込まれていません", fr="Module VPN non chargé", es="Módulo VPN no cargado",
  de="VPN-Modul nicht geladen", it="Modulo VPN non caricato", nl="VPN-module niet geladen",
  pt="Módulo VPN não carregado", ru="Модуль VPN не загружен", ko="VPN 모듈이 로드되지 않음",
  zht="未載入 VPN 模組", zhs="未加载 VPN 模块", fi="VPN-moduulia ei ole ladattu",
  sv="VPN-modulen är inte inläst", da="VPN-modulet er ikke indlæst", no="VPN-modulen er ikke lastet",
  pl="Moduł VPN nie jest załadowany", ptbr="Módulo VPN não carregado", tr="VPN modülü yüklenmedi")
s("ST_OLD_MODULE", en="VPN module is a different version: install matching files",
  ja="VPNモジュールのバージョンが違います：対応するファイルをインストールしてください",
  fr="Le module VPN est d'une autre version : installez les fichiers correspondants",
  es="El módulo VPN es de otra versión: instala los archivos que coincidan",
  de="VPN-Modul hat eine andere Version: passende Dateien installieren",
  it="Il modulo VPN è di un'altra versione: installa i file corrispondenti",
  nl="VPN-module heeft een andere versie: installeer bijpassende bestanden",
  pt="O módulo VPN é de outra versão: instale os ficheiros correspondentes",
  ru="Модуль VPN другой версии: установите подходящие файлы",
  ko="VPN 모듈 버전이 다릅니다: 맞는 파일을 설치하세요",
  zht="VPN 模組版本不同：請安裝相符的檔案", zhs="VPN 模块版本不同：请安装匹配的文件",
  fi="VPN-moduuli on eri versio: asenna yhteensopivat tiedostot",
  sv="VPN-modulen har en annan version: installera matchande filer",
  da="VPN-modulet har en anden version: installer filer, der passer",
  no="VPN-modulen har en annen versjon: installer filer som passer",
  pl="Moduł VPN ma inną wersję: zainstaluj pasujące pliki",
  ptbr="O módulo VPN é de outra versão: instale os arquivos correspondentes",
  tr="VPN modülü farklı sürümde: uyumlu dosyaları yükleyin")
s("ST_NO_PROFILE", en="No profile yet: connect once from the app",
  ja="プロファイルがありません：アプリから一度接続してください",
  fr="Aucun profil : connectez-vous une fois depuis l'app",
  es="Aún no hay perfil: conéctate una vez desde la app",
  de="Noch kein Profil: einmal in der App verbinden",
  it="Nessun profilo: connettiti una volta dall'app",
  nl="Nog geen profiel: maak één keer verbinding via de app",
  pt="Ainda sem perfil: ligue uma vez a partir da aplicação",
  ru="Профиля пока нет: подключитесь один раз из приложения",
  ko="아직 프로필 없음: 앱에서 한 번 연결하세요",
  zht="尚無設定檔：請先從應用程式連線一次", zhs="尚无配置文件：请先从应用连接一次",
  fi="Ei vielä profiilia: yhdistä kerran sovelluksesta",
  sv="Ingen profil än: anslut en gång från appen",
  da="Ingen profil endnu: forbind én gang fra appen",
  no="Ingen profil ennå: koble til én gang fra appen",
  pl="Brak profilu: połącz się raz z aplikacji",
  ptbr="Ainda sem perfil: conecte uma vez pelo app",
  tr="Henüz profil yok: uygulamadan bir kez bağlanın")
s("ST_TEST_BUILD", en="(test build)",
  ja="（テスト版）", fr="(version de test)", es="(versión de prueba)", de="(Testversion)",
  it="(versione di prova)", nl="(testversie)", pt="(versão de teste)", ru="(тестовая сборка)",
  ko="(테스트 빌드)", zht="（測試版）", zhs="（测试版）", fi="(testiversio)", sv="(testversion)",
  da="(testversion)", no="(testversjon)", pl="(wersja testowa)", ptbr="(versão de teste)",
  tr="(test sürümü)")
# "<profile>: <state>"; Japanese and Chinese use the full-width colon, French
# a space before it
s("FMT_NAME_STATE", en="%s: %s", ja="%s：%s", zht="%s：%s", zhs="%s：%s", fr="%s : %s",
  **{l: "%s: %s" for l in ["es", "de", "it", "nl", "pt", "ru", "ko", "fi", "sv", "da",
                           "no", "pl", "ptbr", "tr"]})

# Quick menu. Rows read "VPN - VPN", as Wi-Fi Turbo's
# "Wi-Fi Turbo - Keep Wi-Fi on"; the pause button is 400 px wide.
same("QM_PREFIX", "VPN")
same("QM_FMT_ROW", "%s - %s")
same("QM_ROW_TOGGLE", "VPN")
same("QM_ROW_HUD", "HUD")              # "VPN - HUD"
s("QM_LBL_PROFILE", en="Profile",
  ja="プロファイル", fr="Profil", es="Perfil", de="Profil", it="Profilo", nl="Profiel",
  pt="Perfil", ru="Профиль", ko="프로필", zht="設定檔", zhs="配置文件", fi="Profiili",
  sv="Profil", da="Profil", no="Profil", pl="Profil", ptbr="Perfil", tr="Profil")
s("QM_LBL_STATUS", en="Status",
  ja="状態", fr="État", es="Estado", de="Status", it="Stato", nl="Status", pt="Estado",
  ru="Состояние", ko="상태", zht="狀態", zhs="状态", fi="Tila", sv="Status", da="Status",
  no="Status", pl="Stan", ptbr="Status", tr="Durum")
s("QM_FMT_LABEL", en="%s: %s", ja="%s：%s", zht="%s：%s", zhs="%s：%s", fr="%s : %s",
  **{l: "%s: %s" for l in ["es", "de", "it", "nl", "pt", "ru", "ko", "fi", "sv", "da",
                           "no", "pl", "ptbr", "tr"]})
s("QM_NO_PROFILE", en="none yet",
  ja="まだありません", fr="pas encore", es="ninguno aún", de="noch keins",
  it="ancora nessuno", nl="nog geen", pt="nenhum ainda", ru="пока нет", ko="아직 없음",
  zht="尚無", zhs="尚无", fi="ei vielä", sv="ingen än", da="ingen endnu", no="ingen ennå",
  pl="jeszcze brak", ptbr="nenhum ainda", tr="henüz yok")
s("QM_PAUSE", en="Pause VPN 5 min",
  ja="VPNを5分間一時停止", fr="Pause VPN 5 min", es="Pausar VPN 5 min", de="VPN 5 Min. pausieren",
  it="Pausa VPN 5 min", nl="VPN 5 min pauzeren", pt="Pausar VPN 5 min", ru="Пауза VPN на 5 мин",
  ko="VPN 5분 일시 정지", zht="暫停 VPN 5 分鐘", zhs="暂停 VPN 5 分钟", fi="Keskeytä VPN 5 min",
  sv="Pausa VPN 5 min", da="Sæt VPN på pause 5 min", no="Pause VPN 5 min",
  pl="Wstrzymaj VPN na 5 min", ptbr="Pausar VPN 5 min", tr="VPN'i 5 dk duraklat")
s("QM_RESUME", en="Resume VPN",
  ja="VPNを再開", fr="Reprendre le VPN", es="Reanudar VPN", de="VPN fortsetzen",
  it="Riprendi VPN", nl="VPN hervatten", pt="Retomar VPN", ru="Возобновить VPN",
  ko="VPN 재개", zht="繼續 VPN", zhs="继续 VPN", fi="Jatka VPN:ää", sv="Återuppta VPN",
  da="Genoptag VPN", no="Gjenoppta VPN", pl="Wznów VPN", ptbr="Retomar VPN",
  tr="VPN'i sürdür")

# Short state words for the quick menu's "Status: <state>" row only (the
# Settings page keeps the full ST_* sentences). Each at most QM_STATE_MAX
# columns (check() counts code points, CJK and full-width as 2, %u as 2
# digits), so the longest label ("Состояние: ", 11) plus a state stays within
# the row budget (QM_ROW_MAX). vtw_state_word()'s ids map one to one
# (status.c, vtw_state_short_id).
s("QM_ST_OFF", en="Off",
  ja="オフ", fr="Désactivé", es="Desactivado", de="Aus", it="Disattivato", nl="Uit",
  pt="Desligado", ru="Выкл.", ko="끔", zht="關閉", zhs="关闭", fi="Pois",
  sv="Av", da="Fra", no="Av", pl="Wyłączony", ptbr="Desligado", tr="Kapalı")
s("QM_ST_CONNECTING", en="Connecting",
  ja="接続中", fr="Connexion", es="Conectando", de="Verbinden", it="Connessione",
  nl="Verbinden", pt="A ligar", ru="Подключение", ko="연결 중", zht="連線中", zhs="正在连接",
  fi="Yhdistetään", sv="Ansluter", da="Forbinder", no="Kobler til", pl="Łączenie",
  ptbr="Conectando", tr="Bağlanıyor")
s("QM_ST_CONNECTED", en="Connected",
  ja="接続済み", fr="Connecté", es="Conectado", de="Verbunden", it="Connesso", nl="Verbonden",
  pt="Ligado", ru="Подключено", ko="연결됨", zht="已連線", zhs="已连接", fi="Yhdistetty",
  sv="Ansluten", da="Forbundet", no="Tilkoblet", pl="Połączono", ptbr="Conectado", tr="Bağlı")
s("QM_ST_NOT_PROTECTED", en="Not protected",
  ja="未保護", fr="Non protégé", es="Sin protección", de="Ungeschützt", it="Non protetto",
  nl="Niet beschermd", pt="Sem proteção", ru="Не защищено", ko="보호되지 않음",
  zht="未受保護", zhs="未受保护", fi="Ei suojattu", sv="Inte skyddad", da="Ikke beskyttet",
  no="Ikke beskyttet", pl="Brak ochrony", ptbr="Sem proteção", tr="Korunmuyor")
s("QM_ST_KILL_HOLDING", en="Traffic held",
  ja="通信保留中", fr="Trafic retenu", es="Tráfico parado", de="Zurückgehalten",
  it="Traffico fermo", nl="Verkeer vast", pt="Tráfego retido", ru="Трафик удержан",
  ko="트래픽 보류", zht="流量攔截中", zhs="流量拦截中", fi="Liikenne seis", sv="Trafik hålls",
  da="Trafik holdt", no="Trafikk holdt", pl="Ruch stoi", ptbr="Tráfego retido",
  tr="Trafik tutuldu")
s("QM_ST_BLOCKED", en="Blocked",
  ja="ブロック中", fr="Bloqué", es="Bloqueado", de="Blockiert", it="Bloccato", nl="Geblokkeerd",
  pt="Bloqueado", ru="Заблокировано", ko="차단됨", zht="已封鎖", zhs="已阻止", fi="Estetty",
  sv="Blockerad", da="Blokeret", no="Blokkert", pl="Zablokowano", ptbr="Bloqueado",
  tr="Engellendi")
s("QM_ST_LOCKDOWN", en="Lockdown",
  ja="ロックダウン", fr="Verrouillage", es="Bloqueo", de="Sperrmodus", it="Blocco",
  nl="Vergrendeld", pt="Bloqueio", ru="Блокировка", ko="잠금", zht="鎖定", zhs="锁定",
  fi="Lukitus", sv="Låst", da="Låst", no="Låst", pl="Blokada", ptbr="Bloqueio", tr="Kilit")
s("QM_ST_PAUSED", en="Paused",
  ja="一時停止中", fr="En pause", es="En pausa", de="Pausiert", it="In pausa", nl="Gepauzeerd",
  pt="Em pausa", ru="Приостановлено", ko="일시 정지됨", zht="已暫停", zhs="已暂停",
  fi="Tauolla", sv="Pausad", da="På pause", no="Pauset", pl="Wstrzymano", ptbr="Pausado",
  tr="Duraklatıldı")
s("QM_ST_PAUSED_LEFT", en="Paused %u min",
  ja="一時停止 %u分", fr="Pause %u min", es="Pausa %u min", de="Pause %u Min.",
  it="Pausa %u min", nl="Pauze %u min", pt="Pausa %u min", ru="Пауза %u мин",
  ko="일시 정지 %u분", zht="暫停 %u 分鐘", zhs="暂停 %u 分钟", fi="Tauko %u min",
  sv="Paus %u min", da="Pause %u min", no="Pause %u min", pl="Pauza %u min",
  ptbr="Pausa %u min", tr="Durakl. %u dk")
s("QM_ST_NO_MODULE", en="Not loaded",
  ja="未読み込み", fr="Non chargé", es="No cargado", de="Nicht geladen", it="Non caricato",
  nl="Niet geladen", pt="Não carregado", ru="Не загружен", ko="로드 안 됨", zht="未載入",
  zhs="未加载", fi="Ei ladattu", sv="Inte laddad", da="Ikke indlæst", no="Ikke lastet",
  pl="Nie wczytano", ptbr="Não carregado", tr="Yüklenmedi")
s("QM_ST_OLD_MODULE", en="Wrong version",
  ja="バージョン違い", fr="Autre version", es="Otra versión", de="Andere Version",
  it="Altra versione", nl="Andere versie", pt="Outra versão", ru="Другая версия",
  ko="버전 다름", zht="版本不同", zhs="版本不同", fi="Eri versio", sv="Annan version",
  da="Anden version", no="Annen versjon", pl="Inna wersja", ptbr="Outra versão",
  tr="Farklı sürüm")
s("QM_ST_NO_PROFILE", en="No profile",
  ja="未設定", fr="Aucun profil", es="Sin perfil", de="Kein Profil", it="Nessun profilo",
  nl="Geen profiel", pt="Sem perfil", ru="Нет профиля", ko="프로필 없음", zht="無設定檔",
  zhs="无配置文件", fi="Ei profiilia", sv="Ingen profil", da="Ingen profil", no="Ingen profil",
  pl="Brak profilu", ptbr="Sem perfil", tr="Profil yok")

# System notification (compiled out for now)
s("NOTE_DROP", en="VPN dropped - traffic is not protected",
  ja="VPNが切断されました - 通信は保護されていません",
  fr="VPN coupé - le trafic n'est pas protégé", es="VPN caída - el tráfico no está protegido",
  de="VPN getrennt - Datenverkehr ist ungeschützt", it="VPN caduta - il traffico non è protetto",
  nl="VPN weggevallen - verkeer is niet beschermd", pt="A VPN caiu - o tráfego não está protegido",
  ru="VPN отключился - трафик не защищён", ko="VPN 연결 끊김 - 트래픽이 보호되지 않습니다",
  zht="VPN 已中斷 - 流量未受保護", zhs="VPN 已断开 - 流量未受保护",
  fi="VPN katkesi - liikenne ei ole suojattu", sv="VPN bröts - trafiken är inte skyddad",
  da="VPN afbrudt - trafikken er ikke beskyttet", no="VPN falt ut - trafikken er ikke beskyttet",
  pl="VPN rozłączony - ruch nie jest chroniony", ptbr="A VPN caiu - o tráfego não está protegido",
  tr="VPN koptu - trafik korunmuyor")


def check():
    """Every language complete, with English's placeholders in the same order."""
    import re
    bad = 0
    for k in ORDER:
        want = re.findall(r"%[su]", T[k]["en"])
        for l in LANGS:
            if l not in T[k]:
                print(f"{k}: no {l}")
                bad += 1
            elif re.findall(r"%[su]", T[k][l]) != want:
                print(f"{k}/{l}: placeholders differ from English")
                bad += 1
    return bad + check_lengths()


# Quick-menu width budgets, in columns (cols()). The text rows are 640 px
# wide (quickmenu.c text_row); Wi-Fi Turbo's quick menu, checked on the
# console, shows "Wi-Fi Turbo - WLAN eingeschaltet lassen" (39) in the same
# 640 px row and "Max performance" (15) on its 260 px buttons (~17 px per
# column). The budgets keep a wide margin for wider glyphs (Cyrillic
# capitals, CJK): a row "<label>: <value>" at most 30, a state value at most
# 14, the 400 px pause button at most 22.
QM_STATE_MAX = 14
QM_ROW_MAX = 30
QM_BUTTON_MAX = 22
QM_LIMITS = {"QM_NO_PROFILE": QM_STATE_MAX, "QM_PAUSE": QM_BUTTON_MAX,
             "QM_RESUME": QM_BUTTON_MAX}


def cols(text):
    """Display columns: one per code point, two for East Asian wide or
    full-width ones; a %u counts as two digits, %s as nothing."""
    import unicodedata
    text = text.replace("%u", "99").replace("%s", "")
    return sum(2 if unicodedata.east_asian_width(c) in "WF" else 1 for c in text)


def qm_widths(k):
    """{lang: columns} for one id."""
    return {l: cols(T[k][l]) for l in LANGS if l in T[k]}


def check_lengths():
    bad = 0
    for k in ORDER:
        lim = QM_STATE_MAX if k.startswith("QM_ST_") else QM_LIMITS.get(k)
        if lim is None:
            continue
        for l, n in qm_widths(k).items():
            if n > lim:
                print(f"{k}/{l}: {n} columns, the quick menu allows {lim}: {T[k][l]!r}")
                bad += 1
    # The Status and Profile rows: the longest label with its separator and
    # the widest value it can show.
    for l in LANGS:
        fmt = T["QM_FMT_LABEL"][l]
        for lbl, values in (("QM_LBL_STATUS", [k for k in ORDER if k.startswith("QM_ST_")]),
                            ("QM_LBL_PROFILE", ["QM_NO_PROFILE"])):
            for v in values:
                row = fmt.replace("%s", T[lbl][l], 1).replace("%s", T[v][l], 1)
                if cols(row) > QM_ROW_MAX:
                    print(f"{lbl}+{v}/{l}: {cols(row)} columns, the row allows {QM_ROW_MAX}: {row!r}")
                    bad += 1
    return bad


def esc(t):
    return t.replace("\\", "\\\\").replace('"', '\\"')


def main():
    if check():
        raise SystemExit("gen_i18n.py: fix the table first")
    here = os.path.dirname(os.path.abspath(__file__))
    src = os.path.join(here, "..", "settings_app")
    head = "/* Generated by tools/gen_i18n.py - edit the strings there, then regenerate. */"

    # The quick menu's ids first (stable order inside each group).
    order = [k for k in ORDER if is_qm(k)] + [k for k in ORDER if not is_qm(k)]
    qm_count = sum(1 for k in ORDER if is_qm(k))

    ids = [head, "",
           "/* The quick menu's ids are the first VTW_I18N_QM_COUNT: its build",
           "   (VTW_I18N_QM_ONLY) keeps only those rows. */",
           f"#define VTW_I18N_QM_COUNT {qm_count}", "",
           "#define VTW_I18N_IDS(X) \\"]
    ids += [f"\tX({k}) \\" for k in order]
    ids[-1] = ids[-1][:-2]   # no continuation after the last id
    with open(os.path.join(src, "i18n_ids.h"), "w") as f:
        f.write("\n".join(ids) + "\n")

    out = [head,
           "/* Columns: " + " ".join(LANGS) + " (enum vtw_lang_id order). */",
           "/* clang-format off */",
           "",
           "#ifdef VTW_I18N_QM_ONLY",
           "#define VTW_I18N_ROWS VTW_I18N_QM_COUNT",
           "#else",
           "#define VTW_I18N_ROWS VTW_I18N_COUNT",
           "#endif",
           "",
           "static const char *const g_tbl[VTW_I18N_ROWS][VTW_LANG_COUNT] = {"]
    copies = 0
    for n, k in enumerate(order):
        row = T[k]
        if n == qm_count:
            out.append("#ifndef VTW_I18N_QM_ONLY   /* the quick menu needs only the rows above */")
        out.append(f"[VTW_I18N_{k}] = {{")
        for l in LANGS:
            if l in row:
                out.append(f'\t"{esc(row[l])}",   /* {l} */')
            else:
                copies += 1
                out.append(f'\t"{esc(row["en"])}",   /* {l}: English copy, to translate */')
        out.append("},")
    out.append("#endif /* VTW_I18N_QM_ONLY */")
    out.append("};")
    with open(os.path.join(src, "i18n_table.h"), "w") as f:
        f.write("\n".join(out) + "\n")
    print("wrote", len(ORDER), "strings x", len(LANGS), "languages;",
          copies, "entries are English copies to translate")
    if "--widths" in sys.argv:
        for k in ORDER:
            if k.startswith("QM_") and (k.startswith("QM_ST_") or k in QM_LIMITS):
                w = qm_widths(k)
                print(f"{k:22} max {max(w.values()):2}  " +
                      " ".join(f"{l}={n}" for l, n in w.items()))


if __name__ == "__main__":
    main()
