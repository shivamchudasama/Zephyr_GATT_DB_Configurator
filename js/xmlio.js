// ═══════════════════════════════════════════════════════
// IMPORT XML
// ═══════════════════════════════════════════════════════
function importXML() {
  const inp = document.createElement('input');
  inp.type = 'file';
  inp.accept = '.xml';
  inp.multiple = false; // single consolidated file
  inp.onchange = e => {
    const file = e.target.files[0];
    if (!file) return;
    const reader = new FileReader();
    reader.onload = ev => {
      try {
        const parsed = parseXMLProject(ev.target.result, file.name);
        let loaded = 0;
        parsed.forEach(svc => {
          const existing = state.services.findIndex(s => s.name === svc.name);
          if (existing >= 0) {
            state.services[existing] = svc;
          } else {
            state.services.push(svc);
          }
          state.openSvcs[svc.id] = true;
          state.selectedSvcId = svc.id;
          loaded++;
        });
        renderTree(); renderEditor(); renderCode();
        showNotif(`✓ Loaded ${loaded} service(s) from ${file.name}`);
      } catch(err) {
        showNotif(`Error: ${err.message}`, true);
        console.error('XML import error:', err);
      }
    };
    reader.readAsText(file);
  };
  inp.click();
}

// Parses a project XML file — handles both:
//   • Consolidated: <gatt name="PROJECT"> with multiple <service> children
//   • Legacy:       <gatt name="SVC"> with a single <service> child
function parseXMLProject(xmlText, filename) {
  const parser = new DOMParser();
  const doc = parser.parseFromString(xmlText, 'application/xml');

  const parseErr = doc.querySelector('parsererror');
  if (parseErr) throw new Error('Invalid XML');

  const svcEls = Array.from(doc.querySelectorAll('gatt > service'));
  if (!svcEls.length) throw new Error('No <service> elements found inside <gatt>');

  const gattEl = doc.querySelector('gatt');

  // ── Base UUID ──────────────────────────────────────────────────────────────
  // Primary: read the dedicated base-uuid attribute on <gatt> (new consolidated format)
  // Fallback: infer from first service UUID suffix (legacy per-service files)
  const baseUUIDAttr = gattEl?.getAttribute('base-uuid') || '';
  let uuidParts = baseUUIDAttr ? baseUUIDAttr.split('-') : [];

  if (uuidParts.length !== 5) {
    // Legacy fallback — derive from first service UUID
    const firstUUID = svcEls[0].getAttribute('uuid') || '';
    uuidParts = firstUUID.split('-');
  }

  if (uuidParts.length === 5) {
    // Always overwrite from the file — it is the authoritative source on import
    state.baseUUID.p2 = uuidParts[1].toUpperCase(); const e2 = document.getElementById('buuid-p2'); if (e2) e2.value = state.baseUUID.p2;
    state.baseUUID.p3 = uuidParts[2].toUpperCase(); const e3 = document.getElementById('buuid-p3'); if (e3) e3.value = state.baseUUID.p3;
    state.baseUUID.p4 = uuidParts[3].toUpperCase(); const e4 = document.getElementById('buuid-p4'); if (e4) e4.value = state.baseUUID.p4;
    state.baseUUID.p5 = uuidParts[4].replace(/-/g,'').toUpperCase(); const e5 = document.getElementById('buuid-p5'); if (e5) e5.value = state.baseUUID.p5;
    refreshBaseUUIDDisplay();
  }

  // Auto-populate project name from <gatt name="..."> if not already set
  const gattName = gattEl?.getAttribute('name') || '';
  if (gattName && !state.profileMeta.project && svcEls.length > 1) {
    state.profileMeta.project = gattName;
    const el = document.getElementById('profile-project');
    if (el) el.value = gattName;
  }

  // Auto-populate author from <gatt author="...">
  const gattAuthor = gattEl?.getAttribute('author') || '';
  if (gattAuthor) {
    state.profileMeta.author = gattAuthor;
    const el = document.getElementById('profile-author');
    if (el) el.value = gattAuthor;
  }

  // Restore use-generic-cbs from <gatt use-generic-cbs="true">
  const useGenericCbs = gattEl?.getAttribute('use-generic-cbs') === 'true';
  state.useGenericCbs = useGenericCbs;
  const cbEl = document.getElementById('profile-generic-cbs');
  if (cbEl) cbEl.checked = useGenericCbs;

  // Restore domains from <domains><domain name="..." hex="..."/></domains>
  const domainsEl = doc.querySelector('gatt > domains');
  if (domainsEl) {
    const domainEls = Array.from(domainsEl.querySelectorAll('domain'));
    if (domainEls.length) {
      state.domains = domainEls.map((d, i) => ({
        id: nextDomainId++,
        name: d.getAttribute('name') || 'MY_DOMAIN',
        hex:  d.getAttribute('hex')  || '0x01',
      }));
      renderDomainList();
    }
  }

  return svcEls.map(svcEl => parseXMLServiceEl(svcEl));
}

