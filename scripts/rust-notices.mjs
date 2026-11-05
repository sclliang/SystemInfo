import { execFileSync } from "node:child_process";
import { readFileSync, readdirSync, writeFileSync } from "node:fs";
import { basename, dirname, join } from "node:path";

// Keep the distributable notices tied to the resolved Windows dependency tree.
const data = JSON.parse(execFileSync("cargo", ["metadata","--locked","--format-version","1",
  "--filter-platform","x86_64-pc-windows-msvc","--manifest-path","src-tauri/Cargo.toml"],
  {encoding:"utf8",maxBuffer:48*1024*1024}));
const texts = new Map(), rows = [];
for (const pkg of data.packages.filter(p => p.source).sort((a,b) => a.name.localeCompare(b.name))) {
  const dir = dirname(pkg.manifest_path);
  const files = readdirSync(dir,{withFileTypes:true})
    .filter(f => f.isFile() && /^(licen[cs]e|copying|notice)([-_.].*)?$/i.test(f.name)).map(f => join(dir,f.name));
  if (pkg.license_file) files.push(join(dir,pkg.license_file));
  const refs = [];
  for (const file of new Set(files)) {
    const text = readFileSync(file,"utf8").trim();
    if (!text) continue;
    if (!texts.has(text)) texts.set(text,texts.size + 1);
    refs.push(basename(file) + " [" + texts.get(text) + "]");
  }
  rows.push("- " + pkg.name + " " + pkg.version + " · " + (pkg.license || "见许可文件") + (refs.length ? " · " + refs.join(", ") : ""));
}
let result = "# SystemInfo — Third-party notices\n\nAtaSmartCore: see licenses/AtaSmartCore-LICENSE.txt.\n\n" + rows.join("\n");
for (const [text,id] of texts) result += "\n\n## [" + id + "]\n\n" + text;
writeFileSync(process.argv[2], result + "\n");
