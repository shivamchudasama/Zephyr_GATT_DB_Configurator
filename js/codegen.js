// ═══════════════════════════════════════════════════════
// CODE GENERATION
// ═══════════════════════════════════════════════════════
// ═══════════════════════════════════════════════════════
// CODE GENERATION CONSTANTS
// ═══════════════════════════════════════════════════════
function getAUTHOR() { return state.profileMeta.author || ''; }
function getORG() { return state.profileMeta.org || state.profileMeta.project || ''; }

function switchTab(t) {
  state.activeTab = t;
  ['h','c','buuid','xml'].forEach(x => {
    document.getElementById(`tab-${x}`).classList.toggle('active', x===t);
  });
  renderCode();
}

function renderCode() {
  // Update CB mode status bar item
  const cbModeEl = document.getElementById('statCbMode');
  if (cbModeEl) {
    cbModeEl.textContent = state.useGenericCbs
      ? 'CB Mode: Generic (GATT_GenericCallbacks.h)'
      : 'CB Mode: Per-Characteristic';
    cbModeEl.style.color = state.useGenericCbs ? 'var(--green)' : 'var(--blue)';
  }
  const pre = document.getElementById('codePreview');
  if (state.activeTab === 'xml') {
    if (state.services.length === 0) {
      pre.innerHTML = '<span style="color:var(--text-dim)">// Add services to preview the consolidated project XML.</span>';
    } else {
      pre.innerHTML = generateXML();
    }
    return;
  }
  if (state.activeTab === 'buuid') {
    pre.innerHTML = generateBaseUUIDs();
    return;
  }
  const svc = getSvc(state.selectedSvcId);
  if (!svc) { pre.innerHTML = '<span style="color:var(--text-dim)">// Select a service to preview generated code.</span>'; return; }
  if (state.activeTab === 'h') pre.innerHTML = generateH(svc);
  else if (state.activeTab === 'c') pre.innerHTML = generateC(svc);
}

