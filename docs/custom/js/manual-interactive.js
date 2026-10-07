/* manual-interactive.js — pan, zoom and click for the manuals' workflow and
 * process diagrams.
 *
 * Two kinds of diagram are made interactive:
 *
 *   1. Mermaid workflows: `<pre class="mermaid">` blocks. When a block is
 *      followed by `<div class="workflow-links" data-workflow="id">` whose
 *      `<span data-node="A">` children hold links (written as @ref in the
 *      markdown and resolved by Doxygen), a Mermaid `click` directive is
 *      injected per node before rendering, so the node opens that page.
 *
 *   2. Generated figures with hotspots: an `<img>` whose page carries
 *      `<div class="fig-hotspots" data-fig="<fig_id>">` with
 *      `<span class="hs" data-box="x0,y0,x1,y1">` children (fractions of the
 *      image, top-left origin) holding a resolved link each. The spans are
 *      written by scripts/build_manual_figures.py from the generator's
 *      sink.hotspot() calls, so the boxes match the drawing exactly.
 *
 * Both get the same viewport: drag to pan, wheel to zoom once the diagram is
 * activated by a click (or with Ctrl/⌘ held, so a page scroll is never
 * hijacked), pinch on touch, double-click to reset, and a small toolbar.
 * Everything degrades to the static image or diagram without JavaScript.
 *
 * Rendering is driven from docs/custom/html/header.html, which imports
 * Mermaid and calls ManualInteractive.init(mermaid).
 */
