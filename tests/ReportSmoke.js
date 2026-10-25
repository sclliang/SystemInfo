// Development-only DOM verification. No browser, network access or EXE dependency.
// npm install --prefix build/html-validation --no-save jsdom@30.1.3
// node tests/ReportSmoke.js <diagnostic-fixture-root> <collection-directory>
"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const { pathToFileURL, fileURLToPath } = require("node:url");
const { JSDOM, VirtualConsole } = require("../build/html-validation/node_modules/jsdom");
const viewerScript = fs.readFileSync(path.join(__dirname,"../assets/report.html"),"utf8").match(/<script>\s*([\s\S]*?)<\/script>/)[1];
assert(process.argv.length >= 4, "Provide fixture and collection directories");
const [fixtureRoot, collectionRoot] = process.argv.slice(2).map(value => path.resolve(value));
let passed = 0;
function check(condition, message) { assert(condition, message); passed++; console.log("PASS: " + message); }
function open(file) {
    const errors = [], console = new VirtualConsole();
    console.on("jsdomError", error => errors.push(error.message));
    const html = fs.readFileSync(file,"utf8");
    assert.equal(html.match(/<script>\s*([\s\S]*?)<\/script>/)[1],viewerScript,"Generated report must contain the current embedded viewer");
    const dom = new JSDOM(html, {
        url:pathToFileURL(file).href, runScripts:"dangerously", virtualConsole:console
    });
    assert.equal(errors.length,0,errors.join("\n"));
    assert.equal(dom.window.document.querySelector(".render-error"),null,"Report renderer failed: " + file);
    assert(!dom.window.document.getElementById("subtitle").textContent.includes("正在整理"));
    return dom;
}
function linksExist(dom) {
    const links = [...dom.window.document.querySelectorAll("a[href]")];
    for (const link of links) {
        if (link.getAttribute("href").startsWith("#")) continue;
        const target = new URL(link.href);
        assert.equal(target.protocol,"file:","Only relative local file links are expected");
        assert(fs.existsSync(fileURLToPath(target)),"Broken link: " + target.href);
        assert(!path.isAbsolute(link.getAttribute("href")),"Links must survive moving the collection");
    }
}
function reports(root) {
    return fs.readdirSync(root,{withFileTypes:true}).flatMap(entry => {
        if (entry.isSymbolicLink()) return [];
        const file = path.join(root,entry.name);
        if (entry.isDirectory()) return reports(file);
        return entry.name.endsWith(".html") && fs.readFileSync(file,"utf8").includes('id="report-data"') ? [file] : [];
    });
}
const collectionReports = reports(collectionRoot);
assert(collectionReports.length >= 10,"Expected a complete real collection");
for (const report of collectionReports) {
    const dom = open(report), document = dom.window.document;
    linksExist(dom);
    for (const table of document.querySelectorAll("table")) assert(table.querySelectorAll("tr.data-row").length <= 50);
    check(document.querySelectorAll(".section").length > 0,"Real HTML report renders and local links resolve: " + path.relative(collectionRoot,report));
    if (report.endsWith("SystemInfo.html")) check(document.body.textContent.includes("操作系统"),"Hardware section captions are localized");
    if (report.endsWith("Timeline.html")) {
        const timeline = JSON.parse(document.getElementById("report-data").textContent);
        if (timeline.events.length) {
            document.querySelector("button.quiet").click();
            const details = document.querySelector(".detail-row:not([hidden])");
            check(details.textContent.includes(timeline.events[0].xml),"Timeline details preserve complete event XML as text");
        }
    }
    dom.window.close();
}
const previewRoot = path.join(fixtureRoot,"HTML 报告 #100%");
const deviceFile = path.join(previewRoot,"Devices.html");
const dom = open(deviceFile), document = dom.window.document;
const rows = () => [...document.querySelectorAll("tr.data-row")];
const tick = () => new Promise(resolve => setTimeout(resolve,220));
async function search(text) {
    const input = document.getElementById("search"); input.value = text;
    input.dispatchEvent(new dom.window.Event("input",{bubbles:true})); await tick();
}
(async () => {
    check(rows().length === 50 && document.querySelector("output").textContent.includes("123"),"First page displays 50 of 123 devices");
    const next = [...document.querySelectorAll(".pager button")].find(button => button.textContent === "下一页");
    next.click(); check(rows()[0].textContent.includes("FIXTURE_50"),"Next page shows subsequent records");
    next.click(); check(rows().length === 23 && next.disabled,"Last page contains all remaining records");
    check(document.body.textContent.includes("</script><script>window.reportInjected=true</script>") &&
        document.querySelectorAll("img").length === 0 && dom.window.reportInjected === undefined,
        "Malicious evidence is displayed literally and cannot create executable markup");
    await search("PCI\\FIXTURE_75");
    check(rows().length === 1 && rows()[0].textContent.includes("测试设备 75"),"Search covers records beyond the current page");
    document.querySelector("button.quiet").click();
    check(document.querySelector(".detail-row:not([hidden])").textContent.includes("fixture.inf"),"Expanded details preserve nested driver fields");
    await search("1.2.3.456");
    check(document.querySelector("output").textContent.includes("123"),"Search includes fields hidden in collapsed details");
    await search("");
    const filter = document.querySelector("select"); filter.value = "issue"; filter.dispatchEvent(new dom.window.Event("change"));
    check(rows().length === 1 && rows()[0].textContent.includes("测试设备 75"),"Device problem filter selects the actual nonzero code");
    await search("no-matching-device-xyz"); check(rows().length === 0 && document.body.textContent.includes("没有符合筛选条件"),"Empty search results have an explicit message");
    filter.value = "all"; filter.dispatchEvent(new dom.window.Event("change")); await search("");
    check(rows().length === 50 && document.querySelector("output").textContent.includes("1–50"),"Clearing search and filters resets pagination");
    dom.window.close();
    const evidence = open(path.join(previewRoot,"CrashDumps","index.html"));
    const href = evidence.window.document.querySelector("a.file-link").getAttribute("href");
    check(href.includes("%23") && href.includes("%25") && href.includes("%E6"),"Unicode, hash signs and percent signs are encoded in relative file links");
    linksExist(evidence); evidence.window.close();
    const movedRoot = path.join(__dirname,"../build/html-validation","moved-" + Date.now());
    fs.cpSync(previewRoot,movedRoot,{recursive:true});
    for (const report of reports(movedRoot)) { const moved = open(report); linksExist(moved); moved.window.close(); }
    check(true,"All report and evidence links remain valid after copying the whole collection");
    const overview = open(path.join(previewRoot,"manifest.html"));
    const overviewText = overview.window.document.querySelector("main").textContent;
    check(overviewText.includes("计算机名称") && !overviewText.includes("undefined"),"Overview metadata is localized and fully rendered");
    overview.window.close();
    console.log("Completed: " + passed + " HTML DOM checks.");
})().catch(error => { console.error(error); process.exitCode = 1; });
