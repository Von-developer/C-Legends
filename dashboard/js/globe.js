/* C-Legends — Three.js interactive globe.
 * Public API:
 *   Globe.init(canvas, opts)              opts: { onCountryClick, getRiskForCountry }
 *   Globe.setCountryData([{name,count}])  marker size by count, color by risk
 *   Globe.focusCountry(name)              animate camera to country centroid
 *   Globe.clearFocus()
 *   Globe.resize()
 *   Globe.setEmpty(bool)                  toggle "no data" overlay
 *
 * Depends on globals: THREE, THREE.OrbitControls, COUNTRY_CENTROIDS, CITIES
 */
(function (root) {
  const GLOBE_R = 1.0;
  const MARKER_R = GLOBE_R * 1.005;
  const LABEL_R  = GLOBE_R * 1.012;
  const MIN_DIST = 1.4;
  const MAX_DIST = 5.0;

  let renderer, scene, camera, controls, raycaster, pointer;
  let earthMesh, atmosphereMesh, gridMesh, starsMesh;
  let markerGroup, cityDotGroup, cityLabelGroup;
  let canvas, hostEl;
  let onCountryClickCB = null;
  let getRiskForCountryCB = () => 0;
  let countryData = [];          // [{name, count}]
  let countryByName = {};        // name → { mesh, count, risk }
  let mobileMode = false;
  let focusedCountry = null;
  let animTarget = null;         // { from, to, t0, dur }
  let raf = 0;
  let lastTouchEnd = 0;

  function isMobile() {
    return window.innerWidth < 768 || /Mobi|Android|iPhone|iPad/i.test(navigator.userAgent);
  }

  function latLonToVec3(lat, lon, r = GLOBE_R) {
    const phi = (90 - lat) * Math.PI / 180;
    const theta = (lon + 180) * Math.PI / 180;
    return new THREE.Vector3(
      -r * Math.sin(phi) * Math.cos(theta),
       r * Math.cos(phi),
       r * Math.sin(phi) * Math.sin(theta)
    );
  }

  function makeTextSprite(text, opts = {}) {
    const fontSize = opts.fontSize || 13;
    const padding = 4;
    const cv = document.createElement('canvas');
    const ctx = cv.getContext('2d');
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    ctx.font = `600 ${fontSize}px system-ui, -apple-system, sans-serif`;
    const w = Math.ceil(ctx.measureText(text).width) + padding * 2;
    const h = fontSize + padding * 2;
    cv.width = w * dpr; cv.height = h * dpr;
    ctx.scale(dpr, dpr);
    ctx.font = `600 ${fontSize}px system-ui, -apple-system, sans-serif`;
    ctx.fillStyle = opts.bg || 'rgba(13,15,26,0.78)';
    ctx.fillRect(0, 0, w, h);
    ctx.fillStyle = opts.color || '#cbd5e1';
    ctx.textBaseline = 'middle';
    ctx.fillText(text, padding, h / 2);

    const tex = new THREE.CanvasTexture(cv);
    tex.minFilter = THREE.LinearFilter;
    tex.needsUpdate = true;
    const mat = new THREE.SpriteMaterial({ map: tex, depthTest: true, transparent: true });
    const sprite = new THREE.Sprite(mat);
    const scale = 0.0042 * fontSize;
    sprite.scale.set((w / h) * scale * (h / fontSize), scale * (h / fontSize), 1);
    sprite.userData.text = text;
    return sprite;
  }

  function makeStarsTexture() {
    const cv = document.createElement('canvas');
    cv.width = 1024; cv.height = 512;
    const ctx = cv.getContext('2d');
    ctx.fillStyle = '#05070f';
    ctx.fillRect(0, 0, cv.width, cv.height);
    for (let i = 0; i < 800; i++) {
      const x = Math.random() * cv.width;
      const y = Math.random() * cv.height;
      const r = Math.random() * 1.2;
      ctx.fillStyle = `rgba(255,255,255,${0.4 + Math.random() * 0.6})`;
      ctx.beginPath(); ctx.arc(x, y, r, 0, Math.PI * 2); ctx.fill();
    }
    const tex = new THREE.CanvasTexture(cv);
    tex.colorSpace = THREE.SRGBColorSpace || tex.colorSpace;
    return tex;
  }

  function makeGridTexture() {
    const cv = document.createElement('canvas');
    cv.width = 2048; cv.height = 1024;
    const ctx = cv.getContext('2d');
    // Background: dark blue gradient (ocean feel)
    const grad = ctx.createLinearGradient(0, 0, 0, cv.height);
    grad.addColorStop(0, '#0d1430');
    grad.addColorStop(0.5, '#0a1230');
    grad.addColorStop(1, '#0d1430');
    ctx.fillStyle = grad;
    ctx.fillRect(0, 0, cv.width, cv.height);

    // Latitude lines every 15deg
    ctx.strokeStyle = 'rgba(79,142,247,0.18)';
    ctx.lineWidth = 1;
    for (let lat = -90; lat <= 90; lat += 15) {
      const y = ((90 - lat) / 180) * cv.height;
      ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(cv.width, y); ctx.stroke();
    }
    // Longitude lines every 15deg
    for (let lon = -180; lon <= 180; lon += 15) {
      const x = ((lon + 180) / 360) * cv.width;
      ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, cv.height); ctx.stroke();
    }
    // Equator + prime meridian brighter
    ctx.strokeStyle = 'rgba(79,142,247,0.4)';
    ctx.lineWidth = 1.5;
    ctx.beginPath(); ctx.moveTo(0, cv.height / 2); ctx.lineTo(cv.width, cv.height / 2); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(cv.width / 2, 0); ctx.lineTo(cv.width / 2, cv.height); ctx.stroke();

    const tex = new THREE.CanvasTexture(cv);
    tex.colorSpace = THREE.SRGBColorSpace || tex.colorSpace;
    tex.anisotropy = 4;
    return tex;
  }

  function buildScene() {
    scene = new THREE.Scene();

    // Star backdrop
    const starsGeo = new THREE.SphereGeometry(50, 32, 32);
    const starsMat = new THREE.MeshBasicMaterial({
      map: makeStarsTexture(),
      side: THREE.BackSide,
      depthWrite: false
    });
    starsMesh = new THREE.Mesh(starsGeo, starsMat);
    scene.add(starsMesh);

    // Earth sphere
    const earthGeo = new THREE.SphereGeometry(GLOBE_R, 64, 64);
    const earthMat = new THREE.MeshPhongMaterial({
      map: makeGridTexture(),
      specular: new THREE.Color(0x223060),
      shininess: 12
    });
    earthMesh = new THREE.Mesh(earthGeo, earthMat);
    scene.add(earthMesh);

    // Atmosphere (soft outer glow shell)
    const atmoGeo = new THREE.SphereGeometry(GLOBE_R * 1.04, 48, 48);
    const atmoMat = new THREE.ShaderMaterial({
      transparent: true,
      side: THREE.BackSide,
      depthWrite: false,
      vertexShader: `
        varying vec3 vNormal;
        void main() {
          vNormal = normalize(normalMatrix * normal);
          gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
        }`,
      fragmentShader: `
        varying vec3 vNormal;
        void main() {
          float intensity = pow(0.65 - dot(vNormal, vec3(0.0, 0.0, 1.0)), 2.0);
          gl_FragColor = vec4(0.31, 0.56, 0.97, 1.0) * intensity;
        }`
    });
    atmosphereMesh = new THREE.Mesh(atmoGeo, atmoMat);
    scene.add(atmosphereMesh);

    // Lights
    const ambient = new THREE.AmbientLight(0x6080c0, 0.7);
    scene.add(ambient);
    const dir = new THREE.DirectionalLight(0xffffff, 1.0);
    dir.position.set(5, 3, 5);
    scene.add(dir);

    // Marker group (event countries)
    markerGroup = new THREE.Group();
    scene.add(markerGroup);

    // City dots + labels
    cityDotGroup = new THREE.Group();
    cityLabelGroup = new THREE.Group();
    scene.add(cityDotGroup);
    scene.add(cityLabelGroup);
    buildCityDots();
  }

  function buildCityDots() {
    if (!root.CITIES) return;
    const dotGeo = new THREE.SphereGeometry(0.0035, 8, 8);
    const dotMat = new THREE.MeshBasicMaterial({ color: 0xfbbf24, transparent: true, opacity: 0.85 });
    root.CITIES.forEach((c, i) => {
      const dot = new THREE.Mesh(dotGeo, dotMat);
      dot.position.copy(latLonToVec3(c.lat, c.lon, MARKER_R));
      dot.userData.city = c;
      cityDotGroup.add(dot);
    });
  }

  function rebuildCityLabels() {
    cityLabelGroup.clear();
    if (mobileMode) return;
    const dist = camera.position.length();
    let limit = 0;
    if (dist < 1.6)      limit = 80;
    else if (dist < 2.2) limit = 30;
    else if (dist < 3.2) limit = 12;
    else                 limit = 0;
    for (let i = 0; i < limit && i < root.CITIES.length; i++) {
      const c = root.CITIES[i];
      const sprite = makeTextSprite(c.name, { fontSize: 12, color: '#e2e8f0' });
      sprite.position.copy(latLonToVec3(c.lat, c.lon, LABEL_R + 0.005));
      cityLabelGroup.add(sprite);
    }
  }

  function buildCountryMarker(name, count, maxCount, risk) {
    const ctr = root.COUNTRY_CENTROIDS && root.COUNTRY_CENTROIDS[name];
    if (!ctr) return null;
    const ratio = Math.max(0.25, Math.min(1, count / maxCount));
    const baseSize = 0.012 + ratio * 0.025;
    const color = risk >= 60 ? 0xef4444
                : risk >= 30 ? 0xf59e0b
                : 0x4f8ef7;

    // Glowing dot
    const dotGeo = new THREE.SphereGeometry(baseSize, 16, 16);
    const dotMat = new THREE.MeshBasicMaterial({ color, transparent: true, opacity: 0.95 });
    const dot = new THREE.Mesh(dotGeo, dotMat);
    dot.position.copy(latLonToVec3(ctr.lat, ctr.lon, MARKER_R));
    dot.userData.country = name;

    // Halo ring
    const ringGeo = new THREE.RingGeometry(baseSize * 1.4, baseSize * 1.7, 24);
    const ringMat = new THREE.MeshBasicMaterial({
      color, transparent: true, opacity: 0.45, side: THREE.DoubleSide
    });
    const ring = new THREE.Mesh(ringGeo, ringMat);
    ring.position.copy(dot.position);
    ring.lookAt(0, 0, 0);
    ring.userData.country = name;
    ring.userData.pulsePhase = Math.random() * Math.PI * 2;
    ring.userData.baseScale = 1;

    const grp = new THREE.Group();
    grp.add(dot); grp.add(ring);
    grp.userData.country = name;
    grp.userData.ring = ring;
    grp.userData.dot = dot;
    return grp;
  }

  function rebuildCountryMarkers() {
    markerGroup.clear();
    countryByName = {};
    const max = countryData.reduce((m, c) => Math.max(m, c.count), 1);
    countryData.forEach(c => {
      const risk = getRiskForCountryCB(c.name) || 0;
      const grp = buildCountryMarker(c.name, c.count, max, risk);
      if (!grp) return;
      grp.userData.count = c.count;
      grp.userData.risk = risk;
      markerGroup.add(grp);
      countryByName[c.name] = grp;
    });
  }

  function animate() {
    raf = requestAnimationFrame(animate);
    const now = performance.now() / 1000;

    // Pulse rings
    markerGroup.children.forEach(grp => {
      const ring = grp.userData.ring;
      if (!ring) return;
      const phase = ring.userData.pulsePhase || 0;
      const s = 1 + Math.sin(now * 1.6 + phase) * 0.18;
      ring.scale.set(s, s, s);
      ring.material.opacity = 0.5 - Math.sin(now * 1.6 + phase) * 0.18;
    });

    // Camera animation
    if (animTarget) {
      const t = Math.min(1, (performance.now() - animTarget.t0) / animTarget.dur);
      const eased = t * t * (3 - 2 * t);
      camera.position.lerpVectors(animTarget.from, animTarget.to, eased);
      camera.lookAt(0, 0, 0);
      if (controls) controls.update();
      if (t >= 1) animTarget = null;
    } else {
      controls && controls.update();
    }

    renderer.render(scene, camera);
  }

  function handlePointerDown(ev) {
    const rect = canvas.getBoundingClientRect();
    const cx = ev.touches ? ev.touches[0].clientX : ev.clientX;
    const cy = ev.touches ? ev.touches[0].clientY : ev.clientY;
    canvas._downX = cx; canvas._downY = cy; canvas._downT = performance.now();
  }

  function handlePointerUp(ev) {
    if (!canvas._downX) return;
    const cx = ev.changedTouches ? ev.changedTouches[0].clientX : ev.clientX;
    const cy = ev.changedTouches ? ev.changedTouches[0].clientY : ev.clientY;
    const dx = cx - canvas._downX, dy = cy - canvas._downY;
    const dt = performance.now() - canvas._downT;
    canvas._downX = null;
    if (Math.abs(dx) + Math.abs(dy) > 6 || dt > 600) return;  // drag, not click

    const rect = canvas.getBoundingClientRect();
    pointer.x = ((cx - rect.left) / rect.width) * 2 - 1;
    pointer.y = -((cy - rect.top) / rect.height) * 2 + 1;
    raycaster.setFromCamera(pointer, camera);

    // 1. Try direct marker hit (precise)
    const candidates = [];
    markerGroup.traverse(o => { if (o.isMesh && o.userData.country) candidates.push(o); });
    let hits = raycaster.intersectObjects(candidates, false);
    let countryName = null;
    if (hits.length) {
      countryName = hits[0].object.userData.country;
    } else {
      // 2. Fallback: ray-vs-globe → nearest centroid
      const globeHits = raycaster.intersectObject(earthMesh, false);
      if (globeHits.length) {
        const p = globeHits[0].point.clone().normalize();
        let bestDist = Infinity, bestName = null;
        const centroids = root.COUNTRY_CENTROIDS || {};
        for (const name in centroids) {
          const c = centroids[name];
          const v = latLonToVec3(c.lat, c.lon, 1).normalize();
          const d = p.distanceTo(v);
          if (d < bestDist) { bestDist = d; bestName = name; }
        }
        // Only register hit if within ~700km (chord ~0.11 on unit sphere)
        if (bestDist < 0.18) countryName = bestName;
      }
    }

    if (countryName && onCountryClickCB) onCountryClickCB(countryName);
  }

  const Globe = {
    init(canvasEl, opts = {}) {
      canvas = canvasEl;
      hostEl = canvasEl.parentElement;
      onCountryClickCB = opts.onCountryClick || null;
      getRiskForCountryCB = opts.getRiskForCountry || (() => 0);
      mobileMode = isMobile();

      renderer = new THREE.WebGLRenderer({ canvas, antialias: true, alpha: true });
      renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, mobileMode ? 1.5 : 2));
      renderer.setClearColor(0x05070f, 0);

      const w = hostEl.clientWidth || 800;
      const h = hostEl.clientHeight || 600;
      camera = new THREE.PerspectiveCamera(45, w / h, 0.05, 200);
      camera.position.set(0, 0.3, 2.6);

      buildScene();

      controls = new THREE.OrbitControls(camera, canvas);
      controls.enableDamping = true;
      controls.dampingFactor = 0.08;
      controls.rotateSpeed = 0.45;
      controls.minDistance = MIN_DIST;
      controls.maxDistance = MAX_DIST;
      controls.enablePan = false;
      controls.autoRotate = !mobileMode;
      controls.autoRotateSpeed = 0.3;

      raycaster = new THREE.Raycaster();
      pointer = new THREE.Vector2();

      canvas.addEventListener('pointerdown', handlePointerDown);
      canvas.addEventListener('pointerup', handlePointerUp);
      controls.addEventListener('start', () => { controls.autoRotate = false; });
      controls.addEventListener('change', rebuildCityLabels);

      this.resize();
      animate();
    },

    setCountryData(arr) {
      countryData = Array.isArray(arr) ? arr : [];
      rebuildCountryMarkers();
      this.setEmpty(countryData.length === 0);
    },

    focusCountry(name) {
      const c = root.COUNTRY_CENTROIDS && root.COUNTRY_CENTROIDS[name];
      if (!c) return;
      focusedCountry = name;
      const target = latLonToVec3(c.lat, c.lon, 1.8);
      animTarget = {
        from: camera.position.clone(),
        to: target,
        t0: performance.now(),
        dur: 800
      };
      controls.autoRotate = false;
      // Highlight pulse on selected marker
      Object.entries(countryByName).forEach(([n, grp]) => {
        const ring = grp.userData.ring;
        if (!ring) return;
        ring.material.color.setHex(n === name ? 0x4f8ef7 : (grp.userData.risk >= 60 ? 0xef4444 : grp.userData.risk >= 30 ? 0xf59e0b : 0x4f8ef7));
      });
    },

    clearFocus() {
      focusedCountry = null;
      animTarget = {
        from: camera.position.clone(),
        to: new THREE.Vector3(0, 0.3, 2.6),
        t0: performance.now(),
        dur: 700
      };
    },

    resize() {
      if (!renderer || !hostEl) return;
      const w = hostEl.clientWidth;
      const h = hostEl.clientHeight;
      mobileMode = isMobile();
      renderer.setSize(w, h, false);
      camera.aspect = w / h;
      camera.updateProjectionMatrix();
      rebuildCityLabels();
    },

    toggleAutoRotate() {
      if (!controls) return false;
      controls.autoRotate = !controls.autoRotate;
      return controls.autoRotate;
    },

    setAutoRotate(on) {
      if (!controls) return;
      controls.autoRotate = !!on;
    },

    setEmpty(empty) {
      if (!hostEl) return;
      let overlay = hostEl.querySelector('.globe-empty');
      if (empty) {
        if (!overlay) {
          overlay = document.createElement('div');
          overlay.className = 'globe-empty';
          overlay.textContent = 'No geolocated events yet. Load a log file with public IPs to populate the globe.';
          hostEl.appendChild(overlay);
        }
      } else if (overlay) {
        overlay.remove();
      }
    },

    destroy() {
      cancelAnimationFrame(raf);
      controls && controls.dispose();
      renderer && renderer.dispose();
    }
  };

  root.Globe = Globe;
})(typeof window !== 'undefined' ? window : globalThis);
