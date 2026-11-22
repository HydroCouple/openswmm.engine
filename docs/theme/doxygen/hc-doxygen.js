/* hc-doxygen.js: HydroCouple documentation theme behavior for Doxygen manuals.
 *
 *  - Manuals menu in the top bar: opens on click, closes on Escape, outside click or
 *    focus leaving it; arrow keys move between entries.
 *  - Color-mode button: drives doxygen-awesome's own toggle (DoxygenAwesomeDarkModeToggle),
 *    so the choice is stored where doxygen-awesome expects it; that button is hidden.
 *  - Landing hero on the main page (index.html), from an optional
 *    <script type="application/json" id="hc-hero">{"kicker", "title", "lede",
 *    "actions": [{"label", "url", "style"}]}</script> in the header. Without it, the
 *    title and brief come from the page. Relative action URLs resolve against the page.
 *  - Table of contents on every page: Doxygen 1.13 and earlier show one only when the page
 *    asks for it ([TOC]), so one is built from the page's headings, at every level, in the
 *    markup Doxygen uses. Doxygen 1.14+ draws its own outline panel (#page-nav) instead.
 */
(() => {
  "use strict";

  function initManuals() {
    const menu = document.querySelector(".hc-manuals");
    if (!menu) return;
    const button = menu.querySelector(".hc-manuals__button");
    const panel = menu.querySelector(".hc-manuals__menu");
    const items = () => [...panel.querySelectorAll("a")];
    const setOpen = (open) => {
      panel.hidden = !open;
      button.setAttribute("aria-expanded", String(open));
    };
    button.addEventListener("click", () => {
      const open = panel.hidden;
      setOpen(open);
      if (open) (panel.querySelector("a.active") || items()[0])?.focus();
    });
    document.addEventListener("click", (e) => { if (!menu.contains(e.target)) setOpen(false); });
    menu.addEventListener("focusout", (e) => { if (!menu.contains(e.relatedTarget)) setOpen(false); });
    menu.addEventListener("keydown", (e) => {
      if (e.key === "Escape" && !panel.hidden) { setOpen(false); button.focus(); e.preventDefault(); return; }
      if (panel.hidden || !["ArrowDown", "ArrowUp"].includes(e.key)) return;
      const list = items();
      const i = list.indexOf(document.activeElement);
      list[(i + (e.key === "ArrowDown" ? 1 : list.length - 1)) % list.length].focus();
      e.preventDefault();
    });
  }

  function initModeToggle() {
    const button = document.querySelector(".hc-mode-toggle");
    if (!button) return;
    const update = () => {
      const dark = document.documentElement.classList.contains("dark-mode");
      button.setAttribute("aria-pressed", String(dark));
      button.title = dark ? "Switch to light mode" : "Switch to dark mode";
    };
    button.addEventListener("click", () => {
      const T = window.DoxygenAwesomeDarkModeToggle;
      if (T) {
        T.userPreference = !T.userPreference;
      } else {
        const dark = !document.documentElement.classList.contains("dark-mode");
        document.documentElement.classList.toggle("dark-mode", dark);
        document.documentElement.classList.toggle("light-mode", !dark);
      }
      update();
    });
    new MutationObserver(update).observe(document.documentElement, { attributes: true, attributeFilter: ["class"] });
    update();
  }

  function initHero() {
    const page = location.pathname.split("/").pop() || "index.html";
    if (page !== "index.html") return;
    const contents = document.querySelector("div.contents");
    if (!contents) return;
    let data = {};
    const json = document.getElementById("hc-hero");
    if (json) {
      try { data = JSON.parse(json.textContent); } catch (e) { console.error("hc-hero: invalid JSON", e); }
    }
    const title = data.title || document.querySelector("div.header .title")?.textContent.trim();
    if (!title) return;
    const make = (tag, cls, text) => {
      const e = document.createElement(tag);
      e.className = cls;
      if (text) e.textContent = text;
      return e;
    };
    const hero = make("section", "hc-hero");
    hero.setAttribute("aria-label", title);
    const inner = make("div", "hc-hero__inner");
    if (data.kicker) inner.append(make("p", "hc-hero__kicker", data.kicker));
    inner.append(make("p", "hc-hero__title", title));
    const lede = data.lede || document.querySelector('meta[name="description"]')?.content;
    if (lede) inner.append(make("p", "hc-hero__lede", lede));
    if (Array.isArray(data.actions) && data.actions.length) {
      const actions = make("div", "hc-hero__actions");
      for (const action of data.actions) {
        const a = make("a", `hc-btn hc-btn--${action.style || "ghost"}`, action.label);
        a.href = action.url;
        actions.append(a);
      }
      inner.append(actions);
    }
    hero.append(inner);
    contents.before(hero);
    document.body.classList.add("hc-landing");
  }

  function initToc() {
    const contents = document.querySelector("div.contents");
    if (!contents || contents.querySelector(":scope > div.toc") || document.getElementById("page-nav")) return;
    if (document.querySelector("h2.groupheader")) return; // class, file and group reference pages
    const heads = [...contents.querySelectorAll("div.textblock :is(h1, h2, h3, h4, h5, h6)")]
      .filter((h) => h.querySelector("a.anchor[id]"));
    if (heads.length < 2) return;
    const toc = document.createElement("div");
    toc.className = "toc";
    const title = document.createElement("h3");
    title.textContent = "Table of Contents";
    const root = document.createElement("ul");
    toc.append(title, root);
    const stack = [{ level: 0, list: root }];
    for (const h of heads) {
      const level = Number(h.tagName[1]);
      while (stack.at(-1).level >= level) stack.pop();
      const parent = stack.at(-1);
      if (!parent.list) parent.li.append((parent.list = document.createElement("ul")));
      const li = document.createElement("li");
      li.className = `level${level}`;
      const a = document.createElement("a");
      a.href = `#${h.querySelector("a.anchor[id]").id}`;
      a.textContent = h.textContent.trim();
      li.append(a);
      parent.list.append(li);
      stack.push({ level, li, list: null });
    }
    contents.prepend(toc);
  }

  const start = () => { initManuals(); initModeToggle(); initHero(); initToc(); };
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", start);
  else start();
})();
