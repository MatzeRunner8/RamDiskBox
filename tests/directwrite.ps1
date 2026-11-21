# Testinstanz (RDB_INSTANCE=dev, startet und beendet sich selbst; nutzt nur Y:):
# 1) Korrektheit bei Cache-Umgehung (FILE_SEQUENTIAL_ONLY + Schreiben, seit 1.6.1)
# 2) Auffrischen der Metadaten nach dem Schliessen (seit 1.6.2) blockiert keine Programme, die die
#    Datei sofort wieder oeffnen (FileShare.Read, exklusiv, Ersetzen)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$bin = "$root\build\bin"
$env:RDB_INSTANCE = 'dev'; $env:RDB_LANG = 'de'
Remove-Item Env:RDB_DIRECTWRITE -ErrorAction SilentlyContinue
if (Test-Path Y:\) { throw "Y: belegt" }
$h = Start-Process "$bin\RamDiskBoxSvc.exe" -ArgumentList run -WindowStyle Hidden -PassThru
Start-Sleep -Seconds 2
& "$bin\rdbctl.exe" create Y 2G label=RDBDirectTest noauto cache | Out-Null
Start-Sleep -Seconds 1
if (((cmd /c vol Y:) -join ' ') -notmatch 'RDBDirectTest') { throw "Y: ist nicht das Testlaufwerk" }
Add-Type -TypeDefinition @'
using System; using System.IO; using System.IO.MemoryMappedFiles; using System.Linq; using System.Collections.Generic;
public static class DW {
    static List<string> log = new List<string>();
    static void Check(string name, bool ok) { log.Add((ok ? "OK     " : "FEHLER ") + name); }
    static FileStream Seq(string p, FileMode m) { return new FileStream(p, m, FileAccess.ReadWrite, FileShare.ReadWrite, 1, FileOptions.SequentialScan); }
    static FileStream Cached(string p) { return new FileStream(p, FileMode.Open, FileAccess.ReadWrite, FileShare.ReadWrite, 1, FileOptions.None); }
    public static string Run(string dir) {
        var rnd = new Random(5);
        string a = Path.Combine(dir, "a.bin"); var exp = new List<byte>();
        using (var f = Seq(a, FileMode.CreateNew))
            foreach (int n in new[] { 7, 4093, 65537, 1, 131071, 512, 3, 1000003 }) { var b = new byte[n]; rnd.NextBytes(b); f.Write(b, 0, n); exp.AddRange(b); }
        Check("Ungerade Blockgroessen geschrieben und gelesen", File.ReadAllBytes(a).SequenceEqual(exp));
        using (var f = Seq(a, FileMode.Open)) {
            bool ok = true;
            for (int k = 0; k < 200 && ok; k++) { long pos = rnd.Next(0, exp.Count - 5000); int len = rnd.Next(1, 5000); var b = new byte[len]; f.Position = pos; int got = f.Read(b, 0, len); for (int j = 0; j < got; j++) if (b[j] != exp[(int)pos + j]) { ok = false; break; } }
            Check("Ungerade Lesepositionen", ok);
        }
        var arr = exp.ToArray();
        using (var c = Cached(a)) using (var s = Seq(a, FileMode.Open)) {
            var p1 = new byte[3000]; rnd.NextBytes(p1); c.Position = 100; c.Write(p1, 0, p1.Length); Array.Copy(p1, 0, arr, 100, p1.Length);
            var p2 = new byte[777]; rnd.NextBytes(p2); s.Position = 500001; s.Write(p2, 0, p2.Length); Array.Copy(p2, 0, arr, 500001, p2.Length);
            var p3 = new byte[50]; rnd.NextBytes(p3); s.Position = 120; s.Write(p3, 0, p3.Length); Array.Copy(p3, 0, arr, 120, p3.Length);
            var back = new byte[200]; c.Position = 100; c.Read(back, 0, 200);
            bool seen = true; for (int j = 0; j < 200; j++) if (back[j] != arr[100 + j]) { seen = false; break; }
            Check("Cache-Handle sieht direkte Schreibvorgaenge sofort", seen);
        }
        Check("Gemischte Schreibvorgaenge nach dem Schliessen vollstaendig", File.ReadAllBytes(a).SequenceEqual(arr));
        using (var s = Seq(a, FileMode.Open)) { s.SetLength(70001); s.SetLength(90000); }
        var d = File.ReadAllBytes(a);
        Check("Kuerzen/Verlaengern", d.Length == 90000 && d.Take(70001).SequenceEqual(arr.Take(70001)) && d.Skip(70001).All(x => x == 0));
        string e = Path.Combine(dir, "e.log");
        using (var s = new FileStream(e, FileMode.Append, FileAccess.Write, FileShare.Read, 1, FileOptions.SequentialScan)) for (int k = 0; k < 1000; k++) s.WriteByte((byte)(k % 251));
        var eb = File.ReadAllBytes(e); bool eok = eb.Length == 1000; for (int k = 0; k < eb.Length && eok; k++) if (eb[k] != (byte)(k % 251)) eok = false;
        Check("1000 x 1 Byte anhaengen", eok);
        string m = Path.Combine(dir, "m.bin"); File.WriteAllBytes(m, new byte[1 << 20]);
        using (var mfs = new FileStream(m, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
        using (var mmf = MemoryMappedFile.CreateFromFile(mfs, null, 0, MemoryMappedFileAccess.Read, HandleInheritability.None, true))
        using (var view = mmf.CreateViewAccessor(0, 0, MemoryMappedFileAccess.Read)) {
            using (var s = Seq(m, FileMode.Open)) { s.Position = 4096; s.WriteByte(0x5A); }
            log.Add("INFO   Eingeblendete Ansicht nach direktem Schreiben: " + (view.ReadByte(4096) == 0x5A ? "aktuell" : "veraltet (wie bei NTFS ungepuffert)"));
        }
        Check("Dateiinhalt nach direktem Schreiben", File.ReadAllBytes(m)[4096] == 0x5A);
        // Auffrischen nach dem Schliessen darf sofortiges Wiederoeffnen nicht stoeren
        var data = new byte[2 << 20]; rnd.NextBytes(data);
        int failRead = 0, failExcl = 0, failReplace = 0;
        for (int i = 0; i < 200; i++) {
            string p = Path.Combine(dir, "r" + (i % 4) + ".bin");
            File.WriteAllBytes(p, data);
            try { File.ReadAllBytes(p); } catch (IOException) { failRead++; }
            File.WriteAllBytes(p, data);
            try { using (new FileStream(p, FileMode.Open, FileAccess.ReadWrite, FileShare.None)) { } } catch (IOException) { failExcl++; }
            File.WriteAllBytes(p + ".tmp", data);
            try { File.Delete(p); File.Move(p + ".tmp", p); } catch (IOException) { failReplace++; }
        }
        Check(string.Format("Sofort wieder oeffnen (200 Runden): Lesen {0}, exklusiv {1}, Ersetzen {2} Fehlschlaege", failRead, failExcl, failReplace),
              failRead + failExcl + failReplace == 0);
        return string.Join("\n", log);
    }
}
'@ -ReferencedAssemblies System.Core
$fails = 0
try { $out = [DW]::Run('Y:\'); $out; $fails = ([regex]::Matches($out, '^FEHLER', 'Multiline')).Count }
catch { "FEHLER Ausnahme: $($_.Exception.InnerException.Message)"; $fails = 1 }
& "$bin\rdbctl.exe" delete Y force discard | Out-Null
Start-Sleep -Seconds 1
& "$bin\rdbctl.exe" quit 2>$null | Out-Null
Start-Sleep -Seconds 1
if (-not $h.HasExited) { Stop-Process -Id $h.Id -Force }
"Fehler gesamt: $fails"
