// Small DOM helpers. No framework — the app is five screens and must stay
// deployable by copying a folder.

export function h(tag, attrs = {}, ...children) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (v == null || v === false) continue;
    if (k === 'class') el.className = v;
    else if (k === 'html') el.innerHTML = v;
    else if (k === 'dataset') Object.assign(el.dataset, v);
    else if (k.startsWith('on') && typeof v === 'function') {
      // Recorded as well as attached, so morph() can swap a reused node's
      // handlers for the new render's closures instead of leaving it wired to
      // stale state.
      (el.__listeners ||= []).push([k.slice(2), v]);
      el.addEventListener(k.slice(2), v);
    }
    else if (k === 'value') el.value = v;
    else if (v === true) el.setAttribute(k, '');
    else el.setAttribute(k, String(v));
  }
  for (const c of children.flat(Infinity)) {
    if (c == null || c === false) continue;
    el.append(c instanceof Node ? c : document.createTextNode(String(c)));
  }
  return el;
}

export function clear(node) { while (node.firstChild) node.removeChild(node.firstChild); }

export function mount(node, ...children) {
  clear(node);
  node.append(...children.flat(Infinity).filter(Boolean));
  return node;
}

// --- morph ------------------------------------------------------------
//
// Replacing the whole view on every poll is what made the screen blink: the
// browser tears down and repaints everything, and scroll position, focus,
// hover and any open <details> go with it. Instead, walk the freshly built
// tree against the live one and change only what actually differs.
//
// This is not a general virtual DOM. It matches children by position, which is
// right for these screens: the lists are the same shape poll to poll, and when
// severity re-sorts a row the content is rewritten in place rather than moved.

/** Attributes never copied from the new tree onto a reused node. */
const PRESERVE = new Set(['open']);   // a <details> the user opened stays open

function sameKind(a, b) {
  if (a.nodeType !== b.nodeType) return false;
  if (a.nodeType === 1) return a.tagName === b.tagName;
  return true;
}

function syncAttributes(from, to) {
  for (const { name } of [...from.attributes]) {
    if (PRESERVE.has(name)) continue;
    if (!to.hasAttribute(name)) from.removeAttribute(name);
  }
  for (const { name, value } of [...to.attributes]) {
    if (PRESERVE.has(name)) continue;
    if (from.getAttribute(name) !== value) from.setAttribute(name, value);
  }
}

function syncFormState(from, to) {
  // Never fight the person typing. A poll landing mid-keystroke must not
  // rewrite the field under the cursor, and must not undo a dropdown they
  // just changed but have not saved.
  if (from === document.activeElement) return;
  if (from.tagName === 'INPUT' || from.tagName === 'TEXTAREA') {
    if (from.type === 'checkbox' || from.type === 'radio') {
      if (from.checked !== to.checked) from.checked = to.checked;
    } else if (from.value !== to.value) {
      from.value = to.value;
    }
  } else if (from.tagName === 'SELECT' && from.value !== to.value) {
    from.value = to.value;
  }
}

function swapListeners(from, to) {
  for (const [type, fn] of from.__listeners || []) from.removeEventListener(type, fn);
  from.__listeners = [];
  for (const [type, fn] of to.__listeners || []) {
    from.addEventListener(type, fn);
    from.__listeners.push([type, fn]);
  }
}

export function morphNode(from, to) {
  if (!sameKind(from, to)) {
    from.replaceWith(to);
    return to;
  }
  if (from.nodeType !== 1) {
    if (from.nodeValue !== to.nodeValue) from.nodeValue = to.nodeValue;
    return from;
  }
  syncAttributes(from, to);
  swapListeners(from, to);
  syncFormState(from, to);
  morphChildren(from, to);
  return from;
}

export function morphChildren(from, to) {
  const a = [...from.childNodes];
  const b = [...to.childNodes];
  const n = Math.min(a.length, b.length);
  for (let i = 0; i < n; i++) morphNode(a[i], b[i]);
  for (let i = n; i < a.length; i++) a[i].remove();
  for (let i = n; i < b.length; i++) from.append(b[i]);
}

