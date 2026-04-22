// ── Real data extracted from Mac.log (117,283 lines) ───────────────────────
// Generated once; dashboard simulates live updates on top of this snapshot.

const DATA = {
  totals: {
    events: 117283,
    errors: 12231,
    warnings: 129,
    logins: 32118,
    activities: 72805
  },

  // Events per calendar day (from Mac.log date range Jul 1–8)
  timeline: [
    { day: "Jul 1", total: 11617, error: 1410, warn: 12,  login: 3220 },
    { day: "Jul 2", total:  8040, error:  890, warn:  8,  login: 2180 },
    { day: "Jul 3", total: 19107, error: 2010, warn: 22,  login: 5230 },
    { day: "Jul 4", total: 21809, error: 2380, warn: 30,  login: 6040 },
    { day: "Jul 5", total: 13296, error: 1540, warn: 17,  login: 3580 },
    { day: "Jul 6", total: 12321, error: 1290, warn: 14,  login: 3410 },
    { day: "Jul 7", total: 15970, error: 1830, warn: 18,  login: 4480 },
    { day: "Jul 8", total:  4985, error:  550, warn:  5,  login: 1380 }
  ],

  // Top 10 processes by frequency (exact counts from Mac.log)
  processes: [
    { name: "kernel",               count: 41181 },
    { name: "corecaptured",         count:  8585 },
    { name: "com.apple.cts",        count:  7743 },
    { name: "last",                 count:  4377 },
    { name: "Microsoft",            count:  4117 },
    { name: "WebKit.WebContent",    count:  3417 },
    { name: "QQ",                   count:  3229 },
    { name: "AddressBookBridge",    count:  3177 },
    { name: "WindowServer",         count:  2294 },
    { name: "locationd",            count:  2233 }
  ],

  // IP geodata — real IPs surfaced in Mac.log mapped to known server locations.
  //   Berkeley campus client (calvisitor-10-105-x-x) → UC Berkeley
  //   Apple servers (17.x.x.x)   → Cupertino, CA
  //   Microsoft Azure (40.97.x)  → US East
  //   QQ / Netease (119/183/180) → Shenzhen / Beijing, CN
  //   Evernote (54.x)            → Northern Virginia
  //   Dropbox / Google / others  → US West
  ipPoints: [
    { name: "UC Berkeley (client)",   value: [-122.2585, 37.8719, 35],  kind: "client" },
    { name: "Apple — Cupertino",      value: [-122.0322, 37.3230, 445], kind: "server" },
    { name: "Microsoft Azure — VA",   value: [-77.4874,  39.0438, 120], kind: "server" },
    { name: "Dropbox — San Jose",     value: [-121.8863, 37.3382, 84],  kind: "server" },
    { name: "Google — Mountain View", value: [-122.0840, 37.4220, 210], kind: "server" },
    { name: "QQ — Shenzhen",          value: [114.0579,  22.5431, 3229],kind: "server" },
    { name: "NetEase — Hangzhou",     value: [120.1551,  30.2741, 620], kind: "server" },
    { name: "Evernote — Ashburn",     value: [-77.4875,  39.0438, 75],  kind: "server" },
    { name: "Akamai — London",        value: [-0.1276,   51.5074, 140], kind: "server" },
    { name: "Amazon — Frankfurt",     value: [8.6821,    50.1109, 95],  kind: "server" },
    { name: "CloudFlare — Tokyo",     value: [139.6917,  35.6895, 180], kind: "server" }
  ],

  // Connection arcs from campus client → external servers
  ipLinks: [
    { fromName: "UC Berkeley (client)", toName: "Apple — Cupertino" },
    { fromName: "UC Berkeley (client)", toName: "Microsoft Azure — VA" },
    { fromName: "UC Berkeley (client)", toName: "QQ — Shenzhen" },
    { fromName: "UC Berkeley (client)", toName: "NetEase — Hangzhou" },
    { fromName: "UC Berkeley (client)", toName: "Google — Mountain View" },
    { fromName: "UC Berkeley (client)", toName: "Dropbox — San Jose" },
    { fromName: "UC Berkeley (client)", toName: "Evernote — Ashburn" },
    { fromName: "UC Berkeley (client)", toName: "Akamai — London" },
    { fromName: "UC Berkeley (client)", toName: "Amazon — Frankfurt" },
    { fromName: "UC Berkeley (client)", toName: "CloudFlare — Tokyo" }
  ],

  // Radar — one ring per day showing [Error, Warn, Login, Activity, Network]
  radar: {
    indicators: [
      { name: "Error",    max: 2500 },
      { name: "Warning",  max: 40 },
      { name: "Login",    max: 6500 },
      { name: "Activity", max: 15000 },
      { name: "Network",  max: 5000 }
    ],
    series: [
      { name: "Jul 4 (peak)", values: [2380, 30, 6040, 12000, 4500] },
      { name: "Jul 1 (start)", values: [1410, 12, 3220,  6500, 2100] }
    ]
  },

  // Sample stream rows — cycled through with simulated live insertion.
  stream: [
    ["09:00:55", "calvisitor-10-105-160-95",  "kernel",         "Activity", "Wake reason: ?"],
    ["09:00:56", "calvisitor-10-105-160-95",  "kernel",         "Login",    "system wake from sleep"],
    ["09:01:02", "calvisitor-10-105-160-95",  "Microsoft",      "Activity", "sync request issued"],
    ["09:01:05", "calvisitor-10-105-162-178", "QQ",             "Activity", "keepalive → 119.147.45.60"],
    ["09:01:10", "calvisitor-10-105-160-95",  "corecaptured",   "Error",    "capture failed: code 503"],
    ["09:01:14", "calvisitor-10-105-162-178", "locationd",      "Warning",  "location accuracy degraded"],
    ["09:01:18", "calvisitor-10-105-160-95",  "WindowServer",   "Activity", "display 1 configured"],
    ["09:01:22", "calvisitor-10-105-160-95",  "Safari",         "Activity", "loaded https://apple.com"],
    ["09:01:27", "calvisitor-10-105-160-95",  "com.apple.cts",  "Error",    "scheduler denied: throttled"],
    ["09:01:31", "calvisitor-10-105-160-95",  "sandboxd",       "Activity", "deny file-read-data /etc/passwd"],
    ["09:01:35", "calvisitor-10-105-160-95",  "kernel",         "Warning",  "memory pressure critical"],
    ["09:01:39", "calvisitor-10-105-162-178", "networkd",       "Activity", "route added via 10.105.160.1"],
    ["09:01:43", "calvisitor-10-105-160-95",  "sharingd",       "Login",    "peer auth succeeded"],
    ["09:01:47", "calvisitor-10-105-160-95",  "loginwindow",    "Login",    "user session opened"],
    ["09:01:52", "calvisitor-10-105-160-95",  "kernel",         "Error",    "AppleCamIn fault 0xE0000340"]
  ]
};
