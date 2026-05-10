// ═══════════════════════════════════════════════════════
// RENDER EDITOR
// ═══════════════════════════════════════════════════════
function renderEditor() {
  const es = document.getElementById('emptyState');
  const ed = document.getElementById('serviceEditor');
  const svc = getSvc(state.selectedSvcId);
  if (!svc) { es.style.display='flex'; ed.style.display='none'; return; }
  es.style.display='none'; ed.style.display='block';

  const svcUUID = computeUUID(svc, 0);
  const macroSvc = toMacroName(svc.name);

  let html = `
    <div class="section-divider">
      <div class="section-divider-line"></div>
      <div class="section-divider-label">Service</div>
      <div class="section-divider-line"></div>
    </div>

    <div class="form-card">
      <div class="form-card-header">
        <span style="font-size:18px;">⬡</span>
        <div>
          <div class="form-card-title">${svc.name}</div>
          <div class="form-card-subtitle">GATT Primary Service</div>
        </div>
        <div style="margin-left:auto; display:flex; gap:8px;">
          <button class="btn btn-danger" onclick="deleteSvc(${svc.id})">✕ Delete</button>
        </div>
      </div>
      <div class="form-card-body">
        <div class="field-grid" style="margin-bottom:12px;">
          <div class="field">
            <label>Service Name</label>
            <input type="text" value="${svc.name}" oninput="updateSvc(${svc.id},'name',this.value)" placeholder="e.g. Device Provisioning"/>
          </div>
          <div class="field">
            <label>Variable Name</label>
            <input type="text" value="${svc.varName}" oninput="updateSvc(${svc.id},'varName',this.value)" placeholder="e.g. sstar_mySvc"/>
          </div>
          <div class="field">
            <label>Service ID (hex)</label>
            <input type="text" value="${svc.serviceId}" oninput="updateSvc(${svc.id},'serviceId',this.value)" placeholder="0x01"/>
          </div>
          <div class="field">
            <label>Domain</label>
            <select onchange="selectSvcDomain(${svc.id},this.value)">
              ${state.domains.map(d=>`<option value="${d.id}" ${d.id===svc.domainId?'selected':''}>${d.name} (${d.hex})</option>`).join('')}
            </select>
          </div>
        </div>
        <div class="field" style="margin-bottom:12px;">
          <label>Service UUID (auto-computed)</label>
          <div class="uuid-display">
            <span id="svc-uuid-${svc.id}">${svcUUID}</span>
            <span class="uuid-copy" onclick="copyUUID('svc-uuid-${svc.id}')">Copy</span>
          </div>
        </div>
        <div class="field" style="margin-bottom:12px;">
          <label>Brief</label>
          <textarea rows="3" placeholder="Describe what this service does…" oninput="updateSvc(${svc.id},'brief',this.value)">${svc.brief||''}</textarea>
        </div>
        <div style="margin-top:12px;">
          <label class="checkbox-row">
            <input type="checkbox" ${svc.advertise?'checked':''} onchange="updateSvc(${svc.id},'advertise',this.checked)"/>
            <span class="checkbox-box"></span>
            <span class="checkbox-label">Advertise this service</span>
          </label>
        </div>
      </div>
    </div>

    <div class="section-divider">
      <div class="section-divider-line"></div>
      <div class="section-divider-label">Characteristics (${svc.chars.length})</div>
      <div class="section-divider-line"></div>
    </div>`;

  svc.chars.forEach((c, idx) => {
    const pk = getPropKey(c);
    const charUUID = computeUUID(svc, c.charIdNum);
    const isOpen = state.openChars[c.id];
    const propDefs = [
      {k:'broadcast',  lbl:'Broadcast',      cls:'active-broadcast', tip: PROP_TIPS.broadcast},
      {k:'read',       lbl:'Read',            cls:'active-read',      tip: PROP_TIPS.read},
      {k:'write',      lbl:'Write',           cls:'active-write',     tip: PROP_TIPS.write},
      {k:'writeNoResp',lbl:'Write w/o Resp',  cls:'active-write-nr',  tip: PROP_TIPS.writeNoResp},
      {k:'notify',     lbl:'Notify',          cls:'active-notify',    tip: PROP_TIPS.notify},
      {k:'indicate',   lbl:'Indicate',        cls:'active-indicate',  tip: PROP_TIPS.indicate},
      {k:'auth',       lbl:'Auth ⚠',          cls:'active-auth',      tip: PROP_TIPS.auth},
      {k:'extProp',    lbl:'Ext Prop',        cls:'active-ext-prop',  tip: PROP_TIPS.extProp},
    ];
    const propBtns = propDefs.map(d=>`
      <div class="prop-toggle ${c.props[d.k]?d.cls:''}" onclick="toggleProp(${svc.id},${c.id},'${d.k}')" data-tooltip="${d.tip.replace(/"/g,'&quot;')}">
        <input type="checkbox" ${c.props[d.k]?'checked':''} onclick="event.stopPropagation()"/>
        ${d.lbl}
      </div>`).join('');

    html += `
    <div class="char-card" id="charcard-${c.id}">
      <div class="char-card-header" onclick="toggleChar(${c.id})">
        <span class="char-card-name">${c.name}</span>
        <div class="tag-strip">
          ${c.props.broadcast?'<span class="char-prop-tag" style="background:rgba(230,126,34,0.12);color:#e67e22;">BCAST</span>':''}
          ${c.props.read?'<span class="char-prop-tag tag-read">READ</span>':''}
          ${c.props.write?'<span class="char-prop-tag tag-write">WRITE</span>':''}
          ${c.props.writeNoResp?'<span class="char-prop-tag" style="background:rgba(93,173,226,0.12);color:#5dade2;">WRITE NR</span>':''}
          ${c.props.notify?'<span class="char-prop-tag tag-notify">NOTIFY</span>':''}
          ${c.props.indicate?'<span class="char-prop-tag tag-rw">INDICATE</span>':''}
          ${c.props.auth?'<span class="char-prop-tag" style="background:rgba(231,76,60,0.12);color:var(--red);">AUTH ⚠</span>':''}
          ${c.props.extProp?'<span class="char-prop-tag" style="background:rgba(149,165,166,0.12);color:#95a5a6;">EXT</span>':''}
        </div>
        <span style="color:var(--text-dim);margin-left:8px;font-size:11px;">${isOpen?'▲':'▼'}</span>
      </div>
      <div class="char-card-body ${isOpen?'open':''}">
        <div class="field-grid" style="margin-bottom:12px;">
          <div class="field">
            <label>Characteristic Name</label>
            <input type="text" value="${c.name}" oninput="updateChar(${svc.id},${c.id},'name',this.value)"/>
          </div>
          ${!state.useGenericCbs ? `
          <div class="field">
            <label>User Data Variable</label>
            <input type="text" value="${c.varName}" oninput="updateChar(${svc.id},${c.id},'varName',this.value)"/>
          </div>` : ''}
          <div class="field">
            <label>Char ID (hex)</label>
            <input type="text" value="${c.charIdHex}" oninput="updateChar(${svc.id},${c.id},'charIdHex',this.value);updateChar(${svc.id},${c.id},'charIdNum',parseInt(this.value))"/>
          </div>
          ${!state.useGenericCbs ? `
          <div class="field">
            <label>Callback Function</label>
            <input type="text" value="${c.callback}" oninput="updateChar(${svc.id},${c.id},'callback',this.value)"/>
          </div>` : ''}
          <div class="field">
            <label>Data Length (bytes)</label>
            <input type="number" value="${c.length}" min="1" max="512" oninput="updateChar(${svc.id},${c.id},'length',parseInt(this.value)||1)"/>
          </div>
          <div class="field" style="justify-content:flex-end;">
            <label class="checkbox-row">
              <input type="checkbox" ${c.varLength?'checked':''} onchange="updateChar(${svc.id},${c.id},'varLength',this.checked)"/>
              <span class="checkbox-box"></span>
              <span class="checkbox-label">Variable length</span>
            </label>
          </div>
          ${!state.useGenericCbs ? `
          <div class="field" style="justify-content:flex-end;">
            <label class="checkbox-row">
              <input type="checkbox" ${c.isPointer?'checked':''} onchange="updateChar(${svc.id},${c.id},'isPointer',this.checked)"/>
              <span class="checkbox-box"></span>
              <span class="checkbox-label">User data is pointer</span>
            </label>
          </div>` : ''}
        </div>

        <div class="field" style="margin-bottom:12px;">
          <label>Characteristic UUID (auto-computed)</label>
          <div class="uuid-display">
            <span id="char-uuid-${c.id}">${charUUID}</span>
            <span class="uuid-copy" onclick="copyUUID('char-uuid-${c.id}')">Copy</span>
          </div>
        </div>

        <div class="field" style="margin-bottom:12px;">
          <label>CUD String (User Description)</label>
          <input type="text" value="${c.cud}" oninput="updateChar(${svc.id},${c.id},'cud',this.value)"/>
        </div>

        ${!state.useGenericCbs ? `
        <div class="field" style="margin-bottom:12px;">
          <label>Brief</label>
          <textarea rows="3" placeholder="Describe what this callback should do…" oninput="updateChar(${svc.id},${c.id},'brief',this.value)">${c.brief||''}</textarea>
        </div>` : ''}

        ${state.useGenericCbs ? `
        <div style="margin-bottom:12px; padding:8px 10px; border-radius:4px; background:var(--green-dim); border:1px solid var(--green); font-size:11px; color:var(--green); line-height:1.5;">
          ⚙ Generic callbacks mode — this characteristic will use
          <code>gt_GATT_GenericRead</code> / <code>gt_GATT_GenericWrite</code> with an auto-generated
          <code>GATTCharDescriptor_T</code>. No per-characteristic stub will be emitted.
        </div>` : ''}

        ${state.useGenericCbs ? `
        <div class="field-grid" style="margin-bottom:12px;">
          ${c.props.read ? `
          <div class="field">
            <label>Custom Read Hook <span style='color:var(--text-dim);font-weight:400;font-size:10px;'>(optional — fpt_customReadCb)</span></label>
            <input type='text' value='${c.customReadCb||''}'
              placeholder='e.g. st_OnMyCharRead'
              oninput="updateChar(${svc.id},${c.id},'customReadCb',this.value)"/>
          </div>` : ''}
          ${(c.props.write || c.props.writeNoResp) ? `
          <div class="field">
            <label>Custom Write Hook <span style='color:var(--text-dim);font-weight:400;font-size:10px;'>(optional — fpt_customWriteCb)</span></label>
            <input type='text' value='${c.customWriteCb||''}'
              placeholder='e.g. st_OnMyCharWrite'
              oninput="updateChar(${svc.id},${c.id},'customWriteCb',this.value)"/>
          </div>` : ''}
        </div>` : ''}

        <div class="field" style="margin-bottom:12px;">
          <label>Properties</label>
          <div class="prop-grid">${propBtns}</div>
        </div>

        <div class="field" style="margin-bottom:12px;">
          <label>Permissions</label>
          <div style="display:flex;flex-direction:column;gap:6px;">
            <div style="font-size:9px;font-weight:600;text-transform:uppercase;letter-spacing:0.07em;color:var(--text-dim);">Read-side</div>
            <div class="prop-grid">
              ${[
                ['read',        'Read',         PERM_TIPS.read],
                ['readEncrypt', 'Read Encrypt',  PERM_TIPS.readEncrypt],
                ['readAuthen',  'Read Authen',   PERM_TIPS.readAuthen],
                ['readLesc',    'Read LESC',     PERM_TIPS.readLesc],
              ].map(([k,lbl,tip]) => `<div class="prop-toggle ${c.perms[k]?'active-perm-read':''}" onclick="togglePerm(${svc.id},${c.id},'${k}')" data-tooltip="${tip.replace(/"/g,'&quot;')}"><input type="checkbox" ${c.perms[k]?'checked':''} onclick="event.stopPropagation()"/>${lbl}</div>`).join('')}
            </div>
            <div style="font-size:9px;font-weight:600;text-transform:uppercase;letter-spacing:0.07em;color:var(--text-dim);margin-top:2px;">Write-side</div>
            <div class="prop-grid">
              ${[
                ['write',        'Write',         PERM_TIPS.write],
                ['writeEncrypt', 'Write Encrypt',  PERM_TIPS.writeEncrypt],
                ['writeAuthen',  'Write Authen',   PERM_TIPS.writeAuthen],
                ['prepareWrite', 'Prepare Write',  PERM_TIPS.prepareWrite],
                ['writeLesc',    'Write LESC',     PERM_TIPS.writeLesc],
              ].map(([k,lbl,tip]) => `<div class="prop-toggle ${c.perms[k]?'active-perm-write':''}" onclick="togglePerm(${svc.id},${c.id},'${k}')" data-tooltip="${tip.replace(/"/g,'&quot;')}"><input type="checkbox" ${c.perms[k]?'checked':''} onclick="event.stopPropagation()"/>${lbl}</div>`).join('')}
            </div>
          </div>
        </div>

        <div class="char-card-actions">
          <button class="btn btn-danger" onclick="deleteChar(${svc.id},${c.id})">✕ Delete</button>
        </div>
      </div>
    </div>`;
  });

  html += `<div class="add-svc-btn" style="margin:0;" onclick="openAddChar(${svc.id})">+ Add Characteristic</div>`;
  ed.innerHTML = html;
}

// ═══════════════════════════════════════════════════════
// UPDATE STATE
// ═══════════════════════════════════════════════════════
function updateSvc(id, key, val) {
  const svc = getSvc(id);
  if (!svc) return;
  svc[key] = val;
  if (key === 'name') svc.partUuidService = 'PART_UUID_SERVICE_' + toMacroName(val);
  // update uuid display
  const el = document.getElementById(`svc-uuid-${id}`);
  if (el) el.textContent = computeUUID(svc, 0);
  renderTree();
  renderCode();
}
function updateChar(svcId, charId, key, val) {
  const svc = getSvc(svcId);
  const c = getChar(svc, charId);
  if (!c) return;
  c[key] = val;
  if (key === 'charIdHex') { c.charIdNum = parseInt(val) || 0; }
  if (key === 'name') { c.cud = val; c.callback = autoCallback(c); }
  const el = document.getElementById(`char-uuid-${charId}`);
  if (el) el.textContent = computeUUID(svc, c.charIdNum);
  renderTree();
  renderCode();
}
function autoCallback(c) {
  const pk = getPropKey(c);
  const name = toCamelCase(c.name);
  const isWriteSide = pk === 'write' || pk === 'rw' || c.props.auth;
  return `st_${name.charAt(0).toUpperCase()+name.slice(1)}${isWriteSide ? 'Write' : 'Read'}`;
}
function toggleProp(svcId, charId, prop) {
  const svc = getSvc(svcId);
  const c = getChar(svc, charId);
  if (!c) return;
  c.props[prop] = !c.props[prop];
  // regenerate callback name to match the new property
  c.callback = autoCallback(c);
  // Clear custom hooks whose property side is no longer active
  if (!c.props.read) c.customReadCb = '';
  if (!c.props.write && !c.props.writeNoResp && !c.props.auth) c.customWriteCb = '';
  renderEditor();
  renderTree();
  renderCode();
}
function togglePerm(svcId, charId, perm) {
  const svc = getSvc(svcId);
  const c = getChar(svc, charId);
  if (!c) return;
  c.perms[perm] = !c.perms[perm];
  renderEditor();
  renderTree();
  renderCode();
}
function toggleChar(id) {
  state.openChars[id] = !state.openChars[id];
  renderEditor();
}
function deleteSvc(id) {
  if (!confirm(`Delete service "${getSvc(id)?.name}"?`)) return;
  state.services = state.services.filter(s=>s.id!==id);
  if (state.selectedSvcId===id) { state.selectedSvcId=null; state.selectedCharId=null; }
  renderTree(); renderEditor(); renderCode();
}
function deleteChar(svcId, charId) {
  const svc = getSvc(svcId);
  if (!svc) return;
  svc.chars = svc.chars.filter(c=>c.id!==charId);
  if (state.selectedCharId===charId) state.selectedCharId=null;
  renderTree(); renderEditor(); renderCode();
}
