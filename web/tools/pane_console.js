// pane_console.js -- paste into the page (browser pane / DevTools console) to drive the game console and read the profiler from JavaScript.
//   await typeCmd('/noclip');  await typeCmd('/setviewpos 440 300 165 85');   // console command (a line without a leading slash is sent as chat)
//   await typeCmd('/gfxexp 16');                                              // see "Performance" in web/README.md
//   profLast(1)                                                               // last [prof] report (page started with ?args=--prof)
// The engine listens for key events on `document`, so synthetic KeyboardEvents work; Backquote opens / closes the console.
window.kk = (code, key, shift = false) => { for (const type of ['keydown', 'keyup']) document.dispatchEvent(new KeyboardEvent(type, { code, key, shiftKey: shift, bubbles: true, cancelable: true })); };
window.sleep = ms => new Promise(r => setTimeout(r, ms));
const CODES = { ' ': ['Space', ' '], '-': ['Minus', '-'], '_': ['Minus', '_', true], '.': ['Period', '.'], '/': ['Slash', '/'], '+': ['Equal', '+', true], '=': ['Equal', '='], ';': ['Semicolon', ';'], '"': ['Quote', '"', true] };
window.typeCmd = async (str) => {
  kk('Backquote', '`'); await sleep(150);
  for (const ch of str) {
    let c = CODES[ch];
    if (!c) { if (/[a-z]/.test(ch)) c = ['Key' + ch.toUpperCase(), ch]; else if (/[A-Z]/.test(ch)) c = ['Key' + ch, ch, true]; else if (/[0-9]/.test(ch)) c = ['Digit' + ch, ch]; else continue; }
    kk(c[0], c[1], !!c[2]); await sleep(40);
  }
  kk('Enter', 'Enter'); await sleep(150); kk('Backquote', '`'); await sleep(100);
};
// the last n [prof] windows (header + zones + counters)
window.profLast = (n = 1) => {
  const l = document.getElementById('log').textContent.split('\n').filter(x => x.startsWith('[prof]') && !x.includes('HITCH'));
  const idx = l.map((x, i) => x.includes('----') ? i : -1).filter(i => i >= 0);
  return l.slice(idx[Math.max(0, idx.length - n)]).join('\n');
};
// fps / frame-time header lines of every window whose `gfxexp` mask was set by typeCmd('/gfxexp N') (the 2nd full window after each change)
window.expReport = () => {
  const out = []; let mask = 'start', n = 0;
  for (const l of document.getElementById('log').textContent.split('\n')) {
    if (l.includes('gfx experiment mask =')) { mask = l.split('=').pop().trim(); n = 0; }
    else if (l.startsWith('[prof] ----')) { n++; if (n === 2 || (mask === 'start' && n === 1)) out.push('mask ' + mask + ': ' + l.replace('[prof] ---- ', '')); }
  }
  return out.join('\n');
};
