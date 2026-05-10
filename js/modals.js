// ═══════════════════════════════════════════════════════
// TOOLTIP DEFINITIONS
// ═══════════════════════════════════════════════════════
const PROP_TIPS = {
  broadcast:    'BT_GATT_CHRC_BROADCAST\nPermits broadcasts of the Characteristic Value using Server Characteristic Configuration Descriptor.',
  read:         'BT_GATT_CHRC_READ\nPermits reads of the Characteristic Value.',
  write:        'BT_GATT_CHRC_WRITE\nPermits writes of the Characteristic Value with a response (acknowledgment) from the server.',
  writeNoResp:  'BT_GATT_CHRC_WRITE_WITHOUT_RESP\nPermits writes of the Characteristic Value without a response. Faster but no delivery confirmation.',
  notify:       'BT_GATT_CHRC_NOTIFY\nPermits server-initiated notifications of the Characteristic Value without acknowledgment.',
  indicate:     'BT_GATT_CHRC_INDICATE\nPermits server-initiated indications of the Characteristic Value with acknowledgment.',
  auth:         'BT_GATT_CHRC_AUTH (Deprecated)\nPermits signed writes to the Characteristic Value. This property is deprecated in Zephyr.',
  extProp:      'BT_GATT_CHRC_EXT_PROP\nAdditional properties are defined in the Characteristic Extended Properties Descriptor.',
};
const PERM_TIPS = {
  read:         'BT_GATT_PERM_READ\nAttribute can be read. No encryption required.',
  readEncrypt:  'BT_GATT_PERM_READ_ENCRYPT\nAttribute read requires an encrypted link (unauthenticated pairing is sufficient).',
  readAuthen:   'BT_GATT_PERM_READ_AUTHEN\nAttribute read requires encryption using an authenticated link-key (MITM protection).',
  readLesc:     'BT_GATT_PERM_READ_LESC\nAttribute read requires LE Secure Connections (stronger key generation than legacy pairing).',
  write:        'BT_GATT_PERM_WRITE\nAttribute can be written. No encryption required.',
  writeEncrypt: 'BT_GATT_PERM_WRITE_ENCRYPT\nAttribute write requires an encrypted link (unauthenticated pairing is sufficient).',
  writeAuthen:  'BT_GATT_PERM_WRITE_AUTHEN\nAttribute write requires encryption using an authenticated link-key (MITM protection).',
  prepareWrite: 'BT_GATT_PERM_PREPARE_WRITE\nAllows prepare writes. Enables BT_GATT_WRITE_FLAG_PREPARE in the write callback for long-write sequences.',
  writeLesc:    'BT_GATT_PERM_WRITE_LESC\nAttribute write requires LE Secure Connections (stronger key generation than legacy pairing).',
};

// ═══════════════════════════════════════════════════════
// ADD SERVICE MODAL
// ═══════════════════════════════════════════════════════
function openAddService() {
  document.getElementById('modalTitle').textContent = 'Add New Service';
  document.getElementById('modalBody').innerHTML = `
    <div style="display:flex;flex-direction:column;gap:12px;">
      <div class="field">
        <label>Service Name</label>
        <input type="text" id="newSvcName" placeholder="e.g. My Custom Service"/>
      </div>
      <div class="field-grid">
        <div class="field">
          <label>Service ID (hex)</label>
          <input type="text" id="newSvcId" placeholder="0x03"/>
        </div>
        <div class="field">
          <label>Domain (hex)</label>
          <input type="text" id="newSvcDomain" value="0x01"/>
        </div>
      </div>
      <label class="checkbox-row">
        <input type="checkbox" id="newSvcAdv" checked/>
        <span class="checkbox-box"></span>
        <span class="checkbox-label">Advertise this service</span>
      </label>
    </div>`;
  document.getElementById('modalFooter').innerHTML = `
    <button class="btn btn-ghost" onclick="closeModalDirect()">Cancel</button>
    <button class="btn btn-primary" onclick="confirmAddService()">Add Service</button>`;
  document.getElementById('modalBackdrop').style.display='flex';
}
function confirmAddService() {
  const name = document.getElementById('newSvcName').value.trim();
  const svcId = document.getElementById('newSvcId').value.trim() || '0x03';
  const domain = document.getElementById('newSvcDomain').value.trim() || '0x01';
  const adv = document.getElementById('newSvcAdv').checked;
  if (!name) { alert('Please enter a service name.'); return; }
  const id = nextSvcId++;
  const macro = toMacroName(name);
  const firstDomain = state.domains[0] || { id:1, name:'MY_DOMAIN', hex:'0x01' };
  const newSvc = {
    id, name, varName: `sstar_${toCamelCase(name)}Svc`,
    brief: '',
    advertise: adv,
    domainId: firstDomain.id,
    domain: firstDomain.hex,
    serviceId: svcId,
    serviceIdNum: parseInt(svcId)||0,
    uuid: '',
    partUuidDomain: `PART_UUID_DOMAIN_${firstDomain.name}`,
    partUuidService: `PART_UUID_SERVICE_${macro}`,
    chars: []
  };
  newSvc.uuid = computeUUID(newSvc, 0);
  state.services.push(newSvc);
  state.selectedSvcId = id; state.openSvcs[id] = true;
  closeModalDirect(); renderTree(); renderEditor(); renderCode();
  showNotif('Service added');
}

