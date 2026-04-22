// ── Theme palette ─────────────────────────────────────────────────────────
const C = {
  cyan: '#00e5ff', cyanDim: '#0af', amber: '#ffc93c',
  pink: '#ff3d9a', green: '#24ff9b', red: '#ff4d6d',
  text: '#cfe9ff', textDim: '#6f8ab5', grid: 'rgba(0,229,255,0.08)'
};

const baseTextStyle = { color: C.text, fontFamily: 'Rajdhani, sans-serif' };
const axisCommon = {
  axisLine:  { lineStyle: { color: 'rgba(0,229,255,0.35)' } },
  axisLabel: { color: C.textDim, fontFamily: 'Rajdhani' },
  splitLine: { lineStyle: { color: C.grid } }
};

// ── Clock ─────────────────────────────────────────────────────────────────
const clockEl = document.getElementById('clock');
function tickClock() {
  const d = new Date();
  clockEl.textContent = d.toTimeString().slice(0, 8);
}
setInterval(tickClock, 1000); tickClock();

// ── Gauge factory ────────────────────────────────────────────────────────
function gauge(domId, value, max, color, suffix = '') {
  const chart = echarts.init(document.getElementById(domId));
  const pct = Math.min(100, Math.round((value / max) * 100));
  chart.setOption({
    series: [{
      type: 'gauge',
      startAngle: 210, endAngle: -30,
      min: 0, max: 100,
      progress: { show: true, width: 10, itemStyle: { color } },
      axisLine: { lineStyle: { width: 10, color: [[1, 'rgba(0,229,255,0.12)']] } },
      pointer: { show: false },
      axisTick: { show: false },
      splitLine: { show: false },
      axisLabel: { show: false },
      anchor: { show: false },
      detail: {
        valueAnimation: true,
        formatter: () => value.toLocaleString() + suffix,
        color: C.text,
        fontSize: 26, fontWeight: 700,
        fontFamily: 'Rajdhani',
        offsetCenter: [0, '5%']
      },
      title: {
        offsetCenter: [0, '35%'],
        fontSize: 11, color: C.textDim, fontFamily: 'Rajdhani'
      },
      data: [{ value: pct, name: `${pct}% of capacity` }]
    }]
  });
  return chart;
}

const charts = {
  total: gauge('gaugeTotal', DATA.totals.events,   150000, C.cyan),
  error: gauge('gaugeError', DATA.totals.errors,    20000, C.red),
  warn:  gauge('gaugeWarn',  DATA.totals.warnings,    200, C.amber),
  login: gauge('gaugeLogin', DATA.totals.logins,    50000, C.green)
};

// ── World map with IP links ──────────────────────────────────────────────
const mapChart = echarts.init(document.getElementById('map'));
const byName = Object.fromEntries(DATA.ipPoints.map(p => [p.name, p.value]));
const linesData = DATA.ipLinks.map(l => ({
  coords: [byName[l.fromName], byName[l.toName]],
  fromName: l.fromName, toName: l.toName
}));

const WORLD_JSON_URL = 'https://cdn.jsdelivr.net/npm/echarts-map-collection@1.0.0/custom/world.json';

function renderMap() { mapChart.setOption(mapOption); }

fetch(WORLD_JSON_URL)
  .then(r => r.json())
  .then(json => { echarts.registerMap('world', json); renderMap(); })
  .catch(err => {
    console.warn('world map load failed, falling back to scatter-only:', err);
    // Fallback: render effectScatter on a Cartesian grid so dashboard still works offline
    mapChart.setOption({
      backgroundColor: 'transparent',
      tooltip: { trigger: 'item' },
      xAxis: { min: -180, max: 180, show: false },
      yAxis: { min: -60,  max: 80,  show: false },
      series: [{
        type: 'effectScatter',
        data: DATA.ipPoints.map(p => ({ name: p.name, value: p.value, kind: p.kind })),
        symbolSize: v => Math.max(6, Math.min(22, Math.sqrt(v[2]) * 1.2)),
        itemStyle: { color: C.cyan, shadowBlur: 10, shadowColor: C.cyan }
      }]
    });
  });

