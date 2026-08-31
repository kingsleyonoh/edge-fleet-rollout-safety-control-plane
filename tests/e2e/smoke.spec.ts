import { test, expect } from '@playwright/test';

test('health and login surfaces are reachable', async ({ page }) => {
  const health = await page.request.get('/health');
  expect(health.ok()).toBeTruthy();
  await page.goto('/login');
  await expect(page.getByRole('heading', { name: /sign in/i })).toBeVisible();
  await expect(page.getByLabel(/api key/i)).toBeVisible();
});

test('admin console journey renders data-backed screens and viewer controls stay hidden', async ({ page, request }) => {
  test.setTimeout(60_000);
  const suffix = `${Date.now()}-${Math.random().toString(16).slice(2)}`;
  await page.context().setExtraHTTPHeaders({'X-Peer-Address': `e2e-admin-${suffix}`});
  const registration = await request.post('/api/tenants/register', {headers: {'X-Peer-Address': `e2e-admin-${suffix}`}, data: {name: `Browser Tenant ${suffix}`}});
  expect(registration.status()).toBe(201);
  const admin = await registration.json();
  const viewerCredential = await request.post('/api/credentials', {
    headers: {Authorization: `Bearer ${admin.api_key}`, 'Idempotency-Key': `browser-viewer-${suffix}`},
    data: {label: 'browser-viewer', role: 'viewer'},
  });
  expect(viewerCredential.status()).toBe(201);
  const viewer = await viewerCredential.json();

  const signIn = async (apiKey: string) => {
    await page.goto('/login');
    await page.getByLabel(/api key/i).fill(apiKey);
    await page.getByRole('button', {name: /create browser session/i}).click();
    await expect(page).toHaveURL(/\/app$/);
  };
  await signIn(admin.api_key);
  await expect(page.locator('#live-releases[data-state="loaded"]')).toBeVisible();
  await expect(page.locator('#live-jobs[data-state="loaded"]')).toBeVisible();
  for (const [path, heading] of [
    ['/app', /fleet overview/i], ['/app/fleets', /^fleets$/i], ['/app/devices', /device truth/i], ['/app/artifacts', /^artifacts$/i],
    ['/app/policies', /^policies$/i], ['/app/releases', /releases/i], ['/app/approvals', /^approvals$/i], ['/app/simulations', /^simulations$/i],
    ['/app/replays', /replay evidence/i], ['/app/evidence', /evidence chain/i], ['/app/settings/integrations', /optional integrations/i],
    ['/app/settings/tenant', /tenant identity/i],
  ] as const) {
    const response = await page.goto(path);
    expect(response?.status(), path).toBe(200);
    await expect(page.locator('main')).toHaveCount(1);
    await expect(page.locator('h1')).toHaveCount(1);
    await expect(page.getByRole('heading', {name: heading})).toBeVisible();
  }
  await page.locator('#sign-out').click();
  await expect(page).toHaveURL(/\/login$/);
  await signIn(viewer.api_key);
  const fleetsResponse = await page.goto('/app/fleets');
  expect(fleetsResponse?.status()).toBe(200);
  await expect(page.getByRole('heading', {name: /^fleets$/i})).toBeVisible();
  await expect(page.getByRole('heading', {name: /create fleet/i})).toHaveCount(0);
  const settingsResponse = await page.goto('/app/settings/integrations');
  expect(settingsResponse?.status()).toBe(403);
});

test('operator UI exposes role-scoped actions, API truth, keyboard focus, and mobile controls', async ({ page, request }) => {
  test.setTimeout(90_000);
  const suffix = `${Date.now()}-${Math.random().toString(16).slice(2)}`;
  await page.context().setExtraHTTPHeaders({'X-Peer-Address': `e2e-contract-${suffix}`});
  const registration = await request.post('/api/tenants/register', {headers: {'X-Peer-Address': `e2e-contract-${suffix}`}, data: {name: `UI Contract Tenant ${suffix}`}});
  expect(registration.status()).toBe(201);
  const admin = await registration.json();
  const createCredential = async (role: string) => {
    const response = await request.post('/api/credentials', {
      headers: {Authorization: `Bearer ${admin.api_key}`, 'Idempotency-Key': `ui-${role}-${suffix}`},
      data: {label: `ui-${role}`, role},
    });
    expect(response.status()).toBe(201);
    return response.json();
  };
  const viewer = await createCredential('viewer');
  const manager = await createCredential('release_manager');
  const approver = await createCredential('approver');
  const fleet = await request.post('/api/fleets', {
    headers: {Authorization: `Bearer ${admin.api_key}`, 'Idempotency-Key': `ui-fleet-${suffix}`},
    data: {name: 'UI fleet', slug: `ui-${suffix.toLowerCase().replace(/[^a-z0-9-]/g, '-')}`, environment: 'staging', label_schema: {}},
  });
  expect(fleet.status()).toBe(201);

  const signIn = async (apiKey: string) => {
    await page.goto('/login');
    await page.getByLabel(/api key/i).fill(apiKey);
    await page.getByRole('button', {name: /create browser session/i}).click();
    await expect(page).toHaveURL(/\/app$/);
  };
  await signIn(admin.api_key);
  await page.goto('/app/fleets');
  await expect(page.locator('.resource-state[data-state="loaded"]')).toBeVisible();
  const apiCount = await page.evaluate(async () => (await (await fetch('/api/fleets')).json()).items.length);
  expect(await page.locator('.resource-state tbody tr').count()).toBe(apiCount);
  await page.locator('.skip-link').focus();
  await expect(page.locator('.skip-link')).toBeFocused();
  await page.emulateMedia({reducedMotion: 'reduce'});
  expect(await page.evaluate(() => getComputedStyle(document.documentElement).scrollBehavior)).toBe('auto');
  const undersized = await page.evaluate(() => [...document.querySelectorAll('button, input, select, textarea, nav a')].filter((element) => {
    const box = element.getBoundingClientRect(); return box.width > 0 && (box.width < 44 || box.height < 44);
  }).map((element) => `${element.tagName}:${element.textContent?.trim() || element.getAttribute('name') || element.getAttribute('href')}`));
  expect(undersized).toEqual([]);
  await expect(page.getByRole('link', {name: 'Open'})).toHaveCount(1);
  await expect(page.getByRole('link', {name: 'Open'})).toHaveAttribute('href', /\/app\/fleets\//);

  await page.locator('#sign-out').click();
  await expect(page).toHaveURL(/\/login$/);
  await signIn(manager.api_key);
  await page.goto('/app/artifacts');
  await expect(page.getByRole('heading', {name: /stream an artifact/i})).toBeVisible();
  await expect(page.getByRole('link', {name: 'Integrations'})).toHaveCount(0);

  await page.locator('#sign-out').click();
  await expect(page).toHaveURL(/\/login$/);
  await signIn(approver.api_key);
  await page.goto('/app/approvals');
  await expect(page.getByRole('heading', {name: /pending safety decisions/i})).toBeVisible();
  await expect(page.getByRole('link', {name: 'Approvals'})).toBeVisible();

  await page.locator('#sign-out').click();
  await expect(page).toHaveURL(/\/login$/);
  await signIn(viewer.api_key);
  await page.goto('/app/fleets');
  await expect(page.getByRole('heading', {name: /create fleet/i})).toHaveCount(0);
  await expect(page.getByRole('link', {name: 'Integrations'})).toHaveCount(0);
});