/**
 * Write into a slot that a redraw may have swapped underneath you.
 *
 * A view that fetches asynchronously builds a placeholder, then fills it when
 * the data lands. Under morph() the LIVE node is kept and the freshly built
 * one is discarded — so a captured reference can be detached by the time the
 * fetch resolves, and the fill lands on a node nobody can see. (That is
 * exactly how the Menu tab got stuck on "Loading menu…": navigation renders
 * twice, and the second pass morphed the placeholder back over the filled
 * box while the promise still held the discarded one.)
 *
 * Looking the slot up by id at write time makes the fill land wherever the
 * slot currently is. The captured node is the fallback for the case where the
 * user has navigated away entirely.
 */
export function fillSlot(id, fallback, ...content) {
  const target = document.getElementById(id) || fallback;
  target.replaceChildren(...content.flat(Infinity).filter(Boolean));
  return target;
}

/**
 * Bring `node`'s children in line with `children` without rebuilding them.
 * Same call signature as mount(), so a view swaps one for the other.
 */
export function reconcile(node, ...children) {
  const next = document.createElement(node.tagName);
  next.append(...children.flat(Infinity).filter(Boolean));
  morphChildren(node, next);
  return node;
}

/** A status pill. The colour never travels alone — glyph and word go with it,
 *  because two of the four status colours are sub-3:1 on the light surface. */
export function badge(status, glyph, text, title) {
  return h('span', { class: `badge is-${status}`, title: title || text },
    h('span', { class: 'g', 'aria-hidden': 'true' }, glyph),
    text);
}

export function empty(text) {
  return h('div', { class: 'empty' }, text);
}

export function banner(kind, glyph, ...content) {
  return h('div', { class: `banner is-${kind}` },
    h('span', { class: 'g', 'aria-hidden': 'true' }, glyph),
    h('div', {}, ...content));
}

/** Level column. levels[0] is the BOTTOM bowl, so the array is reversed for
 *  display — a stack grows upward on screen as it does on the station. */
export function levelColumn(levels, big = false) {
  const arr = Array.isArray(levels) && levels.length ? levels : ['unknown', 'unknown', 'unknown', 'unknown'];
  const col = h('div', {
    class: `levels${big ? ' lg' : ''}`,
    role: 'img',
    'aria-label': arr.map((s, i) => `f${i + 1} ${s}`).join(', '),
  });
  for (let i = arr.length - 1; i >= 0; i--) col.append(h('i', { class: arr[i] }));
  return col;
}

/** A 4-segment battery glyph.
 *  Fill count carries the level alongside colour (colour never travels
 *  alone): good=4 green, medium=3 green, low=2 amber, critical=1 red,
 *  no cell=0 with a dashed outline. A charger on the rail overlays a bolt.
 *  The band is the server's hysteretic battery_level — never a percentage. */
export function batteryBar(level, charging, title) {
  const lvl = level == null ? 'none' : level;
  return h('span', {
    class: `batt lvl-${lvl}`,
    role: 'img',
    title,
    'aria-label': title,
  },
    h('i'), h('i'), h('i'), h('i'),
    // U+FE0E forces text presentation: bare ⚡ is Emoji_Presentation=Yes and
    // Segoe UI Emoji paints it yellow, ignoring color and the halo shadow.
    charging ? h('span', { class: 'bolt', 'aria-hidden': 'true' }, '⚡\uFE0E') : null);
}

let toastTimer = null;
export function toast(message, isError = false) {
  const el = document.getElementById('toast');
  el.textContent = message;
  el.className = `toast${isError ? ' err' : ''}`;
  el.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.hidden = true; }, isError ? 8000 : 3000);
}

export async function copyText(text, okMessage = 'Copied') {
  try {
    await navigator.clipboard.writeText(text);
    toast(okMessage);
  } catch {
    // Clipboard needs a secure context; a kitchen tablet on plain http will
    // land here, so fall back rather than failing silently.
    const ta = h('textarea', { style: 'position:fixed;opacity:0' });
    ta.value = text;
    document.body.append(ta);
    ta.select();
    try { document.execCommand('copy'); toast(okMessage); }
    catch { toast('Could not copy — select the text manually', true); }
    ta.remove();
  }
}

export function confirmAction(message) {
  return window.confirm(message);
}