const mapOption = {
  backgroundColor: 'transparent',
  geo: {
    map: 'world', roam: true,
    zoom: 1.2, center: [20, 30],
    itemStyle: {
      areaColor: 'rgba(8, 20, 50, 0.9)',
      borderColor: 'rgba(0, 229, 255, 0.25)',
      borderWidth: 0.6
    },
    emphasis: { itemStyle: { areaColor: 'rgba(0, 229, 255, 0.15)' }, label: { show: false } }
  },
  tooltip: { trigger: 'item', backgroundColor: 'rgba(10,16,36,0.95)', borderColor: C.cyan, textStyle: { color: C.text } },
  series: [
    {
      name: 'connections',
      type: 'lines',
      coordinateSystem: 'geo',
      zlevel: 2,
      effect: {
        show: true, period: 4, trailLength: 0.2,
        symbol: 'arrow', symbolSize: 6, color: C.cyan
      },
      lineStyle: {
        color: C.cyan, width: 1, opacity: 0.6, curveness: 0.3
      },
      data: linesData
    },
    {
      name: 'endpoints',
      type: 'effectScatter',
      coordinateSystem: 'geo',
      zlevel: 3,
      rippleEffect: { brushType: 'stroke', scale: 4 },
      symbolSize: v => Math.max(6, Math.min(22, Math.sqrt(v[2]) * 1.2)),
      itemStyle: {
        color: p => p.data.kind === 'client' ? C.pink : C.cyan,
        shadowBlur: 10, shadowColor: C.cyan
      },
      data: DATA.ipPoints
    }
  ]
};

// ── Event type bar ───────────────────────────────────────────────────────
const typesChart = echarts.init(document.getElementById('types'));
typesChart.setOption({
  grid: { top: 20, right: 16, bottom: 24, left: 60 },
  tooltip: { trigger: 'axis' },
  xAxis: { type: 'value', ...axisCommon },
  yAxis: {
    type: 'category',
    data: ['Activity', 'Login', 'Error', 'Warning'],
    ...axisCommon
  },
  series: [{
    type: 'bar',
    data: [
      { value: DATA.totals.activities, itemStyle: { color: C.cyan } },
      { value: DATA.totals.logins,     itemStyle: { color: C.green } },
      { value: DATA.totals.errors,     itemStyle: { color: C.red } },
      { value: DATA.totals.warnings,   itemStyle: { color: C.amber } }
    ],
    barWidth: 14,
    itemStyle: { borderRadius: [0, 2, 2, 0] },
    label: { show: true, position: 'right', color: C.text, fontFamily: 'Rajdhani' },
    animationDuration: 1200
  }]
});

// ── Timeline (stacked area) ──────────────────────────────────────────────
const timelineChart = echarts.init(document.getElementById('timeline'));
timelineChart.setOption({
  grid: { top: 30, right: 20, bottom: 30, left: 50 },
  tooltip: { trigger: 'axis', backgroundColor: 'rgba(10,16,36,0.95)', borderColor: C.cyan, textStyle: { color: C.text } },
  legend: { data: ['Errors', 'Warnings', 'Logins'], textStyle: { color: C.textDim }, top: 0 },
  xAxis: { type: 'category', data: DATA.timeline.map(t => t.day), ...axisCommon, boundaryGap: false },
  yAxis: { type: 'value', ...axisCommon },
  series: [
    {
      name: 'Errors', type: 'line', smooth: true, symbol: 'circle', symbolSize: 6,
      data: DATA.timeline.map(t => t.error),
      itemStyle: { color: C.red },
      lineStyle: { width: 2, color: C.red, shadowBlur: 10, shadowColor: C.red },
      areaStyle: { color: new echarts.graphic.LinearGradient(0,0,0,1,[
        { offset: 0, color: 'rgba(255,77,109,0.4)' },
        { offset: 1, color: 'rgba(255,77,109,0)'   }]) }
    },
    {
      name: 'Warnings', type: 'line', smooth: true, symbol: 'circle', symbolSize: 6,
      data: DATA.timeline.map(t => t.warn),
      itemStyle: { color: C.amber },
      lineStyle: { width: 2, color: C.amber }
    },
    {
      name: 'Logins', type: 'line', smooth: true, symbol: 'circle', symbolSize: 6,
      data: DATA.timeline.map(t => t.login),
      itemStyle: { color: C.green },
      lineStyle: { width: 2, color: C.green, shadowBlur: 10, shadowColor: C.green },
      areaStyle: { color: new echarts.graphic.LinearGradient(0,0,0,1,[
        { offset: 0, color: 'rgba(36,255,155,0.3)' },
        { offset: 1, color: 'rgba(36,255,155,0)'   }]) }
    }
  ]
});

