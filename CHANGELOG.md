# Änderungen / Changelog

## 1.7.0 (Build 14)

- **Neu: Extras → Cache verlagern …** legt den Cache von Browsern und Programmen auf ein RAM-Laufwerk. Das schont die SSD und kann schneller sein.
  - Unterstützt: Chrome (auch Beta/Canary), Chromium, Edge (alle Kanäle), Brave, Vivaldi, Yandex, Cent Browser, Comodo Dragon, Ulaa, Opera, Opera GX, Firefox, LibreWolf, Waterfox, Pale Moon, Thunderbird, VS Code (auch Insiders), Discord, Slack, Teams classic, Telegram, Steam und DuckDuckGo.
  - Bei Chromium-Browsern wird jedes Profil einzeln angezeigt.
  - Die Liste zeigt Größe und Dateizahl jedes Caches, und ob er schon umgeleitet ist (auch durch andere Programme) oder ob das Ziel fehlt.
  - Jedes Programm und Profil bekommt einen eigenen Ordner, z. B. `R:\Cache\Chrome-Default`. RamDiskBox trägt ihn in die Ordnerliste des Laufwerks ein und legt ihn so bei jedem Einhängen neu an.
  - Die Umleitung ist eine Verzeichnisverbindung (Junction) und braucht keine Administratorrechte.
  - Laufen die betroffenen Programme noch, verweigert RamDiskBox das Verschieben und nennt sie.
  - Beim Löschen des alten Caches folgt RamDiskBox keinen Verknüpfungen. War der Cache schon woandershin umgeleitet, wird nur die Verbindung ersetzt, das alte Ziel bleibt unangetastet.
  - **Standard wiederherstellen** entfernt die Umleitung; das Programm legt seinen Cache wieder an der normalen Stelle an.

**English**

- **New: Tools → Relocate caches …** moves the cache of browsers and programs to a RAM drive. This spares the SSD and can be faster.
  - Supported: Chrome (also Beta/Canary), Chromium, Edge (all channels), Brave, Vivaldi, Yandex, Cent Browser, Comodo Dragon, Ulaa, Opera, Opera GX, Firefox, LibreWolf, Waterfox, Pale Moon, Thunderbird, VS Code (also Insiders), Discord, Slack, Teams classic, Telegram, Steam and DuckDuckGo.
  - Chromium browsers list every profile separately.
  - The list shows size and file count of each cache, and whether it is already redirected (also by other programs) or its target is missing.
  - Every program and profile gets its own folder, e.g. `R:\Cache\Chrome-Default`. RamDiskBox adds it to the drive's folder list, so it is recreated at every mount.
  - The redirection is a directory junction and needs no administrator rights.
  - If the affected programs are still running, RamDiskBox refuses to move and names them.
  - When deleting the old cache, RamDiskBox follows no links. If the cache was already redirected elsewhere, only the link is replaced and the old target is left alone.
  - **Restore default** removes the redirection; the program creates its cache in the normal place again.

## 1.6.4 (Build 13)

- **Speicherleiste zählt die Größe der RAM-Laufwerke:**
  - Neuer, schraffierter Abschnitt „Reserviert“: der Teil der eingehängten RAM-Laufwerke, der noch nicht im Arbeitsspeicher belegt ist.
  - „Frei“ ist jetzt der verfügbare Speicher abzüglich dieser Reserve.
  - Laufwerke von SoftPerfect RAM Disk werden erkannt und mitgezählt.
  - Ist mehr reserviert als frei, wird der Abschnitt rot („zu viel“); wird es knapp, wird „Frei“ gelb.
  - Die Kurzinfo nennt alle Werte und jedes Laufwerk einzeln.
- **Spalten passen sich an:**
  - Laufwerk, Größe und Komprimierung richten sich nach dem Inhalt.
  - Bezeichnung, Belegung und Image teilen sich den restlichen Platz.
  - Rechts bleibt keine leere Fläche, und es gibt keinen Rollbalken mehr.
  - Bezeichnung und Belegung lassen sich weiter von Hand ziehen; ein Doppelklick auf den Trenner macht sie wieder automatisch.
  - Lange Image-Pfade werden in der Mitte gekürzt, der Dateiname bleibt sichtbar.
- **Fenstergröße:**
  - Die Mindestgröße richtet sich nach dem Inhalt (kleiner als bisher).
  - Die Statuszeile lässt bei Platzmangel zuerst die Betriebsart weg.
  - Gekürzter Text erscheint als Kurzinfo.
  - Weniger Flackern beim Ziehen.
  - Spalten und Schrift passen sich beim Wechsel auf einen Monitor mit anderer Skalierung an.
