/* MC2 review pack -- in-page annotation layer.
 *
 * Inlined into INDEX.html by scripts/build_review_pack.py.  Offline, no
 * network, no build step, no server: the page is opened from file://.
 *
 * WHAT IT BINDS TO
 *   Every reviewable artifact carries data-rid (stable lookup id), data-adigest
 *   (content proof) and data-akind, emitted by iter_anchors() in the generator.
 *
 * THE SAFETY RULE
 *   An annotation re-attaches silently ONLY when its stored rid is found AND
 *   the stored digest equals the live digest.  Digest differs -> CHANGED badge.
 *   rid missing -> ORPHANED list.  The user's words are never moved onto
 *   content he did not see.
 *
 * SECURITY
 *   Annotation text and imported JSON are untrusted (an export can be edited or
 *   come from elsewhere).  Nothing here uses innerHTML: every dynamic value goes
 *   in as a text node or an attribute via E(), so a comment containing markup is
 *   displayed, never parsed.
 *
 * OUTPUT
 *   Export writes schema "mc2.review.feedback/1"; scripts/read_review_feedback.py
 *   consumes it.  See docs/review-feedback-schema.md.
 */
(function () {
  'use strict';

  var FEEDBACK_SCHEMA = 'mc2.review.feedback/1';
  var metaEl = document.getElementById('rv-pack-meta');
  if (!metaEl) { return; }
  var PACK = JSON.parse(metaEl.textContent);
  var STORE_KEY = FEEDBACK_SCHEMA + '::' + PACK.pack_id;

  var VERDICTS = [
    ['good',  'GOOD',              'g'],
    ['bad',   'BAD',               'b'],
    ['notit', 'NOT WHAT I MEANT',  'n'],
    ['known', 'ALREADY REPORTED',  'k']
  ];
  var VSHORT = { good: 'GOOD', bad: 'BAD', notit: 'NOT IT', known: 'KNOWN' };
  var VCLS = { good: 'g', bad: 'b', notit: 'n', known: 'k' };
  var DOT = '·', TIMES = '×', CARET = '▾';

  // ------------------------------------------------------------------ state
  var els = new Map();              // rid -> element
  var anns = Object.create(null);   // rid -> annotation record
  var status = Object.create(null); // rid -> 'ok' | 'changed' | 'orphan'
  var selected = null;              // rid open in the drawer
  var drawArmed = null;             // rid whose image is in region-draw mode
  var storageOk = true, storageErr = '';

  // ------------------------------------------------------------- DOM helper
  function E(tag, attrs, kids) {
    var n = document.createElement(tag);
    if (attrs) {
      Object.keys(attrs).forEach(function (k) {
        var v = attrs[k];
        if (v == null || v === false) { return; }
        if (k === 'class') { n.className = v; }
        else if (k === 'text') { n.textContent = v; }
        else if (k.slice(0, 2) === 'on') { n.addEventListener(k.slice(2), v); }
        else { n.setAttribute(k, v); }
      });
    }
    (kids || []).forEach(function (c) {
      if (c == null || c === false) { return; }
      n.appendChild(typeof c === 'string' ? document.createTextNode(c) : c);
    });
    return n;
  }
  function clear(n) { while (n && n.firstChild) { n.removeChild(n.firstChild); } }
  function nowIso() { return new Date().toISOString().replace(/\.\d+Z$/, 'Z'); }
  function txt(el, sel) {
    var q = el && el.querySelector(sel);
    return q ? q.textContent.replace(/\s+/g, ' ').trim() : null;
  }

  // ---------------------------------------------------------------- storage
  function probeStorage() {
    try {
      window.localStorage.setItem(STORE_KEY + '::probe', '1');
      window.localStorage.removeItem(STORE_KEY + '::probe');
      storageOk = true;
    } catch (e) {
      storageOk = false;
      storageErr = (e && e.name) || 'unavailable';
    }
  }

  function load() {
    if (!storageOk) { return; }
    var raw;
    try { raw = window.localStorage.getItem(STORE_KEY); } catch (e) { return; }
    if (!raw) { return; }
    try {
      var d = JSON.parse(raw);
      (d.annotations || []).forEach(function (a) { if (a && a.id) { anns[a.id] = sane(a); } });
    } catch (e) { /* corrupt payload: start empty rather than throw */ }
  }

  /* Normalise a record that came from storage or an imported file.  Everything
     downstream then knows the shape is trustworthy even if the file was not. */
  function sane(a) {
    var vs = VERDICTS.map(function (v) { return v[0]; });
    return {
      id: String(a.id),
      kind: String(a.kind || 'unknown'),
      verdict: vs.indexOf(a.verdict) >= 0 ? a.verdict : null,
      comment: typeof a.comment === 'string' ? a.comment : '',
      regions: (Array.isArray(a.regions) ? a.regions : []).map(function (r) {
        return {
          x: +r.x || 0, y: +r.y || 0, w: +r.w || 0, h: +r.h || 0,
          note: typeof r.note === 'string' ? r.note : ''
        };
      }),
      anchor: (a.anchor && typeof a.anchor === 'object') ? a.anchor : {},
      created_utc: String(a.created_utc || nowIso()),
      updated_utc: String(a.updated_utc || nowIso())
    };
  }

  function save() {
    if (!storageOk) { return; }
    try {
      window.localStorage.setItem(STORE_KEY, JSON.stringify(buildPayload()));
    } catch (e) {
      storageOk = false;
      storageErr = (e && e.name) || 'write failed';
      renderDock();
    }
  }

  // ---------------------------------------------------------------- anchors
  function nearestHeading(el) {
    var n = el;
    while (n && n.tagName !== 'SECTION') {
      var p = n.previousElementSibling;
      while (p) {
        if (p.tagName === 'H3') { return p.textContent.replace(/\s+/g, ' ').trim(); }
        p = p.previousElementSibling;
      }
      n = n.parentElement;
    }
    return null;
  }

  /* The hover bar and chips live INSIDE the artifact they annotate, so a naive
     textContent snapshot captures this layer's own button labels and hands them
     to a lane as if they were the pack's words.  Snapshot a stripped clone. */
  function ownText(el) {
    var c = el.cloneNode(true);
    c.querySelectorAll('#rv-bar,.rv-chip,.rv-rgn-layer').forEach(function (n) { n.remove(); });
    return c.textContent.replace(/\s+/g, ' ').trim();
  }

  function anchorOf(el) {
    var sec = el.closest('section[data-akind="section"]') || el.closest('section');
    var img = el.querySelector('img');
    var body = (el.getAttribute('data-akind') === 'image')
      ? null : ownText(el).slice(0, 600);
    return {
      section_id: sec ? (sec.id || null) : null,
      section_heading: sec ? txt(sec, 'h2') : null,
      heading: nearestHeading(el),
      label: el.getAttribute('data-alabel') || null,
      src: img ? img.getAttribute('src') : null,
      caption: txt(el, 'figcaption'),
      text: body || null,
      digest: el.getAttribute('data-adigest') || null
    };
  }

  function ann(rid, create) {
    var a = anns[rid];
    if (a || !create) { return a; }
    var el = els.get(rid);
    if (!el) { return null; }
    a = {
      id: rid, kind: el.getAttribute('data-akind') || 'unknown',
      verdict: null, comment: '', regions: [], anchor: anchorOf(el),
      created_utc: nowIso(), updated_utc: nowIso()
    };
    anns[rid] = a;
    status[rid] = 'ok';
    return a;
  }

  function isEmpty(a) {
    return !a || (!a.verdict && !(a.comment || '').trim() && !(a.regions || []).length);
  }

  function touch(rid) {
    var a = anns[rid];
    if (!a) { return; }
    if (isEmpty(a)) {
      delete anns[rid];
      delete status[rid];
      if (selected === rid) { closeDrawer(); }
    } else {
      a.updated_utc = nowIso();
      // Re-anchor to the LIVE element so an export always carries a current
      // snapshot of what the comment was actually written against.
      var el = els.get(rid);
      if (el) { a.anchor = anchorOf(el); status[rid] = 'ok'; }
    }
    decorate(rid);
    save();
    renderDock();
    if (indexOpen) { renderIndex(); }
  }

  function annList() {
    return Object.keys(anns).map(function (k) { return anns[k]; })
      .filter(function (a) { return !isEmpty(a); });
  }

  // ------------------------------------------------------------ reconcile
  function reconcile() {
    // rebuild from scratch: a status left behind by a deleted record would keep
    // inflating the CHANGED / ORPHANED counters for the rest of the session.
    Object.keys(status).forEach(function (k) { delete status[k]; });
    Object.keys(anns).forEach(function (rid) {
      var el = els.get(rid);
      if (!el) { status[rid] = 'orphan'; return; }
      var live = el.getAttribute('data-adigest');
      var was = (anns[rid].anchor || {}).digest;
      status[rid] = (was && live && was !== live) ? 'changed' : 'ok';
    });
  }

  // ------------------------------------------------------------- decoration
  function hostFor(el) {
    if (el.getAttribute('data-akind') === 'image') { return el.querySelector('a') || el; }
    if (el.tagName === 'TR') { return el.lastElementChild || el; }
    return el;
  }

  /* Direct-children lookup, never querySelector.  Anchors NEST -- a figure and
     a table row both live inside an annotatable <section> -- so a descendant
     search from a section would find (and destroy) its children's furniture. */
  function ownChild(host, cls) {
    for (var i = 0; host && i < host.children.length; i++) {
      if (host.children[i].classList.contains(cls)) { return host.children[i]; }
    }
    return null;
  }

  function decorate(rid) {
    var el = els.get(rid);
    if (!el) { return; }
    var a = anns[rid];
    var old = ownChild(hostFor(el), 'rv-chip');
    if (old) { old.remove(); }
    if (!a) {
      el.classList.remove('rv-has');
      el.removeAttribute('data-averdict');
      renderRegions(rid);
      return;
    }
    el.classList.add('rv-has');
    if (a.verdict) { el.setAttribute('data-averdict', a.verdict); }
    else { el.removeAttribute('data-averdict'); }

    var kids = [];
    if (status[rid] === 'changed') { kids.push(E('b', { class: 'chg', text: 'CHANGED' }), ' ' + DOT + ' '); }
    if (a.verdict) { kids.push(E('b', { class: VCLS[a.verdict], text: VSHORT[a.verdict] }), ' ' + DOT + ' '); }
    if ((a.regions || []).length) {
      kids.push(a.regions.length + ' region' + (a.regions.length > 1 ? 's' : '') + ' ' + DOT + ' ');
    }
    var c = (a.comment || '').trim();
    if (c) { kids.push(c.slice(0, 40)); }
    if (!kids.length) { kids.push('note'); }
    var chip = E('div', {
      class: 'rv-chip', title: 'Open this annotation',
      onclick: function (e) { e.preventDefault(); e.stopPropagation(); openDrawer(rid); }
    }, kids);
    hostFor(el).appendChild(chip);
    renderRegions(rid);
  }

  function decorateAll() { Object.keys(anns).forEach(decorate); }

  // ---------------------------------------------------------------- regions
  function layerFor(rid) {
    var el = els.get(rid);
    if (!el || el.getAttribute('data-akind') !== 'image') { return null; }
    var host = el.querySelector('a') || el;
    var layer = ownChild(host, 'rv-rgn-layer');
    if (!layer) {
      layer = E('div', { class: 'rv-rgn-layer', 'data-for': rid });
      host.appendChild(layer);
      bindDraw(layer, rid);
    }
    return layer;
  }

  function renderRegions(rid) {
    var el = els.get(rid);
    if (!el || el.getAttribute('data-akind') !== 'image') { return; }
    var a = anns[rid];
    var host = el.querySelector('a') || el;
    var layer = ownChild(host, 'rv-rgn-layer');
    if (!a || !(a.regions || []).length) {
      if (layer && drawArmed !== rid) { layer.remove(); }
      else if (layer) { clear(layer); }
      return;
    }
    layer = layerFor(rid);
    clear(layer);
    a.regions.forEach(function (r, i) {
      var d = E('div', {
        class: 'rv-rgn', title: r.note || ('region ' + (i + 1)),
        style: 'left:' + (r.x * 100) + '%;top:' + (r.y * 100) + '%;' +
               'width:' + (r.w * 100) + '%;height:' + (r.h * 100) + '%'
      }, [E('i', { text: String(i + 1) })]);
      layer.appendChild(d);
    });
  }

  function bindDraw(layer, rid) {
    var start = null, live = null;
    layer.addEventListener('dragstart', function (e) { e.preventDefault(); });
    layer.addEventListener('mousedown', function (e) {
      if (!layer.classList.contains('rv-draw') || e.button !== 0) { return; }
      e.preventDefault(); e.stopPropagation();
      var b = layer.getBoundingClientRect();
      start = { x: (e.clientX - b.left) / b.width, y: (e.clientY - b.top) / b.height };
      live = E('div', { class: 'rv-rgn rv-live' });
      layer.appendChild(live);
      document.addEventListener('mousemove', move, true);
      document.addEventListener('mouseup', up, true);
    });
    function at(e) {
      var b = layer.getBoundingClientRect();
      return norm(start.x, start.y,
        Math.min(1, Math.max(0, (e.clientX - b.left) / b.width)),
        Math.min(1, Math.max(0, (e.clientY - b.top) / b.height)));
    }
    function move(e) {
      if (!start) { return; }
      e.preventDefault();
      var r = at(e);
      live.setAttribute('style', 'left:' + (r.x * 100) + '%;top:' + (r.y * 100) + '%;' +
        'width:' + (r.w * 100) + '%;height:' + (r.h * 100) + '%');
    }
    function up(e) {
      document.removeEventListener('mousemove', move, true);
      document.removeEventListener('mouseup', up, true);
      if (!start) { return; }
      e.preventDefault(); e.stopPropagation();
      var r = at(e);
      start = null;
      if (live) { live.remove(); live = null; }
      swallowNextClickOn(layer);
      if (r.w < 0.012 || r.h < 0.012) { return; }   // stray click, not a box
      var a = ann(rid, true);
      a.regions.push({ x: r4(r.x), y: r4(r.y), w: r4(r.w), h: r4(r.h), note: '' });
      touch(rid);
      if (selected !== rid) { openDrawer(rid); } else { renderDrawer(); }
      var last = drawer.querySelector('input[data-rgn="' + (a.regions.length - 1) + '"]');
      if (last) { last.focus(); }
    }
  }

  /* A drag ends with mouseup, and the browser then fires a click on the <a>
     wrapping the image, which would navigate to the full-size PNG.  Swallow
     exactly that one -- but ONLY if it lands on this image, and give up after a
     moment.  An unscoped, unexpiring one-shot swallows the user's next real
     click anywhere on the page (a graded verdict that silently never lands),
     which is precisely the kind of lost input this whole feature exists to
     stop. */
  function swallowNextClickOn(node) {
    var done = false;
    function off() {
      if (done) { return; }
      done = true;
      window.clearTimeout(timer);
      window.removeEventListener('click', k, true);
    }
    function k(ev) {
      if (node.contains(ev.target) || node === ev.target) {
        ev.preventDefault();
        ev.stopPropagation();
      }
      off();   // either way, never stay armed for a later click
    }
    var timer = window.setTimeout(off, 400);
    window.addEventListener('click', k, true);
  }

  function norm(x0, y0, x1, y1) {
    return { x: Math.min(x0, x1), y: Math.min(y0, y1),
             w: Math.abs(x1 - x0), h: Math.abs(y1 - y0) };
  }
  function r4(v) { return Math.round(v * 10000) / 10000; }

  function setDraw(rid, on) {
    if (drawArmed && drawArmed !== rid) { setDraw(drawArmed, false); }
    drawArmed = on ? rid : null;
    var el = els.get(rid);
    var layer = on ? layerFor(rid)
      : (el ? ownChild(el.querySelector('a') || el, 'rv-rgn-layer') : null);
    if (layer) { layer.classList.toggle('rv-draw', !!on); }
    if (!on) { renderRegions(rid); }
    renderDrawer();
    var b = bar && bar.querySelector('.rv-region');
    if (b) { b.classList.toggle('armed', !!on); }
  }

  // -------------------------------------------------------------- hover bar
  var bar = null, barFor = null;

  function buildBar() {
    var kids = VERDICTS.map(function (v) {
      return E('button', {
        class: 'rv-v rv-v-' + v[0], 'data-v': v[0],
        title: v[1] + '  (key: ' + v[2] + ')', text: VSHORT[v[0]],
        onclick: function (e) { e.preventDefault(); e.stopPropagation(); if (barFor) { toggleVerdict(barFor, v[0]); } }
      });
    });
    kids.push(E('button', {
      class: 'rv-act rv-region', text: 'REGION',
      title: 'Drag a box on this image  (key: r)',
      onclick: function (e) {
        e.preventDefault(); e.stopPropagation();
        if (!barFor) { return; }
        var t = barFor; openDrawer(t); setDraw(t, drawArmed !== t);
      }
    }));
    kids.push(E('button', {
      class: 'rv-act rv-note', text: 'COMMENT', title: 'Write a comment  (key: c)',
      onclick: function (e) { e.preventDefault(); e.stopPropagation(); if (barFor) { openDrawer(barFor); } }
    }));
    bar = E('div', { id: 'rv-bar', onmousedown: function (e) { e.stopPropagation(); } }, kids);
    document.body.appendChild(bar);
  }

  function showBar(el) {
    var rid = el.getAttribute('data-rid');
    barFor = rid;
    var isImg = el.getAttribute('data-akind') === 'image';
    bar.querySelector('.rv-region').style.display = isImg ? '' : 'none';
    var a = anns[rid];
    bar.querySelectorAll('.rv-v').forEach(function (b) {
      b.classList.toggle('on', !!a && a.verdict === b.getAttribute('data-v'));
    });
    bar.querySelector('.rv-region').classList.toggle('armed', drawArmed === rid);
    hostFor(el).appendChild(bar);
    bar.classList.add('rv-show');
  }

  function hideBar() {
    if (bar) { bar.classList.remove('rv-show'); }
    barFor = null;
  }

  function toggleVerdict(rid, v) {
    var a = ann(rid, true);
    if (!a) { return; }
    a.verdict = (a.verdict === v) ? null : v;
    touch(rid);
    if (barFor === rid && bar) {
      bar.querySelectorAll('.rv-v').forEach(function (b) {
        b.classList.toggle('on', !!anns[rid] && anns[rid].verdict === b.getAttribute('data-v'));
      });
    }
    if (selected === rid) { renderDrawer(); }
  }

  // ----------------------------------------------------------------- drawer
  var drawer = null, dBody = null, dKind = null, dTitle = null;

  function buildDrawer() {
    dKind = E('span', { class: 'rv-kind' });
    dTitle = E('h3');
    dBody = E('div', { class: 'rv-body' });
    drawer = E('aside', { id: 'rv-drawer' }, [
      E('header', {}, [dKind, dTitle,
        E('button', { class: 'rv-x', title: 'Close (Esc)', text: TIMES, onclick: closeDrawer })]),
      dBody,
      E('div', { class: 'rv-foot' }, [
        E('button', {
          class: 'rv-btn pri', text: 'SCROLL TO ITEM',
          onclick: function () { if (selected) { scrollToRid(selected); } }
        }),
        E('button', {
          class: 'rv-btn danger', text: 'DELETE',
          onclick: function () {
            if (!selected || !window.confirm('Delete this annotation?')) { return; }
            var rid = selected;
            delete anns[rid]; delete status[rid];
            decorate(rid); save(); renderDock();
            if (indexOpen) { renderIndex(); }
            closeDrawer();
          }
        })])
    ]);
    document.body.appendChild(drawer);
  }

  function openDrawer(rid) {
    selected = rid;
    ann(rid, true);
    document.querySelectorAll('.rv-sel').forEach(function (n) { n.classList.remove('rv-sel'); });
    var el = els.get(rid);
    if (el) { el.classList.add('rv-sel'); }
    drawer.classList.add('rv-show');
    renderDrawer();
  }

  function closeDrawer() {
    if (drawArmed) { setDraw(drawArmed, false); }
    if (selected && isEmpty(anns[selected])) { touch(selected); }
    document.querySelectorAll('.rv-sel').forEach(function (n) { n.classList.remove('rv-sel'); });
    selected = null;
    drawer.classList.remove('rv-show');
  }

  function renderDrawer() {
    if (!selected) { return; }
    var rid = selected;
    var a = anns[rid] || { verdict: null, comment: '', regions: [], anchor: {}, kind: 'unknown' };
    var el = els.get(rid);
    var an = a.anchor || (el ? anchorOf(el) : {});
    var isImg = a.kind === 'image';

    dKind.textContent = a.kind || '?';
    dTitle.textContent = (an.section_id || 'pack') +
      (an.heading ? ' / ' + an.heading.slice(0, 40) : '');

    clear(dBody);
    if (status[rid] === 'changed') {
      dBody.appendChild(E('div', { class: 'rv-warn bad' }, [
        E('b', { text: 'CONTENT CHANGED. ' }),
        'This item was regenerated with different content after you wrote this. ' +
        'Your words are preserved exactly as written, shown here against the ' +
        'ORIGINAL snapshot. Re-read the item, then edit and save to re-attach.'
      ]));
    }
    if (status[rid] === 'orphan') {
      dBody.appendChild(E('div', { class: 'rv-warn bad' }, [
        E('b', { text: 'ORPHANED. ' }),
        'The item this was written on is not in this page any more. It is kept ' +
        'verbatim and still exports, with the snapshot below so it can be traced.'
      ]));
    }
    dBody.appendChild(E('div', { class: 'rv-where', text: an.section_heading || '' }));
    dBody.appendChild(E('div', { class: 'rv-label', text: an.label || an.caption || rid }));
    if (isImg && an.src) {
      dBody.appendChild(E('div', { class: 'rv-where' }, [E('code', { text: an.src })]));
    }
    if (!isImg && an.text) {
      dBody.appendChild(E('div', { class: 'rv-snip', text: an.text }));
    }

    dBody.appendChild(E('div', { class: 'rv-h', text: 'Verdict — one click' }));
    dBody.appendChild(E('div', { class: 'rv-vgrid' }, VERDICTS.map(function (v) {
      return E('button', {
        class: 'rv-v-' + v[0] + (a.verdict === v[0] ? ' on' : ''), text: v[1],
        onclick: function () { toggleVerdict(rid, v[0]); }
      });
    })));

    dBody.appendChild(E('div', { class: 'rv-h', text: 'Comment' }));
    var ta = E('textarea', {
      id: 'rv-d-comment',
      placeholder: 'What is wrong, in your words. Plain language is fine — a lane reads this verbatim.'
    });
    ta.value = a.comment || '';
    ta.addEventListener('input', function () {
      var rec = ann(rid, true);
      rec.comment = ta.value;
      rec.updated_utc = nowIso();
      save(); renderDock();
    });
    ta.addEventListener('blur', function () { touch(rid); });
    dBody.appendChild(ta);

    if (isImg) {
      dBody.appendChild(E('div', { class: 'rv-h', text: 'Image regions' }));
      dBody.appendChild(E('button', {
        class: 'rv-btn' + (drawArmed === rid ? ' armed' : ''),
        text: drawArmed === rid
          ? 'DRAWING — DRAG ON THE IMAGE (click to stop)'
          : 'DRAW A BOX ON THE IMAGE',
        onclick: function () { setDraw(rid, drawArmed !== rid); }
      }));
      if (!(a.regions || []).length) {
        dBody.appendChild(E('div', {
          class: 'rv-where', style: 'margin-top:8px',
          text: 'No regions yet. Draw a box, then say what is wrong inside it. ' +
                'Boxes are stored as fractions of the image, so they survive any resolution.'
        }));
      } else {
        var wrap = E('div', { style: 'margin-top:8px' });
        a.regions.forEach(function (r, i) {
          var inp = E('input', {
            'data-rgn': String(i), placeholder: 'what is wrong in box ' + (i + 1)
          });
          inp.value = r.note || '';
          inp.addEventListener('input', function () {
            var rec = anns[rid];
            if (!rec || !rec.regions[i]) { return; }
            rec.regions[i].note = inp.value;
            rec.updated_utc = nowIso();
            save();
          });
          inp.addEventListener('blur', function () { decorate(rid); });
          wrap.appendChild(E('div', { class: 'rv-rgn-row' }, [
            E('div', { class: 'n', text: String(i + 1) }), inp,
            E('button', {
              class: 'x', title: 'Delete box', text: TIMES,
              onclick: function () {
                var rec = anns[rid];
                if (!rec) { return; }
                rec.regions.splice(i, 1);
                touch(rid); renderDrawer();
              }
            })
          ]));
        });
        dBody.appendChild(wrap);
      }
    }
  }

  function scrollToRid(rid) {
    var el = els.get(rid);
    if (!el) {
      window.alert('That item is not in this page any more (orphaned annotation).');
      return;
    }
    el.scrollIntoView({ behavior: 'smooth', block: 'center' });
    el.classList.add('rv-sel');
  }

  // ---------------------------------------------------------------- payload
  function buildPayload() {
    var list = annList();
    var tally = { good: 0, bad: 0, notit: 0, known: 0, none: 0 };
    var regions = 0;
    list.forEach(function (a) {
      var k = a.verdict || 'none';
      tally[k] = (tally[k] || 0) + 1;
      regions += (a.regions || []).length;
    });
    // count over the LIVE list, never over the status map -- the map can hold
    // keys for records that no longer exist.
    function count(s) {
      return list.filter(function (a) { return status[a.id] === s; }).length;
    }
    return {
      schema: FEEDBACK_SCHEMA,
      pack: {
        id: PACK.pack_id, title: PACK.title, anchor_schema: PACK.schema,
        anchors: PACK.anchors, images: PACK.images
      },
      exported_utc: nowIso(),
      page_url: String(location.href),
      counts: {
        annotations: list.length, regions: regions, verdicts: tally,
        changed: count('changed'), orphaned: count('orphan')
      },
      annotations: list.map(function (a) {
        var o = JSON.parse(JSON.stringify(a));
        o.status = status[a.id] || 'ok';
        return o;
      })
    };
  }

  function doExport() {
    var p = buildPayload();
    if (!p.annotations.length &&
        !window.confirm('No annotations yet. Export an empty file anyway?')) { return; }
    var blob = new Blob([JSON.stringify(p, null, 1)], { type: 'application/json' });
    var stamp = nowIso().replace(/[-:]/g, '').replace('T', '-').replace('Z', '');
    var a = E('a', {
      href: URL.createObjectURL(blob),
      download: 'mc2-review-feedback-' + PACK.pack_id + '-' + stamp + '.json'
    });
    document.body.appendChild(a);
    a.click();
    setTimeout(function () { URL.revokeObjectURL(a.href); a.remove(); }, 2000);
  }

  function doImport(file) {
    var fr = new FileReader();
    fr.onload = function () {
      var d;
      try { d = JSON.parse(String(fr.result)); }
      catch (e) { window.alert('Not valid JSON: ' + e.message); return; }
      if (!d || !Array.isArray(d.annotations)) {
        window.alert('That file has no "annotations" array — not a review feedback export.');
        return;
      }
      if (d.schema !== FEEDBACK_SCHEMA &&
          !window.confirm('This file says schema "' + d.schema + '", this page expects "' +
            FEEDBACK_SCHEMA + '". Import anyway?')) { return; }
      if (!window.confirm('Import ' + d.annotations.length + ' annotation(s)?\n\n' +
        'You currently have ' + Object.keys(anns).length + '. They will be MERGED; ' +
        'on a clash the IMPORTED one wins. Nothing is deleted.')) { return; }
      d.annotations.forEach(function (a) { if (a && a.id) { anns[String(a.id)] = sane(a); } });
      reconcile();
      document.querySelectorAll('.rv-has').forEach(function (n) {
        n.classList.remove('rv-has'); n.removeAttribute('data-averdict');
      });
      document.querySelectorAll('.rv-chip,.rv-rgn-layer').forEach(function (n) { n.remove(); });
      decorateAll();
      save(); renderDock();
      if (indexOpen) { renderIndex(); }
      window.alert('Imported. ' + Object.keys(anns).length + ' annotation(s) now loaded.');
    };
    fr.readAsText(file);
  }

  // ------------------------------------------------------------------- dock
  var dock = null, dTally = null, dProg = null, dStorage = null, dFile = null;

  function buildDock() {
    dTally = E('div', { class: 'rv-tally' });
    dProg = E('progress', { max: '100', value: '0' });
    dStorage = E('div');
    dFile = E('input', {
      type: 'file', accept: '.json,application/json', style: 'display:none',
      onchange: function () {
        if (this.files && this.files[0]) { doImport(this.files[0]); }
        this.value = '';
      }
    });
    function act(cls, label, fn) {
      return E('button', {
        class: 'rv-btn ' + cls, text: label,
        onclick: function (e) { e.stopPropagation(); fn(); }
      });
    }
    dock = E('div', { id: 'rv-dock' }, [
      E('div', {
        class: 'rv-dock-hd',
        onclick: function () { dock.classList.toggle('rv-min'); }
      }, [E('b', { text: 'REVIEW — YOUR FEEDBACK' }),
          E('span', { title: 'Collapse / expand', text: CARET })]),
      E('div', { class: 'rv-dock-body' }, [
        dProg, dTally,
        E('div', { class: 'rv-acts' }, [
          act('pri wide', 'EXPORT JSON (hand this back)', doExport),
          act('', 'ALL NOTES', toggleIndex),
          act('', 'IMPORT', function () { dFile.click(); }),
          act('danger wide', 'CLEAR EVERYTHING', clearAll)
        ]),
        dStorage,
        E('div', {
          class: 'rv-warn',
          style: 'background:#151a20;border-color:#333b45;color:#9aa3ad'
        }, ['Hover any image, paragraph or table row to grade it. Keys while hovering: ',
            E('b', { text: 'g' }), ' good, ', E('b', { text: 'b' }), ' bad, ',
            E('b', { text: 'n' }), ' not what I meant, ', E('b', { text: 'k' }),
            ' already reported, ', E('b', { text: 'r' }), ' draw a box, ',
            E('b', { text: 'c' }), ' comment.']),
        dFile
      ])
    ]);
    document.body.appendChild(dock);
  }

  function clearAll() {
    if (!window.confirm('Delete ALL ' + annList().length +
      ' annotation(s) from this browser?\n\nEXPORT FIRST if you have not. ' +
      'This cannot be undone.')) { return; }
    Object.keys(anns).forEach(function (rid) {
      delete anns[rid]; delete status[rid];
      var el = els.get(rid);
      if (el) { el.classList.remove('rv-has'); el.removeAttribute('data-averdict'); }
    });
    document.querySelectorAll('.rv-chip,.rv-rgn-layer').forEach(function (n) { n.remove(); });
    closeDrawer(); save(); renderDock();
    if (indexOpen) { renderIndex(); }
  }

  function renderDock() {
    var list = annList();
    var t = { good: 0, bad: 0, notit: 0, known: 0 };
    var regions = 0, imgs = 0;
    list.forEach(function (a) {
      if (a.verdict) { t[a.verdict]++; }
      regions += (a.regions || []).length;
      if (a.kind === 'image') { imgs++; }
    });
    function count(s) {
      return list.filter(function (a) { return status[a.id] === s; }).length;
    }
    var chg = count('changed'), orp = count('orphan');

    clear(dTally);
    VERDICTS.forEach(function (v) {
      dTally.appendChild(E('span', {
        style: 'color:var(--rv-' + v[0] + ')', text: t[v[0]] + ' ' + VSHORT[v[0]]
      }));
    });
    dTally.appendChild(E('span', { text: regions + ' box' + (regions === 1 ? '' : 'es') }));
    dTally.appendChild(E('span', { text: imgs + '/' + (PACK.images || 0) + ' images' }));
    if (chg) { dTally.appendChild(E('span', { style: 'color:var(--rv-chg)', text: chg + ' CHANGED' })); }
    if (orp) { dTally.appendChild(E('span', { style: 'color:var(--rv-bad)', text: orp + ' ORPHANED' })); }
    dProg.value = PACK.images ? Math.round(100 * imgs / PACK.images) : 0;

    clear(dStorage);
    if (!storageOk) {
      dStorage.appendChild(E('div', { class: 'rv-warn bad' }, [
        E('b', { text: 'AUTOSAVE IS OFF' }), ' (' + storageErr + '). Nothing is being ' +
        'stored. ', E('b', { text: 'Export before you close this tab' }),
        ' or you lose everything you type.'
      ]));
    } else {
      dStorage.appendChild(E('div', { class: 'rv-warn' }, [
        E('b', { text: 'Saved in this browser only.' }),
        ' localStorage is scoped to this browser profile and this page’s location. ' +
        'Opening a ', E('b', { text: 'copy from another folder' }), ', a ',
        E('b', { text: 'different browser' }), ', a ', E('b', { text: 'private window' }),
        ', or clearing site data all show ', E('b', { text: 'nothing' }),
        ' — your notes are not lost, they are just not visible there. ',
        E('b', { text: 'Export' }), ' when you are done; that JSON is the thing to hand back.'
      ]));
    }
  }

  // ------------------------------------------------------------ index drawer
  var indexEl = null, iBody = null, indexOpen = false;

  function buildIndex() {
    iBody = E('div', { class: 'rv-body' });
    indexEl = E('aside', { id: 'rv-index' }, [
      E('header', {
        style: 'padding:10px 12px;border-bottom:1px solid #333b45;display:flex;' +
               'align-items:center;gap:8px;background:#1d2128'
      }, [
        E('h3', { style: 'margin:0;font-size:13px;flex:1;letter-spacing:.03em', text: 'ALL YOUR NOTES' }),
        E('button', { class: 'rv-x', text: TIMES, onclick: toggleIndex })
      ]),
      iBody
    ]);
    document.body.appendChild(indexEl);
  }

  function toggleIndex() {
    indexOpen = !indexOpen;
    indexEl.classList.toggle('rv-show', indexOpen);
    if (indexOpen) { renderIndex(); }
  }

  function renderIndex() {
    var order = { bad: 0, notit: 1, known: 2, good: 3 };
    var list = annList();
    var groups = [
      ['ORPHANED — the item these were written on is gone from the page',
       list.filter(function (a) { return status[a.id] === 'orphan'; })],
      ['CHANGED — the item was regenerated with different content',
       list.filter(function (a) { return status[a.id] === 'changed'; })],
      ['ATTACHED', list.filter(function (a) { return (status[a.id] || 'ok') === 'ok'; })]
    ];
    clear(iBody);
    if (!list.length) {
      iBody.appendChild(E('div', {
        class: 'rv-where',
        text: 'Nothing graded yet. Hover an image and click GOOD or BAD.'
      }));
      return;
    }
    groups.forEach(function (g) {
      if (!g[1].length) { return; }
      g[1].sort(function (x, y) {
        var d = (order[x.verdict] == null ? 9 : order[x.verdict]) -
                (order[y.verdict] == null ? 9 : order[y.verdict]);
        return d || String((x.anchor || {}).section_id)
          .localeCompare(String((y.anchor || {}).section_id));
      });
      iBody.appendChild(E('div', { class: 'rv-sec-h', text: g[0] + ' (' + g[1].length + ')' }));
      g[1].forEach(function (a) {
        var an = a.anchor || {};
        var st = status[a.id];
        var meta = [an.section_id || ''];
        if (an.heading) { meta.push(an.heading.slice(0, 60)); }
        if (a.verdict) { meta.push(VSHORT[a.verdict]); }
        if ((a.regions || []).length) { meta.push(a.regions.length + ' box'); }
        var kids = [];
        if (st === 'orphan') { kids.push(E('span', { class: 'rv-badge orphan', text: 'ORPHANED' })); }
        if (st === 'changed') { kids.push(E('span', { class: 'rv-badge changed', text: 'CHANGED' })); }
        kids.push(E('span', { class: 't', text: an.label || an.caption || a.id }));
        kids.push(E('div', { class: 'm', text: meta.join(' ' + DOT + ' ') }));
        if ((a.comment || '').trim()) {
          kids.push(E('div', { class: 'c', text: a.comment.trim() }));
        }
        (a.regions || []).forEach(function (r, i) {
          if (r.note) { kids.push(E('div', { class: 'c', text: '[box ' + (i + 1) + '] ' + r.note })); }
        });
        iBody.appendChild(E('div', {
          class: 'rv-item ' + (a.verdict || ''),
          onclick: function () {
            if (els.get(a.id)) { scrollToRid(a.id); }
            openDrawer(a.id);
          }
        }, kids));
      });
    });
  }

  // ------------------------------------------------------------------- boot
  function boot() {
    document.querySelectorAll('[data-rid]').forEach(function (el) {
      els.set(el.getAttribute('data-rid'), el);
    });
    probeStorage();
    load();
    reconcile();
    buildBar(); buildDrawer(); buildDock(); buildIndex();
    decorateAll();
    renderDock();

    document.addEventListener('mouseover', function (e) {
      if (e.target.closest('#rv-dock,#rv-drawer,#rv-index,#rv-bar')) { return; }
      var el = e.target.closest('[data-rid]');
      if (!el) { hideBar(); return; }
      if (el.getAttribute('data-rid') !== barFor) { showBar(el); }
    });

    document.addEventListener('keydown', function (e) {
      if (e.ctrlKey || e.metaKey || e.altKey) { return; }
      var tag = (document.activeElement || {}).tagName;
      if (tag === 'INPUT' || tag === 'TEXTAREA') {
        if (e.key === 'Escape') { document.activeElement.blur(); }
        return;
      }
      if (e.key === 'Escape') {
        if (drawArmed) { setDraw(drawArmed, false); }
        else if (indexOpen) { toggleIndex(); }
        else { closeDrawer(); }
        return;
      }
      var target = selected || barFor;
      if (!target) { return; }
      var k = e.key.toLowerCase();
      var hit = VERDICTS.filter(function (v) { return v[2] === k; })[0];
      if (hit) { e.preventDefault(); toggleVerdict(target, hit[0]); return; }
      if (k === 'c') { e.preventDefault(); openDrawer(target); return; }
      var el = els.get(target);
      if (k === 'r' && el && el.getAttribute('data-akind') === 'image') {
        e.preventDefault(); openDrawer(target); setDraw(target, drawArmed !== target);
      }
    });

    window.addEventListener('beforeunload', function (e) {
      if (!storageOk && annList().length) { e.preventDefault(); e.returnValue = ''; }
    });

    // exposed for the automated harness (scripts/test_review_annotate.py)
    window.__rvDebug = {
      anns: anns, status: status, payload: buildPayload,
      rids: function () { return Array.from(els.keys()); },
      setAnns: function (list) {
        Object.keys(anns).forEach(function (k) { delete anns[k]; });
        list.forEach(function (a) { anns[a.id] = sane(a); });
        reconcile();
        return status;
      }
    };
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot);
  } else { boot(); }
})();
