/**
 * /www/js/editable-deck.js — editable card deck (vanilla, no React)
 *
 * Port of the design system's editable.jsx + MasonryDeck. Provides:
 *   • drag-reorder via Pointer Events (works on touch + mouse + pen)
 *   • per-card span chips (3 / 4 / 6 / 8 / 12 of 12)
 *   • hide-to-tray + restore + add-from-library
 *   • live-mode masonry packing (no row-height gaps)
 *   • localStorage persistence per page
 *
 * Used by Overview, Sensors and Alerts pages. Each page calls
 *   EditableDeck.mount({ pageId, container, registry, defaults });
 * and supplies a `registry` mapping id → { title, icon, render(card) }.
 *
 * No external deps. CSS lives in style.css under "EDITABLE DECK".
 */

"use strict";

(function () {

  var STORAGE_PREFIX = "esp32logger.layout.";
  var ALLOWED_SPANS  = [3, 4, 6, 8, 12];
  var COLUMNS        = 12;
  var GAP            = 14;
  var THUMB_HEIGHT   = 96;

  // ── Helpers ────────────────────────────────────────────────────────────────
  function reIcons(el) {
    if (window.Icons && Icons.swap) Icons.swap(el || document.body);
  }

  function loadCards(pageId, defaults) {
    try {
      var raw = localStorage.getItem(STORAGE_PREFIX + pageId);
      if (raw) {
        var parsed = JSON.parse(raw);
        var known = {};
        defaults.forEach(function (d) { known[d.id] = true; });
        var filtered = parsed.filter(function (c) { return known[c.id]; });
        var have = {};
        filtered.forEach(function (c) { have[c.id] = true; });
        var add = defaults.filter(function (d) { return !have[d.id]; });
        return filtered.concat(add.map(function (d) { return { id:d.id, span:d.span }; }));
      }
    } catch (e) {}
    return defaults.map(function (d) { return { id:d.id, span:d.span }; });
  }

  function saveCards(pageId, cards) {
    try { localStorage.setItem(STORAGE_PREFIX + pageId, JSON.stringify(cards)); }
    catch (e) {}
  }

  // ── Masonry packer ─────────────────────────────────────────────────────────
  // Places each card at the leftmost columns whose top cursors share the
  // lowest position. The columns it spans then advance to that card's bottom.
  // Result: a tall card never leaves a row-height gap below short neighbours.
  function packMasonry(container) {
    var slots = Array.prototype.slice.call(container.children).filter(function (n) {
      return n.classList && n.classList.contains("deck-slot");
    });
    if (!slots.length) {
      container.style.height = "0px";
      return;
    }
    var width = container.clientWidth;
    if (width <= 0) return;
    var colW = (width - GAP * (COLUMNS - 1)) / COLUMNS;
    var tops = new Array(COLUMNS).fill(0);

    // First pass: set widths so heights are measured at the correct width.
    slots.forEach(function (slot) {
      var span = Math.max(1, Math.min(COLUMNS, +slot.dataset.span || 4));
      slot.style.width = (span * colW + (span - 1) * GAP) + "px";
    });
    // Force layout flush before measuring.
    // eslint-disable-next-line no-unused-expressions
    container.offsetHeight;

    slots.forEach(function (slot) {
      var span = Math.max(1, Math.min(COLUMNS, +slot.dataset.span || 4));
      var bestCol = 0, bestTop = Infinity;
      for (var c = 0; c <= COLUMNS - span; c++) {
        var t = 0;
        for (var k = c; k < c + span; k++) if (tops[k] > t) t = tops[k];
        if (t < bestTop) { bestTop = t; bestCol = c; }
      }
      slot.style.left = (bestCol * (colW + GAP)) + "px";
      slot.style.top  = bestTop + "px";
      var newTop = bestTop + slot.offsetHeight + GAP;
      for (var i = bestCol; i < bestCol + span; i++) tops[i] = newTop;
    });

    var totalH = 0;
    for (var j = 0; j < tops.length; j++) if (tops[j] > totalH) totalH = tops[j];
    container.style.height = Math.max(0, totalH - GAP) + "px";
  }

  // Pointer-events drag-reorder. Lifts the dragged slot (CSS class) and walks
  // the deck to find the slot whose midpoint the pointer is currently over.
  function attachPointerDrag(slot, grip, ondrop) {
    var dragging = false;
    var startX = 0, startY = 0;
    var pointerId = null;
    var ghost = null;
    var lastTargetId = null;
    var rectCache = null;

    function buildRectCache() {
      var deck = slot.parentNode;
      var children = Array.prototype.slice.call(deck.children).filter(function (n) {
        return n.classList && n.classList.contains("deck-slot");
      });
      rectCache = children.map(function (s) {
        var r = s.getBoundingClientRect();
        return { id: s.dataset.id, x: r.left + r.width / 2, y: r.top + r.height / 2, el: s };
      });
    }

    function onMove(e) {
      if (!dragging) {
        var dx = e.clientX - startX, dy = e.clientY - startY;
        if (Math.abs(dx) + Math.abs(dy) < 4) return;
        dragging = true;
        slot.classList.add("dragging");
        document.body.classList.add("deck-dragging");
        buildRectCache();
        // Create the ghost label
        ghost = document.createElement("div");
        ghost.className = "deck-ghost";
        ghost.textContent = slot.dataset.title || "Card";
        document.body.appendChild(ghost);
      }
      if (ghost) {
        ghost.style.left = e.clientX + "px";
        ghost.style.top  = e.clientY + "px";
      }
      var best = null, bestDist = Infinity;
      for (var i = 0; i < rectCache.length; i++) {
        var r = rectCache[i];
        if (r.id === slot.dataset.id) continue;
        var d = Math.hypot(e.clientX - r.x, e.clientY - r.y);
        if (d < bestDist) { bestDist = d; best = r; }
      }
      // Only flag as a drop target when reasonably close (within thumb height).
      var threshold = THUMB_HEIGHT * 1.2;
      var newTargetId = (best && bestDist < threshold) ? best.id : null;
      if (newTargetId !== lastTargetId) {
        rectCache.forEach(function (r) { r.el.classList.remove("dropover"); });
        if (newTargetId && best) best.el.classList.add("dropover");
        lastTargetId = newTargetId;
      }
      e.preventDefault();
    }

    function onUp() {
      grip.removeEventListener("pointermove", onMove);
      grip.removeEventListener("pointerup", onUp);
      grip.removeEventListener("pointercancel", onUp);
      if (pointerId !== null) {
        try { grip.releasePointerCapture(pointerId); } catch (e) {}
        pointerId = null;
      }
      if (ghost) { ghost.parentNode.removeChild(ghost); ghost = null; }
      document.body.classList.remove("deck-dragging");
      slot.classList.remove("dragging");
      Array.prototype.forEach.call(slot.parentNode.children, function (n) {
        n.classList && n.classList.remove("dropover");
      });
      if (dragging && lastTargetId) ondrop(lastTargetId);
      dragging = false;
      lastTargetId = null;
      rectCache = null;
    }

    // Pointer Events (preferred, single API for mouse / touch / pen) with
    // a mousedown fallback for older browsers — captive-portal clients on
    // 2014-era Android stock browsers don't ship PointerEvent.  We only
    // wire one of the two paths to avoid a double-fire on modern browsers.
    if ("PointerEvent" in window) {
      grip.addEventListener("pointerdown", function (e) {
        if (e.button && e.button !== 0) return;
        startX = e.clientX; startY = e.clientY;
        pointerId = e.pointerId;
        try { grip.setPointerCapture(e.pointerId); } catch (err) {}
        grip.addEventListener("pointermove", onMove);
        grip.addEventListener("pointerup", onUp);
        grip.addEventListener("pointercancel", onUp);
      });
    } else {
      // Mouse-only fallback.  No touch support here — phones without
      // PointerEvent are vanishingly rare, and span chips / hide / library
      // tray still work for them.
      grip.addEventListener("mousedown", function (e) {
        if (e.button !== 0) return;
        startX = e.clientX; startY = e.clientY;
        function moveBridge(ev) { onMove(ev); }
        function upBridge() {
          document.removeEventListener("mousemove", moveBridge);
          document.removeEventListener("mouseup", upBridge);
          onUp();
        }
        document.addEventListener("mousemove", moveBridge);
        document.addEventListener("mouseup", upBridge);
      });
    }
  }

  // ── Deck instance ──────────────────────────────────────────────────────────
  function mount(opts) {
    var pageId    = opts.pageId;
    var container = opts.container;
    var registry  = opts.registry;
    var defaults  = opts.defaults;
    var toolbarSlot = opts.toolbar; // element to inject Customise/Done/Reset into
    var editingHooks = opts.onEdit || function () {}; // notified when editing toggles

    var cards = loadCards(pageId, defaults);
    var editing = false;
    var resizeObserver = null;

    function persist() { saveCards(pageId, cards); }

    function visibleCards() {
      return cards.filter(function (c) { return !c.hidden; });
    }

    function findCard(id) {
      for (var i = 0; i < cards.length; i++) if (cards[i].id === id) return cards[i];
      return null;
    }

    function moveCard(fromId, toId) {
      if (fromId === toId) return;
      var fromIdx = -1, toIdx = -1;
      for (var i = 0; i < cards.length; i++) {
        if (cards[i].id === fromId) fromIdx = i;
        if (cards[i].id === toId)   toIdx   = i;
      }
      if (fromIdx < 0 || toIdx < 0) return;
      var item = cards.splice(fromIdx, 1)[0];
      cards.splice(toIdx, 0, item);
      persist();
      render();
    }

    function setSpan(id, span) {
      var c = findCard(id); if (!c) return;
      c.span = span; persist(); render();
    }
    function setHidden(id, h) {
      var c = findCard(id); if (!c) return;
      c.hidden = h; persist(); render();
    }
    function addCard(id, span) {
      if (findCard(id)) return;
      cards.push({ id: id, span: span || 4 });
      persist(); render();
    }
    function resetLayout() {
      cards = defaults.map(function (d) { return { id:d.id, span:d.span }; });
      persist(); render();
    }

    function renderToolbar() {
      if (!toolbarSlot) return;
      toolbarSlot.innerHTML = "";
      if (!editing) {
        var btn = document.createElement("button");
        btn.type = "button";
        btn.className = "btn deck-customise-btn";
        btn.innerHTML = '<span data-icon="sliders-horizontal"></span> Customise';
        btn.addEventListener("click", function () {
          editing = true; renderToolbar(); render(); editingHooks(true);
        });
        toolbarSlot.appendChild(btn);
      } else {
        var badge = document.createElement("span");
        badge.className = "badge acc mono deck-editing-badge";
        badge.textContent = "EDITING";
        var reset = document.createElement("button");
        reset.type = "button";
        reset.className = "btn";
        reset.innerHTML = '<span data-icon="rotate-ccw"></span> Reset';
        reset.addEventListener("click", function () {
          // Snapshot current layout, reset immediately, give the user an
          // 8 s undo window via the standard toast helper.
          var snapshot = cards.map(function (c) { return { id:c.id, span:c.span, hidden:c.hidden }; });
          resetLayout();
          if (typeof showUndoToast === "function") {
            showUndoToast(
              "Layout reset",
              "Restored defaults — press Undo to revert",
              function () {
                cards = snapshot;
                persist(); render();
              }
            );
          }
        });
        var done = document.createElement("button");
        done.type = "button";
        done.className = "btn primary";
        done.innerHTML = '<span data-icon="check"></span> Done';
        done.addEventListener("click", function () {
          editing = false; renderToolbar(); render(); editingHooks(false);
        });
        toolbarSlot.appendChild(badge);
        toolbarSlot.appendChild(reset);
        toolbarSlot.appendChild(done);
      }
      reIcons(toolbarSlot);
    }

    function renderLive() {
      // Disconnect any previous resize observer
      if (resizeObserver) { try { resizeObserver.disconnect(); } catch (e) {} resizeObserver = null; }
      container.classList.remove("editing");
      container.classList.add("deck", "live");
      container.innerHTML = "";

      visibleCards().forEach(function (card) {
        var meta = registry[card.id]; if (!meta) return;
        var slot = document.createElement("div");
        slot.className = "deck-slot";
        slot.dataset.id = card.id;
        slot.dataset.span = card.span;
        slot.dataset.title = meta.title;
        var content = meta.render(card);
        if (typeof content === "string") slot.innerHTML = content;
        else if (content instanceof Node) slot.appendChild(content);
        container.appendChild(slot);
      });
      reIcons(container);

      // Pack now, and re-pack on container/card size changes.
      packMasonry(container);
      if (typeof ResizeObserver !== "undefined") {
        var pending = false;
        var schedule = function () {
          if (pending) return;
          pending = true;
          requestAnimationFrame(function () { pending = false; packMasonry(container); });
        };
        resizeObserver = new ResizeObserver(schedule);
        resizeObserver.observe(container);
        Array.prototype.forEach.call(container.children, function (n) {
          resizeObserver.observe(n);
        });
      } else {
        // Old browsers without ResizeObserver: fall back to a window resize
        // listener.  Wrap it in a fake-observer shim so the existing
        // disconnect() path in renderLive() / renderEditing() removes it
        // cleanly — without this the handler accumulates on every render
        // and leaks (gemini review PR #108).
        var handleResize = function () { packMasonry(container); };
        window.addEventListener("resize", handleResize);
        resizeObserver = {
          disconnect: function () { window.removeEventListener("resize", handleResize); },
        };
      }
    }

    function renderEditing() {
      if (resizeObserver) { try { resizeObserver.disconnect(); } catch (e) {} resizeObserver = null; }
      container.classList.add("deck", "editing");
      container.classList.remove("live");
      container.innerHTML = "";
      container.style.height = ""; // let CSS grid drive the height
      container.style.position = "";

      var grid = document.createElement("div");
      grid.className = "deck-edit-grid";

      visibleCards().forEach(function (card) {
        var meta = registry[card.id]; if (!meta) return;
        var slot = document.createElement("div");
        slot.className = "deck-slot deck-thumb span-" + card.span;
        slot.dataset.id = card.id;
        slot.dataset.span = card.span;
        slot.dataset.title = meta.title;

        var chrome = document.createElement("div");
        chrome.className = "thumb-chrome";

        var grip = document.createElement("span");
        grip.className = "edit-grip";
        grip.title = "Drag to reorder";
        grip.innerHTML = '<span data-icon="grip-vertical"></span>';
        chrome.appendChild(grip);

        var spans = document.createElement("span");
        spans.className = "edit-spans";
        ALLOWED_SPANS.forEach(function (s) {
          var b = document.createElement("button");
          b.type = "button";
          b.className = "edit-span" + (card.span === s ? " active" : "");
          b.textContent = s;
          b.title = "Set width to " + s + "/12";
          b.addEventListener("click", function () { setSpan(card.id, s); });
          spans.appendChild(b);
        });
        chrome.appendChild(spans);

        var hide = document.createElement("button");
        hide.type = "button";
        hide.className = "edit-hide";
        hide.title = "Hide card";
        hide.innerHTML = '<span data-icon="eye-off"></span>';
        hide.addEventListener("click", function () { setHidden(card.id, true); });
        chrome.appendChild(hide);

        slot.appendChild(chrome);

        var body = document.createElement("div");
        body.className = "thumb-body";
        body.innerHTML =
          (meta.icon ? '<span data-icon="' + esc(meta.icon) + '"></span>' : "") +
          '<span class="thumb-title">' + esc(meta.title) + '</span>' +
          '<span class="thumb-span mono">' + card.span + '/12</span>';
        slot.appendChild(body);

        attachPointerDrag(slot, grip, function (toId) { moveCard(card.id, toId); });

        grid.appendChild(slot);
      });
      container.appendChild(grid);

      // ── Tray ────────────────────────────────────────────────────────────────
      var tray = document.createElement("div");
      tray.className = "deck-tray";

      var hidden = cards.filter(function (c) { return c.hidden; });
      var presentIds = {};
      cards.forEach(function (c) { presentIds[c.id] = true; });
      var available = Object.keys(registry).filter(function (id) { return !presentIds[id]; });

      tray.innerHTML =
        '<div class="deck-tray-section">' +
          '<div class="deck-tray-eyebrow">HIDDEN (' + hidden.length + ')</div>' +
          (hidden.length === 0
            ? '<div class="deck-tray-hint">All cards are visible. Hide a card with the eye icon to stash it here.</div>'
            : '<div class="deck-tray-chips" data-role="hidden"></div>') +
        '</div>' +
        '<div class="deck-tray-section">' +
          '<div class="deck-tray-eyebrow">ADD CARD (' + available.length + ')</div>' +
          (available.length === 0
            ? '<div class="deck-tray-hint">Every card type is on the page. Hide some to free up the library.</div>'
            : '<div class="deck-tray-chips" data-role="library"></div>') +
        '</div>';
      container.appendChild(tray);

      function chip(id, meta, action) {
        var b = document.createElement("button");
        b.type = "button";
        b.className = "deck-chip";
        b.innerHTML =
          (meta.icon ? '<span data-icon="' + esc(meta.icon) + '"></span>' : "") +
          '<span>' + esc(meta.title) + '</span>' +
          '<span class="deck-chip-add"><span data-icon="plus"></span></span>';
        b.addEventListener("click", action);
        return b;
      }

      var hiddenWrap = tray.querySelector('[data-role="hidden"]');
      if (hiddenWrap) {
        hidden.forEach(function (c) {
          var meta = registry[c.id]; if (!meta) return;
          hiddenWrap.appendChild(chip(c.id, meta, function () { setHidden(c.id, false); }));
        });
      }
      var libWrap = tray.querySelector('[data-role="library"]');
      if (libWrap) {
        available.forEach(function (id) {
          var meta = registry[id]; if (!meta) return;
          libWrap.appendChild(chip(id, meta, function () { addCard(id, 4); }));
        });
      }

      reIcons(container);
    }

    function render() {
      if (editing) renderEditing(); else renderLive();
    }

    renderToolbar();
    render();

    return {
      render: render,
      isEditing: function () { return editing; },
      setEditing: function (v) {
        editing = !!v; renderToolbar(); render(); editingHooks(editing);
      },
      repack: function () { if (!editing) packMasonry(container); },
    };
  }

  window.EditableDeck = { mount: mount };

})();