- **Behoben:** Beim Hochfahren startet der RamDiskBox-Dienst oft vor dem Windows Defender. Die Defender-Ausnahme schlug dann fehl (z. B. Fehler 0x80041001), und das Laufwerk zeigte ein Warnsymbol. RamDiskBox versucht es jetzt im Hintergrund erneut (nach 15 s, 30 s, 60 s, ... höchstens alle 5 Minuten), bis es klappt; das Warnsymbol verschwindet dann von selbst.
- **Behoben:** Hatte man das Laufwerk selbst in den Defender-Ausnahmen eingetragen, entfernte RamDiskBox diese Ausnahme beim Aushängen. Eine bereits vorhandene Ausnahme wird jetzt nur übernommen und bleibt erhalten. Eine eigene, übrig gebliebene Ausnahme räumt RamDiskBox später zuverlässig auf.

**English**

- **Memory bar counts the size of RAM drives:**
  - New hatched "Reserved" segment: the part of the mounted RAM drives not yet taken from memory.
  - "Free" is now the available memory minus this reserve.
  - SoftPerfect RAM Disk drives are detected and included.
  - If more is reserved than free, the segment turns red ("too much"); when memory gets low, "Free" turns amber.
  - The tooltip lists all values and every drive.
- **Columns adapt:**
  - Drive, size and compression follow their content.
  - Label, usage and image share the remaining space.
  - No empty area on the right and no scroll bar any more.
  - Label and usage can still be dragged; double-clicking the divider makes them automatic again.
  - Long image paths are shortened in the middle so the file name stays visible.
- **Window size:**
  - The minimum size follows the content (smaller than before).
  - The status bar drops the mode first when space is short.
  - Shortened text appears as a tooltip.
  - Less flicker while resizing.
  - Columns and font adapt when moving to a monitor with different scaling.
- **Fixed:** at system startup the RamDiskBox service often starts before Windows Defender. The Defender exclusion then failed (e.g. error 0x80041001), and the drive showed a warning icon. RamDiskBox now retries in the background (after 15 s, 30 s, 60 s, ... at most every 5 minutes) until it succeeds; the warning icon then disappears by itself.
- **Fixed:** if you had added the drive to the Defender exclusions yourself, RamDiskBox removed that exclusion on dismount. An existing exclusion is now only adopted and kept. RamDiskBox reliably cleans up a leftover exclusion of its own later.

## 1.6.2 (Build 11)

- **Frisch geschriebene Dateien bleiben im Cache:** Damit Programme nach dem Ändern einer Datei die richtige Änderungszeit sehen (seit 1.4.1), hat RamDiskBox Windows bisher über die Änderung benachrichtigt. WinFsp verwirft dabei aber immer auch den Cache der Datei, das erste Lesen danach war deutlich langsamer. Bei Dateien ab 1 MB frischt RamDiskBox die Angaben jetzt auf einem anderen Weg auf, und der Cache bleibt erhalten. Gemessen im Dienstmodus: erstes Lesen einer gerade geschriebenen 2-GB-Datei 5–6 statt 2,5 GB/s. Änderungszeit, Größe und Attribute stimmen weiterhin sofort.

**English**

- **Freshly written files stay in the cache:** so that programs see the correct modification time after changing a file (since 1.4.1), RamDiskBox notified Windows about the change. WinFsp always discards the file's cache when doing that, so the first read afterwards was much slower. For files of 1 MB and more, RamDiskBox now refreshes the information another way, and the cache is kept. Measured in service mode: first read of a just-written 2 GB file 5–6 instead of 2.5 GB/s. Modification time, size and attributes are still correct immediately.

## 1.6.1 (Build 10)

- **Große Dateien schneller hineinkopieren:** Bei Laufwerken mit Windows-Cache schreiben Kopiervorgänge (Explorer, CopyFile, robocopy) jetzt am Cache vorbei. Bisher füllte eine große Kopie den Cache, und Windows hielt den Kopiervorgang immer wieder für Sekunden an (Einbrüche in der Kopieranzeige). Gemessen mit einer 8-GB-Datei: Explorer 1,85 statt 1,05 GB/s, gleichmäßig und ohne Einbrüche. Lesen und alle anderen Programme nutzen den Cache weiter wie bisher.

**English**

- **Faster copying of large files onto the drive:** on drives with the Windows cache, copy operations (Explorer, CopyFile, robocopy) now write past the cache. Previously a large copy filled the cache, and Windows repeatedly paused the copy for seconds (dips in the copy dialog). Measured with an 8 GB file: Explorer 1.85 instead of 1.05 GB/s, steady and without dips. Reading and all other programs keep using the cache as before.

## 1.6.0 (Build 9)

- **Aufgeräumte Oberfläche:**
  - Symbolleiste mit großen, farbigen Symbolen statt Textknöpfen, mit Kurzhilfe beim Überfahren.
  - Weniger Spalten: Status als Symbol vor dem Buchstaben, Belegung als Balken mit Prozent.
  - Speicherleiste unten: RAM-Laufwerke, frei und andere Programme.
  - Die Statuszeile zeigt die Angaben zum ausgewählten Laufwerk.
  - Gesamtzähler und Höchstwerte stehen jetzt unter **Extras → Statistik**.
- Die Symbole kommen aus der Windows-Symbolschrift, sind bei jeder Skalierung scharf und passen sich Hell/Dunkel an.

**English**

