// ═══════════════════════════════════════════════════════
// DOMAIN MANAGEMENT
// ═══════════════════════════════════════════════════════
function renderDomainList() {
  const el = document.getElementById('domain-list');
  if (!el) return;
  el.innerHTML = state.domains.map(d => `
    <div class="domain-entry" data-domain-id="${d.id}">
      <input class="domain-input-name" maxlength="20"
        value="${d.name}" placeholder="NAME"
        oninput="updateDomain(${d.id},'name',this.value)"
        title="Domain macro name (e.g. MY_DOMAIN)"/>
      <span class="domain-sep">=</span>
      <input class="domain-input-hex" maxlength="6"
        value="${d.hex}" placeholder="0x01"
        oninput="updateDomain(${d.id},'hex',this.value)"
        title="Domain hex value"/>
      ${state.domains.length > 1
        ? `<span class="domain-delete" onclick="deleteDomain(${d.id})" title="Delete domain">✕</span>`
        : ''}
    </div>`).join('');
}

function addDomain() {
  const id = nextDomainId++;
  state.domains.push({ id, name: 'NEW_DOMAIN', hex: '0x02' });
  renderDomainList();
  renderEditor(); renderCode();
}

function updateDomain(id, key, val) {
  const d = state.domains.find(x => x.id === id);
  if (!d) return;

  if (key === 'name') {
    const cleaned = val.replace(/[^A-Za-z0-9_]/g,'').toUpperCase();
    d.name = cleaned;
    // Patch the input value in-place so the cursor stays put
    const inp = document.querySelector(`#domain-list [data-domain-id="${id}"] .domain-input-name`);
    if (inp && inp !== document.activeElement) inp.value = cleaned;
  } else if (key === 'hex') {
    const cleaned = val.replace(/[^0-9a-fA-FxX]/g,'');
    d.hex = cleaned;
    const inp = document.querySelector(`#domain-list [data-domain-id="${id}"] .domain-input-hex`);
    if (inp && inp !== document.activeElement) inp.value = cleaned;
  }

  // Update partUuidDomain on all services that use this domain
  state.services.forEach(svc => {
    if (svc.domainId === id) {
      svc.domain = d.hex;
      svc.partUuidDomain = `PART_UUID_DOMAIN_${d.name}`;
    }
  });
  // Do NOT call renderDomainList() here — it would destroy focus
  renderEditor(); renderCode();
}

function deleteDomain(id) {
  if (state.domains.length <= 1) return;
  state.domains = state.domains.filter(d => d.id !== id);
  // Reassign services using deleted domain to first remaining domain
  const fallback = state.domains[0];
  state.services.forEach(svc => {
    if (svc.domainId === id) {
      svc.domainId = fallback.id;
      svc.domain = fallback.hex;
      svc.partUuidDomain = `PART_UUID_DOMAIN_${fallback.name}`;
    }
  });
  renderDomainList();
  renderEditor(); renderCode();
}

function selectSvcDomain(svcId, domainIdStr) {
  const svc = getSvc(svcId);
  if (!svc) return;
  const domId = parseInt(domainIdStr);
  const d = state.domains.find(x => x.id === domId);
  if (!d) return;
  svc.domainId = domId;
  svc.domain = d.hex;
  svc.partUuidDomain = `PART_UUID_DOMAIN_${d.name}`;
  const uEl = document.getElementById(`svc-uuid-${svcId}`);
  if (uEl) uEl.textContent = computeUUID(svc, 0);
  renderTree(); renderCode();
}
