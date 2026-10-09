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

  // ④ 座位标定探针：上传视频 → 后端跑 C++ 探针 → 展示热力图
  const probeFile = $("probeFile"), probeBtn = $("probeBtn"), probeStatus = $("probeStatus"),
        probeOut = $("probeOut"), probeImg = $("probeImg"), probeSuggest = $("probeSuggest"),
        probeLabels = $("probeLabels"), probeNote = $("probeNote"), probeBanner = $("probeBanner");

  // P1 座位标定编辑器：在同一张底图上点选 zone → 回写 occupancy.seats
  const editWrap = $("editWrap"), editImg = $("editImg"), editSvg = $("editSvg"),
        editStage = $("editStage"), editSeatsBox = $("editSeats"),
        editToolSelect = $("editToolSelect"), editToolRect = $("editToolRect"),
        editToolPoly = $("editToolPoly"), editUseSuggest = $("editUseSuggest"),
        editReload = $("editReload"), editDel = $("editDel"), editHeat = $("editHeat"),
        editValidate = $("editValidate"), editSave = $("editSave"),
        editStatus = $("editStatus"), editMode = $("editMode"),
        editChk = $("editChk"), editTip = $("editTip"),
        editBanner = $("editBanner"), editPreviewBox = $("editPreviewBox"),
        editPreview = $("editPreview"), editHint = $("editHint");

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
        occBanner = $("occBanner"), resRefresh = $("resRefresh"), resDiag = $("resDiag"),
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
  /* ---- P3：「为什么一条告警都没有」自证面板 ----
     数据全部来自 /api/events 的 diagnosis 字段（后端从**日志原文**抽的，每条都带原文行）。
     前端只做呈现 —— 一条判据都不在这里重写（与 P2 同样的取舍）。 */
  const DIAG_ICON = { err: "⛔", warn: "⚠️", ok: "✅", info: "ℹ️" };

  function renderDiag(d) {
    if (!d || !d.verdicts) { resDiag.style.display = "none"; return; }
    const c = d.config || {};
    let head = '<div class="diag-head">本轮「为什么是这样」' +
      '<span class="muted">（每条都附日志原文，点开核对；判定口径仍在 C++）</span>' +
      '<div class="diag-cfg">网页在编辑的配置：<code>' + esc(c.path || "?") + "</code> ";
    if (!c.exists) head += '<b>—— 文件不存在</b>';
    else if (!c.has_occupancy) head += '<b>—— 这份里没有 occupancy 段，你画的座位不在这里</b>';
    else head += "（有 occupancy 段" + (c.enabled_text != null ? "，enabled=" + esc(c.enabled_text) : "") + "）";
    if (c.mtime) head += ' <span class="muted">· ' + esc(c.mtime) + "</span>";
    head += "</div></div>";

    const rows = d.verdicts.map((v) =>
      '<div class="diag-row ' + esc(v.level || "info") + '">' +
      '<span class="diag-icon">' + (DIAG_ICON[v.level] || "•") + "</span>" +
      '<div class="diag-body"><b>' + esc(v.title || "") + "</b>" +
      (v.detail ? '<div class="diag-detail">' + esc(v.detail) + "</div>" : "") +
      (v.evidence ? '<details class="diag-ev"><summary>日志原文</summary><div>' +
                    esc(v.evidence) + "</div></details>" : "") +
      "</div></div>").join("");
    resDiag.innerHTML = head + rows;
    resDiag.style.display = "block";
  }

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
      renderDiag(s.diagnosis);            // 「为什么没有告警」：全部来自日志原文
      renderEvents(list, s.hint);
      applyOccupied();
    } catch (err) {
      eventsEl.innerHTML = '<div class="muted">读取事件失败：' + esc(String(err)) + "</div>";
      occBanner.style.display = "none";
      resDiag.style.display = "none";
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

  /* ---- ④ 座位标定探针：上传视频 → 后端跑一段 C++ 探针 → 展示热力图 ---- */
  // 后端返回的 suggested_zone / cells 坐标就是**视频像素坐标**（口径由 C++ 决定），
  // P1 的可视化点选会直接吃这份数据来画/拖 zone；这里先只负责"看一眼"。
  const probeErr = (m) => {
    probeBanner.textContent = m || "";
    probeBanner.style.display = m ? "block" : "none";
  };

  probeFile.addEventListener("change", () => {
    const f = probeFile.files && probeFile.files[0];
    probeBtn.disabled = !f;
    probeStatus.textContent = f ? f.name + "（" + fmtSize(f.size) + "）" : "";
    probeErr("");
  });

  function renderProbe(d) {
    if (d.heatmap) {
      probeImg.src = d.heatmap;
      probeOut.style.display = "flex";
    } else {
      probeOut.style.display = "none";
    }
    const z = d.suggested_zone;
    if (d.suggestion_valid && z) {
      probeSuggest.className = "probe-suggest";
      probeSuggest.innerHTML = "建议座位区 rect：<b>[" + z.x + ", " + z.y + ", " +
        z.width + ", " + z.height + "]</b>" +
        '<div class="muted" style="font-size:11.5px">画面 ' + d.width + "×" + d.height +
        "，网格 " + d.cols + "×" + d.rows + "，每格 " + d.cell_w + "×" + d.cell_h + "</div>";
    } else {
      probeSuggest.className = "probe-suggest warn";
      probeSuggest.textContent = "没有测到可用的物品落区 —— 把 min_item_hits 调小、" +
        "换更长的视频，或先看下方「检出标签」有没有你的物品类别（没有的话问题在模型/场景，改坐标也没用）。";
    }
    const labels = d.label_top || [];
    probeLabels.innerHTML = "<b>检出标签（次数降序）：</b>" + (labels.length
      ? labels.map((l) => '<span class="probe-chip">' + esc(l.label) + " ×" + l.count + "</span>").join("")
      : '<span class="muted">无</span>');
    probeNote.textContent = "热格上的数字 = 该格物品命中次数（p 前缀 = 人命中）；青色框 = 建议 zone。" +
      "归属口径与线上占座判定同源（检测框底边中点落格）。" + (d.warn ? " ⚠ " + d.warn : "");
    // 「我在改哪份文件」必须是看得见的事实：网页写回的是后端这份 CONFIG_PATH，
    // 而程序加载的是它启动目录下的相对路径 —— 两者不是同一份时，画得再准也不会生效。
    if (d.config) {
      probeNote.textContent += " 标定会写回：" + d.config + "（只改 occupancy.seats 这一段）";
    }

    // P1: 热力图看清楚了 → 直接在同一张底图上**点选**标定, 并回写 occupancy.seats
    lastSuggest = (d.suggestion_valid && z) ? z : null;
    // P2: 把探针的**格子热力**留下 —— 本地提示（"这一区压到多少物品/人的落点"）只读它,
    // 不重算"哪个框算进哪一格"（那个口径在 C++ 的 SeatProbe）。
    edCells = d.cells || [];
    edCols = d.cols || 0;
    edRows = d.rows || 0;
    edCellW = d.cell_w || 0;
    edCellH = d.cell_h || 0;
    edLabelTop = d.label_top || [];      // 只认出的类别 —— 用来区分"画错了"和"根本没抓到物品"
    // 配置里写的 occupancy.item_labels（后端随探针一起回）：用它把**模型没认出来**与
    // **认出来了但配置没算成物品**分开。这里只做集合比较，不含任何判定（判定仍在 C++）。
    edItemLabels = d.item_labels || [];
    edStride = d.stride || 1;            // 计数是整段视频每 N 帧抽 1 帧累出来的（底图只是首帧）
    if (edOpen(d.frame, d.heatmap, d.width, d.height)) {
      if (!edSeats.length) edLoadFromConfig(false);   // 有现成座位就先摆出来让人改, 别从零画
      edSay("标定区已在下方展开：画/拖好座位后，点「保存到配置」");
    }
    // P2: 换了现场/换了视频, 先把这一版的即时结论跑出来（读进来的旧坐标可能已经越界/重叠）
    edCheckSoon(60);
  }

  probeBtn.addEventListener("click", async () => {
    const f = probeFile.files && probeFile.files[0];
    if (!f) return;
    probeErr("");
    probeBtn.disabled = true;
    probeBtn.innerHTML = '<span class="spinner"></span> 探针跑模型中…';
    probeStatus.textContent = "正在跑一遍视频（跳帧采样），大视频请稍候…";
    try {
      const fd = new FormData();
      fd.append("video", f);
      const res = await fetch("/api/seat-probe", { method: "POST", body: fd });
      const data = await res.json().catch(() => ({ ok: false, error: "响应不是 JSON" }));
      if (!res.ok || !data.ok) {
        probeErr(data.error || ("探针失败（HTTP " + res.status + "）"));
        probeStatus.textContent = "";
        return;
      }
      renderProbe(data);
      probeStatus.textContent = "完成：" + (data.video_name || "");
    } catch (err) {
      probeErr("请求失败：" + err);
      probeStatus.textContent = "";
    } finally {
      probeBtn.disabled = false;
      probeBtn.textContent = "生成热力图";
    }
  });

  /* ============== P1: 点选座位 zone → 回写 occupancy.seats ==============
     坐标系: editSvg 的 viewBox 直接设成**视频像素尺寸** ⇒ 图层里写的坐标就是配置里
     要写的坐标, 前端不做任何换算, 也就不会出现"看到的位置"与"存下去的位置"不一致。
     口径: "重叠多少算多"**不在这里判** —— 保存/校验时由后端交给 C++ `--check-config`
     验一遍(与程序启动时同一份规则, 见 app.py 的 _check_config)。
     P2: 同一份口径还做成了**即时反馈**(见下面 edCheckNow/edTipUpdate), 边拖边告诉你哪不对。
  */
  let edW = 0, edH = 0;      // 视频像素尺寸
  let edSeats = [];          // [{id,name,kind,rect:[x,y,w,h] | polygon:[[x,y],…]}]
  let edSel = null;          // 选中座位 id
  let edTool = "select";     // select | rect | polygon
  let edDraw = null;         // 正在画的临时图形
  let edDrag = null;         // 正在拖的东西
  let edSeq = 0;
  let edHeatUrl = "";        // 热力图 dataURL（可选叠加，只为了看清位置）
  // P2 即时反馈的两份"原料"：① 探针算好的格子热力（本地提示的唯一数据来源）
  //                           ② 被服务端点名的座位（只用来描红, 不是判定）
  let edCells = [], edCols = 0, edRows = 0, edCellW = 0, edCellH = 0;
  let edLabelTop = [], edStride = 1, edItemLabels = [];
  let edBad = {};

  const ED_HANDLE_PX = 11;   // 把手**屏幕**像素尺寸（按缩放折算成用户单位）
  const edUid = () => "ed" + (++edSeq);
  const edNum = (v) => Math.round(Number(v) || 0);
  const edCX = (v) => Math.max(0, Math.min(edW, edNum(v)));
  const edCY = (v) => Math.max(0, Math.min(edH, edNum(v)));

  function edErr(msg) {
    editBanner.textContent = msg || "";
    editBanner.style.display = msg ? "block" : "none";
  }
  const edSay = (msg) => { editStatus.textContent = msg || ""; };

  /* ---- P2 即时校验反馈：一边拖，一边告诉你"这一版能不能存" ----
     分两路, 各管一段, 谁也不许越界:
       ① 本地提示（edTipUpdate, 拖动中每帧, 零延迟）：只拿**探针已经算好的**格子热力做一次
          测量 —— "这个 zone 压住了多少次物品落点 / 多少次人的落点"。它是**测量**不是判定：
          不设阈值、不下结论（页面末尾写明"判定以红/绿那行为准"）。两条提醒都不需要魔数：
            · 整段视频一个物品标签都没抓到（gItem<=0）⇒ 先把"模型/occupancy.item_labels 对不对"
              说清楚, 不冤枉 zone（旧版会直接怪"你画到人坐的地方");
            · person 落点 > item 落点 ⇒ 只当**线索**列两种可能, 让商家对着原视频判断。
          口径必读: 这些次数是探针跑**整段视频**按 stride 抽样累出来的, 而页面底图只是**首帧**
          ⇒ "图上没人"不等于"这段视频里没人"(有人从桌边走过/坐下都记进那一格)。这条歧义
          实测踩过: 商家画在只有物品的桌面上, 却看到"人的落点多于物品"。
       ② 服务端即时校验（edCheckNow → POST /api/seats, dry_run=true）："重叠多少算多 / 顶点
          是否越界 / 名字是否重复"这些**判定口径**一律不在这里重写 —— 一律问 C++ 的
          --check-config, 红字用的是后端原文。前端一旦自己写个阈值, 就会出现
          "网页说能存、程序起不来"的口径分叉（这正是这条产品线最贵的一类 bug）。
     为什么要节流: 每次校验都要起一个 C++ 进程（实测 ~100ms）, 拖动 60fps 不可能每帧都发；
     于是拖动中最多 ~300ms 发一次, 且同一时刻只允许一个在飞（期间改过就补发一次）。
  */
  const ED_CHK_MS = 300;         // 拖动中两次即时校验的最小间隔
  const ED_SAMPLE = 6;           // 每格 6×6 采样点估覆盖比例（提示用, 精度够了）
  let edChkBusy = false, edChkDirty = false, edChkT = null, edChkSeq = 0;

  const edApprox = (v) => String(v >= 10 ? Math.round(v) : Math.round(v * 10) / 10);

  // 格子被 zone 盖住的比例：6×6 采样点落在多边形内的占比。
  // 复用 edInPoly ⇒ "一个点在不在区内"全项目只有一份实现。
  function edCellFrac(pts, x, y, w, h) {
    let hit = 0;
    for (let r = 0; r < ED_SAMPLE; ++r) {
      const py = y + (r + 0.5) * h / ED_SAMPLE;
      for (let c = 0; c < ED_SAMPLE; ++c) {
        if (edInPoly({ x: x + (c + 0.5) * w / ED_SAMPLE, y: py }, pts)) hit += 1;
      }
    }
    return hit / (ED_SAMPLE * ED_SAMPLE);
  }

  // 一个 zone 折算出的落点次数。只**读**探针给的每格计数, 不重算"哪个框算进哪一格"
  // —— 那个口径在 C++（SeatProbe），Python/JS 都只负责呈现。
  function edCover(s) {
    const pts = edPolyOf(s);
    let item = 0, person = 0, cells = 0;
    for (const c of edCells) {
      const cx = c.col * edCellW, cy = c.row * edCellH;
      const cw = Math.min(edCellW, Math.max(0, edW - cx));
      const ch = Math.min(edCellH, Math.max(0, edH - cy));
      if (cw <= 0 || ch <= 0) continue;
      const f = edCellFrac(pts, cx, cy, cw, ch);
      if (f <= 0) continue;
      cells += 1;
      item += f * (c.item_hits || 0);
      person += f * (c.person_hits || 0);
    }
    return { item: item, person: person, cells: cells };
  }

  // 只认出的类别名单（用于把"没抓到物品"说清楚，而不是怪 zone 画错）
  function edLabelTopText(n) {
    return edLabelTop.slice(0, n || 4)
      .map((l) => (l && l.label ? l.label + "×" + l.count : "")).filter(Boolean).join("、")
      || "无";
  }

  // 检出了、但**不在** occupancy.item_labels 里的类别 —— 把"没识别到"和"没算它"分开。
  // 实测场景: 视频里 handbag 检出 90 次, 而配置里写的是 "bag"（COCO 里压根没有这个词）
  //   ⇒ 规则层从没把它当物品, 事件列表当然是空的 —— 但页面此前只会说"你画错 zone 了"。
  // 这里只做集合比较（配置里的名单 vs 探针的检出统计），不含任何判定。
  function edExcludedLabels() {
    if (!edItemLabels.length || !edLabelTop.length) return [];
    const known = {};
    edItemLabels.forEach((l) => { known[String(l).trim()] = true; });
    return edLabelTop.filter((l) => l && l.label && !known[l.label]);
  }

  function edExListText(list) {
    return list.slice(0, 4).map((l) => l.label + "×" + l.count).join("、") || "无";
  }

  function edTipUpdate() {
    const s = edSelSeat() || edSeats[0];
    if (!s || !edCells.length || !edW || !edH) { editTip.textContent = ""; return; }
    const cov = edCover(s);
    if (!cov.cells) { editTip.textContent = ""; return; }
    let gItem = 0;
    for (const c of edCells) gItem += c.item_hits || 0;   // 整段视频的物品落点总数

    let t = "探针实测（" + edCols + "×" + edRows + " 网格；底图是首帧，计数却是**整段视频**每 "
            + edStride + " 帧抽 1 帧累出来的）：「" + (s.name || "(未命名)") + "」这一区≈"
            + edApprox(cov.item) + " 次物品落点";
    if (cov.person > 0) t += "、" + edApprox(cov.person) + " 次人的落点";
    t += "。";

    const excl = edExcludedLabels();
    if (gItem <= 0) {
      // 整段视频一个物品标签都没抓到 ⇒ 再说什么"画到人坐的地方"都是冤枉 zone。
      t += "⚠ 这段视频**没有抓到任何物品标签**（认出的类别：" + edLabelTopText() + "）—— "
         + "先确认模型类别 / occupancy.item_labels 对得上，再看 zone 画得准不准。";
      if (excl.length) {
        t += " 具体说：这些类别**确实被检出过**，却不在 occupancy.item_labels 里（"
           + edExListText(excl) + "），规则层不会把它们算成「物品」——"
           + "标签是**精确相等**匹配，多一个词、少一个字母都算不进来。";
      }
    } else {
      if (excl.length) {
        t += "⚠ 另有类别被检出过、却不在 occupancy.item_labels 里（" + edExListText(excl) + "）—— "
           + "不是模型没认出来，是配置没把它们算成「物品」（精确相等匹配）。";
      }
      if (cov.person > cov.item) {
        // person > item 只当**线索**摆出来, 不当判决: 计数是整段视频的, 而画面上只有首帧 ——
        // 首帧没人 ≠ 这段视频里没人（有人从桌边走过/坐下都会被记进这一格）。
        t += "⚠ 这一区人的落点多于物品。";
        if (cov.item > 0) {
          t += " 而且这 " + edApprox(cov.item) + " 次物品落点几乎都伴随人的落点（人 "
             + edApprox(cov.person) + " 次）⇒ 多半是人**随身/正在用**的物品："
             + "规则层会按「物品在人身上」（包含度）把它排除掉，不判占座。"
             + "要验证占座，请换「物品在、人走开」的镜头。";
        }
        t += " 两种常见原因：①首帧看不到人，但这段视频里有人在这一带"
           + "走过/坐下（计数看整段, 不是看首帧）；②这一区确实画到了「人坐的位置」——"
           + "那样 C2（人的底边中点在区内）会一直命中，占了座也不报警。请对着原视频判断是哪一种。";
      }
    }
    editTip.textContent = t + "（只把实测数据摆出来, 判定以红/绿那行为准）";
  }

  function edChkShow(kind, html) {
    editChk.className = "live-row" + (kind ? " " + kind : "");
    editChk.innerHTML = html || "";
  }

  // 把"这一版行不行"写进即时行 + 给被点名的座位描红。
  // 手工「校验/保存」按钮也走这里 —— 两边结论同源, 才不会一个绿一个红地打脸。
  // note 非空 = 后端说"C++ 程序没找到、这次是跳过校验"(checked_by=skipped) ⇒ 只给橙色提示,
  // 绝不打绿字: 否则商家会以为 C++ 认过了, 而这正是最不该骗人的一处。
  function edMirrorLive(ok, issues, saved, note) {
    if (ok) {
      edMarkBad("");
      if (note) {
        edChkShow("tip", "⚠ " + esc(note));
        return;
      }
      edChkShow("ok", saved
        ? "✓ 已写回配置，而且这一版本身就是 C++ `--check-config` 认过的（重叠/重名/越界同一份口径）。"
        : "✓ 即时校验通过 —— 重叠/重名/越界等口径是 C++ `--check-config` 判的，"
          + "可以直接「保存到配置」。");
      return;
    }
    edMarkBad((issues || []).join(" "));
    edChkShow("bad", "✗ 这一版存不进去（配置一个字都没改）：<br>• " +
              (issues || []).map(esc).join("<br>• "));
  }

  // 后端点名了哪个座位 → 描红。只做**呈现**：从原文里认 "[名字]" / "座位名重复：名字" /
  // "第 N 个座位"（未命名时 seats_yaml 就是这么称呼的）。
  function edMarkBad(joined) {
    const bad = {};
    edSeats.forEach((s, i) => {
      const nm = (s.name || "").trim();
      const hits = [];
      if (nm) hits.push("[" + nm + "]", "座位名重复：" + nm);
      hits.push("[第 " + (i + 1) + " 个座位]");
      hits.push("第 " + (i + 1) + " 个座位");
      if (hits.some((h) => joined.indexOf(h) >= 0)) bad[s.id] = true;
    });
    const changed = Object.keys(bad).join() !== Object.keys(edBad).join();
    edBad = bad;
    if (changed) { edRender(); edRenderList(); }
  }

  async function edCheckNow() {
    if (!edW || !edH || editWrap.style.display === "none") { edChkShow("", ""); return; }
    if (!edSeats.length) {
      edChkShow("hint", "还没有座位 —— 在画面里框出桌面, 这里会即时告诉你这一版能不能存。");
      return;
    }
    if (edChkBusy) { edChkDirty = true; return; }      // 上一个还在飞：记一笔, 它回来时补发
    edChkBusy = true;
    const seq = ++edChkSeq;
    try {
      const res = await fetch("/api/seats", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ seats: edPayload(), width: edW, height: edH, dry_run: true }),
      });
      const d = await res.json().catch(() => ({ ok: false, error: "响应不是 JSON" }));
      if (seq !== edChkSeq) return;                    // 期间又发了新的, 这份作废
      if (!res.ok || !d.ok) {
        const issues = (d.issues && d.issues.length) ? d.issues
          : [d.error || ("校验失败（HTTP " + res.status + "）")];
        edMarkBad(issues.join(" "));
        edMirrorLive(false, issues);
        return;
      }
      edMarkBad("");
      edMirrorLive(true, null, false, d.checked_note);
    } catch (err) {
      if (seq === edChkSeq) edChkShow("hint", "即时校验没跑起来（不影响手工点「校验」）：" + err);
    } finally {
      edChkBusy = false;
      if (edChkDirty) { edChkDirty = false; edCheckSoon(0); }
    }
  }

  // 防抖入口：拖动中每帧都会调, 所以"已经排好队就不再排"—— 两次校验至少隔 ED_CHK_MS
  function edCheckSoon(ms) {
    if (edChkT) return;
    edChkT = setTimeout(() => { edChkT = null; edCheckNow(); },
                        ms == null ? ED_CHK_MS : ms);
  }

  function edShowPreview(text) {
    if (!text) { editPreviewBox.style.display = "none"; return; }
    editPreview.textContent = text;
    editPreviewBox.style.display = "block";
  }

  // 屏幕 1px 等于多少用户单位：SVG 会被 CSS 缩放, 把手/线宽都得跟着折算
  function edUnit() {
    const r = editSvg.getBoundingClientRect();
    return (r.width > 0 && edW > 0) ? edW / r.width : 1;
  }

  function edPolyOf(s) {
    if (s.kind === "rect") {
      const [x, y, w, h] = s.rect;
      return [[x, y], [x + w, y], [x + w, y + h], [x, y + h]];
    }
    return s.polygon;
  }

  function edRectHandles(r) {
    const [x, y, w, h] = r, mx = x + w / 2, my = y + h / 2;
    return { nw: [x, y], n: [mx, y], ne: [x + w, y], e: [x + w, my],
             se: [x + w, y + h], s: [mx, y + h], sw: [x, y + h], w: [x, my] };
  }

  // 拖把手：把对应的边挪到 (px,py)，拖过头就自然翻转, 但宽高恒为正
  function edWithHandle(r, key, px, py) {
    const [x, y, w, h] = r;
    let l = x, t = y, rr = x + w, b = y + h;
    if (key.indexOf("n") >= 0) t = py;
    if (key.indexOf("s") >= 0) b = py;
    if (key.indexOf("w") >= 0) l = px;
    if (key.indexOf("e") >= 0) rr = px;
    const nx = edCX(Math.min(l, rr)), ny = edCY(Math.min(t, b));
    return [nx, ny, Math.max(1, Math.min(Math.abs(rr - l), edW - nx)),
            Math.max(1, Math.min(Math.abs(b - t), edH - ny))];
  }

  const SVGNS = SVG_NS;
  const edEl = (tag, cls) => {
    const el = document.createElementNS(SVGNS, tag);
    if (cls) el.setAttribute("class", cls);
    return el;
  };

  function edRender() {
    editSvg.setAttribute("viewBox", "0 0 " + edW + " " + edH);
    editSvg.innerHTML = "";
    const u = edUnit(), sw = Math.max(1, u * 2), hr = ED_HANDLE_PX * u / 2;
    const fs = 22 * u;

    edSeats.forEach((s) => {
      const pts = edPolyOf(s);
      const poly = edEl("polygon", "edit-poly" + (s.id === edSel ? " sel" : "")
                        + (edBad[s.id] ? " bad" : ""));
      poly.setAttribute("points", pts.map((p) => p[0] + "," + p[1]).join(" "));
      poly.setAttribute("stroke-width", sw);
      poly.dataset.id = s.id;
      editSvg.appendChild(poly);

      const xs = pts.map((p) => p[0]), ys = pts.map((p) => p[1]);
      const tx = edEl("text", "edit-label");
      tx.setAttribute("x", Math.min.apply(null, xs) + 10 * u);
      tx.setAttribute("y", Math.min.apply(null, ys) + 30 * u);
      tx.setAttribute("font-size", fs);
      tx.textContent = s.name || "(未命名)";
      editSvg.appendChild(tx);
    });

    const cur = edSeats.filter((s) => s.id === edSel)[0];
    if (cur && edTool === "select") {
      if (cur.kind === "rect") {
        const h8 = edRectHandles(cur.rect);
        Object.keys(h8).forEach((k) => {
          const el = edEl("rect", "edit-handle");
          el.setAttribute("x", h8[k][0] - hr);
          el.setAttribute("y", h8[k][1] - hr);
          el.setAttribute("width", hr * 2);
          el.setAttribute("height", hr * 2);
          el.dataset.id = cur.id;
          el.dataset.handle = k;
          editSvg.appendChild(el);
        });
      } else {
        cur.polygon.forEach((p, i) => {
          const el = edEl("circle", "edit-handle");
          el.setAttribute("cx", p[0]);
          el.setAttribute("cy", p[1]);
          el.setAttribute("r", hr);
          el.dataset.id = cur.id;
          el.dataset.handle = "v" + i;
          editSvg.appendChild(el);
        });
      }
    }
    // 正在画：矩形 = 虚线框；多边形 = 折线 + 顶点 + 连到光标的那一段
    if (edDraw) {
      if (edDraw.mode === "rect") {
        const [x0, y0, x1, y1] = edDraw.box;
        const el = edEl("rect", "edit-draft");
        el.setAttribute("x", Math.min(x0, x1));
        el.setAttribute("y", Math.min(y0, y1));
        el.setAttribute("width", Math.abs(x1 - x0));
        el.setAttribute("height", Math.abs(y1 - y0));
        el.setAttribute("stroke-width", sw);
        editSvg.appendChild(el);
      } else {
        const pts = edDraw.pts.slice();
        if (edDraw.hover) pts.push(edDraw.hover);
        if (pts.length >= 2) {
          const pl = edEl("polyline", "edit-draft");
          pl.setAttribute("points", pts.map((p) => p[0] + "," + p[1]).join(" "));
          pl.setAttribute("stroke-width", sw);
          editSvg.appendChild(pl);
        }
        edDraw.pts.forEach((p, i) => {
          const c = edEl("circle", i === 0 ? "edit-handle first" : "edit-handle");
          c.setAttribute("cx", p[0]);
          c.setAttribute("cy", p[1]);
          c.setAttribute("r", hr);
          editSvg.appendChild(c);
        });
      }
    }
    edTipUpdate();   // P2 本地提示：跟几何同步刷（纯本地算术, 不发请求）
  }

  function edRenderList() {
    if (!edSeats.length) {
      editSeatsBox.innerHTML = '<span class="muted" style="font-size:12px">还没有座位 —— '
        + "用上面的「▭ 画矩形 / ⬠ 画多边形」在画面里框出桌面，或点「用建议 zone」。</span>";
    } else {
      editSeatsBox.innerHTML = edSeats.map((s, i) => {
        const geo = s.kind === "rect"
          ? "rect " + s.rect.join(",")
          : "polygon " + edPolyOf(s).length + " 点";
        return '<span class="edit-seat' + (s.id === edSel ? " sel" : "")
          + (edBad[s.id] ? " bad" : "") + '" data-id="' + s.id + '">'
          + '<b>' + (i + 1) + "</b>"
          + '<input class="edit-name" data-id="' + s.id + '" maxlength="64" value="'
          + esc(s.name) + '" placeholder="座位名，如 A-12">'
          + '<span class="muted" style="font-size:11px">' + esc(geo) + "</span>"
          + '</span>';
      }).join("");
    }
    editMode.textContent = edTool === "select"
      ? "拖框内可整体移动；拖方形把手可改大小；多边形拖圆点改顶点。"
      : (edTool === "rect"
        ? "在画面里按住并拖动，画出一个矩形座位区。"
        : "在画面里逐点单击描出桌面轮廓，双击（或回到第一个点）闭合。");
  }

  function edSetTool(t) {
    edTool = t;
    edDraw = null;
    edDrag = null;
    [[editToolSelect, "select"], [editToolRect, "rect"], [editToolPoly, "polygon"]]
      .forEach((pair) => pair[0].classList.toggle("on", pair[1] === t));
    editSvg.style.cursor = (t === "select") ? "default" : "crosshair";
    edRender();
    edRenderList();
  }

  // ---- 指针 → 视频像素坐标（全流程只在这一处换算, 之后统一用这个坐标系）----
  function edPoint(ev) {
    const r = editSvg.getBoundingClientRect();
    if (!r.width || !r.height) return { x: 0, y: 0 };
    return { x: edCX((ev.clientX - r.left) * edW / r.width),
             y: edCY((ev.clientY - r.top) * edH / r.height) };
  }

  function edInPoly(p, pts) {                    // 射线法判断点在多边形内
    let inside = false;
    for (let i = 0, j = pts.length - 1; i < pts.length; j = i++) {
      const xi = pts[i][0], yi = pts[i][1], xj = pts[j][0], yj = pts[j][1];
      if (((yi > p.y) !== (yj > p.y))
          && (p.x < (xj - xi) * (p.y - yi) / (yj - yi) + xi)) inside = !inside;
    }
    return inside;
  }

  function edSelSeat() { return edSeats.filter((s) => s.id === edSel)[0]; }
  const edSeat = (id) => edSeats.filter((s) => s.id === id)[0];

  // 命中把手：必须自己算, 因为把手很小、还要跨缩放保持"跟手"的容差
  function edHitHandle(p) {
    const cur = edSelSeat();
    if (!cur || edTool !== "select") return null;
    const tol = ED_HANDLE_PX * 0.8 * edUnit();
    if (cur.kind === "rect") {
      const h8 = edRectHandles(cur.rect);
      for (const k in h8) {
        if (Math.abs(h8[k][0] - p.x) <= tol && Math.abs(h8[k][1] - p.y) <= tol)
          return { seat: cur, handle: k };
      }
      return null;
    }
    for (let i = 0; i < cur.polygon.length; ++i) {
      const q = cur.polygon[i];
      if (Math.abs(q[0] - p.x) <= tol && Math.abs(q[1] - p.y) <= tol)
        return { seat: cur, handle: "v" + i };
    }
    return null;
  }

  const edSnapshot = (s) => (s.kind === "rect" ? s.rect.slice()
                                                : s.polygon.map((q) => q.slice()));

  function edAutoName() {
    const used = edSeats.map((s) => s.name);
    for (let i = edSeats.length + 1; ; ++i) {
      if (used.indexOf("座位" + i) < 0) return "座位" + i;
    }
  }

  function edAddSeat(s) {
    s.name = edAutoName();
    edSeats.push(s);
    edSel = s.id;
    edDraw = null;
    edErr("");
    edSay("");
    edShowPreview("");
    edRender();
    edRenderList();
    edCheckSoon(0);      // P2 一画完就告诉你行不行
  }

  editSvg.addEventListener("pointerdown", (ev) => {
    if (!edW || !edH) return;
    const p = edPoint(ev);
    ev.preventDefault();

    if (edTool === "rect") {
      edDraw = { mode: "rect", box: [p.x, p.y, p.x, p.y] };
      try { editSvg.setPointerCapture(ev.pointerId); } catch (e) { /* 老浏览器 */ }
      edRender();
      return;
    }
    if (edTool === "polygon") {
      if (!edDraw) {
        edDraw = { mode: "polygon", pts: [p], hover: p };
      } else {
        const tol = 16 * edUnit(), first = edDraw.pts[0];
        if (edDraw.pts.length >= 3 && Math.abs(first[0] - p.x) <= tol
            && Math.abs(first[1] - p.y) <= tol) { edClosePoly(); return; }
        edDraw.pts.push(p);
      }
      edRender();
      return;
    }
    // select
    const h = edHitHandle(p);
    if (h) {
      edDrag = { seat: h.seat, handle: h.handle, from: p, base: edSnapshot(h.seat) };
      try { editSvg.setPointerCapture(ev.pointerId); } catch (e) { /* 老浏览器 */ }
      return;
    }
    const s = edSeats.slice().reverse().filter((x) => edInPoly(p, edPolyOf(x)))[0];
    edSel = s ? s.id : null;
    edDrag = s ? { seat: s, handle: "", from: p, base: edSnapshot(s) } : null;
    if (edDrag) { try { editSvg.setPointerCapture(ev.pointerId); } catch (e) { /* noop */ } }
    edRender();
    edRenderList();
  });

  editSvg.addEventListener("pointermove", (ev) => {
    if (!edW || !edH) return;
    const p = edPoint(ev);
    if (edDraw && edDraw.mode === "polygon") { edDraw.hover = p; edRender(); return; }
    if (edDraw && edDraw.mode === "rect") { edDraw.box[2] = p.x; edDraw.box[3] = p.y; edRender(); return; }
    if (!edDrag) return;
    const s = edDrag.seat, b = edDrag.base;
    if (!edDrag.handle) {                        // 整体平移（先夹到画面里, 不许拖出去）
      const dx = p.x - edDrag.from.x, dy = p.y - edDrag.from.y;
      if (s.kind === "rect") {
        s.rect = [Math.max(0, Math.min(edW - b[2], b[0] + dx)),
                  Math.max(0, Math.min(edH - b[3], b[1] + dy)), b[2], b[3]];
      } else {
        s.polygon = b.map((q) => [edCX(q[0] + dx), edCY(q[1] + dy)]);
      }
    } else if (s.kind === "rect") {
      s.rect = edWithHandle(b, edDrag.handle, p.x, p.y);
    } else {
      const i = Number(edDrag.handle.slice(1));
      s.polygon = b.map((q, k) => (k === i ? [p.x, p.y] : q));
    }
    edRender();
    edCheckSoon();     // P2 拖动中：最多每 ~300ms 问一次 C++（见 edCheckSoon 的节流）
  });

  editSvg.addEventListener("pointerup", () => {
    if (edDraw && edDraw.mode === "rect") {
      const b = edDraw.box;
      edDraw = null;
      if (Math.abs(b[2] - b[0]) >= 4 && Math.abs(b[3] - b[1]) >= 4) {
        edAddRect(b[0], b[1], b[2], b[3]);
      } else {
        edErr("这个框太小了（几乎是一个点）—— 按住拖出一个桌面大小的矩形");
        edRender();
      }
      return;
    }
    edDrag = null;
    edCheckSoon(60);   // P2 松手：把这一版的结论补齐（排队的会合并, 不会连发）
  });

  editSvg.addEventListener("pointercancel", () => { edDrag = null; edDraw = null; edRender(); });
  editSvg.addEventListener("dblclick", (ev) => {
    if (edDraw && edDraw.mode === "polygon") { ev.preventDefault(); edClosePoly(); }
  });

  function edClosePoly() {
    const pts = (edDraw && edDraw.pts ? edDraw.pts : []).slice();
    edDraw = null;
    if (pts.length < 3) {
      edErr("多边形至少要 3 个顶点 —— 沿着桌面的边逐点单击，最后双击（或点回第一个点）闭合");
      edRender();
      return;
    }
    edAddSeat({ id: edUid(), name: "", kind: "polygon", polygon: pts });
  }

  function edAddRect(x0, y0, x1, y1) {
    const x = edCX(Math.min(x0, x1)), y = edCY(Math.min(y0, y1));
    const w = Math.max(1, Math.min(edNum(Math.abs(x1 - x0)), edW - x));
    const h = Math.max(1, Math.min(edNum(Math.abs(y1 - y0)), edH - y));
    edAddSeat({ id: edUid(), name: "", kind: "rect", rect: [x, y, w, h] });
  }

  function edDelete() {
    if (!edSel) { edErr("先点一下要删的座位（画面里点它，或点下面的座位卡片）"); return; }
    edSeats = edSeats.filter((s) => s.id !== edSel);
    edSel = edSeats.length ? edSeats[edSeats.length - 1].id : null;
    edErr("");
    edSay("");
    edShowPreview("");
    edRender();
    edRenderList();
    edCheckSoon(0);      // P2 删除后结论会变（可能从"重叠"变成"通过"）
  }

  // 座位卡片：点卡片 = 选中；改输入框 = 改名（改名不影响几何, 只重画标签）
  editSeatsBox.addEventListener("click", (ev) => {
    if (ev.target.classList.contains("edit-name")) return;
    const box = ev.target.closest(".edit-seat");
    if (!box) return;
    edSel = box.dataset.id;
    edRender();
    edRenderList();
  });
  editSeatsBox.addEventListener("input", (ev) => {
    if (!ev.target.classList.contains("edit-name")) return;
    const s = edSeat(ev.target.dataset.id);
    if (s) {
      s.name = ev.target.value;
      edRender();            // 只重画图层, 别抢输入焦点
      edCheckSoon(400);      // P2 改名可能撞重名 —— 停手 400ms 后再问一次
    }
  });

  function edPayload() {
    return edSeats.map((s) => (s.kind === "rect"
      ? { name: (s.name || "").trim(), rect: s.rect.slice() }
      : { name: (s.name || "").trim(), polygon: s.polygon.map((q) => [q[0], q[1]]) }));
  }

  function edSetHeat(show) {
    if (!edHeatUrl) { editHeat.checked = false; return; }
    editImg.src = show ? edHeatUrl : edBaseFrame;
  }
  let edBaseFrame = "";      // 干净首帧（热力图可叠加显示, 但不参与坐标）
  let lastSuggest = null;    // 探针给的建议 zone（整数像素）

  // 校验 / 保存：都走同一个接口, 区别只在 dry_run。
  // 前端**不判**"重叠多少算多" —— 那由后端交给 C++ --check-config 用同一份规则判。
  async function edSend(dry) {
    edErr("");
    edSay("");
    edShowPreview("");
    if (!edSeats.length) {
      edErr("还没有画任何座位 —— 先用「▭ 画矩形」在画面里框出一个座位区");
      return;
    }
    editValidate.disabled = true;
    editSave.disabled = true;
    if (edChkT) { clearTimeout(edChkT); edChkT = null; }   // 别让排队的即时校验插进按钮的结果里
    edChkSeq += 1;                                        // 在飞的即时校验结果作废
    edSay(dry ? "校验中（交给 C++ 判）…" : "保存中（先校验、再写盘）…");
    try {
      const res = await fetch("/api/seats", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          seats: edPayload(), width: edW, height: edH, dry_run: !!dry,
        }),
      });
      const d = await res.json().catch(() => ({ ok: false, error: "响应不是 JSON" }));
      edShowPreview(d.preview || "");
      if (!res.ok || !d.ok) {
        const issues = (d.issues && d.issues.length) ? d.issues
          : [d.error || ("失败（HTTP " + res.status + "）")];
        edErr("配置没通过校验，原文件一个字都没改：\n• " + issues.join("\n• "));
        edMirrorLive(false, issues);      // P2 即时行跟着按钮结论走, 两处不打脸
        edSay("");
        return;
      }
      if (dry) {
        edSay(d.checked_note ? "⚠ 没找到 C++ 程序：这一版是跳过校验的（也没写入配置）"
                             : "✓ 校验通过（重叠/顶点由 C++ 判定），未写入配置");
        edMirrorLive(true, null, false, d.checked_note);
        return;
      }
      edSay("✓ 已写回配置：" + (d.config || "") + "（" + d.count + " 个座位）");
      edMirrorLive(true, null, true, d.checked_note);
      editHint.textContent = (d.hint || "") + (d.backup ? "　备份：" + d.backup : "");
      loadScene();          // 上方「画面示意」读的是同一份配置, 顺手刷新
    } catch (err) {
      edErr("请求失败：" + err);
      edSay("");
    } finally {
      editValidate.disabled = false;
      editSave.disabled = false;
    }
  }

  async function edLoadFromConfig(force) {
    try {
      const d = await (await fetch("/api/seats")).json();
      const seats = d.seats || [];
      if (!seats.length) {
        if (force) edErr(d.hint || "配置里没有座位 —— 直接在画面里画一个");
        return;
      }
      edSeats = seats.map((s) => ({
        id: edUid(),
        name: s.name || "",
        kind: s.kind === "rect" ? "rect" : "polygon",
        rect: (s.kind === "rect" && s.rect) ? s.rect.map(edNum) : undefined,
        polygon: (s.polygon || []).map((q) => [edNum(q[0]), edNum(q[1])]),
      }));
      edSel = edSeats.length ? edSeats[0].id : null;
      edErr("");
      edSay("");
      edShowPreview("");
      edRender();
      edRenderList();
      editHint.textContent = "已读入配置里的 " + edSeats.length + " 个座位（"
        + (d.config || "") + "）—— 拖把手把它对准桌面，再保存。";
      edCheckSoon(0);      // P2: 读进来的配置也可能本身就是坏的（越界/重叠），先报出来
    } catch (err) {
      edErr("读配置里的座位失败：" + err);
    }
  }

  // 底图占多宽：按**视频纵横比 + 视口高度**算 —— 竖屏视频别铺满整页（一张桌子
  // 要滚三屏才看得完），横屏也别小到看不清。改完尺寸要重画图层（把手尺寸跟着缩放）。
  function edFit() {
    const vh = window.innerHeight || 800;
    const box = Math.max(200, Math.min(560, 0.60 * vh * (edW / Math.max(1, edH))));
    editStage.style.maxWidth = Math.round(box) + "px";
    edRender();
  }
  window.addEventListener("resize", edFit);

  // 探针跑完 → 打开编辑器：底图 = 这段视频的**干净首帧**（热力图可随时叠加）
  function edOpen(frame, heatmap, w, h) {
    edW = edNum(w);
    edH = edNum(h);
    if (!frame || edW <= 0 || edH <= 0) {
      editWrap.style.display = "none";
      return false;
    }
    edBaseFrame = frame;
    edHeatUrl = heatmap || "";
    editImg.src = frame;
    editHeat.checked = false;
    editHeat.disabled = !edHeatUrl;
    editWrap.style.display = "block";
    edErr("");
    edSay("");
    edShowPreview("");
    edFit();                     // 先按纵横比定画布宽度, 再画图层(顺手把 viewBox 定成视频像素)
    edSetTool("select");
    // 这句纯文案放最后: 万一它出错, 不许把上面"定尺寸/定工具"那串正事带下水 ——
    // 实测踩过: 拼错手柄名 ⇒ 编辑器看着开了, 其实没 fit、没载入座位、也没触发即时校验。
    editHint.textContent = "底图 = 刚跑的这段视频的首帧（" + edW + "×" + edH +
      " 像素坐标，与配置里写的是同一套坐标）。保存时**只改 occupancy.seats** 这一段，" +
      "旁边的注释原样保留；写坏了还有 .bak 备份。";
    return true;
  }

  editToolSelect.addEventListener("click", () => edSetTool("select"));
  editToolRect.addEventListener("click", () => edSetTool("rect"));
  editToolPoly.addEventListener("click", () => edSetTool("polygon"));
  editDel.addEventListener("click", edDelete);
  editReload.addEventListener("click", () => edLoadFromConfig(true));
  editHeat.addEventListener("change", () => edSetHeat(editHeat.checked));
  editValidate.addEventListener("click", () => edSend(true));
  editSave.addEventListener("click", () => edSend(false));
  editUseSuggest.addEventListener("click", () => {
    if (!lastSuggest) {
      edErr("这次探针没给出建议 zone（或还没跑探针）—— 先在上面上传视频生成热力图");
      return;
    }
    const z = lastSuggest;
    edAddSeat({ id: edUid(), name: "", kind: "rect",
                rect: [edNum(z.x), edNum(z.y), edNum(z.width), edNum(z.height)] });
    edSay("已把建议 zone 加成第 " + edSeats.length + " 个座位（拖把手微调到贴住桌面）");
  });

  // 键盘：Delete/Backspace 删除选中（在改名输入框里打字不受影响）；Esc 放弃正在画的
  editStage.addEventListener("keydown", (ev) => {
    if (ev.target.tagName === "INPUT") return;
    if (ev.key === "Delete" || ev.key === "Backspace") {
      ev.preventDefault();
      edDelete();
    } else if (ev.key === "Escape" && edDraw) {
      edDraw = null;
      edRender();
    }
  });

  checkStatus();
  // 每 10s 顺带刷新一次事件：跑完视频后占座事件会自动冒出来，不用手动点“刷新”
  setInterval(() => { checkStatus(); loadEvents(); }, 10000);
})();