function esc(s) { return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;'); }

function generateH(svc) {
  const macro = toMacroName(svc.name);
  // Build PascalCase filename: e.g. MY_SERVICE -> MyService
  function toPascalCase(s) {
    return s.split('_').map((w,i) => {
      if (w.length <= 2) return w; // keep acronyms like OOB, ECU
      return w.charAt(0) + w.slice(1).toLowerCase();
    }).join('');
  }
  const filename = toPascalCase(macro);
  const guard = `_${macro}_H`;
  const svcMacro = `PART_UUID_SERVICE_${macro}`;
  const svcValMacro = `BT_UUID_${macro}_SERVICE_VAL`;
  const svcPtrMacro = `BT_UUID_${macro}_SERVICE`;
  const today = new Date();
  const dateStr = String(today.getDate()).padStart(2,'0') + '/' +
                  String(today.getMonth()+1).padStart(2,'0') + '/' +
                  today.getFullYear();

  let lines = [];
  const kw  = s => `<span class="tok-kw">${s}</span>`;
  const fn  = s => `<span class="tok-fn">${s}</span>`;
  const mac = s => `<span class="tok-mac">${s}</span>`;
  const str = s => `<span class="tok-str">${s}</span>`;
  const num = s => `<span class="tok-num">${s}</span>`;
  const cmt = s => `<span class="tok-cmt">${s}</span>`;
  const typ = s => `<span class="tok-type">${s}</span>`;
  const L = s => lines.push(s);

  // ── Section header: label is centred within the 76-char inner content width ──
  // Total line width: '/*' + 76 chars + '*/' = 80
  const SEP = cmt('/******************************************************************************/');
  const SEC = label => {
    const inner = 76;
    const totalPad = inner - label.length;
    const padL = Math.floor(totalPad / 2);
    const padR = totalPad - padL;
    L(SEP);
    L(cmt('/*                                                                            */'));
    L(cmt(`/*${' '.repeat(padL)}${label}${' '.repeat(padR)}*/`));
    L(cmt('/*                                                                            */'));
    L(SEP);
  };

  // ── #define emitter: value always starts at absolute column 45.
  // '#define ' prefix = 8 chars, so the name field width = 45 - 8 = 37.
  // If the name is >= 37 chars the value drops to the next line, indented to col 45. ──
  const COL = 37;  // name field width; value col = 8 + 37 = 45
  const DEF1 = (macroToken, value) => {
    const nameLen = macroToken.length;
    if (nameLen < COL) {
      const pad = ' '.repeat(COL - nameLen);
      L(`${kw('#define')} ${mac(macroToken)}${pad}${value}`);
    } else {
      const indent = ' '.repeat(8 + COL);
      L(`${kw('#define')} ${mac(macroToken)} \\`);
      L(`${indent}${value}`);
    }
  };
  // DEF_ML: for multi-line continuation defines (e.g. BT_UUID_128_ENCODE blocks).
  // Aligns the first value token at col 45 if name fits, otherwise drops to next line.
  const IND = ' '.repeat(8 + COL);
  const DEF_ML = (macroToken, firstLine, extraLines) => {
    const nameLen = macroToken.length;
    if (nameLen < COL) {
      const pad = ' '.repeat(COL - nameLen);
      L(`${kw('#define')} ${mac(macroToken)}${pad}${firstLine} \\`);
    } else {
      L(`${kw('#define')} ${mac(macroToken)} \\`);
      L(`${IND}${firstLine} \\`);
    }
    extraLines.forEach((ln, i) => {
      const isLast = i === extraLines.length - 1;
      L(`${IND}   ${ln}${isLast ? '' : ' \\'}`);
    });
  };

  // File-level Doxygen block
  const projectStr = state.profileMeta.project || '';
  L(cmt('/**'));
  L(cmt(` * @file          ${filename}.h`));
  L(cmt(` * @brief         Header file containing GATT database for ${svc.name} service.${projectStr ? ' Project: ' + projectStr : ''}`));
  L(cmt(` * @date          ${dateStr}`));
  L(cmt(` * @author        ${getAUTHOR()}`));
  L(cmt(' */'));
  L('');
  L(`${kw('#ifndef')} ${guard}`);
  L(`${kw('#define')} ${guard}`);
  L('');

  // Includes section
  SEC('INCLUDES');
  L(`${kw('#include')} ${str(esc('<zephyr/kernel.h>'))}`);
  L(`${kw('#include')} ${str(esc('<zephyr/bluetooth/gatt.h>'))}`);
  L(`${kw('#include')} ${str(esc('<string.h>'))}`);
  L(`${kw('#include')} ${str('"BaseUUIDs.h"')}`);
  L(`${kw('#include')} ${str('"GATT_CB_Types.h"')}`);
  L(`${kw('#include')} ${str('"AppLog.h"')}`);
  if (state.useGenericCbs) {
    L(`${kw('#include')} ${str('"GATT_GenericCallbacks.h"')}`);
  }
  L('');

  // Defines section
  SEC('DEFINES');
  L(cmt('// Service UUID'));
  L(cmt('/**'));
  L(cmt(` * @def           ${svcMacro}`));
  L(cmt(` * @brief         ${svc.name} service.`));
  L(cmt(' */'));
  DEF1(svcMacro, `(${num(svc.serviceId)})`);
  L('');
  L(cmt('/**'));
  L(cmt(` * @def           ${macro}_SERVICE_UUID_FIRST_PART_32BIT`));
  L(cmt(` * @brief         First part of ${svc.name} service UUID (32-bits).`));
  L(cmt(' */'));
  // Multi-line #define: name line always ends with ' \', continuation indented to col 45
  DEF_ML(`${macro}_SERVICE_UUID_FIRST_PART_32BIT`,
    `(${mac('UUID_FIRST_PART_32BIT')}(`,
    [`${mac(svc.partUuidDomain)},`, `${mac(svcMacro)},`, `${num('0x0000')}))`]
  );
  L('');
  L(cmt('/**'));
  L(cmt(` * @def           ${svcValMacro}`));
  L(cmt(` * @brief         128-bit value encoding for ${svc.name} characteristic UUID.`));
  L(cmt(' */'));
  DEF_ML(svcValMacro,
    `${mac('BT_UUID_128_ENCODE')}(`,
    [`${mac(`${macro}_SERVICE_UUID_FIRST_PART_32BIT`)},`, `${mac('BASE_UUID_SECOND_PART_16BIT')},`, `${mac('BASE_UUID_THIRD_PART_16BIT')},`, `${mac('BASE_UUID_FOURTH_PART_16BIT')},`, `${mac('BASE_UUID_FIFTH_PART_48BIT')})`]
  );
  L('');
  L(cmt('/**'));
  L(cmt(` * @def           ${svcPtrMacro}`));
  L(cmt(` * @brief         Creating UUID structure pointer (const struct bt_uuid *) for`));
  L(cmt(` *                ${svc.name} characteristic.`));
  L(cmt(' */'));
  DEF1(svcPtrMacro, `${mac('BT_UUID_DECLARE_128')}(${mac(svcValMacro)})`);
  L('');
  L(cmt('// Characteristic UUID'));

  svc.chars.forEach(c => {
    const cm = toMacroName(c.name);
    const cNameLower = c.name; // use name as-is
    const partMac = `PART_UUID_CHAR_${cm}`;
    const firstMac = `${cm}_CHAR_UUID_FIRST_PART_32BIT`;
    const valMac = `BT_UUID_${cm}_CHAR_VAL`;
    const ptrMac = `BT_UUID_${cm}_CHAR`;
    L(cmt('/**'));
    L(cmt(` * @def           ${partMac}`));
    L(cmt(` * @brief         UUID '${cNameLower}' characteristic.`));
    L(cmt(' */'));
    DEF1(partMac, `(${num(c.charIdHex)})`);
    L('');
    L(cmt('/**'));
    L(cmt(` * @def           ${firstMac}`));
    L(cmt(` * @brief         First part of '${cNameLower}' characteristic UUID (32-bits).`));
    L(cmt(' */'));
    DEF_ML(firstMac,
      `(${mac('UUID_FIRST_PART_32BIT')}(`,
      [`${mac(svc.partUuidDomain)},`, `${mac(svcMacro)},`, `${mac(partMac)}))`]
    );
    L('');
    L(cmt('/**'));
    L(cmt(` * @def           ${valMac}`));
    L(cmt(` * @brief         128-bit value encoding for '${cNameLower}' characteristic UUID.`));
    L(cmt(' */'));
    DEF_ML(valMac,
      `${mac('BT_UUID_128_ENCODE')}(`,
      [`${mac(firstMac)},`, `${mac('BASE_UUID_SECOND_PART_16BIT')},`, `${mac('BASE_UUID_THIRD_PART_16BIT')},`, `${mac('BASE_UUID_FOURTH_PART_16BIT')},`, `${mac('BASE_UUID_FIFTH_PART_48BIT')})`]
    );
    L('');
    L(cmt('/**'));
    L(cmt(` * @def           ${ptrMac}`));
    L(cmt(` * @brief         Creating UUID structure pointer (const struct bt_uuid *) for`));
    L(cmt(` *                '${cNameLower}' characteristic.`));
    L(cmt(' */'));
    DEF1(ptrMac, `${mac('BT_UUID_DECLARE_128')}(${mac(valMac)})`);
    L('');
  });

  // Enums, Structures, Unions, Extern Variables sections (empty, matching project style)
  SEC('ENUMS');
  L('');
  SEC('STRUCTURES');
  L('');
  SEC('UNIONS');
  L('');
  SEC('EXTERN VARIABLES');
  L('');
  SEC('EXTERN FUNCTIONS');
  L('');

  L(`${kw('#endif')} ${cmt(`//!${guard}`)}`);
  L('');
  // Generated-by footer
  L(cmt(` * Generated by GATT Configurator for Zephyr RTOS (https://github.com/shivamchudasama/gatt-configurator)`));

  return lines.join('\n');
}

function generateC(svc) {
  const macro = toMacroName(svc.name);
  function toPascalCase(s) {
    return s.split('_').map(w => w.length <= 2 ? w : w.charAt(0) + w.slice(1).toLowerCase()).join('');
  }
  const filename = toPascalCase(macro);
  const svcMacro = `BT_UUID_${macro}_SERVICE`;
  const today = new Date();
  const dateStr = String(today.getDate()).padStart(2,'0') + '/' +
                  String(today.getMonth()+1).padStart(2,'0') + '/' +
                  today.getFullYear();

  let lines = [];
  const kw  = s => `<span class="tok-kw">${s}</span>`;
  const fn  = s => `<span class="tok-fn">${s}</span>`;
  const mac = s => `<span class="tok-mac">${s}</span>`;
  const str = s => `<span class="tok-str">${s}</span>`;
  const num = s => `<span class="tok-num">${s}</span>`;
  const cmt = s => `<span class="tok-cmt">${s}</span>`;
  const typ = s => `<span class="tok-type">${s}</span>`;
  const L = s => lines.push(s);
  const SEP = cmt('/******************************************************************************/');
  const SEC = label => {
    const inner = 76;
    const totalPad = inner - label.length;
    const padL = Math.floor(totalPad / 2);
    const padR = totalPad - padL;
    L(SEP);
    L(cmt('/*                                                                            */'));
    L(cmt(`/*${' '.repeat(padL)}${label}${' '.repeat(padR)}*/`));
    L(cmt('/*                                                                            */'));
    L(SEP);
  };

  const projectStr = state.profileMeta.project || '';
  L(cmt('/**'));
  L(cmt(` * @file          ${filename}.c`));
  L(cmt(` * @brief         Source file containing GATT database for ${svc.name} service.${projectStr ? ' Project: ' + projectStr : ''}`));
  L(cmt(` * @date          ${dateStr}`));
  L(cmt(` * @author        ${getAUTHOR()}`));
  L(cmt(' */'));
  L('');

  // ── INCLUDES ──
  SEC('INCLUDES');
  L(`${kw('#include')} ${str(`"${filename}.h"`)}`);
  L('');

  // ── DEFINES ──
  SEC('DEFINES');
  L('');

  // ── ENUMS ──
  SEC('ENUMS');
  L('');

  // ── STRUCTURES ──
  SEC('STRUCTURES');
  L('');

  if (!state.useGenericCbs) {
    // ── PRIVATE FUNCTION DECLARATIONS (per-characteristic stubs) ──
    SEC('PRIVATE FUNCTION DECLARATIONS');
    svc.chars.forEach(c => {
      if (c.props.read) {
        L(`${kw('static')} ${typ('ssize_t')} ${fn(c.callback)}(${kw('struct')} ${typ('bt_conn')} *stpt_connHandle,`);
        L(`   ${kw('const')} ${kw('struct')} ${typ('bt_gatt_attr')} *stpt_attr, ${kw('void')} *vpt_buf, ${typ('uint16_t')} u16_length,`);
        L(`   ${typ('uint16_t')} u16_offset);`);
      }
      if (c.props.write || c.props.writeNoResp || c.props.auth) {
        L(`${kw('static')} ${typ('ssize_t')} ${fn(c.callback)}(${kw('struct')} ${typ('bt_conn')} *stpt_connHandle,`);
        L(`   ${kw('const')} ${kw('struct')} ${typ('bt_gatt_attr')} *stpt_attr, ${kw('const')} ${kw('void')} *vpt_buf, ${typ('uint16_t')} u16_length,`);
        L(`   ${typ('uint16_t')} u16_offset, ${typ('uint8_t')} u8_flags);`);
      }
    });
    L('');
  }

  // ── PRIVATE VARIABLES ──
  SEC('PRIVATE VARIABLES');

  if (state.useGenericCbs) {
    // ── LOCAL DATA VARIABLES ──
    L(cmt('/* ========================================================================== */'));
    L(cmt('/* Local data variables                                                       */'));
    L(cmt('/*                                                                            */'));
    L(cmt('/* One variable per characteristic. These are the authoritative local copies  */'));
    L(cmt('/* of each characteristic\'s value. gt_GATT_GenericRead and                    */'));
    L(cmt('/* gt_GATT_GenericWrite access them exclusively through the descriptor\'s      */'));
    L(cmt('/* vpt_data pointer; no other code in this file touches them directly.        */'));
    L(cmt('/* ========================================================================== */'));
    L('');
    svc.chars.forEach(c => {
      // Lower camel case: split words, lowercase all, capitalise from word 2 onward
      const _cmWords = c.name.trim().split(/\s+/);
      const cm = _cmWords.map((w, i) => i === 0 ? w.toLowerCase() : w.charAt(0).toUpperCase() + w.slice(1).toLowerCase()).join('');
      const isArr = c.varLength || c.length > 1;
      const varDecl = isArr
        ? `su8ar_${cm.replace(/[^a-zA-Z0-9_]/g,'')}`
        : `su8_${cm.replace(/[^a-zA-Z0-9_]/g,'')}`;
      L(cmt('/**'));
      L(cmt(` * @var           ${varDecl}`));
      L(cmt(` * @brief         Local data variable for the '${c.name}' characteristic.`));
      if (isArr) {
        L(cmt(` *                Declared as a byte array sized to the characteristic's`));
        L(cmt(` *                maximum length (${c.length} bytes, per the GATT XML descriptor).`));
      }
      L(cmt(' */'));
      if (isArr) {
        L(`${kw('static')} ${typ('uint8_t')} ${varDecl}[${num(c.length + 'U')}] = { ${num('0U')} };`);
      } else {
        L(`${kw('static')} ${typ('uint8_t')} ${varDecl} = ${num('0U')};`);
      }
      L('');
    });

    // ── CHARACTERISTIC DESCRIPTORS ──
    L(cmt('/* ========================================================================== */'));
    L(cmt('/* Characteristic descriptors                                                 */'));
    L(cmt('/*                                                                            */'));
    L(cmt('/* One GATTCharDescriptor_T per characteristic. Passed as user_data to        */'));
    L(cmt('/* BT_GATT_CHARACTERISTIC. The generic callbacks retrieve the descriptor via  */'));
    L(cmt('/* attr->user_data and use it to locate the local variable, enforce length    */'));
    L(cmt('/* constraints, and optionally invoke a custom hook.                          */'));
    L(cmt('/*                                                                            */'));
    L(cmt('/* Custom hooks (fpt_customReadCb / fpt_customWriteCb) are NULL when no       */'));
    L(cmt('/* hook name was provided in the configurator. When a hook name is supplied,  */'));
    L(cmt('/* a static forward declaration is emitted above BT_GATT_SERVICE_DEFINE so    */'));
    L(cmt('/* that the descriptor initialiser compiles without an external header.       */'));
    L(cmt('/* Implement the hook body in this file or a companion .c file.               */'));
    L(cmt('/* ========================================================================== */'));
    L('');
    svc.chars.forEach(c => {
      // Lower camel case: split words, lowercase all, capitalise from word 2 onward
      const _cmWords = c.name.trim().split(/\s+/);
      const cm = _cmWords.map((w, i) => i === 0 ? w.toLowerCase() : w.charAt(0).toUpperCase() + w.slice(1).toLowerCase()).join('');
      const isArr = c.varLength || c.length > 1;
      const dataVar = isArr
        ? `su8ar_${cm.replace(/[^a-zA-Z0-9_]/g,'')}`
        : `su8_${cm.replace(/[^a-zA-Z0-9_]/g,'')}`;
      const descVar = `sst_${cm.replace(/[^a-zA-Z0-9_]/g,'')}Desc`;
      const lenExpr = isArr ? `sizeof(${dataVar})` : `sizeof(${dataVar})`;
      const actualExpr = isArr ? `0U` : `sizeof(${dataVar})`;
      const varLenBool = (c.varLength || isArr) ? `${kw('true')}` : `${kw('false')}`;
      L(cmt('/**'));
      L(cmt(` * @var           ${descVar}`));
      L(cmt(` * @brief         Descriptor for the '${c.name}' characteristic.`));
      if (c.varLength) {
        L(cmt(` *                Variable-length: u16_actualLen starts at 0 and is updated`));
        L(cmt(` *                after each successful write. Maximum length: ${c.length} bytes.`));
      } else if (isArr) {
        L(cmt(` *                Fixed-length array (${c.length} bytes).`));
      } else {
        L(cmt(` *                Fixed-length (1 byte).`));
      }
      L(cmt(' */'));
      const readHook  = (c.customReadCb  || '').trim();
      const writeHook = (c.customWriteCb || '').trim();
      L(`${kw('static')} ${typ('GATTCharDescriptor_T')} ${descVar} = {`);
      L(`   .vpt_data          = ${isArr ? dataVar : `&${dataVar}`},`);
      L(`   .u16_dataLen       = ${lenExpr},`);
      L(`   .u16_actualLen     = ${actualExpr},`);
      L(`   .b_variableLength  = ${varLenBool},`);
      L(`   .stpt_mutex        = ${kw('NULL')},`);
      L(`   .fpt_customReadCb  = ${readHook  ? fn(readHook)  : kw('NULL')},`);
      L(`   .fpt_customWriteCb = ${writeHook ? fn(writeHook) : kw('NULL')},`);
      L(`};`);
      L('');
    });
  }

  // ── Forward declarations for custom hook functions (if any) ──
  if (state.useGenericCbs) {
    const allHooks = [];
    svc.chars.forEach(c => {
      const rh = (c.customReadCb  || '').trim();
      const wh = (c.customWriteCb || '').trim();
      if (rh && !allHooks.includes(rh)) allHooks.push({ name: rh, isWrite: false });
      if (wh && !allHooks.includes(wh)) allHooks.push({ name: wh, isWrite: true  });
    });
    if (allHooks.length > 0) {
      L(cmt('/* ── Custom hook forward declarations ─────────────────────────────────────── */'));
      L(cmt('/* Implement these functions in this file (below PUBLIC FUNCTION DEFINITIONS)  */'));
      L(cmt('/* or in a companion .c file that is linked into the same translation unit.    */'));
      allHooks.forEach(h => {
        if (!h.isWrite) {
          L(`${kw('static')} ${typ('ssize_t')} ${fn(h.name)}(${kw('struct')} ${typ('bt_conn')} *stpt_connHandle,`);
          L(`   ${kw('const')} ${kw('struct')} ${typ('bt_gatt_attr')} *stpt_attr, ${kw('void')} *vpt_buf, ${typ('uint16_t')} u16_length,`);
          L(`   ${typ('uint16_t')} u16_offset);`);
        } else {
          L(`${kw('static')} ${typ('ssize_t')} ${fn(h.name)}(${kw('struct')} ${typ('bt_conn')} *stpt_connHandle,`);
          L(`   ${kw('const')} ${kw('struct')} ${typ('bt_gatt_attr')} *stpt_attr, ${kw('const')} ${kw('void')} *vpt_buf, ${typ('uint16_t')} u16_length,`);
          L(`   ${typ('uint16_t')} u16_offset, ${typ('uint8_t')} u8_flags);`);
        }
        L('');
      });
    }
  }

  // ── BT_GATT_SERVICE_DEFINE ──
  L(cmt('/**'));
  L(cmt(` * @var           ${svc.varName}`));
  if ((svc.brief || '').trim()) {
    L(cmt(` * @brief         ${svc.brief.trim()}`));
  } else {
    L(cmt(` * @brief         ${svc.name} service instance. Creates a structure of bt_gatt_attr type.`));
    L(cmt(' *                It statically define and register this GATT service.'));
    if (state.useGenericCbs) {
      L(cmt(' *'));
      L(cmt(' *                All characteristics use gt_GATT_GenericRead or gt_GATT_GenericWrite'));
      L(cmt(' *                as their Zephyr callback and pass their GATTCharDescriptor_T'));
      L(cmt(' *                as user_data. No per-characteristic callback functions exist'));
      L(cmt(' *                in this file; all read/write logic is centralised in'));
      L(cmt(' *                GATT_GenericCallbacks.c.'));
    }
  }
  L(cmt(' */'));
  L(`${mac('BT_GATT_SERVICE_DEFINE')}(${svc.varName},`);
  L(`   ${cmt(`// Primary service declaration with ${svc.name} service UUID`)}`);
  L(`   ${mac('BT_GATT_PRIMARY_SERVICE')}(`);
  L(`      ${cmt('// UUID')}`);
  L(`      ${mac(svcMacro)}`);
  L(`   ),`);

  svc.chars.forEach((c, idx) => {
    const cm = c.name.trim().split(/\s+/).map((w,i)=>i===0?w.toLowerCase():w.charAt(0).toUpperCase()+w.slice(1).toLowerCase()).join('');
    const charMacro = toMacroName(c.name);
    const ptrMac = `BT_UUID_${charMacro}_CHAR`;
    const propParts = [];
    if (c.props.broadcast)    propParts.push(mac('BT_GATT_CHRC_BROADCAST'));
    if (c.props.read)         propParts.push(mac('BT_GATT_CHRC_READ'));
    if (c.props.writeNoResp)  propParts.push(mac('BT_GATT_CHRC_WRITE_WITHOUT_RESP'));
    if (c.props.write)        propParts.push(mac('BT_GATT_CHRC_WRITE'));
    if (c.props.notify)       propParts.push(mac('BT_GATT_CHRC_NOTIFY'));
    if (c.props.indicate)     propParts.push(mac('BT_GATT_CHRC_INDICATE'));
    if (c.props.auth)         propParts.push(mac('BT_GATT_CHRC_AUTH'));
    if (c.props.extProp)      propParts.push(mac('BT_GATT_CHRC_EXT_PROP'));
    const permParts = [];
    if (c.perms.read)         permParts.push(mac('BT_GATT_PERM_READ'));
    if (c.perms.readEncrypt)  permParts.push(mac('BT_GATT_PERM_READ_ENCRYPT'));
    if (c.perms.readAuthen)   permParts.push(mac('BT_GATT_PERM_READ_AUTHEN'));
    if (c.perms.readLesc)     permParts.push(mac('BT_GATT_PERM_READ_LESC'));
    if (c.perms.write)        permParts.push(mac('BT_GATT_PERM_WRITE'));
    if (c.perms.writeEncrypt) permParts.push(mac('BT_GATT_PERM_WRITE_ENCRYPT'));
    if (c.perms.writeAuthen)  permParts.push(mac('BT_GATT_PERM_WRITE_AUTHEN'));
    if (c.perms.prepareWrite) permParts.push(mac('BT_GATT_PERM_PREPARE_WRITE'));
    if (c.perms.writeLesc)    permParts.push(mac('BT_GATT_PERM_WRITE_LESC'));
    if (permParts.length === 0) permParts.push(mac('BT_GATT_PERM_NONE'));

    const propLabel = [
      c.props.broadcast  ? 'Broadcast'  : '',
      c.props.read       ? 'Read'       : '',
      c.props.write      ? 'Write'      : '',
      c.props.writeNoResp? 'Write NR'   : '',
      c.props.notify     ? 'Notify'     : '',
      c.props.indicate   ? 'Indicate'   : '',
      c.props.auth       ? 'Auth (deprecated)' : '',
      c.props.extProp    ? 'Ext Prop'   : '',
    ].filter(Boolean).join(' | ') || 'None';
    const permLabel = [
      c.perms.read         ? 'Read'          : '',
      c.perms.readEncrypt  ? 'Read Encrypt'  : '',
      c.perms.readAuthen   ? 'Read Authen'   : '',
      c.perms.readLesc     ? 'Read LESC'     : '',
      c.perms.write        ? 'Write'         : '',
      c.perms.writeEncrypt ? 'Write Encrypt' : '',
      c.perms.writeAuthen  ? 'Write Authen'  : '',
      c.perms.prepareWrite ? 'Prepare Write' : '',
      c.perms.writeLesc    ? 'Write LESC'    : '',
    ].filter(Boolean).join(' | ') || 'None';

    let readCb, writeCb, userData;
    if (state.useGenericCbs) {
      const isArr = c.varLength || c.length > 1;
      const descVar = `sst_${cm.replace(/[^a-zA-Z0-9_]/g,'')}Desc`;
      readCb  = c.props.read ? fn('gt_GATT_GenericRead') : kw('NULL');
      writeCb = (c.props.write || c.props.writeNoResp || c.props.auth) ? fn('gt_GATT_GenericWrite') : kw('NULL');
      userData = `&${descVar}`;
    } else {
      readCb  = c.props.read ? fn(c.callback) : kw('NULL');
      writeCb = (c.props.write || c.props.writeNoResp || c.props.auth) ? fn(c.callback) : kw('NULL');
      userData = c.isPointer ? c.varName : `&${c.varName}`;
    }

    const readCbName  = state.useGenericCbs
      ? (c.props.read ? 'gt_GATT_GenericRead' : 'NULL')
      : (c.props.read ? c.callback : 'NULL');
    const writeCbName = state.useGenericCbs
      ? ((c.props.write || c.props.writeNoResp || c.props.auth) ? 'gt_GATT_GenericWrite' : 'NULL')
      : ((c.props.write || c.props.writeNoResp || c.props.auth) ? c.callback : 'NULL');

    L(`   ${cmt(`// Characteristic declaration for '${c.name}'`)}`);
    L(`   ${mac('BT_GATT_CHARACTERISTIC')}(`);
    L(`      ${cmt('// UUID')}`);
    L(`      ${mac(ptrMac)},`);
    L(`      ${cmt('// Properties - ' + propLabel)}`);
    L(`      ${propParts.join(' | ')},`);
    L(`      ${cmt('// Permissions - ' + permLabel)}`);
    L(`      ${permParts.join(' | ')},`);
    L(`      ${cmt('// Read callback - ' + readCbName)}`);
    L(`      ${readCb},`);
    L(`      ${cmt('// Write callback - ' + writeCbName)}`);
    L(`      ${writeCb},`);
    L(`      ${cmt(`// User data - ${userData}`)}`);
    L(`      ${userData}`);
    L(`   ),`);
    L(`   ${mac('BT_GATT_CUD')}(`);
    L(`      ${str(`"${c.cud}"`  )},`);
    L(`      ${mac('BT_GATT_PERM_READ')}`);
    L(`   )${idx < svc.chars.length-1 ? ',' : ''}`);
  });
  L(`);`);
  L('');

  // Sections that exist in both modes but are empty
  SEC('STRUCTURES');
  L('');
  SEC('UNIONS');
  L('');
  SEC('EXTERN VARIABLES');
  L('');
  SEC('PUBLIC VARIABLES');
  L('');
  SEC('EXTERN FUNCTIONS');
  L('');

  if (!state.useGenericCbs) {
    // ── PRIVATE FUNCTION DEFINITIONS (per-characteristic stubs) ──
    SEC('PRIVATE FUNCTION DEFINITIONS');
    svc.chars.forEach(c => {
      if (c.props.read) {
        const briefText = (c.brief || '').trim();
        L(cmt('/**'));
        L(cmt(` * @private       ${c.callback}`));
        if (briefText) {
          L(cmt(` * @brief         ${briefText}`));
        } else {
          L(cmt(` * @brief         Callback function for read operation on '${c.name}' characteristic.`));
          L(cmt(` *                It will accommodate incoming ${c.name} reading request from remote`));
          L(cmt(' *                GATT client.'));
        }
        L(cmt(' * @param[inout]  stpt_connHandle Connection handle.'));
        L(cmt(' * @param[in]     stpt_attr The attribute being read.'));
        L(cmt(' * @param[in]     vpt_buf The buffer to fill with read data.'));
        L(cmt(' * @param[in]     u16_length The length of the read buffer.'));
        L(cmt(' * @param[in]     u16_offset The offset at which the data is being read.'));
        L(cmt(' * @return        Either BT_GATT_ERR() or the number of bytes read.'));
        L(cmt(' */'));
        L(`${kw('static')} ${typ('ssize_t')} ${fn(c.callback)}(${kw('struct')} ${typ('bt_conn')} *stpt_connHandle,`);
        L(`\t${kw('const')} ${kw('struct')} ${typ('bt_gatt_attr')} *stpt_attr, ${kw('void')} *vpt_buf, ${typ('uint16_t')} u16_length,`);
        L(`\t${typ('uint16_t')} u16_offset)`);
        L(`{`);
        L(`\t${typ('ssize_t')} t_retVal = ${num('0')};`);
        L('');
        L(`\t${kw('return')} t_retVal;`);
        L(`}`);
        L('');
      }
      if (c.props.write || c.props.writeNoResp || c.props.auth) {
        const briefText = (c.brief || '').trim();
        const writeTypeStr = [
          c.props.write        ? 'write'                  : '',
          c.props.writeNoResp  ? 'write without response' : '',
          c.props.auth         ? 'authenticated signed write (deprecated)' : '',
        ].filter(Boolean).join(' / ');
        L(cmt('/**'));
        L(cmt(` * @private       ${c.callback}`));
        if (briefText) {
          L(cmt(` * @brief         ${briefText}`));
        } else {
          L(cmt(` * @brief         Callback function for ${writeTypeStr} operation on '${c.name}' characteristic.`));
          L(cmt(' *                It will parse the incoming data as control packets.'));
        }
        if (c.props.auth) {
          L(cmt(' * @deprecated    BT_GATT_CHRC_AUTH (Authenticated Signed Writes) is deprecated in Zephyr.'));
        }
        L(cmt(' * @param[inout]  stpt_connHandle Connection handle.'));
        L(cmt(' * @param[in]     stpt_attr The attribute being written to.'));
        L(cmt(' * @param[in]     vpt_buf The buffer containing the data being written.'));
        L(cmt(' * @param[in]     u16_length The length of the data being written.'));
        L(cmt(' * @param[in]     u16_offset The offset at which the data is being written.'));
        L(cmt(' * @param[in]     u8_flags Write flags, see @ref bt_gatt_attr_write_flag.'));
        L(cmt(' * @return        Either BT_GATT_ERR() or the number of bytes written.'));
        L(cmt(' */'));
        L(`${kw('static')} ${typ('ssize_t')} ${fn(c.callback)}(${kw('struct')} ${typ('bt_conn')} *stpt_connHandle,`);
        L(`   ${kw('const')} ${kw('struct')} ${typ('bt_gatt_attr')} *stpt_attr, ${kw('const')} ${kw('void')} *vpt_buf, ${typ('uint16_t')} u16_length,`);
        L(`   ${typ('uint16_t')} u16_offset, ${typ('uint8_t')} u8_flags)`);
        L(`{`);
        L(`   ${typ('ssize_t')} t_retVal = ${num('0')};`);
        L('');
        L(`   ${kw('return')} t_retVal;`);
        L(`}`);
        L('');
      }
    });
  }

  // ── PUBLIC FUNCTION DEFINITIONS ──
  SEC('PUBLIC FUNCTION DEFINITIONS');
  L('');

  // ── Custom hook stub definitions (generic mode only) ──
  if (state.useGenericCbs) {
    // Collect all unique hooks across all characteristics
    const emittedHooks = [];
    svc.chars.forEach(c => {
      const rh = (c.customReadCb  || '').trim();
      const wh = (c.customWriteCb || '').trim();
      if (rh && !emittedHooks.find(h => h.name === rh)) emittedHooks.push({ name: rh, isWrite: false, charName: c.name, brief: c.brief });
      if (wh && !emittedHooks.find(h => h.name === wh)) emittedHooks.push({ name: wh, isWrite: true,  charName: c.name, brief: c.brief });
    });
    emittedHooks.forEach(h => {
      const briefText = (h.brief || '').trim();
      L(cmt('/**'));
      L(cmt(` * @private       ${h.name}`));
      if (briefText) {
        L(cmt(` * @brief         ${briefText}`));
      } else if (!h.isWrite) {
        L(cmt(` * @brief         Custom post-read hook for the '${h.charName}' characteristic.`));
        L(cmt(` *                Called by gt_GATT_GenericRead after the local value has been`));
        L(cmt(` *                copied into vpt_buf. Return 0 to keep the generic callback's`));
        L(cmt(` *                return value, or a non-zero ssize_t / BT_GATT_ERR() to override it.`));
      } else {
        L(cmt(` * @brief         Custom post-write hook for the '${h.charName}' characteristic.`));
        L(cmt(` *                Called by gt_GATT_GenericWrite after the incoming data has been`));
        L(cmt(` *                validated and written into the local variable. Return 0 to keep`));
        L(cmt(` *                the generic callback's return value, or BT_GATT_ERR() to override.`));
      }
      L(cmt(' * @param[inout]  stpt_connHandle Connection handle.'));
      L(cmt(' * @param[in]     stpt_attr The attribute that was read/written.'));
      if (!h.isWrite) {
        L(cmt(' * @param[inout]  vpt_buf Output buffer already populated by the generic callback.'));
        L(cmt(' * @param[in]     u16_length Requested read length, as passed by the stack.'));
        L(cmt(' * @param[in]     u16_offset Read offset, as passed by the stack.'));
        L(cmt(' * @return        0 to keep the generic return value, or custom ssize_t / BT_GATT_ERR().'));
        L(cmt(' */'));
        L(`${kw('static')} ${typ('ssize_t')} ${fn(h.name)}(${kw('struct')} ${typ('bt_conn')} *stpt_connHandle,`);
        L(`   ${kw('const')} ${kw('struct')} ${typ('bt_gatt_attr')} *stpt_attr, ${kw('void')} *vpt_buf, ${typ('uint16_t')} u16_length,`);
        L(`   ${typ('uint16_t')} u16_offset)`);
        L(`{`);
        L(`   ${typ('ssize_t')} t_retVal = ${num('0')};`);
        L('');
        L(`   ${kw('return')} t_retVal;`);
        L(`}`);
      } else {
        L(cmt(' * @param[in]     vpt_buf Buffer containing the data that was written.'));
        L(cmt(' * @param[in]     u16_length Number of bytes written in this operation.'));
        L(cmt(' * @param[in]     u16_offset Write offset, as passed by the stack.'));
        L(cmt(' * @param[in]     u8_flags Write flags, see @ref bt_gatt_attr_write_flag.'));
        L(cmt(' * @return        0 to keep the generic return value, or custom ssize_t / BT_GATT_ERR().'));
        L(cmt(' */'));
        L(`${kw('static')} ${typ('ssize_t')} ${fn(h.name)}(${kw('struct')} ${typ('bt_conn')} *stpt_connHandle,`);
        L(`   ${kw('const')} ${kw('struct')} ${typ('bt_gatt_attr')} *stpt_attr, ${kw('const')} ${kw('void')} *vpt_buf, ${typ('uint16_t')} u16_length,`);
        L(`   ${typ('uint16_t')} u16_offset, ${typ('uint8_t')} u8_flags)`);
        L(`{`);
        L(`   ${typ('ssize_t')} t_retVal = ${num('0')};`);
        L('');
        L(`   ${kw('return')} t_retVal;`);
        L(`}`);
      }
      L('');
    });
  }

  // Generated-by footer
  L(cmt(` * Generated by GATT Configurator for Zephyr RTOS (https://github.com/shivamchudasama/gatt-configurator)`));

  return lines.join('\n');
}


// ═══════════════════════════════════════════════════════
// GENERATE BaseUUIDs.h
// ═══════════════════════════════════════════════════════
function generateBaseUUIDs() {
  const today = new Date();
  const dateStr = String(today.getDate()).padStart(2,'0') + '/' +
                  String(today.getMonth()+1).padStart(2,'0') + '/' +
                  today.getFullYear();

  const b = state.baseUUID;
  const p2 = (b.p2 || 'XXXX').toUpperCase();
  const p3 = (b.p3 || 'XXXX').toUpperCase();
  const p4 = (b.p4 || 'XXXX').toUpperCase();
  const p5 = (b.p5 || 'XXXXXXXXXXXX').toUpperCase();
  const projectStr = state.profileMeta.project || '';

  let lines = [];
  const kw  = s => `<span class="tok-kw">${s}</span>`;
  const mac = s => `<span class="tok-mac">${s}</span>`;
  const num = s => `<span class="tok-num">${s}</span>`;
  const cmt = s => `<span class="tok-cmt">${s}</span>`;
  const str = s => `<span class="tok-str">${s}</span>`;
  const L   = s => lines.push(s);

  const SEP = cmt('/******************************************************************************/');
  const SEC = label => {
    const inner = 76;
    const totalPad = inner - label.length;
    const padL = Math.floor(totalPad / 2);
    const padR = totalPad - padL;
    L(SEP);
    L(cmt('/*                                                                            */'));
    L(cmt(`/*${' '.repeat(padL)}${label}${' '.repeat(padR)}*/`));
    L(cmt('/*                                                                            */'));
    L(SEP);
  };

  const COL = 37;
  const DEF1 = (macroToken, value) => {
    const nameLen = macroToken.length;
    if (nameLen < COL) {
      const pad = ' '.repeat(COL - nameLen);
      L(`${kw('#define')} ${mac(macroToken)}${pad}${value}`);
    } else {
      const indent = ' '.repeat(8 + COL);
      L(`${kw('#define')} ${mac(macroToken)} \\`);
      L(`${indent}${value}`);
    }
  };

  // ── File header ──
  L(cmt('/**'));
  L(cmt(' * @file          BaseUUIDs.h'));
  L(cmt(` * @brief         Header file containing base UUIDs for the project.${projectStr ? ' Project: ' + projectStr : ''}`));
  L(cmt(` * @date          ${dateStr}`));
  L(cmt(` * @author        ${getAUTHOR()}`));
  L(cmt(' */'));
  L('');
  L(`${kw('#ifndef')} _BASE_UUIDS_H`);
  L(`${kw('#define')} _BASE_UUIDS_H`);
  L('');

  // ── INCLUDES ──
  SEC('INCLUDES');
  L('');

  // ── DEFINES ──
  SEC('DEFINES');
  L(cmt('// Note: To keep the UUIDs of all the services and characteristics v4 compliant'));
  L(cmt('// (RFC 4122 compliant), we\'ve decided the base UUIDs of last 96-bits. Only the'));
  L(cmt('// first 32-bits would be changed throughout the project.'));
  L('');

  L(cmt('/**'));
  L(cmt(' * @def           BASE_UUID_SECOND_PART_16BIT'));
  L(cmt(' * @brief         Second part of base UUID (16-bits).'));
  L(cmt(' */'));
  DEF1('BASE_UUID_SECOND_PART_16BIT', `(${num('0X' + p2)})`);
  L('');

  L(cmt('/**'));
  L(cmt(' * @def           BASE_UUID_THIRD_PART_16BIT'));
  L(cmt(' * @brief         Third part of base UUID (16-bits).'));
  L(cmt(' */'));
  DEF1('BASE_UUID_THIRD_PART_16BIT', `(${num('0X' + p3)})`);
  L('');

  L(cmt('/**'));
  L(cmt(' * @def           BASE_UUID_FOURTH_PART_16BIT'));
  L(cmt(' * @brief         Fourth part of base UUID (16-bits).'));
  L(cmt(' */'));
  DEF1('BASE_UUID_FOURTH_PART_16BIT', `(${num('0X' + p4)})`);
  L('');

  L(cmt('/**'));
  L(cmt(' * @def           BASE_UUID_FIFTH_PART_48BIT'));
  L(cmt(' * @brief         Fifth part of base UUID (48-bits).'));
  L(cmt(' */'));
  DEF1('BASE_UUID_FIFTH_PART_48BIT', `(${num('0X' + p5)})`);
  L('');

  L(cmt('/**'));
  L(cmt(' * @def           UUID_FIRST_PART_32BIT'));
  L(cmt(' * @brief         First part of UUID (32-bits) formulation.'));
  L(cmt(' *                | 8-bit Domain | 8-bit Service ID | 16-bit Characteristic ID |'));
  L(cmt(' */'));
  L(`${kw('#define')} ${mac('UUID_FIRST_PART_32BIT')}(domain, svc, char) \\`);
  L(`                                             (((${kw('uint32_t')})(domain) ${esc('<<')} 24) | \\`);
  L(`                                             ((${kw('uint32_t')})(svc) ${esc('<<')} 16) | \\`);
  L(`                                             ((${kw('uint32_t')})(char)))`);
  L('');

  // ── Domains ──
  L(cmt('// Domains'));
  state.domains.forEach(d => {
    const macroName = `PART_UUID_DOMAIN_${d.name}`;
    L(cmt('/**'));
    L(cmt(` * @def           ${macroName}`));
    L(cmt(` * @brief         ${d.name.charAt(0) + d.name.slice(1).toLowerCase().replace(/_/g,' ')} domain.`));
    L(cmt(' */'));
    DEF1(macroName, `(${num(d.hex)})`);
    L('');
  });

  // ── ENUMS / STRUCTURES / UNIONS / EXTERN VARIABLES / EXTERN FUNCTIONS ──
  SEC('ENUMS');
  L('');
  SEC('STRUCTURES');
  L('');
  SEC('UNIONS');
  L('');
  SEC('EXTERN VARIABLES');
  L('');
  SEC('EXTERN FUNCTIONS');
  L('');

  L(`${kw('#endif')} ${cmt('//!_BASE_UUIDS_H')}`);
  L('');

  // Generated-by footer
  L(cmt(` * Generated by GATT Configurator for Zephyr RTOS (https://github.com/shivamchudasama/gatt-configurator)`));

  return lines.join('\n');
}

// Renders a single <service>…</service> block (no XML declaration, no <gatt> wrapper)
function generateXMLService(svc) {
  const cmt = s => `<span class="tok-cmt">${s}</span>`;
  const kw  = s => `<span class="tok-kw">${s}</span>`;
  const str = s => `<span class="tok-str">${s}</span>`;
  let lines = [];
  const L = s => lines.push(s);
  const shortBar = '&lt;!-- ================================================= --&gt;';

  L(cmt(`  &lt;!-- ===================================================== --&gt;`));
  L(cmt(`  &lt;!-- ${(svc.name.toUpperCase() + ' SERVICE').padEnd(53)} --&gt;`));
  L(cmt(`  &lt;!-- ===================================================== --&gt;`));
  L('');
  L(`  ${kw('&lt;service')}`);;
  L(`      name=${str(`&quot;${svc.name}&quot;`)}`);
  L(`      uuid=${str(`&quot;${computeUUID(svc, 0)}&quot;`)}`);
  L(`      var_name=${str(`&quot;${svc.varName}&quot;`)}`);
  L(`      brief=${str(`&quot;${svc.brief || ''}&quot;`)}`);
  L(`      advertise=${str(`&quot;${svc.advertise}&quot;`)}&gt;`);
  L('');

  svc.chars.forEach(c => {
    const props = [];
    if (c.props.broadcast)    props.push('broadcast');
    if (c.props.read)         props.push('read');
    if (c.props.write)        props.push('write');
    if (c.props.writeNoResp)  props.push('write_no_resp');
    if (c.props.notify)       props.push('notify');
    if (c.props.indicate)     props.push('indicate');
    if (c.props.auth)         props.push('auth');
    if (c.props.extProp)      props.push('ext_prop');
    const perms = [];
    if (c.perms.read)         perms.push('read');
    if (c.perms.readEncrypt)  perms.push('read_encrypt');
    if (c.perms.readAuthen)   perms.push('read_authen');
    if (c.perms.readLesc)     perms.push('read_lesc');
    if (c.perms.write)        perms.push('write');
    if (c.perms.writeEncrypt) perms.push('write_encrypt');
    if (c.perms.writeAuthen)  perms.push('write_authen');
    if (c.perms.prepareWrite) perms.push('prepare_write');
    if (c.perms.writeLesc)    perms.push('write_lesc');

    L(cmt(`    ${shortBar}`));
    L(cmt(`    &lt;!-- ${(c.name.toUpperCase() + ' CHARACTERISTIC').padEnd(49)} --&gt;`));
    L(cmt(`    ${shortBar}`));
    L(`    ${kw('&lt;characteristic')}`);
    L(`        name=${str(`&quot;${c.name}&quot;`)}`);
    L(`        uuid=${str(`&quot;${computeUUID(svc, c.charIdNum)}&quot;`)}`);
    L(`        properties=${str(`&quot;${props.join(' ')}&quot;`)}`);
    L(`        permissions=${str(`&quot;${perms.join(' ')}&quot;`)}`);
    L(`        var_name=${str(`&quot;${c.varName}&quot;`)}`);
    L(`        callback=${str(`&quot;${c.callback}&quot;`)}`);
    L(`        cud=${str(`&quot;${c.cud}&quot;`)}`);
    L(`        brief=${str(`&quot;${c.brief || ''}&quot;`)}`);
    L(`        is_pointer=${str(`&quot;${c.isPointer ? 'true' : 'false'}&quot;`)}`);
    if (c.customReadCb)  L(`        custom_read_cb=${str(`&quot;${c.customReadCb}&quot;`)}`);
    if (c.customWriteCb) L(`        custom_write_cb=${str(`&quot;${c.customWriteCb}&quot;`)}`);
    L(`        &gt;`);
    L('');
    if (c.varLength) {
      L(`      ${kw('&lt;value')}`);
      L(`          type=${str('&quot;uint8&quot;')}`);
      L(`          max_length=${str(`&quot;${c.length}&quot;`)}`);
      L(`          variable_length=${str('&quot;true&quot;')}/&gt;`);
    } else {
      L(`      ${kw('&lt;value')}`);
      L(`          type=${str('&quot;uint8&quot;')}`);
      L(`          length=${str(`&quot;${c.length}&quot;`)}`);
      L(`          variable_length=${str('&quot;false&quot;')}/&gt;`);
    }
    L(`    ${kw('&lt;/characteristic&gt;')}`);
    L('');
  });

  L(`  ${kw('&lt;/service&gt;')}`);
  return lines.join('\n');
}

// Generates the consolidated project-level XML (all services in one <gatt> root)
function generateXML() {
  const cmt = s => `<span class="tok-cmt">${s}</span>`;
  const kw  = s => `<span class="tok-kw">${s}</span>`;
  const str = s => `<span class="tok-str">${s}</span>`;
  let lines = [];
  const L = s => lines.push(s);

  const projectName = state.profileMeta.project || 'PROJECT';
  const BAR = '&lt;!-- ===================================================== --&gt;';

  const b = state.baseUUID;
  const baseUUIDStr = `xxxxxxxx-${b.p2||'XXXX'}-${b.p3||'XXXX'}-${b.p4||'XXXX'}-${b.p5||'XXXXXXXXXXXX'}`;

  L(cmt('&lt;?xml version=&quot;1.0&quot; encoding=&quot;UTF-8&quot; standalone=&quot;no&quot;?&gt;'));
  L('');
  L(cmt(BAR));
  L(cmt(`&lt;!-- BLE ECU GATT DATABASE — ${projectName.padEnd(29)} --&gt;`));
  L(cmt('&lt;!-- Target: Zephyr RTOS                                   --&gt;'));
  L(cmt(BAR));
  L('');
  const authorStr = state.profileMeta.author || '';
  L(`${kw('&lt;gatt')}`);
  L(`    name=${str(`&quot;${projectName}&quot;`)}`);
  L(`    author=${str(`&quot;${authorStr}&quot;`)}`);
  L(`    base-uuid=${str(`&quot;${baseUUIDStr}&quot;`)}`);
  L(`    use-generic-cbs=${str(`&quot;${state.useGenericCbs ? 'true' : 'false'}&quot;`)}&gt;`);
  L('');
  L(cmt('  &lt;!-- ===================================================== --&gt;'));
  L(cmt('  &lt;!-- DOMAINS                                               --&gt;'));
  L(cmt('  &lt;!-- ===================================================== --&gt;'));
  L(`  ${kw('&lt;domains&gt;')}`);
  state.domains.forEach(d => {
    L(`    ${kw('&lt;domain')} name=${str(`&quot;${d.name}&quot;`)} hex=${str(`&quot;${d.hex}&quot;`)}/&gt;`);
  });
  L(`  ${kw('&lt;/domains&gt;')}`);
  L('');

  state.services.forEach(svc => {
    L(generateXMLService(svc));
    L('');
  });

  L(`${kw('&lt;/gatt&gt;')}`);
  return lines.join('\n');
}
