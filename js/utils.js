// ═══════════════════════════════════════════════════════
// HELPERS
// ═══════════════════════════════════════════════════════
function toMacroName(name) {
  return name.trim().toUpperCase().replace(/[^A-Z0-9]+/g, '_');
}
function toCamelCase(name) {
  return name.trim().toLowerCase().replace(/[^a-z0-9]+(.)/g, (_,c) => c.toUpperCase());
}
// Fills any missing perm keys with false — ensures old data & XML imports are safe
function normPerms(p) {
  const keys = ['read','readEncrypt','readAuthen','readLesc','write','writeEncrypt','writeAuthen','prepareWrite','writeLesc'];
  const out = {};
  keys.forEach(k => out[k] = p[k] === true);
  return out;
}
// Fills any missing prop keys with false — ensures old data & XML imports are safe
function normProps(p) {
  const keys = ['broadcast','read','write','writeNoResp','notify','indicate','auth','extProp'];
  const out = {};
  keys.forEach(k => out[k] = p[k] === true);
  return out;
}
function getPropKey(c) {
  const r = c.props.read, w = c.props.write || c.props.writeNoResp;
  if (r && w) return 'rw';
  if (r) return 'read';
  if (w) return 'write';
  if (c.props.notify)    return 'notify';
  if (c.props.broadcast) return 'broadcast';
  return 'read';
}
function computeUUID(svc, charIdNum = 0) {
  const domObj = state.domains.find(d=>d.id===svc.domainId) || state.domains[0];
  const domain = parseInt(domObj ? domObj.hex : svc.domain) || 0x01;
  const svcNum = parseInt(svc.serviceId) || 0x01;
  const first = ((domain << 24) | (svcNum << 16) | charIdNum) >>> 0;
  const hex = first.toString(16).padStart(8,'0').toUpperCase();
  const b = state.baseUUID;
  const p2 = b.p2 || 'XXXX';
  const p3 = b.p3 || 'XXXX';
  const p4 = b.p4 || 'XXXX';
  const p5 = b.p5 || 'XXXXXXXXXXXX';
  return `${hex}-${p2}-${p3}-${p4}-${p5}`;
}

function updateBaseUUID(part, val) {
  // Strip any dashes/spaces the user may have typed
  val = val.replace(/[^0-9a-fA-F]/g, '').toUpperCase();
  const maxLen = { p2: 4, p3: 4, p4: 4, p5: 12 };
  val = val.slice(0, maxLen[part]);
  state.baseUUID[part] = val;
  // Reflect cleaned value back into the input
  const inp = document.getElementById('buuid-' + part);
  if (inp) inp.value = val;
  // Update the assembled display
  refreshBaseUUIDDisplay();
  // Re-render all live UUID pills in the editor
  renderEditor();
  renderCode();
}

function refreshBaseUUIDDisplay() {
  const b = state.baseUUID;
  const p2 = b.p2 || 'XXXX';
  const p3 = b.p3 || 'XXXX';
  const p4 = b.p4 || 'XXXX';
  const p5 = b.p5 || 'XXXXXXXXXXXX';
  const full = document.getElementById('buuid-full');
  if (full) full.textContent = `xxxxxxxx-${p2}-${p3}-${p4}-${p5}`;
}
function getSvc(id) { return state.services.find(s=>s.id===id); }
function getChar(svc, id) { return svc?.chars?.find(c=>c.id===id); }
