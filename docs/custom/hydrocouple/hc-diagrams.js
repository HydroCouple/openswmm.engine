/* hc-diagrams.js: Mermaid diagrams for HydroCouple manuals (Sphinx and Doxygen).
 *
 * One renderer and one viewer for both generators:
 *   - every <pre class="mermaid"> / <div class="mermaid"> block is rendered with Mermaid's
 *     "base" theme, colored from the page's --hc-* tokens, so diagrams match the light or
 *     dark palette;
 *   - a color-mode change (PyData data-theme, doxygen-awesome dark-mode/light-mode class,
 *     or the system setting) re-renders every diagram in place, including an expanded one;
 *   - each diagram gets a toolbar (zoom out, zoom in, fit, expand), drag to pan,
 *     Ctrl/Command + wheel or pinch to zoom, double-click to fit, arrow/+/-/0 keys when
 *     focused; Expand opens the diagram in a full-window dialog; Escape closes it.
 * Mermaid loads only on pages that have a diagram. Without JavaScript the source text stays.
 *
 * Load as a classic script, <script src="hc-diagrams.js"></script>: a module script would
 * be blocked on pages opened from file://, and Mermaid itself comes in by dynamic import.
 * Before rendering, window.HCDiagrams.beforeRender(source, block) (if defined) may
 * rewrite a diagram's source, for example to add click targets.
 */