(function () {
  "use strict";
  var MIN_SCALE = 0.4, MAX_SCALE = 10;

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text) e.textContent = text;
    return e;
  }

  function panZoom(wrap, viewport, canvas) {
    var s = 1, tx = 0, ty = 0, active = false, suppressClick = false, drag = null;
    var pointers = new Map(), pinch = null;

    function apply() { canvas.style.transform = "translate(" + tx + "px," + ty + "px) scale(" + s + ")"; }
    function zoomAt(factor, cx, cy) {
      var ns = Math.min(MAX_SCALE, Math.max(MIN_SCALE, s * factor));
      factor = ns / s;
      tx = cx - factor * (cx - tx);
      ty = cy - factor * (cy - ty);
      s = ns;
      apply();
    }
    function reset() { s = 1; tx = 0; ty = 0; apply(); }
    function centre() { return [viewport.clientWidth / 2, viewport.clientHeight / 2]; }
    function setActive(on) {
      active = on;
      viewport.classList.toggle("is-active", on);
    }

    viewport.addEventListener("wheel", function (e) {
      if (!active && !(e.ctrlKey || e.metaKey)) return;
      e.preventDefault();
      var r = viewport.getBoundingClientRect();
      zoomAt(Math.exp(-e.deltaY * 0.0015), e.clientX - r.left, e.clientY - r.top);
    }, { passive: false });

    viewport.addEventListener("pointerdown", function (e) {
      if (e.button !== 0) return;
      pointers.set(e.pointerId, [e.clientX, e.clientY]);
      viewport.setPointerCapture(e.pointerId);
      if (pointers.size === 1) {
        drag = { x: e.clientX, y: e.clientY, tx: tx, ty: ty, moved: false };
      } else if (pointers.size === 2) {
        var p = Array.from(pointers.values());
        pinch = { d: Math.hypot(p[0][0] - p[1][0], p[0][1] - p[1][1]), s: s };
        drag = null;
      }
    });
    viewport.addEventListener("pointermove", function (e) {
      if (!pointers.has(e.pointerId)) return;
      pointers.set(e.pointerId, [e.clientX, e.clientY]);
      if (pinch && pointers.size === 2) {
        var p = Array.from(pointers.values());
        var d = Math.hypot(p[0][0] - p[1][0], p[0][1] - p[1][1]);
        var r = viewport.getBoundingClientRect();
        var cx = (p[0][0] + p[1][0]) / 2 - r.left, cy = (p[0][1] + p[1][1]) / 2 - r.top;
        zoomAt((pinch.s * d / pinch.d) / s, cx, cy);
        e.preventDefault();
      } else if (drag) {
        var dx = e.clientX - drag.x, dy = e.clientY - drag.y;
        if (Math.abs(dx) + Math.abs(dy) > 3) {
          drag.moved = true;
          viewport.classList.add("is-dragging");
        }
        if (drag.moved) {
          tx = drag.tx + dx; ty = drag.ty + dy;
          apply();
          e.preventDefault();
        }
      }
    });
    function endPointer(e) {
      pointers.delete(e.pointerId);
      if (pointers.size < 2) pinch = null;
      if (drag && drag.moved) suppressClick = true;
      drag = null;
      viewport.classList.remove("is-dragging");
    }
    viewport.addEventListener("pointerup", endPointer);
    viewport.addEventListener("pointercancel", endPointer);

    // a click that ends a drag must not follow a hotspot link; a plain click activates zooming
    viewport.addEventListener("click", function (e) {
      if (suppressClick) { suppressClick = false; e.preventDefault(); e.stopPropagation(); return; }
      setActive(true);
    }, true);
    viewport.addEventListener("dblclick", function (e) { e.preventDefault(); reset(); });
    document.addEventListener("click", function (e) { if (!wrap.contains(e.target)) setActive(false); });
    document.addEventListener("keydown", function (e) { if (e.key === "Escape") setActive(false); });

    // the viewport keeps the diagram's layout height whatever the transform
    function fit() { viewport.style.height = canvas.offsetHeight + "px"; }
    if (window.ResizeObserver) new ResizeObserver(fit).observe(canvas); else window.addEventListener("resize", fit);
    fit();

    return {
      zoomIn: function () { var c = centre(); zoomAt(1.25, c[0], c[1]); },
      zoomOut: function () { var c = centre(); zoomAt(0.8, c[0], c[1]); },
      reset: reset,
      fit: fit
    };
  }

  function toolbar(wrap, pz, extra) {
    var bar = el("div", "mi-toolbar");
    function button(label, title, fn) {
      var b = el("button", "mi-btn", label);
      b.type = "button";
      b.title = title;
      b.addEventListener("click", function (e) { e.stopPropagation(); fn(); });
      bar.appendChild(b);
    }
    button("+", "Zoom in", pz.zoomIn);
    button("−", "Zoom out", pz.zoomOut);
    button("⟲", "Reset the view (or double-click the diagram)", pz.reset);
    if (extra) bar.appendChild(extra);
    wrap.appendChild(bar);
  }

  function wrapElement(target, hint) {
    var wrap = el("div", "mi-wrap");
    var viewport = el("div", "mi-viewport");
    var canvas = el("div", "mi-canvas");
    target.parentNode.insertBefore(wrap, target);
    canvas.appendChild(target);
    viewport.appendChild(canvas);
    wrap.appendChild(viewport);
    var pz = panZoom(wrap, viewport, canvas);
    var hintEl = el("div", "mi-hint", hint || "drag to pan · click the diagram, then scroll to zoom (or hold Ctrl/⌘) · double-click resets");
    wrap.appendChild(hintEl);
    return { wrap: wrap, viewport: viewport, canvas: canvas, pz: pz };
  }

  // ── figures with hotspots ────────────────────────────────────────────────
  function findImage(figId) {
    var imgs = document.querySelectorAll("img");
    for (var i = 0; i < imgs.length; i++) {
      var src = imgs[i].getAttribute("src") || "";
      if (src.split("/").pop() === figId + ".png") return imgs[i];
    }
    return null;
  }

  function initFigures() {
    var blocks = document.querySelectorAll("div.fig-hotspots[data-fig]");
    for (var b = 0; b < blocks.length; b++) {
      var block = blocks[b], figId = block.getAttribute("data-fig");
      var img = findImage(figId);
      if (!img || img.closest(".mi-wrap")) continue;
      var open = el("a", "mi-btn", "⤢");
      open.href = img.getAttribute("src");
      open.target = "_blank";
      open.rel = "noopener";
      open.title = "Open the image in a new tab";
      var w = wrapElement(img, "drag to pan · click the figure, then scroll to zoom (or hold Ctrl/⌘) · double-click resets · click a box to open its chapter");
      var overlay = el("div", "mi-overlay");
      var spans = block.querySelectorAll("span.hs[data-box]");
      for (var i = 0; i < spans.length; i++) {
        var a = spans[i].querySelector("a[href]");
        if (!a) continue;
        var box = spans[i].getAttribute("data-box").split(",").map(Number);
        if (box.length !== 4 || box.some(isNaN)) continue;
        var hs = el("a", "mi-hs");
        hs.href = a.getAttribute("href");
        hs.title = a.textContent + (a.title && a.title !== a.textContent ? " — " + a.title : "");
        hs.setAttribute("aria-label", hs.title);
        hs.style.left = (box[0] * 100) + "%";
        hs.style.top = (box[1] * 100) + "%";
        hs.style.width = ((box[2] - box[0]) * 100) + "%";
        hs.style.height = ((box[3] - box[1]) * 100) + "%";
        overlay.appendChild(hs);
      }
      w.canvas.appendChild(overlay);
      toolbar(w.wrap, w.pz, open);
      if (!img.complete) img.addEventListener("load", w.pz.fit);
    }
  }

  // ── Mermaid workflows ────────────────────────────────────────────────────
  function previousMermaid(node) {
    var p = node.previousSibling;
    while (p) {
      if (p.nodeType === 1) {
        if (p.matches("pre.mermaid")) return p;
        var inner = p.querySelector && p.querySelector("pre.mermaid");
        if (inner) return inner;
        return null;
      }
      if (p.nodeType === 3 && p.textContent.trim() !== "") return null;
      p = p.previousSibling;
    }
    return null;
  }

  function injectClicks() {
    var links = document.querySelectorAll("div.workflow-links[data-workflow]");
    for (var i = 0; i < links.length; i++) {
      var pre = previousMermaid(links[i]);
      if (!pre || pre.getAttribute("data-processed")) continue;
      var spans = links[i].querySelectorAll("span[data-node]");
      var extra = "";
      for (var j = 0; j < spans.length; j++) {
        var a = spans[j].querySelector("a[href]");
        if (!a) continue;
        var node = spans[j].getAttribute("data-node");
        if (!/^[A-Za-z0-9_]+$/.test(node)) continue;
        var tip = (a.textContent || node).replace(/"/g, "'");
        extra += "\nclick " + node + " \"" + a.getAttribute("href").replace(/"/g, "") + "\" \"" + tip + "\"";
      }
      if (extra) pre.textContent = pre.textContent.replace(/\s+$/, "") + extra + "\n";
    }
  }

  function wrapMermaids() {
    var pres = document.querySelectorAll("pre.mermaid");
    for (var i = 0; i < pres.length; i++) {
      if (pres[i].closest(".mi-wrap")) continue;
      var w = wrapElement(pres[i]);
      toolbar(w.wrap, w.pz, null);
    }
  }

  window.ManualInteractive = {
    init: function (mermaid) {
      try { initFigures(); } catch (e) { if (window.console) console.warn("manual-interactive: figures", e); }
      if (!mermaid) return;
      try { injectClicks(); } catch (e) { if (window.console) console.warn("manual-interactive: click injection", e); }
      var run = mermaid.run ? mermaid.run({ querySelector: "pre.mermaid" }) : Promise.resolve(mermaid.init(undefined, "pre.mermaid"));
      Promise.resolve(run).then(wrapMermaids, function (e) {
        if (window.console) console.warn("manual-interactive: mermaid", e);
        wrapMermaids();
      });
    }
  };
})();
