using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.Serialization;
using System.Runtime.Serialization.Json;
using System.Text;

namespace SystemInfo.Models
{
    [DataContract]
    internal sealed class Snapshot
    {
        [DataMember(Name = "revision")] public ulong Revision { get; set; }
        [DataMember(Name = "machine")] public string Machine { get; set; }
        [DataMember(Name = "computerPending")] public bool ComputerPending { get; set; }
        [DataMember(Name = "ssdPending")] public bool SsdPending { get; set; }
        [DataMember(Name = "updated")] public string Updated { get; set; }
        [DataMember(Name = "diagnostic")] public string Diagnostic { get; set; }
        [DataMember(Name = "error")] public string Error { get; set; }
        [DataMember(Name = "rows")] public List<Field> Rows { get; set; }
        [DataMember(Name = "battery")] public Battery Battery { get; set; }
        [DataMember(Name = "log")] public LogStatus Log { get; set; }
        public bool Complete => !ComputerPending && !SsdPending;
        public bool Valid => Complete && string.IsNullOrEmpty(Error) && Rows?.Count == 19
            && Rows.Select(row => row.Label).Distinct().Count() == 19
            && Rows.Any(row => row.Label == "电池信息") && Battery?.Packs != null
            && Rows.Any(row => row.Label == "Windows OEM 密钥")
            && Rows.Any(row => row.Label == "Windows 产品 ID");

        public static Snapshot Parse(string json)
        {
            using (var stream = new MemoryStream(Encoding.UTF8.GetBytes(json)))
                return (Snapshot)new DataContractJsonSerializer(typeof(Snapshot)).ReadObject(stream);
        }
    }

    [DataContract]
    internal sealed class Field
    {
        [DataMember(Name = "label")] public string Label { get; set; }
        [DataMember(Name = "value")] public string Value { get; set; }
    }

    [DataContract]
    internal sealed class Battery
    {
        [DataMember(Name = "state")] public string State { get; set; }
        [DataMember(Name = "source")] public string Source { get; set; }
        [DataMember(Name = "remaining")] public string Remaining { get; set; }
        [DataMember(Name = "present")] public bool? Present { get; set; }
        [DataMember(Name = "percent")] public int? Percent { get; set; }
        [DataMember(Name = "packs")] public List<BatteryPack> Packs { get; set; }
    }

    [DataContract]
    internal sealed class BatteryPack
    {
        [DataMember(Name = "name")] public string Name { get; set; }
        [DataMember(Name = "fields")] public List<Field> Fields { get; set; }
    }

    [DataContract]
    internal sealed class LogStatus
    {
        [DataMember(Name = "state")] public string State { get; set; }
        [DataMember(Name = "item")] public string Item { get; set; }
        [DataMember(Name = "completed")] public int Completed { get; set; }
        [DataMember(Name = "total")] public int Total { get; set; }
        [DataMember(Name = "saved")] public int Saved { get; set; }
        [DataMember(Name = "partial")] public int Partial { get; set; }
        [DataMember(Name = "failed")] public int Failed { get; set; }
        [DataMember(Name = "detail")] public string Detail { get; set; }
        [DataMember(Name = "directory")] public string Directory { get; set; }
        [DataMember(Name = "hasReport")] public bool HasReport { get; set; }
        public bool Active => State == "running" || State == "cancelling";
    }
}
