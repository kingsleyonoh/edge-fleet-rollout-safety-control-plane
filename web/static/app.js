(() => {
  const render = (element, value, state = 'status') => {
    if (!element) return;
    element.textContent = value;
    element.dataset.state = state;
  };
  const cookie = (name) => document.cookie.split(';').map((part) => part.trim()).find((part) => part.startsWith(`${name}=`))?.slice(name.length + 1) ?? '';
  const csrf = () => cookie('edgefleet_csrf');
  const idempotencyKey = () => window.crypto?.randomUUID?.() ?? `${Date.now()}-${Math.random()}`;
  const text = (value) => value === null || value === undefined || value === '' ? '—' : String(value);
  const plural = (count, word) => `${count} ${word}${count === 1 ? '' : 's'}`;
  const load = async (path) => {
    const response = await fetch(path, {headers: {'Accept': 'application/json'}});
    if (!response.ok) throw new Error(`${path} returned ${response.status}`);
    return response.status === 204 ? {} : response.json();
  };
  const mutate = async (path, method, payload, extraHeaders = {}) => {
    const response = await fetch(path, {method, headers: {'Accept': 'application/json', 'Content-Type': 'application/json', 'Idempotency-Key': idempotencyKey(), 'X-CSRF-Token': csrf(), ...extraHeaders}, body: method === 'DELETE' ? undefined : JSON.stringify(payload ?? {})});
    const result = await response.json().catch(() => ({}));
    if (!response.ok) throw new Error(result.error?.message ?? `${path} returned ${response.status}`);
    return result;
  };
  const valueOf = (form, name) => form.elements.namedItem(name)?.value ?? '';
  const formPayload = (form) => {
    const payload = {};
    for (const control of form.elements) {
      if (!control.name || control.type === 'submit' || control.type === 'button' || control.type === 'file') continue;
      const value = control.value.trim();
      if (control.name.endsWith('_json')) {
        try { payload[control.name.slice(0, -5)] = value ? JSON.parse(value) : {}; } catch (_) { throw new Error(`${control.name} must contain valid JSON.`); }
      } else if (control.type === 'number') payload[control.name] = value === '' ? null : Number(value);
      else payload[control.name] = value;
    }
    return payload;
  };
  const showResult = (form, result) => {
    const feedback = form.querySelector('.form-status');
    const identifier = result.id || result.release_id;
    render(feedback, identifier ? `Saved ${identifier}.` : 'Saved.', 'loaded');
    if (result.api_key) { const secret = document.createElement('code'); secret.textContent = result.api_key; feedback?.append(' Store this one-time credential: ', secret); }
  };
  const definitions = {
    fleets: {path: '/api/fleets', title: 'fleet', columns: [['name', 'Name'], ['environment', 'Environment'], ['status', 'Status'], ['version', 'Version']]},
    devices: {path: '/api/devices', title: 'device', columns: [['stable_key', 'Stable key'], ['lifecycle_status', 'Lifecycle'], ['desired_generation', 'Desired'], ['observed_generation', 'Observed']]},
    artifacts: {path: '/api/artifacts', title: 'artifact', columns: [['file_name', 'File'], ['status', 'Status'], ['sha256_digest', 'SHA-256'], ['size_bytes', 'Bytes']]},
    policies: {path: '/api/policies', title: 'policy', columns: [['name', 'Name'], ['version', 'Version'], ['status', 'Status'], ['rollback_requirement', 'Rollback']]},
    releases: {path: '/api/releases', title: 'release', columns: [['name', 'Name'], ['status', 'Status'], ['current_stage_ordinal', 'Stage'], ['version', 'Version']]},
    approvals: {path: '/api/approvals', title: 'approval', columns: [['action', 'Action'], ['status', 'Status'], ['captured_release_version', 'Release version'], ['requested_by_actor_id', 'Requested by']]},
    simulations: {path: '/api/simulations', title: 'simulation', columns: [['scenario_name', 'Scenario'], ['status', 'Status'], ['seed', 'Seed'], ['result_digest', 'Result digest']]},
    replays: {path: '/api/replays', title: 'replay', columns: [['source_kind', 'Source'], ['status', 'Status'], ['actual_decision_digest', 'Decision digest'], ['completed_at', 'Completed']]},
    evidence: {path: '/api/evidence', title: 'event', columns: [['sequence_no', 'Sequence'], ['event_type', 'Event'], ['aggregate_type', 'Aggregate'], ['occurred_at', 'Occurred'], ['event_hash', 'Hash']]},
    integrations: {path: '/api/integrations', title: 'integration', columns: [['adapter_type', 'Adapter'], ['enabled', 'Enabled'], ['required_for_promotion', 'Required'], ['health_status', 'Health'], ['last_error_code', 'Last error']]},
    credentials: {path: '/api/credentials', title: 'credential', columns: [['label', 'Label'], ['role', 'Role'], ['key_prefix', 'Prefix'], ['last_used_at', 'Last used'], ['revoked_at', 'Revoked']]},
    'signing-keys': {path: '/api/artifact-signing-keys', title: 'signing key', columns: [['name', 'Name'], ['algorithm', 'Algorithm'], ['status', 'Status'], ['fingerprint_sha256', 'Fingerprint']]},
    tenant: {path: '/api/tenants/me', title: 'tenant', columns: []}
  };
  const operatorRole = () => document.querySelector('main')?.dataset.role ?? '';
  const canWrite = (resource) => operatorRole() === 'admin' || (operatorRole() === 'release_manager' && ['fleets', 'devices', 'artifacts', 'policies', 'release_drafts', 'simulation', 'evidence_exports'].includes(resource));
  const detailPath = (kind, row) => row.id && ['fleets', 'devices', 'artifacts', 'policies', 'releases', 'simulations', 'replays'].includes(kind) ? `/app/${kind}/${encodeURIComponent(row.id)}` : '';
  const actionSpecs = (kind, row) => {
    const specs = [];
    if (kind === 'artifacts' && canWrite('artifacts')) specs.push(['validate', 'Validate', false]);
    if (kind === 'artifacts' && operatorRole() === 'admin') specs.push(['retire', 'Retire', true]);
    if (kind === 'policies' && canWrite('policies')) specs.push(['validate', 'Validate', false]);
    if (kind === 'policies' && operatorRole() === 'admin') specs.push(['activate', 'Activate', true], ['archive', 'Archive', true]);
    if (kind === 'simulations' && canWrite('simulation') && ['queued', 'running'].includes(row.status)) specs.push(['cancel', 'Cancel', true]);
    if (kind === 'simulations' && canWrite('simulation') && row.status === 'completed') specs.push(['replay', 'Replay', false]);
    if (kind === 'credentials' && operatorRole() === 'admin' && !row.revoked_at) specs.push(['revoke', 'Revoke', true], ['rotate', 'Rotate', false]);
    return specs;
  };
  const appendAction = (cell, path, label, requiresReason, onDone) => {
    const form = document.createElement('form'); form.className = 'row-action';
    if (requiresReason) { const reason = document.createElement('input'); reason.name = 'reason'; reason.required = true; reason.placeholder = 'Reason'; reason.setAttribute('aria-label', `${label} reason`); form.append(reason); }
    const button = document.createElement('button'); button.type = 'submit'; button.className = requiresReason ? 'danger' : 'secondary'; button.textContent = label;
    const status = document.createElement('span'); status.className = 'form-status'; status.setAttribute('aria-live', 'polite');
    form.addEventListener('submit', async (event) => { event.preventDefault(); const reason = form.elements.namedItem('reason'); if (requiresReason && !reason.value.trim()) { render(status, 'A reason is required.', 'danger'); return; } button.disabled = true; render(status, 'Working…'); try { await mutate(path, 'POST', requiresReason ? {reason: reason.value} : {}); render(status, 'Done.', 'loaded'); onDone?.(); } catch (error) { render(status, error.message, 'danger'); } finally { button.disabled = false; } });
    form.append(button, status); cell.append(form);
  };
  const renderTable = (element, rows, columns, title, kind = '') => {
    element.replaceChildren();
    if (!rows.length) { render(element, `No ${title} records yet.`); return; }
    const wrapper = document.createElement('div'); wrapper.className = 'table-scroll';
    const table = document.createElement('table'); const head = document.createElement('thead'); const headerRow = document.createElement('tr');
    const hasActions = rows.some((row) => detailPath(kind, row) || actionSpecs(kind, row).length);
    columns.forEach(([, label]) => { const cell = document.createElement('th'); cell.scope = 'col'; cell.textContent = label; headerRow.append(cell); });
    if (hasActions) { const cell = document.createElement('th'); cell.scope = 'col'; cell.textContent = 'Actions'; headerRow.append(cell); }
    const body = document.createElement('tbody');
    rows.slice(0, 50).forEach((row) => { const tableRow = document.createElement('tr'); columns.forEach(([key]) => { const cell = document.createElement('td'); cell.textContent = text(row[key]); tableRow.append(cell); }); if (hasActions) { const actions = document.createElement('td'); actions.className = 'table-actions'; const path = detailPath(kind, row); if (path) { const link = document.createElement('a'); link.href = path; link.textContent = 'Open'; actions.append(link); } for (const [action, label, requiresReason] of actionSpecs(kind, row)) appendAction(actions, `${definitions[kind].path}/${encodeURIComponent(row.id)}/${action}`, label, requiresReason, () => refreshResource(element)); tableRow.append(actions); } body.append(tableRow); });
    table.append(head, body); wrapper.append(table); element.append(wrapper); element.dataset.state = 'loaded';
  };
  const renderDetail = (element, result, title) => {
    element.replaceChildren(); const heading = document.createElement('h3'); heading.textContent = `${title} state`; element.append(heading);
    const list = document.createElement('dl');
    Object.entries(result).filter(([, value]) => !Array.isArray(value) && typeof value !== 'object').slice(0, 24).forEach(([key, value]) => { const term = document.createElement('dt'); term.textContent = key.replaceAll('_', ' '); const description = document.createElement('dd'); description.textContent = text(value); list.append(term, description); });
    element.append(list);
    Object.entries(result).filter(([, value]) => Array.isArray(value)).forEach(([key, value]) => { const subheading = document.createElement('h3'); subheading.textContent = key.replaceAll('_', ' '); element.append(subheading); if (value.length && typeof value[0] === 'object') { const table = document.createElement('div'); renderTable(table, value, Object.keys(value[0]).slice(0, 6).map((field) => [field, field.replaceAll('_', ' ')]), key); element.append(table); } else { const paragraph = document.createElement('p'); paragraph.textContent = value.map(text).join(', ') || 'None'; element.append(paragraph); } });
    element.dataset.state = 'loaded';
  };
  const renderIntegrations = (element, rows) => {
    element.replaceChildren();
    if (!rows.length) { render(element, 'No adapter configurations yet.', 'status'); return; }
    const wrapper = document.createElement('div'); wrapper.className = 'table-scroll';
    const table = document.createElement('table'); const head = document.createElement('thead'); const header = document.createElement('tr');
    [...definitions.integrations.columns, ['actions', 'Actions']].forEach(([, label]) => { const cell = document.createElement('th'); cell.scope = 'col'; cell.textContent = label; header.append(cell); }); head.append(header);
    const body = document.createElement('tbody');
    rows.forEach((row) => {
      const tableRow = document.createElement('tr');
      definitions.integrations.columns.forEach(([key]) => { const cell = document.createElement('td'); cell.textContent = key === 'enabled' || key === 'required_for_promotion' ? (row[key] ? 'Yes' : 'No') : text(row[key]); tableRow.append(cell); });
      const actions = document.createElement('td'); const form = document.createElement('form'); form.className = 'integration-actions';
      const reason = document.createElement('input'); reason.name = 'reason'; reason.required = true; reason.placeholder = 'Reason'; reason.setAttribute('aria-label', `Reason for ${row.adapter_type}`);
      const test = document.createElement('button'); test.type = 'button'; test.className = 'secondary'; test.textContent = 'Test connection';
      const toggle = document.createElement('button'); toggle.type = 'submit'; toggle.dataset.action = row.enabled ? 'disable' : 'enable'; toggle.textContent = row.enabled ? 'Disable' : 'Enable';
      const status = document.createElement('span'); status.className = 'form-status'; status.setAttribute('aria-live', 'polite');
      const run = async (action) => { if (action !== 'test' && !reason.value.trim()) { render(status, 'A reason is required.', 'danger'); return; } test.disabled = true; toggle.disabled = true; render(status, action === 'test' ? 'Testing…' : `${action === 'enable' ? 'Enabling' : 'Disabling'}…`); try { const result = await mutate(`/api/integrations/${encodeURIComponent(row.adapter_type)}/${action}`, 'POST', action === 'test' ? {} : {reason: reason.value}); render(status, result.status ? `Result: ${result.status}.` : 'Saved.', 'loaded'); if (action !== 'test') refreshResource(element); } catch (error) { render(status, error.message, 'danger'); } finally { test.disabled = false; toggle.disabled = false; } };
      test.addEventListener('click', () => run('test')); form.addEventListener('submit', (event) => { event.preventDefault(); run(toggle.dataset.action); }); form.append(reason, test, toggle, status); actions.append(form); tableRow.append(actions); body.append(tableRow);
    });
    table.append(head, body); wrapper.append(table); element.append(wrapper); element.dataset.state = 'loaded';
  };
  const resourcePath = (element) => {
    const kind = element.dataset.resource; const id = element.dataset.resourceId;
    if (id) { if (kind === 'tenant') return '/api/tenants/me'; if (kind === 'fleet') return `/api/fleets/${encodeURIComponent(id)}`; if (kind === 'device') return `/api/devices/${encodeURIComponent(id)}`; if (kind === 'release') return `/api/releases/${encodeURIComponent(id)}`; if (kind === 'gates') return `/api/releases/${encodeURIComponent(id)}/gates`; if (kind === 'assignments') return `/api/releases/${encodeURIComponent(id)}/assignments`; if (kind === 'membership') return `/api/releases/${encodeURIComponent(id)}/membership`; if (kind === 'simulation') return `/api/simulations/${encodeURIComponent(id)}`; }
    const definition = definitions[kind]; return definition ? `${definition.path}${element.dataset.resourceQuery ? `?${element.dataset.resourceQuery}` : ''}` : '';
  };
  const refreshResource = async (element) => {
    const path = resourcePath(element); if (!path) return;
    try {
      const result = await load(path); const kind = element.dataset.resource;
      if (element.dataset.resourceId || kind === 'tenant') { renderDetail(element, result, kind); if (kind === 'tenant') { const form = document.querySelector('[data-api-action="/api/tenants/me"]'); if (form) { for (const field of ['display_name', 'legal_name']) { const control = form.elements.namedItem(field); if (control && result[field]) control.value = result[field]; } } } }
      else {
        const definition = definitions[kind]; let rows = Array.isArray(result) ? result : (result.items || []);
        if (kind === 'integrations') renderIntegrations(element, rows);
        else if (kind === 'approvals' && element.dataset.canApprove === 'true') {
          renderTable(element, rows, [...definition.columns, ['decision', 'Decision']], definition.title, kind);
          rows.filter((row) => row.status === 'requested').forEach((row) => { const action = document.createElement('form'); action.className = 'approval-action'; const reason = document.createElement('input'); reason.required = true; reason.placeholder = 'Decision reason'; reason.setAttribute('aria-label', `Reason for ${row.action}`); const approve = document.createElement('button'); approve.type = 'submit'; approve.textContent = `Approve ${row.action}`; const reject = document.createElement('button'); reject.type = 'button'; reject.className = 'secondary'; reject.textContent = `Reject ${row.action}`; const decide = async (choice) => { try { await mutate(`/api/approvals/${encodeURIComponent(row.id)}/${choice}`, 'POST', {reason: reason.value}); await refreshResource(element); } catch (error) { render(element, error.message, 'danger'); } }; action.addEventListener('submit', (event) => { event.preventDefault(); decide('approve'); }); reject.addEventListener('click', () => decide('reject')); action.append(reason, approve, reject); element.append(action); });
        } else renderTable(element, rows, definition.columns, definition.title, kind);
      }
      if (Number.isInteger(result.version)) document.querySelectorAll('[name="expected_version"]').forEach((input) => { input.value = result.version; });
    } catch (error) { render(element, `${element.dataset.resource} data unavailable: ${error.message}`, 'warning'); }
  };
  const bindResources = () => document.querySelectorAll('.resource-state').forEach((element) => { element.addEventListener('refresh-resource', () => refreshResource(element)); refreshResource(element); });
  const bindFilters = () => document.querySelectorAll('[data-resource-filter]').forEach((form) => form.addEventListener('submit', (event) => { event.preventDefault(); const resource = document.querySelector(`.resource-state[data-resource="${form.dataset.resourceFilter}"]`); if (!resource) return; const query = new URLSearchParams(new FormData(form)); resource.dataset.resourceQuery = query.toString(); resource.dispatchEvent(new Event('refresh-resource')); }));
  const bindJsonForms = () => document.querySelectorAll('form[data-api-action]').forEach((form) => form.addEventListener('submit', async (event) => { event.preventDefault(); const feedback = form.querySelector('.form-status'); render(feedback, 'Saving…'); try { const result = await mutate(form.dataset.apiAction, form.dataset.apiMethod || 'POST', formPayload(form)); showResult(form, result); document.querySelectorAll('.resource-state').forEach((resource) => resource.dispatchEvent(new Event('refresh-resource'))); } catch (error) { render(feedback, error.message, 'danger'); } }));
  const bindReleaseControl = () => document.querySelectorAll('[data-release-control]').forEach((form) => form.addEventListener('submit', async (event) => { event.preventDefault(); const action = valueOf(form, 'action'); if ((action === 'abort' || action === 'rollback') && !window.confirm(`Confirm ${action}. This safety action cannot be casually undone.`)) return; const feedback = form.querySelector('.form-status'); render(feedback, 'Submitting safety action…'); const payload = {expected_version: Number(valueOf(form, 'expected_version')), reason: valueOf(form, 'reason')}; if (action === 'schedule') payload.scheduled_for = valueOf(form, 'scheduled_for'); try { const result = await mutate(`/api/releases/${encodeURIComponent(form.dataset.releaseId)}/${action}`, 'POST', payload); showResult(form, result); document.querySelectorAll('.resource-state').forEach((resource) => resource.dispatchEvent(new Event('refresh-resource'))); } catch (error) { render(feedback, error.message, 'danger'); } }));
  const bindArtifactUpload = () => document.querySelectorAll('[data-artifact-upload]').forEach((form) => form.addEventListener('submit', async (event) => { event.preventDefault(); const feedback = form.querySelector('.form-status'); const file = form.elements.namedItem('file').files[0]; if (!file) { render(feedback, 'Choose artifact bytes first.', 'danger'); return; } try { const manifest = JSON.parse(valueOf(form, 'manifest_json')); render(feedback, 'Streaming and validating…'); const headers = {'X-Artifact-Name': valueOf(form, 'name'), 'X-Artifact-Version': valueOf(form, 'version'), 'X-Artifact-Hardware-Model': valueOf(form, 'hardware_model'), 'X-Artifact-Architecture': valueOf(form, 'architecture'), 'X-Artifact-File-Name': valueOf(form, 'file_name'), 'X-Artifact-Signing-Key-Id': valueOf(form, 'signing_key_id'), 'X-Artifact-Manifest': JSON.stringify(manifest), 'X-Artifact-Signature': valueOf(form, 'signature'), 'Idempotency-Key': idempotencyKey(), 'X-CSRF-Token': csrf()}; const response = await fetch('/api/artifacts', {method: 'POST', headers, body: file}); const result = await response.json().catch(() => ({})); if (!response.ok) throw new Error(result.error?.message ?? `Upload returned ${response.status}`); showResult(form, result); document.querySelectorAll('[data-resource="artifacts"]').forEach(refreshResource); } catch (error) { render(feedback, error.message, 'danger'); } }));
  const bindOptionLists = async () => { await Promise.all([...document.querySelectorAll('[data-options-resource]')].map(async (select) => { try { const result = await load(definitions[select.dataset.optionsResource].path); const current = [...select.options].map((option) => option.cloneNode(true)); select.replaceChildren(...current); (result.items || []).forEach((row) => { const option = document.createElement('option'); option.value = row.id; option.textContent = row.name || row.file_name || row.id; select.append(option); }); } catch (_) { select.disabled = true; } })); };
  const bindIntegrationConfig = () => document.querySelectorAll('[data-integration-config]').forEach((form) => form.addEventListener('submit', async (event) => { event.preventDefault(); const feedback = form.querySelector('.form-status'); const adapter = valueOf(form, 'adapter_type'); const settings = {fixture_mode: valueOf(form, 'fixture_mode') === 'true'}; const workflowId = valueOf(form, 'workflow_id'); if (workflowId) settings.workflow_id = workflowId; const payload = {endpoint_base_url: valueOf(form, 'endpoint_base_url'), secret_ref: valueOf(form, 'secret_ref'), required_for_promotion: form.elements.namedItem('required_for_promotion')?.checked === true, settings}; render(feedback, 'Saving disabled configuration…'); try { const result = await mutate(`/api/integrations/${encodeURIComponent(adapter)}`, 'PUT', payload); showResult(form, result); document.querySelectorAll('[data-resource="integrations"]').forEach(refreshResource); } catch (error) { render(feedback, error.message, 'danger'); } }));
  const bindSignOut = () => document.querySelector('#sign-out')?.addEventListener('click', async () => { const token = csrf(); try { await fetch('/auth/session', {method: 'DELETE', headers: {'X-CSRF-Token': token}}); } finally { window.location.assign('/login'); } });

  const loginForm = document.querySelector('form[action="/auth/session"]');
  if (loginForm) { const feedback = document.createElement('p'); feedback.className = 'status'; feedback.setAttribute('aria-live', 'polite'); loginForm.insertAdjacentElement('afterend', feedback); loginForm.addEventListener('submit', async (event) => { event.preventDefault(); render(feedback, 'Signing in…'); try { const response = await fetch('/auth/session', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({api_key: new FormData(loginForm).get('api_key')})}); if (!response.ok) { const result = await response.json().catch(() => ({})); throw new Error(result.error?.message ?? 'Sign-in was rejected.'); } window.location.assign('/app'); } catch (error) { render(feedback, error.message, 'danger'); } }); return; }
  if (!document.querySelector('[data-api-state], .resource-state')) return;
  const state = document.querySelector('[data-api-state]');
  if (state) (async () => { try { const [tenant, fleets, releases, notices] = await Promise.all([load('/api/tenants/me'), load('/api/fleets'), load('/api/releases'), load('/api/notices')]); const active = (releases.items || []).filter((release) => ['scheduled', 'running', 'paused', 'aborting', 'rolling_back'].includes(release.status)).length; const unread = (notices.items || []).filter((notice) => !notice.acknowledged_at).length; state.replaceChildren(); const list = document.createElement('ul'); list.className = 'summary-list'; [['Tenant', tenant.display_name || tenant.name || 'Current tenant'], ['Fleets', plural((fleets.items || []).length, 'fleet')], ['Active releases', plural(active, 'release')], ['Unacknowledged notices', plural(unread, 'notice')]].forEach(([label, value]) => { const item = document.createElement('li'); const labelElement = document.createElement('span'); labelElement.textContent = label; const valueElement = document.createElement('strong'); valueElement.textContent = value; item.append(labelElement, valueElement); list.append(item); }); state.append(list); state.dataset.state = 'loaded'; } catch (error) { render(state, `Live API state unavailable: ${error.message}`, 'warning'); } })();
  bindJsonForms(); bindReleaseControl(); bindArtifactUpload(); bindResources(); bindFilters(); bindOptionLists(); bindIntegrationConfig(); bindSignOut();
})();
