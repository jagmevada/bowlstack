// Small DOM helpers. No framework — the app is five screens and must stay
// deployable by copying a folder.

export function h(tag, attrs = {}, ...children) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (v == null || v === false) continue;
    if (k === 'class') el.className = v;
    else if (k === 'html') el.innerHTML = v;
    else if (k === 'dataset') Object.assign(el.dataset, v);
    else if (k.startsWith('on') && typeof v === 'function') el.addEventListener(k.slice(2), v);
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
