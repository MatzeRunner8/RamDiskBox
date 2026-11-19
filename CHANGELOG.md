# Änderungen / Changelog

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
