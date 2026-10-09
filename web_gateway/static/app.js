(function () {
  "use strict";

  const $ = (id) => document.getElementById(id);
  const drop = $("drop"), fileInput = $("file"), previewWrap = $("previewWrap"),
        preview = $("preview"), detectBtn = $("detectBtn"), resetBtn = $("resetBtn"),
        banner = $("banner"), resultEmpty = $("resultEmpty"), resultBody = $("resultBody"),
        resultImg = $("resultImg"), detBody = $("detBody"),
        statCount = $("statCount"), statTime = $("statTime"),
        badge = $("badge"), badgeText = $("badgeText"),
        hl = $("hl"), resultImgWrap = $("resultImgWrap"),
        lightbox = $("lightbox"), lightboxImg = $("lightboxImg"),
        downloadBtn = $("downloadBtn"), pipelineEl = document.querySelector(".pipeline");

  let currentFile = null;
  const state = { dets: [], selIdx: -1, natW: 0, natH: 0, occupiedSeats: new Set() };

  const esc = (s) => String(s).replace(/[&<>"']/g, (c) => (
    { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]
  ));

  const fmtSize = (n) => n < 1024 ? n + " B"
    : n < 1048576 ? (n / 1024).toFixed(1) + " KB"
    : (n / 1048576).toFixed(1) + " MB";

  const showErr = (msg) => { banner.textContent = msg; banner.style.display = "block"; };
  const hideErr = () => { banner.style.display = "none"; };

  // 流水线“流动”动效：跟检测状态同步。CSS 里只跑一轮(≈1.4s)，
  // 这里保证轮次跑完(留点余量)再把 running 摘掉，避免中途硬切。
  const FLOW_MIN_MS = 1600;
  let flowTimer = null;
  let flowStartedAt = 0;

  function setFlow(on) {
    if (!pipelineEl) return;
    if (flowTimer) { clearTimeout(flowTimer); flowTimer = null; }
    if (on) {
      flowStartedAt = performance.now();
      pipelineEl.classList.add("running");
      return;
    }
    const wait = Math.max(0, FLOW_MIN_MS - (performance.now() - flowStartedAt));
    flowTimer = setTimeout(() => {
      pipelineEl.classList.remove("running");
      flowTimer = null;
    }, wait);
  }

  function setLoading(on) {
    detectBtn.disabled = on || !currentFile;
    detectBtn.innerHTML = on ? '<span class="spinner"></span> 推理中…' : "开始检测";
    setFlow(on);
  }

  function setFile(file) {
    if (!file || !file.type.startsWith("image/")) {
      showErr("请选择图片文件（PNG / JPG）");
      return;
    }
    currentFile = file;
    hideErr();
    preview.src = URL.createObjectURL(file);
    previewWrap.style.display = "block";
    drop.querySelector("p").textContent = file.name;
    drop.querySelector("small").textContent = fmtSize(file.size) + " · 点击可重新选择";
    detectBtn.disabled = false;
  }

  function reset() {
    currentFile = null;
    fileInput.value = "";
    previewWrap.style.display = "none";
    drop.querySelector("p").textContent = "点击选择，或把图片拖进来";
    drop.querySelector("small").textContent = "支持 PNG / JPG";
    detectBtn.disabled = true;
    hideErr();
    resultBody.style.display = "none";
    resultEmpty.style.display = "block";
    clearSelection();
    closeLightbox();
  }

  /* ---- 结果图：点击放大（lightbox） ---- */
  function openLightbox() {
    if (!resultImg.src) return;
    lightboxImg.src = resultImg.src;
    lightbox.classList.add("open");
  }
  function closeLightbox() { lightbox.classList.remove("open"); }

  /* ---- 下载带框结果图 ---- */
  function downloadResult() {
    if (!resultImg.src) return;
    const ts = new Date().toISOString().replace(/[:.]/g, "-").slice(0, 19);
    const a = document.createElement("a");
    a.href = resultImg.src;
    a.download = "cvinfer_result_" + ts + ".jpg";
    document.body.appendChild(a);
    a.click();
    a.remove();
  }

  /* ---- 检测列表：点击高亮对应框 ---- */
  function clearSelection() {
    state.dets = [];
    state.selIdx = -1;
    hl.style.display = "none";
    Array.from(detBody.children).forEach((tr) => tr.classList.remove("active"));
  }

  function selectRow(i) {
    state.selIdx = (state.selIdx === i) ? -1 : i;
    Array.from(detBody.children).forEach((tr, idx) =>
      tr.classList.toggle("active", idx === state.selIdx));
    applyHighlight();
  }

  function applyHighlight() {
    const det = state.dets[state.selIdx];
    const nw = state.natW || resultImg.naturalWidth;
    const nh = state.natH || resultImg.naturalHeight;
    if (!det || !nw || !nh) { hl.style.display = "none"; return; }
    hl.style.left = (det.x1 / nw * 100) + "%";
    hl.style.top = (det.y1 / nh * 100) + "%";
    hl.style.width = (det.width / nw * 100) + "%";
    hl.style.height = (det.height / nh * 100) + "%";
    hl.style.display = "block";
  }

  // 图尺寸变化（新结果）后重新对齐高亮框
  resultImg.addEventListener("load", () => {
    state.natW = resultImg.naturalWidth;
    state.natH = resultImg.naturalHeight;
    applyHighlight();
  });
  resultImgWrap.addEventListener("click", openLightbox);
  lightbox.addEventListener("click", closeLightbox);
  downloadBtn.addEventListener("click", downloadResult);
  document.addEventListener("keydown", (e) => { if (e.key === "Escape") closeLightbox(); });

  /* ---- 选择 / 拖拽 ---- */
  drop.addEventListener("click", () => fileInput.click());
  drop.addEventListener("dragover", (e) => { e.preventDefault(); drop.classList.add("drag"); });
  drop.addEventListener("dragleave", () => drop.classList.remove("drag"));
  drop.addEventListener("drop", (e) => {
    e.preventDefault();
    drop.classList.remove("drag");
    if (e.dataTransfer.files.length) setFile(e.dataTransfer.files[0]);
  });
  fileInput.addEventListener("change", () => {
    if (fileInput.files.length) setFile(fileInput.files[0]);
  });
  resetBtn.addEventListener("click", reset);

  /* ---- 提交检测 ---- */
  detectBtn.addEventListener("click", async () => {
    if (!currentFile) return;
    setLoading(true);
    hideErr();

    const fd = new FormData();
    fd.append("image", currentFile);
    const t0 = performance.now();

    try {
      const res = await fetch("/api/detect", { method: "POST", body: fd });
      const data = await res.json().catch(() => ({
        ok: false, error: "服务端返回了非 JSON 响应（HTTP " + res.status + "）",
      }));
      const dt = Math.round(performance.now() - t0);
      if (!data.ok) {
        showErr(data.error || ("检测失败（HTTP " + res.status + "）"));
        return;
      }
      render(data, dt);
    } catch (err) {
      showErr("请求失败：" + err);
    } finally {
      setLoading(false);
    }
  });

  function render(data, dt) {
    resultEmpty.style.display = "none";
    resultBody.style.display = "block";
    clearSelection();
    if (data.image) resultImg.src = data.image;
    statCount.textContent = data.count;
    statTime.textContent = dt + " ms";

    detBody.innerHTML = "";
    const dets = (data.detections || []).slice().sort((a, b) => b.confidence - a.confidence);
    state.dets = dets;
    if (!dets.length) {
      detBody.innerHTML = '<tr><td colspan="4" class="muted">未检测到目标</td></tr>';
      return;
    }
    dets.forEach((d, i) => {
      const pct = Math.round(d.confidence * 100);
      const tr = document.createElement("tr");
      tr.innerHTML =
        "<td>" + (i + 1) + "</td>" +
        '<td><span class="chip">' + esc(d.label) + "</span> " +
          '<span class="muted">#' + d.class_id + "</span></td>" +
        '<td><div class="bar"><i style="width:' + pct + '%"></i></div>' +
          '<span class="muted">' + pct + "%</span></td>" +
        "<td>" + d.width + "×" + d.height + "</td>";
      tr.addEventListener("click", () => selectRow(i));
      detBody.appendChild(tr);
    });
  }

  /* ---- 服务状态徽标 ---- */
  async function checkStatus() {
    try {
      const res = await fetch("/api/status");
      const s = await res.json();
      badge.classList.toggle("online", !!s.reachable);
      badge.classList.toggle("offline", !s.reachable);
      badgeText.textContent = s.reachable ? "C++ 服务在线" : "C++ 服务离线";
    } catch (err) {
      badge.classList.remove("online");
      badge.classList.add("offline");
      badgeText.textContent = "状态未知";
    }
  }

  /* ---- ③ 流水线结果：视频 + 文件清单 + 事件 ---- */
  const resHint = $("resHint"), resFiles = $("resFiles"), videoWrap = $("videoWrap"),
        resVideo = $("resVideo"), videoNote = $("videoNote"), eventsEl = $("events"),
        occBanner = $("occBanner"), resRefresh = $("resRefresh"),
        scene = $("scene"), sceneImg = $("sceneImg"), sceneSvg = $("sceneSvg"),
        sceneLegend = $("sceneLegend"), sceneHint = $("sceneHint"), sceneEmpty = $("sceneEmpty");

  function fileRow(f) {
    if (!f || !f.exists) {
      return '<div class="frow off"><span class="fname">' +
             esc((f && f.name) || "-") + '</span><span class="fmeta">不存在</span></div>';
    }
    return '<div class="frow"><span class="fname">' + esc(f.name) + '</span>' +
           '<span class="fmeta">' + esc(f.size_text) + " · " + esc(f.mtime) + '</span></div>';
  }

  async function loadResults() {
    resHint.textContent = "正在读取结果目录…";
    try {
      const s = await (await fetch("/api/results")).json();
      const v = s.video || {};
      if (!v.exists) {
        resHint.textContent = "还没有结果视频 —— 先跑一次 C++ 主程序（它会写出 output.avi）。";
        videoWrap.style.display = "none";
      } else {
        resHint.textContent = "结果目录：" + s.result_dir;
        videoWrap.style.display = "block";
        if (!s.video_has_ffmpeg) {
          videoNote.textContent = "服务器没装 ffmpeg，无法在线播放（可点右上「下载原始 AVI」用本地播放器）";
          resVideo.removeAttribute("src");
        } else {
          videoNote.textContent = s.video_playable
            ? "已就绪（缓存命中，直接播放）"
            : "首次播放会自动转码（MJPEG→MP4），大视频请稍候…";
          resVideo.src = "/api/result-video?ts=" + encodeURIComponent(v.mtime || "");
        }
      }
      resFiles.innerHTML = fileRow(v) + fileRow(s.log) + fileRow(s.fallback_csv);
    } catch (err) {
      resHint.textContent = "读取结果失败：" + err;
    }
  }

  resVideo.addEventListener("loadeddata", () => { videoNote.textContent = "已就绪"; });
  resVideo.addEventListener("error", () => {
    videoNote.textContent = "视频加载失败（ffmpeg 转码失败或文件正在被写入）";
  });

  /* ---- 画面示意：把日志里的“座位 A-12”落到画面上的具体位置 ---- */
  const SVG_NS = "http://www.w3.org/2000/svg";
  let seatShapes = [];   // [{name, el}]

  function focusSeat(name) {
    if (!seatShapes.length) return;
    seatShapes.forEach((s) => s.el.classList.toggle("focus", s.name === name));
    scene.scrollIntoView({ behavior: "smooth", block: "nearest" });
  }

  async function loadScene() {
    try {
      const s = await (await fetch("/api/scene")).json();
      const size = s.size || {}, seats = s.seats || [];
      const say = (msg) => {
        scene.style.display = "none";
        sceneEmpty.style.display = "block";
        sceneEmpty.textContent = msg;
      };
      if (!s.video_exists || !size.width || !size.height) {
        say("还没有结果视频，画不出画面示意 —— 先跑一次 C++ 主程序（它会写出 output.avi）。");
        return;
      }
      if (!seats.length) {
        say(s.seat_hint || "配置里没有可用的座位区，无法在画面上标注位置。");
        return;
      }
      sceneEmpty.style.display = "none";
      scene.style.display = "block";

      // 底图 = 结果视频首帧（用 frames+mtime 当缓存键，视频重跑后自动换图）
      sceneImg.src = "/api/scene-frame?v=" + encodeURIComponent(
        (size.frames || 0) + "-" + (size.fps || 0));
      // viewBox 用**视频像素坐标** ⇒ SVG 里可直接写配置里的原始坐标，无需换算
      sceneSvg.setAttribute("viewBox", "0 0 " + size.width + " " + size.height);
      sceneSvg.innerHTML = "";
      seatShapes = seats.map((seat) => {
        const xs = seat.polygon.map((p) => p[0]), ys = seat.polygon.map((p) => p[1]);
        const poly = document.createElementNS(SVG_NS, "polygon");
        poly.setAttribute("class", "seat-poly");
        poly.setAttribute("points", seat.polygon.map((p) => p[0] + "," + p[1]).join(" "));
        sceneSvg.appendChild(poly);

        const label = document.createElementNS(SVG_NS, "text");
        label.setAttribute("class", "seat-label");
        label.setAttribute("x", Math.min.apply(null, xs) + 8);
        label.setAttribute("y", Math.min.apply(null, ys) + 28);
        label.textContent = seat.name;
        sceneSvg.appendChild(label);
        return { name: seat.name, el: poly, labelEl: label };
      });
      applyOccupied();   // 事件已先到的话，这里把被占座位标红
      sceneLegend.innerHTML = seatShapes.map((sh) =>
        '<span class="seat-chip">' + esc(sh.name) + "</span>").join("");
      sceneHint.textContent = "底图＝结果视频首帧（" + size.width + "×" + size.height +
        "）；蓝框＝配置里的 occupancy.seats" + (s.config ? "（" + s.config + "）" : "") +
        "。点下方任一条事件，这里会定格到那一刻（那时的检测框已经画在画面里）。";
    } catch (err) {
      scene.style.display = "none";
      sceneEmpty.style.display = "block";
      sceneEmpty.textContent = "读取画面示意失败：" + err;
    }
  }

  function eventTitle(e) {
    if (e.seat) return "座位 " + e.seat;
    if (e.kind === "auth") return "鉴权";
    if (e.kind === "alert_error") return "告警外发";
    return "事件";
  }

  // 结构化字段拼成一行“人话”；字段缺失就不提，原始日志行永远完整保留在下面
  function eventFacts(e) {
    const out = [];
    if (e.items && e.items.length) out.push("物品 " + e.items.join(" / "));
    if (e.dwell_s != null) out.push("物品已滞留 " + e.dwell_s + " 秒");
    if (e.person_absent_s === null) out.push("全程未见人");
    else if (e.person_absent_s != null) out.push("人已离开 " + e.person_absent_s + " 秒");
    if (e.vote) out.push("投票 " + e.vote);
    return out.join(" · ");
  }

  // 占座事件的“事实”用醒目胶囊展示（物品 / 滞留秒数 / 人离座秒数）
  function occupancyBadges(e) {
    const chips = [];
    if (e.items && e.items.length)
      chips.push('<span class="ev-badge item">物品 ' + esc(e.items.join(" / ")) + "</span>");
    if (e.dwell_s != null)
      chips.push('<span class="ev-badge dwell">滞留 ' + e.dwell_s + " 秒</span>");
    if (e.person_absent_s === null)
      chips.push('<span class="ev-badge absent">全程未见人</span>');
    else if (e.person_absent_s != null)
      chips.push('<span class="ev-badge absent">人离座 ' + e.person_absent_s + " 秒</span>");
    if (e.vote) chips.push('<span class="ev-badge">投票 ' + esc(e.vote) + "</span>");
    return chips.join("");
  }

  function renderEvents(list, hint) {
    if (!list.length) {
      eventsEl.innerHTML = '<div class="muted" style="font-size:12.5px">' +
        esc(hint || "暂无事件") + "</div>";
      return;
    }
    // 占座事件按“滞留时长”从久到近置顶（最严重的先看到）；其余事件保持最新在上。
    const occ = list.filter((e) => e.kind === "occupancy")
                    .sort((a, b) => (b.dwell_s || 0) - (a.dwell_s || 0));
    const rest = list.filter((e) => e.kind !== "occupancy").slice().reverse();
    eventsEl.innerHTML = occ.concat(rest).map((e, i) => {
      const seatAttr = e.seat ? ' data-seat="' + esc(e.seat) + '"' : "";
      const tAttr = (e.video_t != null) ? ' data-t="' + e.video_t + '"' : "";
      const go = (e.seat || e.video_t != null)
        ? '<span class="ev-go">定格到那一刻 →</span>' : "";
      const icon = (e.kind === "occupancy") ? "🪑 " : "";
      const worst = (e.kind === "occupancy" && i === 0) ? " ev-worst" : "";
      // 占座：胶囊突出关键事实；其他事件：沿用一行“人话”。
      const badges = (e.kind === "occupancy") ? occupancyBadges(e) : "";
      const body = (e.kind === "occupancy")
        ? (badges ? '<div class="ev-badges">' + badges + "</div>" : "")
        : (eventFacts(e) ? '<div class="ev-body">' + esc(eventFacts(e)) + "</div>" : "");
      return '<div class="ev k-' + esc(e.kind || "other") + worst + '"' + seatAttr + tAttr + ">" +
        '<div class="ev-head"><span class="ev-t">' + esc(e.time || "") + "</span>" +
        '<span class="ev-title">' + icon + esc(eventTitle(e)) + "</span>" + go + "</div>" +
        body +
        '<details class="ev-detail"><summary>原始日志行</summary>' +
        '<div class="ev-raw">' + esc(e.text) + "</div></details></div>";
    }).join("");
  }

  // 汇总事件里的占座情况：几个座位、最严重的几条、被占座位名集合（供座位着色）
  function aggregateOccupancy(events) {
    const occ = (events || []).filter((e) => e.kind === "occupancy" && e.seat);
    const worstBySeat = {};
    occ.forEach((e) => {
      const cur = worstBySeat[e.seat];
      if (!cur || (e.dwell_s || 0) > (cur.dwell_s || 0)) worstBySeat[e.seat] = e;
    });
    const seats = Object.keys(worstBySeat);
    const top = seats.map((s) => worstBySeat[s])
                     .sort((a, b) => (b.dwell_s || 0) - (a.dwell_s || 0)).slice(0, 3);
    return { count: occ.length, seats: new Set(seats), top: top };
  }

  // 顶部“占座告警”横幅：一眼看出本轮有没有占座、占了哪几个座位
  function renderOccBanner(sum, hasLog) {
    if (!hasLog) { occBanner.style.display = "none"; return; }
    if (!sum.seats.size) {
      occBanner.className = "occ-banner clean";
      occBanner.innerHTML = '<span class="occ-icon">✅</span>' +
        '<div class="occ-main"><b>本轮未检测到占座</b>' +
        '<div class="occ-sub">日志中暂无「[占座]」事件。</div></div>';
      occBanner.style.display = "flex";
      return;
    }
    const rows = sum.top.map((e) => {
      const bits = [];
      if (e.items && e.items.length) bits.push(e.items.join(" / "));
      if (e.dwell_s != null) bits.push("物品滞留 " + e.dwell_s + " 秒");
      if (e.person_absent_s === null) bits.push("全程未见人");
      else if (e.person_absent_s != null) bits.push("人离座 " + e.person_absent_s + " 秒");
      return '<div class="occ-row"><span class="occ-seat">座位 ' + esc(e.seat) + "</span>" +
        (bits.length ? '<span class="occ-facts">' + esc(bits.join(" · ")) + "</span>" : "") + "</div>";
    }).join("");
    const more = (sum.seats.size > sum.top.length)
      ? '<div class="occ-more">另有 ' + (sum.seats.size - sum.top.length) +
        " 个座位疑似占座，详见下方事件列表</div>" : "";
    occBanner.className = "occ-banner";
    occBanner.innerHTML = '<span class="occ-icon">⚠️</span>' +
      '<div class="occ-main"><b>检测到 ' + sum.seats.size + ' 个座位疑似占座</b>' +
      (sum.count > sum.seats.size ? '<span class="occ-count">（共 ' + sum.count + " 条占座事件）</span>" : "") +
      '<div class="occ-list">' + rows + more + "</div></div>";
    occBanner.style.display = "flex";
  }

  // 把“被占座位”在画面示意上标红（与点击定位的琥珀 .focus 区分）
  function applyOccupied() {
    if (!seatShapes.length) return;
    seatShapes.forEach((s) => {
      const on = state.occupiedSeats.has(s.name);
      s.el.classList.toggle("occupied", on);
      if (s.labelEl) s.labelEl.classList.toggle("occupied", on);
    });
  }

  async function loadEvents() {
    try {
      const s = await (await fetch("/api/events")).json();
      const list = s.events || [];
      const sum = aggregateOccupancy(list);
      state.occupiedSeats = sum.seats;
      renderOccBanner(sum, !!s.source);   // source 非空 = 找到了日志
      renderEvents(list, s.hint);
      applyOccupied();
    } catch (err) {
      eventsEl.innerHTML = '<div class="muted">读取事件失败：' + esc(String(err)) + "</div>";
      occBanner.style.display = "none";
    }
  }

  // 点事件行 → ①画面示意定格到“那一刻” ②高亮对应座位区 ③播放器也跳到那一刻
  eventsEl.addEventListener("click", (ev) => {
    if (ev.target.closest("summary")) return;   // 展开/收起原始日志行不触发定位
    const row = ev.target.closest(".ev");
    if (!row) return;
    const seat = row.getAttribute("data-seat");
    const t = row.getAttribute("data-t");
    if (seat) focusSeat(seat);
    if (t !== null) showEventFrame(parseFloat(t));
  });

  // 把「画面示意」的底图从**视频首帧**换成**事件发生那一刻**的帧。
  // 这一帧是从 output.avi 抽的，检测框（纯绿色）本来就在上面 ⇒ 点一下就知道当时框住了什么。
  function showEventFrame(t) {
    if (!isFinite(t)) return;
    sceneImg.src = "/api/event-frame?t=" + encodeURIComponent(t) + "&v=" + Date.now();
    if (resVideo.src) {
      try { resVideo.currentTime = t; } catch (e) { /* 元数据未就绪，忽略 */ }
    }
    scene.scrollIntoView({ behavior: "smooth", block: "nearest" });
  }

  resRefresh.addEventListener("click", () => { loadResults(); loadScene(); loadEvents(); });
  loadResults();
  loadScene();
  loadEvents();

  checkStatus();
  // 每 10s 顺带刷新一次事件：跑完视频后占座事件会自动冒出来，不用手动点“刷新”
  setInterval(() => { checkStatus(); loadEvents(); }, 10000);
})();