// ═══════════════════════════════════════════════════════
// ADD CHAR MODAL
// ═══════════════════════════════════════════════════════
// Modal prop state — tracks toggles independently of DOM classes
let _modalProps = {broadcast: false, read: false, write: false, writeNoResp: false, notify: false, indicate: false, auth: false, extProp: false};
let _modalPerms = {read: false, write: false, readEncrypt: false, writeEncrypt: false, readAuthen: false, writeAuthen: false, prepareWrite: false, readLesc: false, writeLesc: false};

function _toggleModalPerm(perm) {
  _modalPerms[perm] = !_modalPerms[perm];
  const side = perm.startsWith('read') || perm === 'prepareWrite' ? 'perm-read' : 'perm-write';
  const el = document.getElementById('pm_' + perm);
  if (el) el.classList.toggle('active-' + side, _modalPerms[perm]);
}

function _toggleModalProp(prop) {
  _modalProps[prop] = !_modalProps[prop];
  const map = {broadcast:'active-broadcast', read:'active-read', write:'active-write', writeNoResp:'active-write-nr', notify:'active-notify', indicate:'active-indicate', auth:'active-auth', extProp:'active-ext-prop'};
  const el = document.getElementById('pt_' + prop);
  if (el) el.classList.toggle(map[prop], _modalProps[prop]);
}