function parseXMLServiceEl(svcEl) {
  const svcName = svcEl.getAttribute('name') || 'Unnamed Service';
  const advertise = svcEl.getAttribute('advertise') !== 'false';
  const uuidStr = svcEl.getAttribute('uuid') || '';

  const first32 = parseInt((uuidStr.split('-')[0] || '01010000'), 16);
  const domainNum = (first32 >>> 24) & 0xFF;
  const svcIdNum  = (first32 >>> 16) & 0xFF;
  const serviceId = '0x' + svcIdNum.toString(16).padStart(2,'0').toUpperCase();

  const macro = toMacroName(svcName);
  const id = nextSvcId++;

  const charEls = Array.from(svcEl.querySelectorAll('characteristic'));
  const chars = charEls.map((el, idx) => {
    const cName    = el.getAttribute('name') || `Characteristic ${idx+1}`;
    const cUUID    = el.getAttribute('uuid') || '';
    const propsStr = el.getAttribute('properties') || '';
    const permsStr = el.getAttribute('permissions') || '';

    const cFirst32  = parseInt((cUUID.split('-')[0] || '00000000'), 16);
    const charIdNum = cFirst32 & 0xFFFF;
    const charIdHex = '0x' + charIdNum.toString(16).padStart(4,'0').toUpperCase();

    const props = normProps({
      broadcast:   propsStr.includes('broadcast'),
      read:        propsStr.includes('read'),
      write:       propsStr.includes('write') && !propsStr.includes('write_no_resp'),
      writeNoResp: propsStr.includes('write_no_resp'),
      notify:      propsStr.includes('notify'),
      indicate:    propsStr.includes('indicate'),
      auth:        propsStr.includes('auth'),
      extProp:     propsStr.includes('ext_prop'),
    });
    const perms = {
      read:         permsStr.includes('read')          && !permsStr.includes('read_encrypt') && !permsStr.includes('read_authen') && !permsStr.includes('read_lesc'),
      readEncrypt:  permsStr.includes('read_encrypt'),
      readAuthen:   permsStr.includes('read_authen'),
      readLesc:     permsStr.includes('read_lesc'),
      write:        permsStr.includes('write')         && !permsStr.includes('write_encrypt') && !permsStr.includes('write_authen') && !permsStr.includes('prepare_write') && !permsStr.includes('write_lesc'),
      writeEncrypt: permsStr.includes('write_encrypt'),
      writeAuthen:  permsStr.includes('write_authen'),
      prepareWrite: permsStr.includes('prepare_write'),
      writeLesc:    permsStr.includes('write_lesc'),
    };

    const valEl = el.querySelector('value');
    const varLength = valEl?.getAttribute('variable_length') === 'true';
    const length    = parseInt(
      varLength
        ? (valEl?.getAttribute('max_length') || '1')
        : (valEl?.getAttribute('length')     || '1')
    ) || 1;

    const camel    = toCamelCase(cName);
    const cbSuffix = (props.write || props.writeNoResp || props.auth) && !props.read ? 'Write' : 'Read';
    const defaultCallback = `st_${camel.charAt(0).toUpperCase()}${camel.slice(1)}${cbSuffix}`;
    const defaultVarName  = (varLength || length > 1) ? `su8ar_${camel}` : `su8_${camel}`;

    const varName  = el.getAttribute('var_name')  || defaultVarName;
    const callback = el.getAttribute('callback')  || defaultCallback;
    const cud      = el.getAttribute('cud')       || cName;
    const brief    = el.getAttribute('brief')     || '';
    const isPointerAttr = el.getAttribute('is_pointer');
    // If attribute present, use it; otherwise fall back to old length-based inference
    const isPointer = isPointerAttr !== null
      ? isPointerAttr === 'true'
      : (varLength || length > 1);

    return {
      id: nextCharId++,
      name: cName, varName, charIdHex, charIdNum,
      props, perms: normPerms(perms), length, varLength,
      cud, callback, brief, isPointer,
      customReadCb:  el.getAttribute('custom_read_cb')  || '',
      customWriteCb: el.getAttribute('custom_write_cb') || '',
    };
  });

  const matchedDomain = state.domains.find(d => parseInt(d.hex) === domainNum) || state.domains[0];
  return {
    id,
    name: svcName,
    varName: svcEl.getAttribute('var_name') || `sstar_${toCamelCase(svcName)}Svc`,
    brief: svcEl.getAttribute('brief') || '',
    advertise,
    domainId: matchedDomain.id,
    domain: matchedDomain.hex,
    serviceId,
    serviceIdNum: svcIdNum,
    uuid: uuidStr,
    partUuidDomain: `PART_UUID_DOMAIN_${matchedDomain.name}`,
    partUuidService: `PART_UUID_SERVICE_${macro}`,
    chars,
  };
}
