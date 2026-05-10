// ═══════════════════════════════════════════════════════
// GENERATE ALL FILES
// ═══════════════════════════════════════════════════════
function generateAll() {
  if (state.services.length === 0) { showNotif('No services to generate.'); return; }

  function stripHTML(html) {
    const d = document.createElement('div');
    d.innerHTML = html;
    return d.innerText;
  }

  function toPascalCase(macro) {
    return macro.split('_').map(w => w.length <= 2 ? w : w.charAt(0) + w.slice(1).toLowerCase()).join('');
  }

  const files = [];

  // BaseUUIDs.h — shared across all services
  files.push({ name: 'BaseUUIDs.h', data: stripHTML(generateBaseUUIDs()) });

  // Per-service .h and .c files
  state.services.forEach(svc => {
    const filename = toPascalCase(toMacroName(svc.name));
    files.push({ name: `${filename}.h`, data: stripHTML(generateH(svc)) });
    files.push({ name: `${filename}.c`, data: stripHTML(generateC(svc)) });
  });

  // Single consolidated project XML
  const projectFilename = (state.profileMeta.project
    ? toPascalCase(toMacroName(state.profileMeta.project))
    : 'project');
  files.push({ name: `${projectFilename}.xml`, data: stripHTML(generateXML()) });

  const blob = InlineZip.build(files);
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'gatt_generated.zip';
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  setTimeout(() => URL.revokeObjectURL(a.href), 100);
  showNotif(`✓ Downloaded ${files.length} files (BaseUUIDs.h + ${state.services.length * 2} source + 1 project XML)`);

  if (state.services.length > 0) {
    state.selectedSvcId = state.services[0].id;
    renderTree(); renderEditor(); renderCode();
  }
}

// ═══════════════════════════════════════════════════════
// UTILITIES
// ═══════════════════════════════════════════════════════
function copyUUID(elId) {
  const el = document.getElementById(elId);
  if (!el) return;
  navigator.clipboard.writeText(el.textContent).then(()=>showNotif('UUID copied'));
}
function copyCode() {
  const pre = document.getElementById('codePreview');
  navigator.clipboard.writeText(pre.innerText).then(()=>showNotif('Code copied'));
}
function showNotif(msg) {
  const n = document.getElementById('notif');
  n.innerHTML = `<span>✓</span> ${msg}`;
  n.style.display='flex';
  clearTimeout(n._t);
  n._t = setTimeout(()=>{ n.style.display='none'; }, 2500);
}
function closeModal(e) { if (e.target.id==='modalBackdrop') closeModalDirect(); }
function closeModalDirect() { document.getElementById('modalBackdrop').style.display='none'; }
function downloadCurrentFile() {
  function toPascalCase(macro) {
    return macro.split('_').map(w => w.length <= 2 ? w : w.charAt(0) + w.slice(1).toLowerCase()).join('');
  }
  function stripHTML(html) { const d = document.createElement('div'); d.innerHTML = html; return d.innerText; }

  if (state.activeTab === 'buuid') {
    const text = stripHTML(generateBaseUUIDs());
    const blob = new Blob([text], {type: 'text/plain'});
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'BaseUUIDs.h';
    document.body.appendChild(a); a.click(); document.body.removeChild(a);
    URL.revokeObjectURL(a.href);
    showNotif('Saved BaseUUIDs.h');
    return;
  }

  if (state.activeTab === 'xml') {
    if (state.services.length === 0) { showNotif('No services to export'); return; }
    const projectFilename = (state.profileMeta.project
      ? toPascalCase(toMacroName(state.profileMeta.project))
      : 'project');
    const name = `${projectFilename}.xml`;
    const text = stripHTML(generateXML());
    const blob = new Blob([text], {type: 'text/xml'});
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = name;
    document.body.appendChild(a); a.click(); document.body.removeChild(a);
    URL.revokeObjectURL(a.href);
    showNotif(`Saved ${name}`);
    return;
  }

  const svc = getSvc(state.selectedSvcId);
  if (!svc) { showNotif('Select a service first'); return; }
  const macro = toMacroName(svc.name);
  const filename = toPascalCase(macro);
  const extMap = {h: `${filename}.h`, c: `${filename}.c`};
  const genMap = {h: generateH, c: generateC};
  const name = extMap[state.activeTab];
  const text = stripHTML(genMap[state.activeTab](svc));
  const blob = new Blob([text], {type: 'text/plain'});
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = name;
  document.body.appendChild(a); a.click(); document.body.removeChild(a);
  URL.revokeObjectURL(a.href);
  showNotif(`Saved ${name}`);
}