- **Tidier user interface:**
  - Toolbar with large colored icons instead of text buttons, with tooltips.
  - Fewer columns: status as an icon before the letter, usage as a bar with percent.
  - Memory bar at the bottom: RAM drives, free and other programs.
  - The status line shows details of the selected drive.
  - Lifetime counters and peaks moved to **Tools → Statistics**.
- Icons come from the Windows icon font, are sharp at any scaling and follow light/dark mode.

## 1.5.0 (Build 8)

- **Schneller im Dienstmodus:** Der Windows-Cache bleibt beim Schließen einer Datei erhalten. Erneutes Öffnen und Lesen ist damit bei aktivem Defender rund 30-mal so schnell (gemessen: 7.200 statt 260 kleine Dateien pro Sekunde). Vor jeder Image-Sicherung wird das Laufwerk zurückgeschrieben; klappt das nicht, wird das Image nicht gespeichert und eine Meldung erscheint. Ohne Dienst bleibt es beim bisherigen, sicheren Verhalten.
- **Defender-Ausnahme per UAC:** Einstellungen, die der Dienst nur von Administratoren annimmt (Defender-Ausnahme, Quellordner, Image außerhalb des Image-Ordners), übernimmt die Oberfläche jetzt nach Rückfrage mit Administratorrechten. Die Defender-Ausnahme macht das Anlegen und Öffnen vieler kleiner Dateien gemessen 19- bis 41-mal schneller.
- **Behoben (Setup):** Ein stilles Update (`/quiet`) entfernte den Autostart der Oberfläche.
- `rdbctl` versteht `--instance=NAME` und `--lang=de|en` als führende Optionen.

**English**

- **Faster in service mode:** the Windows cache is kept when a file is closed. With Defender active, re-opening and reading are about 30 times faster (measured: 7,200 instead of 260 small files per second). Before every image backup the drive is written back; if that fails, the image is not saved and a message is shown. Without the service the previous safe behavior stays.
- **Defender exclusion via UAC:** settings that the service accepts only from administrators (Defender exclusion, source folder, image outside the image folder) are now applied by the user interface with administrator rights after asking. In measurements the Defender exclusion made creating and opening many small files 19 to 41 times faster.
- **Fixed (setup):** a quiet update (`/quiet`) removed the user interface's autostart.
- `rdbctl` accepts `--instance=NAME` and `--lang=de|en` as leading options.

## 1.4.1 (Build 7)

- **Behoben:** Ein beim Einhängen gesperrtes oder nicht lesbares Image (z. B. von einem Sicherungsprogramm geöffnet, fehlende Rechte) wurde fälschlich als beschädigt behandelt. Das Laufwerk startete dann leer und überschrieb beim Aushängen das gute Image. Jetzt wird das Laufwerk in diesem Fall nicht eingehängt, und die Meldung nennt den Grund. Dasselbe gilt, wenn der Speicherort des Images nicht erreichbar ist (Netz- oder USB-Laufwerk fehlt).
- **Behoben:** Nach dem Schreiben in eine vorhandene Datei, dem Kürzen oder Verlängern zeigten Programme eine veraltete Änderungszeit (Laufwerke mit Windows-Cache).
- **Behoben:** Der gemerkte Buchstabe eines getrennten Netzlaufwerks galt als frei und konnte für ein RAM-Laufwerk vorgeschlagen werden.

**English**

- **Fixed:** An image that was locked or unreadable at mount time (e.g. opened by a backup program, missing permissions) was treated as damaged. The drive then started empty and overwrote the good image on unmount. The drive is now not mounted in that case, and the message says why. The same applies when the image location is unreachable (network or USB drive missing).
- **Fixed:** After writing to an existing file, truncating or extending it, programs saw an outdated modification time (drives with Windows cache).
- **Fixed:** The remembered letter of a disconnected network drive was treated as free and could be offered for a RAM drive.

## 1.4.0 (Build 6)

- Setup: für alle Benutzer mit Windows-Dienst oder nur für mich, mit Deinstallation / for all users with Windows service or just for me, with uninstaller
- Sicherheit im Dienstmodus: ohne Administratorrechte Images nur im Image-Ordner, Quellordner und Defender-Ausnahme nur für Administratoren / service mode security: without administrator rights, images only in the image folder; source folder and Defender exclusion for administrators only

## 1.3.0 (Build 4)

- Neuer Name RamDiskBox; Deutsch/Englisch automatisch nach Windows-Sprache (Oberfläche, Kommandozeile, Dienst); Hell- und Dunkelmodus / renamed to RamDiskBox; German/English chosen from the Windows language; light and dark mode

## 1.2.0 (Build 3)

- zstd als Komprimierungsstufe „Stark“, Quellordner beim Einhängen, Ausnahme vom Defender-Echtzeitschutz / zstd as "strong" compression, source folder at mount, Defender exclusion

## 1.1.0 (Build 2)

- Komprimierung im Arbeitsspeicher (LZ4, LZ4 HC) / in-memory compression (LZ4, LZ4 HC)

## 1.0.0 (Build 1)

- Erste Version / first release
