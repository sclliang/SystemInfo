// The column only controls the wide layout. Every section is always on the same page.
export const sections = [
  { id: "hardware", title: "硬件配置", icon: "board", column: "main", fields: [
    ["主板型号", "主板"], ["CPU型号", "处理器", "cpu"], ["内存信息", "内存", "memory"],
    ["GPU型号", "显卡", "gpu"], ["显示器信息", "显示器", "monitor"], ["OS信息", "操作系统"]
  ] },
  { id: "power", title: "电池与电源", icon: "battery", column: "side", fields: [["电池信息"]] },
  { id: "storage", title: "SSD 存储", icon: "storage", column: "main", fields: [["SSD信息"]] },
  { id: "firmware", title: "固件与安全", icon: "firmware", column: "side", fields: [
    ["BIOS版本", "BIOS 版本"], ["EC版本", "EC 版本"], ["TPM型号", "TPM"]
  ] },
  { id: "network", title: "网络连接", icon: "network", column: "main", fields: [
    ["有线网卡", "以太网"], ["无线网卡", "无线网络"], ["蓝牙地址", "蓝牙地址"]
  ] },
  { id: "identity", title: "设备标识", icon: "identity", column: "side", fields: [
    ["系统SN", "系统 SN"], ["主板SN", "主板 SN"], ["UUID", "UUID"]
  ] }
];

export function fieldsFor(snapshot) {
  const rows = new Map(snapshot.rows.map(row => [row.label, row.value]));
  return sections.flatMap(section => section.fields.map(([key]) => {
    const pending = key === "SSD信息" ? snapshot.ssdPending : snapshot.computerPending;
    const value = rows.get(key) || (pending ? "正在读取…" : "N/A");
    return { key, value, pending, unavailable: value === "N/A" };
  }));
}

export function drivesFor(value) {
  return value.trim().split(/\r?\n\s*\r?\n/).map(block => {
    const [model, link = "", ...lines] = block.split(/\r?\n/);
    const [capacity, ...protocol] = link.split(/\s+·\s+/);
    const metrics = [], notes = [];
    for (const line of lines) {
      const uptime = /^上电\s+(.+?)\s*·\s*(.+)$/.exec(line);
      const traffic = /^累计读取\s+(.+?)\s*·\s*写入\s+(.+)$/.exec(line);
      if (uptime) metrics.push(["上电时长", uptime[1]], ["上电次数", uptime[2]]);
      else if (traffic) metrics.push(["累计读取", traffic[1]], ["累计写入", traffic[2]]);
      else notes.push(line);
    }
    return { model, capacity, protocol: protocol.join(" · "), metrics, notes };
  });
}

export function collecting(log) {
  return !!log && (log.state === "running" || log.state === "cancelling");
}