// ── Top processes ────────────────────────────────────────────────────────
const procsChart = echarts.init(document.getElementById('procs'));
const procsSorted = [...DATA.processes].sort((a,b) => a.count - b.count);
procsChart.setOption({
  grid: { top: 10, right: 80, bottom: 10, left: 120 },
  tooltip: { trigger: 'axis' },
  xAxis: { type: 'value', show: false, ...axisCommon },
  yAxis: { type: 'category', data: procsSorted.map(p => p.name), ...axisCommon,
           axisLabel: { color: C.text, fontFamily: 'Rajdhani', fontSize: 11 } },
  series: [{
    type: 'bar',
    data: procsSorted.map(p => p.count),
    barWidth: 10,
    itemStyle: {
      borderRadius: [0, 2, 2, 0],
      color: new echarts.graphic.LinearGradient(0,0,1,0,[
        { offset: 0, color: 'rgba(0,229,255,0.15)' },
        { offset: 1, color: C.cyan }])
    },
    label: { show: true, position: 'right', color: C.cyan, fontFamily: 'Rajdhani' }
  }]
});

// ── Radar ────────────────────────────────────────────────────────────────
const radarChart = echarts.init(document.getElementById('radar'));
radarChart.setOption({
  tooltip: {},
  legend: { data: DATA.radar.series.map(s => s.name), textStyle: { color: C.textDim }, bottom: 0 },
  radar: {
    indicator: DATA.radar.indicators,
    shape: 'polygon',
    splitNumber: 4,
    axisName: { color: C.cyan, fontFamily: 'Rajdhani' },
    splitLine:  { lineStyle: { color: 'rgba(0,229,255,0.15)' } },
    splitArea:  { areaStyle: { color: ['rgba(0,229,255,0.02)','rgba(0,229,255,0.05)'] } },
    axisLine:   { lineStyle: { color: 'rgba(0,229,255,0.25)' } }
  },
  series: [{
    type: 'radar',
    emphasis: { lineStyle: { width: 3 } },
    data: DATA.radar.series.map((s, i) => ({
      value: s.values,
      name:  s.name,
      symbolSize: 6,
      lineStyle: { color: i === 0 ? C.pink : C.cyan, width: 2 },
      itemStyle: { color: i === 0 ? C.pink : C.cyan },
      areaStyle: { color: i === 0 ? 'rgba(255,61,154,0.25)' : 'rgba(0,229,255,0.2)' }
    }))
  }]
});

// ── Live stream table ────────────────────────────────────────────────────
const tbody = document.querySelector('#stream tbody');
let streamIdx = 0;
function addStreamRow() {
  const row = DATA.stream[streamIdx % DATA.stream.length];
  streamIdx++;
  const tr = document.createElement('tr');
  const [time, host, proc, type, msg] = row;
  tr.innerHTML =
    `<td>${time}</td>` +
    `<td>${host}</td>` +
    `<td>${proc}</td>` +
    `<td><span class="tag ${type}">${type}</span></td>` +
    `<td>${msg}</td>`;
  tbody.insertBefore(tr, tbody.firstChild);
  while (tbody.children.length > 14) tbody.removeChild(tbody.lastChild);
}
// seed a few rows immediately
for (let i = 0; i < 6; i++) addStreamRow();
setInterval(addStreamRow, 1800);

// ── Live gauge wiggle (simulated telemetry) ──────────────────────────────
setInterval(() => {
  const bump = (v, pct = 0.002) => v + Math.round((Math.random() - 0.5) * v * pct);
  DATA.totals.events    = bump(DATA.totals.events);
  DATA.totals.errors    = bump(DATA.totals.errors);
  DATA.totals.logins    = bump(DATA.totals.logins);

  charts.total.setOption({ series: [{ data: [{ value: Math.min(100, Math.round(DATA.totals.events / 1500)),
    name: `${Math.min(100, Math.round(DATA.totals.events / 1500))}% of capacity` }],
    detail: { formatter: () => DATA.totals.events.toLocaleString() } }] });
  charts.error.setOption({ series: [{ detail: { formatter: () => DATA.totals.errors.toLocaleString() } }] });
  charts.login.setOption({ series: [{ detail: { formatter: () => DATA.totals.logins.toLocaleString() } }] });
}, 2500);

// ── Resize handling ──────────────────────────────────────────────────────
window.addEventListener('resize', () => {
  Object.values(charts).forEach(c => c.resize());
  [mapChart, typesChart, timelineChart, procsChart, radarChart].forEach(c => c.resize());
});
