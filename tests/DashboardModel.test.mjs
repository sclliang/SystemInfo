import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { sections, fieldsFor, drivesFor, collecting } from "../ui/model.js";

test("the single page has one destination for each of the seventeen collector rows", () => {
  const labels = "主板型号 OS信息 CPU型号 GPU型号 显示器信息 内存信息 SSD信息 BIOS版本 EC版本 主板SN 系统SN UUID 有线网卡 无线网卡 蓝牙地址 TPM型号 电池信息".split(" ");
  const keys = sections.flatMap(section => section.fields.map(([key]) => key));
  assert.deepEqual(keys.sort(), labels.sort());
  for (const section of sections) {
    assert.ok(["main", "side"].includes(section.column));
    const icons = [section.icon, ...section.fields.map(([, , icon]) => icon).filter(Boolean)];
    for (const icon of icons) {
      assert.match(readFileSync(new URL("../ui/icons/" + icon + ".svg", import.meta.url), "utf8"), /<svg/);
    }
  }
});

test("the slow SMART result does not hide the completed hardware rows, in either arrival order", () => {
  const cpuFirst = fieldsFor({rows:[{label:"CPU型号", value:"Real CPU"}], computerPending:false, ssdPending:true});
  assert.deepEqual(cpuFirst.find(f => f.key === "CPU型号"), {key:"CPU型号", value:"Real CPU", pending:false, unavailable:false});
  assert.equal(cpuFirst.find(f => f.key === "SSD信息").pending, true);
  assert.equal(cpuFirst.find(f => f.key === "电池信息").pending, false);
  const ssdFirst = fieldsFor({rows:[{label:"SSD信息", value:"Real SSD"}], computerPending:true, ssdPending:false});
  assert.equal(ssdFirst.find(f => f.key === "SSD信息").value, "Real SSD");
  assert.equal(ssdFirst.find(f => f.key === "CPU型号").pending, true);
});

test("multidisk output retains capacity, the full link, zero and missing SMART values", () => {
  const source = "Drive A\r\n1.00 TB  ·  NVMe · PCIe 4.0 x4\r\n上电 0 小时  ·  0 次\r\n累计读取 N/A  ·  写入 1.25 TiB\r\n\r\nDrive B\r\n2.00 TB · SATA/600\r\n设备补充信息";
  assert.deepEqual(drivesFor(source), [
    {model:"Drive A", capacity:"1.00 TB", protocol:"NVMe · PCIe 4.0 x4", metrics:[
      ["上电时长","0 小时"], ["上电次数","0 次"], ["累计读取","N/A"], ["累计写入","1.25 TiB"]], notes:[]},
    {model:"Drive B", capacity:"2.00 TB", protocol:"SATA/600", metrics:[], notes:["设备补充信息"]}
  ]);
});

test("log actions distinguish running or cancelling from all terminal states", () => {
  assert.equal(collecting({state:"running"}), true);
  assert.equal(collecting({state:"cancelling"}), true);
  for (const state of ["complete","partial","failed","cancelled"]) assert.equal(collecting({state}), false);
});