function openAddChar(svcId) {
  const svc = getSvc(svcId);
  const nextNum = (svc.chars.length + 1);
  _modalProps = {broadcast: false, read: false, write: false, writeNoResp: false, notify: false, indicate: false, auth: false, extProp: false};
  _modalPerms = {read: false, write: false, readEncrypt: false, writeEncrypt: false, readAuthen: false, writeAuthen: false, prepareWrite: false, readLesc: false, writeLesc: false};
  document.getElementById('modalTitle').textContent = 'Add Characteristic';
  document.getElementById('modalBody').innerHTML = `
    <div style="display:flex;flex-direction:column;gap:12px;">
      <div class="field">
        <label>Characteristic Name</label>
        <input type="text" id="newCharName" placeholder="e.g. Sensor Data"/>
      </div>
      <div class="field-grid">
        <div class="field">
          <label>Char ID (hex)</label>
          <input type="text" id="newCharId" value="0x${nextNum.toString(16).padStart(4,'0').toUpperCase()}"/>
        </div>
        <div class="field">
          <label>Data Length</label>
          <input type="number" id="newCharLen" value="1" min="1" max="512"/>
        </div>
        <div class="field" style="justify-content:flex-end;">
          <label class="checkbox-row">
            <input type="checkbox" id="newCharVarLen"/>
            <span class="checkbox-box"></span>
            <span class="checkbox-label">Variable length</span>
          </label>
        </div>
        ${!state.useGenericCbs ? `
        <div class="field" style="justify-content:flex-end;">
          <label class="checkbox-row">
            <input type="checkbox" id="newCharIsPointer"/>
            <span class="checkbox-box"></span>
            <span class="checkbox-label">User data is pointer</span>
          </label>
        </div>` : ''}
      </div>
      <div class="field">
        <label>Properties</label>
        <div class="prop-grid">
          <div class="prop-toggle" id="pt_read"        data-tooltip="${PROP_TIPS.read}"        onclick="_toggleModalProp('read')">Read</div>
          <div class="prop-toggle" id="pt_write"       data-tooltip="${PROP_TIPS.write}"       onclick="_toggleModalProp('write')">Write</div>
          <div class="prop-toggle" id="pt_writeNoResp" data-tooltip="${PROP_TIPS.writeNoResp}" onclick="_toggleModalProp('writeNoResp')">Write w/o Resp</div>
          <div class="prop-toggle" id="pt_notify"      data-tooltip="${PROP_TIPS.notify}"      onclick="_toggleModalProp('notify')">Notify</div>
          <div class="prop-toggle" id="pt_indicate"    data-tooltip="${PROP_TIPS.indicate}"    onclick="_toggleModalProp('indicate')">Indicate</div>
          <div class="prop-toggle" id="pt_broadcast"   data-tooltip="${PROP_TIPS.broadcast}"   onclick="_toggleModalProp('broadcast')">Broadcast</div>
          <div class="prop-toggle" id="pt_auth"        data-tooltip="${PROP_TIPS.auth}"        onclick="_toggleModalProp('auth')">Auth ⚠</div>
          <div class="prop-toggle" id="pt_extProp"     data-tooltip="${PROP_TIPS.extProp}"     onclick="_toggleModalProp('extProp')">Ext Prop</div>
        </div>
      </div>
      ${!state.useGenericCbs ? `
      <div class="field">
        <label>User Data Variable</label>
        <input type="text" id="newCharVar" placeholder="e.g. su8_myVar"/>
      </div>` : ''}
      <div class="field">
        <label>Permissions</label>
        <div style="display:flex;flex-direction:column;gap:6px;">
          <div style="font-size:9px;font-weight:600;text-transform:uppercase;letter-spacing:0.07em;color:var(--text-dim);">Read-side</div>
          <div class="prop-grid">
            <div class="prop-toggle" id="pm_read"        data-tooltip="${PERM_TIPS.read}"         onclick="_toggleModalPerm('read')">Read</div>
            <div class="prop-toggle" id="pm_readEncrypt"  data-tooltip="${PERM_TIPS.readEncrypt}"  onclick="_toggleModalPerm('readEncrypt')">Read Encrypt</div>
            <div class="prop-toggle" id="pm_readAuthen"   data-tooltip="${PERM_TIPS.readAuthen}"   onclick="_toggleModalPerm('readAuthen')">Read Authen</div>
            <div class="prop-toggle" id="pm_readLesc"     data-tooltip="${PERM_TIPS.readLesc}"     onclick="_toggleModalPerm('readLesc')">Read LESC</div>
          </div>
          <div style="font-size:9px;font-weight:600;text-transform:uppercase;letter-spacing:0.07em;color:var(--text-dim);margin-top:2px;">Write-side</div>
          <div class="prop-grid">
            <div class="prop-toggle" id="pm_write"        data-tooltip="${PERM_TIPS.write}"        onclick="_toggleModalPerm('write')">Write</div>
            <div class="prop-toggle" id="pm_writeEncrypt"  data-tooltip="${PERM_TIPS.writeEncrypt}" onclick="_toggleModalPerm('writeEncrypt')">Write Encrypt</div>
            <div class="prop-toggle" id="pm_writeAuthen"   data-tooltip="${PERM_TIPS.writeAuthen}"  onclick="_toggleModalPerm('writeAuthen')">Write Authen</div>
            <div class="prop-toggle" id="pm_prepareWrite"  data-tooltip="${PERM_TIPS.prepareWrite}" onclick="_toggleModalPerm('prepareWrite')">Prepare Write</div>
            <div class="prop-toggle" id="pm_writeLesc"     data-tooltip="${PERM_TIPS.writeLesc}"    onclick="_toggleModalPerm('writeLesc')">Write LESC</div>
          </div>
        </div>
      </div>
    </div>`;
  document.getElementById('modalFooter').innerHTML = `
    <button class="btn btn-ghost" onclick="closeModalDirect()">Cancel</button>
    <button class="btn btn-primary" onclick="confirmAddChar(${svcId})">Add</button>`;
  document.getElementById('modalBackdrop').style.display='flex';
}
function confirmAddChar(svcId) {
  const svc = getSvc(svcId);
  const name = document.getElementById('newCharName').value.trim();
  if (!name) { alert('Enter a name.'); return; }
  const charIdHex = document.getElementById('newCharId').value.trim();
  const charIdNum = parseInt(charIdHex) || 0;
  const length = parseInt(document.getElementById('newCharLen').value)||1;
  const varLen = document.getElementById('newCharVarLen').checked;
  const isPointer = !state.useGenericCbs && (document.getElementById('newCharIsPointer')?.checked || false);
  const varName = state.useGenericCbs
    ? `g${toCamelCase(name)}`
    : (document.getElementById('newCharVar')?.value.trim() || `g${toCamelCase(name)}`);
  const broadcast   = _modalProps.broadcast;
  const read        = _modalProps.read;
  const write       = _modalProps.write;
  const writeNoResp = _modalProps.writeNoResp;
  const notify      = _modalProps.notify;
  const indicate    = _modalProps.indicate;
  const auth        = _modalProps.auth;
  const extProp     = _modalProps.extProp;
  const id = nextCharId++;
  const c = {
    id, name, varName, charIdHex, charIdNum,
    props: normProps({broadcast, read, write, writeNoResp, notify, indicate, auth, extProp}),
    perms: normPerms({
      read:          _modalPerms.read,
      write:         _modalPerms.write,
      readEncrypt:   _modalPerms.readEncrypt,
      writeEncrypt:  _modalPerms.writeEncrypt,
      readAuthen:    _modalPerms.readAuthen,
      writeAuthen:   _modalPerms.writeAuthen,
      prepareWrite:  _modalPerms.prepareWrite,
      readLesc:      _modalPerms.readLesc,
      writeLesc:     _modalPerms.writeLesc,
    }),
    length, varLength: varLen, isPointer,
    cud: name,
    brief: '',
    callback: `st_${name.replace(/\s+/g,'')}${(write||writeNoResp)?'Write':'Read'}`,
    customReadCb: '',
    customWriteCb: '',
  };
  svc.chars.push(c);
  state.openChars[id] = true;
  closeModalDirect(); renderTree(); renderEditor(); renderCode();
  showNotif('Characteristic added');
}