(() => {
"use strict";
const MERMAID_URL = "https://cdn.jsdelivr.net/npm/mermaid@11.12.1/dist/mermaid.esm.min.mjs";
const MIN_SCALE = 0.25;
const MAX_SCALE = 8;

const root = document.documentElement;
const token = (name) => getComputedStyle(root).getPropertyValue(`--hc-${name}`).trim();

function isDark() {
  if (root.dataset.theme) return root.dataset.theme === "dark";
  if (root.classList.contains("dark-mode")) return true;
  if (root.classList.contains("light-mode")) return false;
  return window.matchMedia("(prefers-color-scheme: dark)").matches;
}

function mermaidConfig() {
  const node = token("diagram-node"), edge = token("diagram-edge"), text = token("text");
  const surface = token("surface"), raised = token("raised"), soft = token("accent-soft");
  return {
    startOnLoad: false,
    securityLevel: "loose",
    theme: "base",
    fontFamily: token("font-body") || "sans-serif",
    themeVariables: {
      darkMode: isDark(),
      background: token("bg"),
      fontFamily: token("font-body") || "sans-serif",
      fontSize: "15px",
      primaryColor: node, primaryTextColor: text, primaryBorderColor: edge,
      secondaryColor: soft, secondaryTextColor: token("on-accent-soft"), secondaryBorderColor: edge,
      tertiaryColor: surface, tertiaryTextColor: text, tertiaryBorderColor: token("line-strong"),
      mainBkg: node, nodeBorder: edge, nodeTextColor: text, textColor: text, titleColor: text,
      lineColor: edge, defaultLinkColor: edge, edgeLabelBackground: surface,
      clusterBkg: raised, clusterBorder: token("line-strong"),
      noteBkgColor: token("flag-soft"), noteTextColor: token("on-flag-soft"), noteBorderColor: token("flag"),
      actorBkg: node, actorBorder: edge, actorTextColor: text, actorLineColor: edge,
      signalColor: text, signalTextColor: text, labelBoxBkgColor: raised, labelBoxBorderColor: edge,
      labelTextColor: text, loopTextColor: text, activationBkgColor: soft, activationBorderColor: edge,
      sequenceNumberColor: token("on-primary"),
      labelColor: text, altBackground: raised, compositeBackground: raised, compositeTitleBackground: soft,
      stateLabelColor: text, stateBkg: node, specialStateColor: edge, innerEndBackground: edge,
      attributeBackgroundColorOdd: surface, attributeBackgroundColorEven: raised,
    },
  };
}

let mermaid = null;
let renderCount = 0;
const figures = [];

function el(tag, className, text) {
  const e = document.createElement(tag);
  if (className) e.className = className;
  if (text) e.textContent = text;
  return e;
}

class Figure {
  constructor(block) {
    this.block = block;
    this.source = block.dataset.hcSource || block.textContent.trim();
    if (window.HCDiagrams && typeof window.HCDiagrams.beforeRender === "function") {
      this.source = window.HCDiagrams.beforeRender(this.source, block) || this.source;
    }
    this.scale = 1; this.tx = 0; this.ty = 0; this.natural = null;

    this.wrap = el("div", "hc-diagram");
    this.toolbar = el("div", "hc-diagram__toolbar");
    this.toolbar.setAttribute("role", "toolbar");
    this.toolbar.setAttribute("aria-label", "Diagram controls");
    this.viewport = el("div", "hc-diagram__viewport");
    this.viewport.tabIndex = 0;
    this.viewport.setAttribute("role", "img");
    this.viewport.setAttribute("aria-roledescription", "diagram");
    this.viewport.setAttribute("aria-label", "Diagram. Drag or use the arrow keys to pan, Ctrl or Command and scroll to zoom, double-click or press 0 to fit.");
    this.canvas = el("div", "hc-diagram__canvas");
    this.viewport.append(this.canvas);
    this.wrap.append(this.toolbar, this.viewport);

    const button = (label, title, action) => {
      const b = el("button", "hc-diagram__button", label);
      b.type = "button"; b.title = title; b.setAttribute("aria-label", title);
      b.addEventListener("click", action);
      this.toolbar.append(b);
      return b;
    };
    button("−", "Zoom out", () => this.zoomCenter(1 / 1.25));
    button("+", "Zoom in", () => this.zoomCenter(1.25));
    button("Fit", "Fit to view", () => this.fit());
    this.expandButton = button("Expand", "Expand diagram", () => this.expand());
    this.expandButton.setAttribute("aria-haspopup", "dialog");

    block.dataset.hcSource = this.source;
    block.hidden = true;
    block.after(this.wrap);
    this.bindPointer();
    // A diagram inside a hidden tab has no width until shown; fit it when it gets one.
    let lastWidth = 0;
    new ResizeObserver(() => {
      const w = this.viewport.clientWidth;
      if (!this.dialog && this.natural && w && w !== lastWidth) { lastWidth = w; this.fit(); }
    }).observe(this.wrap);
  }

  async render() {
    const id = `hc-mermaid-${++renderCount}`;
    try {
      const { svg, bindFunctions } = await mermaid.render(id, this.source);
      this.canvas.innerHTML = svg;
      const svgEl = this.canvas.querySelector("svg");
      const box = svgEl.viewBox.baseVal;
      this.natural = { width: box.width || svgEl.getBBox().width, height: box.height || svgEl.getBBox().height };
      svgEl.removeAttribute("style");
      svgEl.setAttribute("width", this.natural.width);
      svgEl.setAttribute("height", this.natural.height);
      if (bindFunctions) bindFunctions(this.canvas);
      this.wrap.classList.remove("hc-diagram--error");
      this.fit();
    } catch (error) {
      this.wrap.classList.add("hc-diagram--error");
      this.canvas.replaceChildren(el("pre", "hc-diagram__source", this.source));
      console.error("Mermaid could not render a diagram:", error);
    } finally {
      document.getElementById("d" + id)?.remove();
    }
  }

  apply() {
    this.canvas.style.transform = `translate(${this.tx}px, ${this.ty}px) scale(${this.scale})`;
  }

  fit() {
    const vw = this.viewport.clientWidth;
    if (!this.natural || !vw) return;
    if (this.dialog) {
      const vh = this.viewport.clientHeight;
      this.scale = Math.min(vw / this.natural.width, vh / this.natural.height) * 0.96;
      this.viewport.style.height = "";
    } else {
      this.scale = Math.min(1, vw / this.natural.width);
      this.viewport.style.height = `${Math.ceil(this.natural.height * this.scale) + 2}px`;
    }
    this.tx = (vw - this.natural.width * this.scale) / 2;
    this.ty = this.dialog ? (this.viewport.clientHeight - this.natural.height * this.scale) / 2 : 0;
    this.apply();
  }

  zoomAt(factor, cx, cy) {
    const next = Math.min(MAX_SCALE, Math.max(MIN_SCALE, this.scale * factor));
    const k = next / this.scale;
    this.tx = cx - (cx - this.tx) * k;
    this.ty = cy - (cy - this.ty) * k;
    this.scale = next;
    this.apply();
  }

  zoomCenter(factor) {
    this.zoomAt(factor, this.viewport.clientWidth / 2, this.viewport.clientHeight / 2);
  }

  bindPointer() {
    const vp = this.viewport;
    let drag = null, moved = false;
    const pointers = new Map();
    let pinch = null;
    vp.addEventListener("pointerdown", (e) => {
      if (e.button !== 0) return;
      pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
      if (pointers.size === 2) {
        const [a, b] = [...pointers.values()];
        pinch = { d: Math.hypot(a.x - b.x, a.y - b.y), s: this.scale };
      } else {
        drag = { x: e.clientX - this.tx, y: e.clientY - this.ty, sx: e.clientX, sy: e.clientY };
        moved = false;
      }
      vp.setPointerCapture(e.pointerId);
    });
    vp.addEventListener("pointermove", (e) => {
      if (!pointers.has(e.pointerId)) return;
      pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
      if (pinch && pointers.size === 2) {
        const [a, b] = [...pointers.values()];
        const r = vp.getBoundingClientRect();
        this.zoomAt((pinch.s * Math.hypot(a.x - b.x, a.y - b.y) / pinch.d) / this.scale,
                    (a.x + b.x) / 2 - r.left, (a.y + b.y) / 2 - r.top);
      } else if (drag) {
        if (Math.abs(e.clientX - drag.sx) + Math.abs(e.clientY - drag.sy) > 4) moved = true;
        if (moved) {
          vp.classList.add("is-dragging");
          this.tx = e.clientX - drag.x; this.ty = e.clientY - drag.y; this.apply();
        }
      }
    });
    const end = (e) => {
      pointers.delete(e.pointerId);
      if (pointers.size < 2) pinch = null;
      if (!pointers.size) { drag = null; vp.classList.remove("is-dragging"); }
    };
    vp.addEventListener("pointerup", end);
    vp.addEventListener("pointercancel", end);
    // A click that ends a drag must not follow a link inside the diagram.
    vp.addEventListener("click", (e) => { if (moved) { e.preventDefault(); e.stopPropagation(); moved = false; } }, true);
    vp.addEventListener("dblclick", (e) => { e.preventDefault(); this.fit(); });
    vp.addEventListener("wheel", (e) => {
      if (!(e.ctrlKey || e.metaKey || this.dialog)) return;
      e.preventDefault();
      const r = vp.getBoundingClientRect();
      this.zoomAt(Math.exp(-e.deltaY * 0.0018), e.clientX - r.left, e.clientY - r.top);
    }, { passive: false });
    vp.addEventListener("keydown", (e) => {
      const step = 40;
      const actions = {
        ArrowLeft: () => { this.tx += step; }, ArrowRight: () => { this.tx -= step; },
        ArrowUp: () => { this.ty += step; }, ArrowDown: () => { this.ty -= step; },
        "+": () => this.zoomCenter(1.25), "=": () => this.zoomCenter(1.25),
        "-": () => this.zoomCenter(0.8), "0": () => this.fit(), Home: () => this.fit(),
      };
      if (!actions[e.key]) return;
      e.preventDefault();
      actions[e.key]();
      this.apply();
    });
  }

  expand() {
    const dialog = el("dialog", "hc-diagram-dialog");
    dialog.setAttribute("aria-label", "Expanded diagram");
    const close = el("button", "hc-diagram-dialog__close", "Close");
    close.type = "button";
    close.addEventListener("click", () => dialog.close());
    const bar = el("div", "hc-diagram-dialog__bar");
    bar.append(this.toolbar, close);
    this.placeholder = el("div", "hc-diagram__placeholder");
    this.placeholder.style.height = `${this.viewport.offsetHeight}px`;
    this.wrap.after(this.placeholder);
    dialog.append(bar, this.viewport);
    document.body.append(dialog);
    this.dialog = dialog;
    this.expandButton.hidden = true;
    dialog.addEventListener("close", () => this.collapse(true));
    // Sphinx's sphinx_highlight.js cancels Escape's default action (it clears search
    // highlights) unless a button or field has focus, so close on Escape explicitly.
    dialog.addEventListener("keydown", (e) => {
      if (e.key === "Escape") { e.preventDefault(); e.stopPropagation(); dialog.close(); }
    });
    dialog.showModal();
    requestAnimationFrame(() => { this.fit(); this.viewport.focus({ preventScroll: true }); });
  }

  collapse(fromClose = false) {
    if (!this.dialog) return;
    const dialog = this.dialog;
    this.dialog = null;
    this.wrap.replaceChildren(this.toolbar, this.viewport);
    this.placeholder.remove();
    this.expandButton.hidden = false;
    if (!fromClose) dialog.close();
    dialog.remove();
    this.fit();
    this.expandButton.focus({ preventScroll: true });
  }
}

async function renderAll() {
  mermaid.initialize(mermaidConfig());
  for (const figure of figures) await figure.render();
}

async function start() {
  const blocks = [...document.querySelectorAll("pre.mermaid, div.mermaid")].filter((b) => !b.dataset.hcSource);
  if (!blocks.length) return;
  ({ default: mermaid } = await import(MERMAID_URL));
  blocks.forEach((block) => figures.push(new Figure(block)));
  await renderAll();

  let pending = null;
  const rerender = () => {
    clearTimeout(pending);
    pending = setTimeout(renderAll, 60);
  };
  let dark = isDark();
  const onModeChange = () => { if (isDark() !== dark) { dark = isDark(); rerender(); } };
  new MutationObserver(onModeChange).observe(root, { attributes: true, attributeFilter: ["data-theme", "class"] });
  window.matchMedia("(prefers-color-scheme: dark)").addEventListener("change", onModeChange);
}

if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", start);
else start();
})();
