PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;

CREATE TABLE IF NOT EXISTS schema_version (version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL);

CREATE TABLE IF NOT EXISTS tenants (
  id TEXT PRIMARY KEY, name TEXT NOT NULL, legal_name TEXT NOT NULL, full_legal_name TEXT NOT NULL,
  display_name TEXT NOT NULL, address TEXT NOT NULL DEFAULT '{}', registration TEXT NOT NULL DEFAULT '{}',
  contact TEXT NOT NULL DEFAULT '{}', wordmark TEXT, brand_color TEXT NOT NULL DEFAULT '#3B82F6',
  default_timezone TEXT NOT NULL DEFAULT 'UTC', api_key_prefix TEXT NOT NULL UNIQUE, api_key_hash TEXT NOT NULL,
  cohort_secret_ciphertext TEXT NOT NULL DEFAULT '', is_active INTEGER NOT NULL DEFAULT 1,
  created_at TEXT NOT NULL, updated_at TEXT NOT NULL, UNIQUE(id)
);
CREATE TABLE IF NOT EXISTS operator_credentials (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, label TEXT NOT NULL, role TEXT NOT NULL CHECK(role IN ('admin','release_manager','approver','viewer')),
  key_prefix TEXT NOT NULL UNIQUE, key_hash TEXT NOT NULL, last_used_at TEXT, expires_at TEXT, revoked_at TEXT,
  created_at TEXT NOT NULL, updated_at TEXT NOT NULL, FOREIGN KEY(tenant_id) REFERENCES tenants(id), UNIQUE(tenant_id,id)
);
CREATE INDEX IF NOT EXISTS idx_operator_credentials_tenant_role ON operator_credentials(tenant_id,role,revoked_at);
CREATE TABLE IF NOT EXISTS fleets (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, name TEXT NOT NULL, slug TEXT NOT NULL, description TEXT NOT NULL DEFAULT '',
  environment TEXT NOT NULL CHECK(environment IN ('development','staging','production')), status TEXT NOT NULL DEFAULT 'active' CHECK(status IN ('active','paused','retired')),
  label_schema_json TEXT NOT NULL DEFAULT '{}', version INTEGER NOT NULL DEFAULT 1, created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
  FOREIGN KEY(tenant_id) REFERENCES tenants(id), UNIQUE(tenant_id,id), UNIQUE(tenant_id,slug)
);
CREATE INDEX IF NOT EXISTS idx_fleets_tenant_status ON fleets(tenant_id,status,environment);
CREATE TABLE IF NOT EXISTS devices (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, fleet_id TEXT NOT NULL, stable_key TEXT NOT NULL, display_name TEXT NOT NULL,
  hardware_model TEXT NOT NULL, architecture TEXT NOT NULL, labels_json TEXT NOT NULL DEFAULT '{}',
  lifecycle_status TEXT NOT NULL DEFAULT 'registered' CHECK(lifecycle_status IN ('registered','active','quarantined','decommissioned')),
  desired_generation INTEGER NOT NULL DEFAULT 0, observed_generation INTEGER, observed_artifact_digest TEXT,
  last_report_sequence INTEGER NOT NULL DEFAULT 0, last_seen_at TEXT, external_device_ref TEXT, device_secret_hash TEXT NOT NULL DEFAULT '',
  device_key_version INTEGER NOT NULL DEFAULT 1, created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
  FOREIGN KEY(tenant_id,fleet_id) REFERENCES fleets(tenant_id,id), UNIQUE(tenant_id,id), UNIQUE(tenant_id,fleet_id,stable_key)
);
CREATE INDEX IF NOT EXISTS idx_devices_tenant_fleet ON devices(tenant_id,fleet_id,lifecycle_status);
CREATE TABLE IF NOT EXISTS device_credentials (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, device_id TEXT NOT NULL, key_version INTEGER NOT NULL, secret_ciphertext TEXT NOT NULL,
  supersedes_credential_id TEXT, activated_at TEXT NOT NULL, acknowledged_at TEXT, expires_at TEXT, revoked_at TEXT, created_at TEXT NOT NULL,
  FOREIGN KEY(tenant_id,device_id) REFERENCES devices(tenant_id,id), FOREIGN KEY(tenant_id,supersedes_credential_id) REFERENCES device_credentials(tenant_id,id), UNIQUE(tenant_id,id), UNIQUE(tenant_id,device_id,key_version)
);
CREATE TABLE IF NOT EXISTS artifact_signing_keys (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, name TEXT NOT NULL, algorithm TEXT NOT NULL CHECK(algorithm='ed25519'),
  public_key_pem TEXT NOT NULL, fingerprint_sha256 TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'active' CHECK(status IN ('active','retired','compromised')),
  created_by_actor_id TEXT NOT NULL, retired_at TEXT, created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
  FOREIGN KEY(tenant_id) REFERENCES tenants(id), UNIQUE(tenant_id,id), UNIQUE(tenant_id,fingerprint_sha256)
);
CREATE TABLE IF NOT EXISTS artifacts (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, name TEXT NOT NULL, version TEXT NOT NULL, hardware_model TEXT NOT NULL, architecture TEXT NOT NULL,
  status TEXT NOT NULL DEFAULT 'uploading' CHECK(status IN ('uploading','validating','ready','blocked','retired')), file_name TEXT NOT NULL,
  storage_key TEXT NOT NULL, size_bytes INTEGER NOT NULL, sha256_digest TEXT NOT NULL, manifest_json TEXT NOT NULL, signature TEXT NOT NULL,
  signature_key_id TEXT NOT NULL, validation_error TEXT, created_by_actor_id TEXT NOT NULL, created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
  FOREIGN KEY(tenant_id,signature_key_id) REFERENCES artifact_signing_keys(tenant_id,id), UNIQUE(tenant_id,id), UNIQUE(tenant_id,name,version,hardware_model,architecture)
);
CREATE INDEX IF NOT EXISTS idx_artifacts_tenant_status ON artifacts(tenant_id,status,hardware_model,architecture);
CREATE TABLE IF NOT EXISTS rollout_policies (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, name TEXT NOT NULL, version INTEGER NOT NULL, status TEXT NOT NULL DEFAULT 'draft' CHECK(status IN ('draft','active','archived')),
  selector_json TEXT NOT NULL, stage_plan_json TEXT NOT NULL, health_gates_json TEXT NOT NULL, max_offline_fraction REAL NOT NULL,
  telemetry_freshness_sec INTEGER NOT NULL, min_observation_sec INTEGER NOT NULL, two_person_approval INTEGER NOT NULL DEFAULT 1,
  require_iot_evidence INTEGER NOT NULL DEFAULT 0, rollback_requirement TEXT NOT NULL CHECK(rollback_requirement IN ('required','allow_first_install')),
  created_by_actor_id TEXT NOT NULL, created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
  FOREIGN KEY(tenant_id) REFERENCES tenants(id), UNIQUE(tenant_id,id), UNIQUE(tenant_id,name,version)
);
CREATE TABLE IF NOT EXISTS releases (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, fleet_id TEXT NOT NULL, artifact_id TEXT NOT NULL, rollback_artifact_id TEXT, policy_id TEXT NOT NULL,
  name TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'draft' CHECK(status IN ('draft','validating','blocked','ready','awaiting_approval','scheduled','running','paused','aborting','rolling_back','completed','aborted','rolled_back','failed','cancelled')), status_reason_code TEXT, status_reason_text TEXT, target_selector_json TEXT NOT NULL,
  frozen_policy_json TEXT, frozen_manifest_json TEXT, frozen_rollback_json TEXT, cohort_salt_ciphertext TEXT, membership_digest TEXT,
  eligible_device_count INTEGER, current_stage_ordinal INTEGER, scheduled_for TEXT, started_at TEXT, ended_at TEXT,
  version INTEGER NOT NULL DEFAULT 1, created_by_actor_id TEXT NOT NULL, created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
  FOREIGN KEY(tenant_id,fleet_id) REFERENCES fleets(tenant_id,id), FOREIGN KEY(tenant_id,artifact_id) REFERENCES artifacts(tenant_id,id), FOREIGN KEY(tenant_id,rollback_artifact_id) REFERENCES artifacts(tenant_id,id),
  FOREIGN KEY(tenant_id,policy_id) REFERENCES rollout_policies(tenant_id,id), UNIQUE(tenant_id,id)
);
CREATE TABLE IF NOT EXISTS release_memberships (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, release_id TEXT NOT NULL, device_id TEXT NOT NULL, cohort_hash_hex TEXT NOT NULL,
  cohort_ordinal INTEGER NOT NULL, frozen_labels_json TEXT NOT NULL, frozen_observed_digest TEXT, frozen_observed_generation INTEGER, included_at TEXT NOT NULL,
  FOREIGN KEY(tenant_id,release_id) REFERENCES releases(tenant_id,id), FOREIGN KEY(tenant_id,device_id) REFERENCES devices(tenant_id,id), UNIQUE(tenant_id,id), UNIQUE(tenant_id,release_id,device_id)
);
CREATE TABLE IF NOT EXISTS release_stages (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, release_id TEXT NOT NULL, ordinal INTEGER NOT NULL, target_percentage REAL NOT NULL,
  status TEXT NOT NULL DEFAULT 'pending' CHECK(status IN ('pending','active','passed','failed','halted','rolled_back')), eligible_count INTEGER NOT NULL,
  assigned_count INTEGER NOT NULL DEFAULT 0, observation_started_at TEXT, observation_ends_at TEXT, gate_decision_json TEXT,
  started_at TEXT, ended_at TEXT, created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
  FOREIGN KEY(tenant_id,release_id) REFERENCES releases(tenant_id,id), UNIQUE(tenant_id,id), UNIQUE(tenant_id,release_id,ordinal)
);
CREATE TABLE IF NOT EXISTS release_assignments (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, release_id TEXT NOT NULL, stage_id TEXT NOT NULL, device_id TEXT NOT NULL, desired_artifact_id TEXT NOT NULL,
  desired_generation INTEGER NOT NULL, state TEXT NOT NULL DEFAULT 'pending' CHECK(state IN ('pending','commanded','acknowledged','converged','failed','cancelling','stranded','rolled_back','superseded')), latest_report_sequence INTEGER NOT NULL DEFAULT 0,
  commanded_at TEXT, acknowledged_at TEXT, converged_at TEXT, failure_code TEXT, updated_at TEXT NOT NULL,
  FOREIGN KEY(tenant_id,release_id) REFERENCES releases(tenant_id,id), FOREIGN KEY(tenant_id,stage_id) REFERENCES release_stages(tenant_id,id),
  FOREIGN KEY(tenant_id,device_id) REFERENCES devices(tenant_id,id), FOREIGN KEY(tenant_id,desired_artifact_id) REFERENCES artifacts(tenant_id,id), UNIQUE(tenant_id,id), UNIQUE(tenant_id,release_id,device_id)
);
CREATE TABLE IF NOT EXISTS rollout_commands (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, release_id TEXT NOT NULL, stage_id TEXT NOT NULL, assignment_id TEXT NOT NULL, device_id TEXT NOT NULL,
  command_type TEXT NOT NULL CHECK(command_type IN ('install','rollback','cancel')), desired_generation INTEGER NOT NULL, artifact_id TEXT,
  payload_json TEXT NOT NULL, idempotency_key TEXT NOT NULL, supersedes_command_id TEXT, not_before TEXT NOT NULL, expires_at TEXT NOT NULL, issued_at TEXT NOT NULL,
  FOREIGN KEY(tenant_id,release_id) REFERENCES releases(tenant_id,id), FOREIGN KEY(tenant_id,stage_id) REFERENCES release_stages(tenant_id,id),
  FOREIGN KEY(tenant_id,assignment_id) REFERENCES release_assignments(tenant_id,id), FOREIGN KEY(tenant_id,device_id) REFERENCES devices(tenant_id,id), FOREIGN KEY(tenant_id,artifact_id) REFERENCES artifacts(tenant_id,id), FOREIGN KEY(tenant_id,supersedes_command_id) REFERENCES rollout_commands(tenant_id,id), UNIQUE(tenant_id,id), UNIQUE(tenant_id,idempotency_key)
);
CREATE TABLE IF NOT EXISTS device_reports (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, device_id TEXT NOT NULL, release_id TEXT, command_id TEXT, report_id TEXT NOT NULL, report_sequence INTEGER NOT NULL,
  report_type TEXT NOT NULL, observed_generation INTEGER, observed_artifact_digest TEXT, health_json TEXT NOT NULL DEFAULT '{}', result_code TEXT,
  payload_digest TEXT NOT NULL, device_recorded_at TEXT, server_received_at TEXT NOT NULL,
  FOREIGN KEY(tenant_id,device_id) REFERENCES devices(tenant_id,id), FOREIGN KEY(tenant_id,release_id) REFERENCES releases(tenant_id,id), FOREIGN KEY(tenant_id,command_id) REFERENCES rollout_commands(tenant_id,id), UNIQUE(tenant_id,id), UNIQUE(tenant_id,device_id,report_id), UNIQUE(tenant_id,device_id,report_sequence)
);
CREATE TABLE IF NOT EXISTS health_samples (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, release_id TEXT NOT NULL, stage_id TEXT NOT NULL, device_id TEXT NOT NULL, source TEXT NOT NULL,
  source_event_id TEXT NOT NULL, metric_name TEXT NOT NULL, metric_value REAL NOT NULL, unit TEXT NOT NULL, observed_at TEXT NOT NULL, received_at TEXT NOT NULL,
  freshness_state TEXT NOT NULL CHECK(freshness_state IN ('fresh','stale','invalid')), created_at TEXT NOT NULL, CHECK(source IN ('device','iot_rest_v1')), FOREIGN KEY(tenant_id,release_id) REFERENCES releases(tenant_id,id), FOREIGN KEY(tenant_id,stage_id) REFERENCES release_stages(tenant_id,id), FOREIGN KEY(tenant_id,device_id) REFERENCES devices(tenant_id,id), UNIQUE(tenant_id,source,source_event_id,metric_name)
);
CREATE TABLE IF NOT EXISTS health_gate_evaluations (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, release_id TEXT NOT NULL, stage_id TEXT NOT NULL, decision TEXT NOT NULL CHECK(decision IN ('insufficient_evidence','pass','pause','abort','rollback')),
  sample_window_start TEXT NOT NULL, sample_window_end TEXT NOT NULL, sample_count INTEGER NOT NULL, eligible_device_count INTEGER NOT NULL,
  fresh_device_count INTEGER NOT NULL, metrics_json TEXT NOT NULL, failed_gates_json TEXT NOT NULL DEFAULT '[]', evidence_digest TEXT NOT NULL, evaluated_at TEXT NOT NULL, UNIQUE(tenant_id,id),
  FOREIGN KEY(tenant_id,release_id) REFERENCES releases(tenant_id,id), FOREIGN KEY(tenant_id,stage_id) REFERENCES release_stages(tenant_id,id)
);
CREATE TABLE IF NOT EXISTS approval_requests (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, release_id TEXT NOT NULL, action TEXT NOT NULL CHECK(action IN ('start','resume','abort','rollback','gate_override')), status TEXT NOT NULL DEFAULT 'requested' CHECK(status IN ('requested','approved','rejected','expired','superseded')),
  captured_release_version INTEGER NOT NULL, approved_release_version INTEGER, gate_evaluation_id TEXT, requested_by_actor_id TEXT NOT NULL,
  decided_by_actor_id TEXT, request_reason TEXT NOT NULL, decision_reason TEXT, evidence_digest TEXT NOT NULL, expires_at TEXT NOT NULL,
  decided_at TEXT, consumed_at TEXT, created_at TEXT NOT NULL, updated_at TEXT NOT NULL, FOREIGN KEY(tenant_id,release_id) REFERENCES releases(tenant_id,id), FOREIGN KEY(tenant_id,gate_evaluation_id) REFERENCES health_gate_evaluations(tenant_id,id)
);
CREATE TABLE IF NOT EXISTS simulation_runs (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, scenario_name TEXT NOT NULL, scenario_version TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'queued' CHECK(status IN ('queued','running','completed','failed','cancelled')), seed INTEGER NOT NULL,
  input_json TEXT NOT NULL, input_digest TEXT NOT NULL, simulator_version TEXT NOT NULL, result_json TEXT, result_digest TEXT, trace_storage_key TEXT, trace_digest TEXT,
  failure_message TEXT, requested_by_actor_id TEXT NOT NULL, attempt_count INTEGER NOT NULL DEFAULT 0, lease_owner TEXT, lease_expires_at TEXT,
  started_at TEXT, completed_at TEXT, created_at TEXT NOT NULL, UNIQUE(tenant_id,id)
);
CREATE TABLE IF NOT EXISTS replay_runs (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, release_id TEXT, simulation_run_id TEXT, source_kind TEXT NOT NULL CHECK(source_kind IN ('evidence','simulation')), status TEXT NOT NULL DEFAULT 'queued' CHECK(status IN ('queued','running','reproduced','diverged','failed')),
  source_event_from INTEGER, source_event_to INTEGER, source_snapshot_json TEXT, expected_decision_digest TEXT, actual_decision_digest TEXT, divergence_json TEXT,
  attempt_count INTEGER NOT NULL DEFAULT 0, lease_owner TEXT, lease_expires_at TEXT, started_at TEXT, completed_at TEXT, created_at TEXT NOT NULL,
  FOREIGN KEY(tenant_id,release_id) REFERENCES releases(tenant_id,id), FOREIGN KEY(tenant_id,simulation_run_id) REFERENCES simulation_runs(tenant_id,id)
);
CREATE TABLE IF NOT EXISTS benchmark_runs (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, corpus_version TEXT NOT NULL, corpus_manifest_json TEXT NOT NULL, corpus_manifest_digest TEXT NOT NULL,
  status TEXT NOT NULL DEFAULT 'queued' CHECK(status IN ('queued','running','completed','failed')), expected_case_count INTEGER NOT NULL, completed_case_count INTEGER NOT NULL DEFAULT 0, aggregate_metrics_json TEXT,
  result_digest TEXT, duckdb_storage_key TEXT, json_report_storage_key TEXT, markdown_report_storage_key TEXT, report_bundle_sha256 TEXT, failure_message TEXT,
  requested_by_actor_id TEXT NOT NULL, attempt_count INTEGER NOT NULL DEFAULT 0, lease_owner TEXT, lease_expires_at TEXT, started_at TEXT, completed_at TEXT, created_at TEXT NOT NULL, UNIQUE(tenant_id,id)
);
CREATE TABLE IF NOT EXISTS benchmark_results (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, benchmark_run_id TEXT NOT NULL, corpus_version TEXT NOT NULL, scenario_name TEXT NOT NULL,
  seed INTEGER NOT NULL, strategy TEXT NOT NULL, metrics_json TEXT NOT NULL, passed INTEGER NOT NULL, result_digest TEXT NOT NULL, created_at TEXT NOT NULL,
  FOREIGN KEY(tenant_id,benchmark_run_id) REFERENCES benchmark_runs(tenant_id,id), UNIQUE(tenant_id,benchmark_run_id,scenario_name,seed,strategy)
);
CREATE TABLE IF NOT EXISTS evidence_events (
  id TEXT PRIMARY KEY, sequence_no INTEGER NOT NULL, tenant_id TEXT NOT NULL, aggregate_type TEXT NOT NULL, aggregate_id TEXT NOT NULL,
  event_type TEXT NOT NULL, actor_type TEXT NOT NULL CHECK(actor_type IN ('operator','device','engine','job','adapter','simulator')), actor_id TEXT NOT NULL, payload_json TEXT NOT NULL, occurred_at TEXT NOT NULL, trace_id TEXT NOT NULL,
  previous_hash TEXT NOT NULL, event_hash TEXT NOT NULL, UNIQUE(tenant_id,id), UNIQUE(tenant_id,sequence_no), UNIQUE(tenant_id,event_hash)
);
CREATE TABLE IF NOT EXISTS evidence_checkpoints (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, sequence_no INTEGER NOT NULL, event_hash TEXT NOT NULL, projection_version TEXT NOT NULL,
  projection_state_json TEXT NOT NULL, verified_at TEXT NOT NULL, created_at TEXT NOT NULL, FOREIGN KEY(tenant_id,sequence_no) REFERENCES evidence_events(tenant_id,sequence_no), UNIQUE(tenant_id,sequence_no,projection_version)
);
CREATE TABLE IF NOT EXISTS evidence_exports (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'queued', source_event_from INTEGER NOT NULL, source_event_to INTEGER NOT NULL,
  source_chain_head_hash TEXT NOT NULL, aggregate_filters_json TEXT NOT NULL DEFAULT '{}', tenant_snapshot_json TEXT NOT NULL, chain_manifest_json TEXT,
  output_storage_key TEXT, output_sha256 TEXT, failure_message TEXT, requested_by_actor_id TEXT NOT NULL, attempt_count INTEGER NOT NULL DEFAULT 0,
  lease_owner TEXT, lease_expires_at TEXT, started_at TEXT, completed_at TEXT, created_at TEXT NOT NULL, FOREIGN KEY(tenant_id,source_chain_head_hash) REFERENCES evidence_events(tenant_id,event_hash)
);
CREATE TABLE IF NOT EXISTS operator_notices (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, release_id TEXT, event_id TEXT NOT NULL, severity TEXT NOT NULL, title TEXT NOT NULL, body TEXT NOT NULL,
  acknowledged_by_actor_id TEXT, acknowledged_at TEXT, created_at TEXT NOT NULL, CHECK(severity IN ('info','low','medium','high','critical')), FOREIGN KEY(tenant_id,release_id) REFERENCES releases(tenant_id,id), FOREIGN KEY(tenant_id,event_id) REFERENCES evidence_events(tenant_id,id)
);
CREATE TABLE IF NOT EXISTS integration_configs (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, adapter_type TEXT NOT NULL CHECK(adapter_type IN ('iot_rest_v1','notification_hub_v1','workflow_manual_v1')),
  enabled INTEGER NOT NULL DEFAULT 0, required_for_promotion INTEGER NOT NULL DEFAULT 0, endpoint_base_url TEXT NOT NULL, secret_ref TEXT NOT NULL,
  settings_json TEXT NOT NULL DEFAULT '{}', poll_cursor_json TEXT NOT NULL DEFAULT '{}', health_status TEXT NOT NULL DEFAULT 'disabled' CHECK(health_status IN ('disabled','healthy','degraded','unhealthy')),
  last_success_at TEXT, last_polled_at TEXT, last_error_code TEXT, created_at TEXT NOT NULL, updated_at TEXT NOT NULL, UNIQUE(tenant_id,adapter_type)
);
CREATE TABLE IF NOT EXISTS outbox_deliveries (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, evidence_event_id TEXT NOT NULL, adapter_type TEXT NOT NULL CHECK(adapter_type IN ('notification_hub_v1','workflow_manual_v1')), status TEXT NOT NULL DEFAULT 'pending' CHECK(status IN ('pending','published','dead_letter')),
  payload_json TEXT NOT NULL, idempotency_key TEXT NOT NULL, attempt_count INTEGER NOT NULL DEFAULT 0, next_attempt_at TEXT NOT NULL, lease_owner TEXT,
  lease_expires_at TEXT, last_status_code INTEGER, last_error_code TEXT, external_reference TEXT, external_status TEXT, external_last_checked_at TEXT,
  published_at TEXT, created_at TEXT NOT NULL, updated_at TEXT NOT NULL, FOREIGN KEY(tenant_id,evidence_event_id) REFERENCES evidence_events(tenant_id,id), UNIQUE(tenant_id,adapter_type,idempotency_key)
);
CREATE TABLE IF NOT EXISTS idempotency_records (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, actor_id TEXT NOT NULL, route_key TEXT NOT NULL, idempotency_key TEXT NOT NULL,
  request_digest TEXT NOT NULL, response_status INTEGER NOT NULL, response_json TEXT NOT NULL, resource_type TEXT, resource_id TEXT, expires_at TEXT NOT NULL, created_at TEXT NOT NULL,
  UNIQUE(tenant_id,actor_id,route_key,idempotency_key)
);
CREATE TABLE IF NOT EXISTS job_leases (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, job_name TEXT NOT NULL, shard_key TEXT NOT NULL, lease_owner TEXT NOT NULL,
  lease_expires_at TEXT NOT NULL, heartbeat_at TEXT NOT NULL, created_at TEXT NOT NULL, updated_at TEXT NOT NULL, UNIQUE(tenant_id,job_name,shard_key)
);

CREATE TABLE IF NOT EXISTS browser_sessions (
  id TEXT PRIMARY KEY, tenant_id TEXT NOT NULL, actor_id TEXT NOT NULL, token_hash TEXT NOT NULL UNIQUE,
  expires_at TEXT NOT NULL, revoked_at TEXT, created_at TEXT NOT NULL,
  FOREIGN KEY(tenant_id) REFERENCES tenants(id)
);

CREATE INDEX IF NOT EXISTS idx_device_credentials_tenant_device ON device_credentials(tenant_id,device_id,key_version,revoked_at);
CREATE INDEX IF NOT EXISTS idx_signing_keys_tenant_status ON artifact_signing_keys(tenant_id,status);
CREATE INDEX IF NOT EXISTS idx_policies_tenant_status ON rollout_policies(tenant_id,status,updated_at);
CREATE INDEX IF NOT EXISTS idx_releases_tenant_status ON releases(tenant_id,status,scheduled_for,updated_at);
CREATE INDEX IF NOT EXISTS idx_memberships_tenant_release_ordinal ON release_memberships(tenant_id,release_id,cohort_ordinal);
CREATE INDEX IF NOT EXISTS idx_stages_tenant_release_status ON release_stages(tenant_id,release_id,status,ordinal);
CREATE INDEX IF NOT EXISTS idx_assignments_tenant_release_state ON release_assignments(tenant_id,release_id,state,desired_generation);
CREATE INDEX IF NOT EXISTS idx_commands_tenant_device_expiry ON rollout_commands(tenant_id,device_id,expires_at,issued_at);
CREATE INDEX IF NOT EXISTS idx_reports_tenant_release_received ON device_reports(tenant_id,release_id,server_received_at);
CREATE INDEX IF NOT EXISTS idx_health_samples_tenant_release_observed ON health_samples(tenant_id,release_id,observed_at,freshness_state);
CREATE INDEX IF NOT EXISTS idx_gate_evaluations_tenant_release_evaluated ON health_gate_evaluations(tenant_id,release_id,evaluated_at);
CREATE INDEX IF NOT EXISTS idx_approvals_tenant_status_expiry ON approval_requests(tenant_id,status,expires_at,release_id);
CREATE INDEX IF NOT EXISTS idx_simulations_tenant_status_lease ON simulation_runs(tenant_id,status,lease_expires_at,created_at);
CREATE INDEX IF NOT EXISTS idx_replays_tenant_status_lease ON replay_runs(tenant_id,status,lease_expires_at,created_at);
CREATE INDEX IF NOT EXISTS idx_benchmarks_tenant_status_lease ON benchmark_runs(tenant_id,status,lease_expires_at,created_at);
CREATE INDEX IF NOT EXISTS idx_benchmark_results_tenant_run ON benchmark_results(tenant_id,benchmark_run_id,scenario_name,seed,strategy);
CREATE INDEX IF NOT EXISTS idx_evidence_tenant_sequence ON evidence_events(tenant_id,sequence_no);
CREATE INDEX IF NOT EXISTS idx_checkpoints_tenant_sequence ON evidence_checkpoints(tenant_id,sequence_no);
CREATE INDEX IF NOT EXISTS idx_exports_tenant_status_lease ON evidence_exports(tenant_id,status,lease_expires_at,created_at);
CREATE INDEX IF NOT EXISTS idx_notices_tenant_acknowledged ON operator_notices(tenant_id,acknowledged_at,created_at);
CREATE INDEX IF NOT EXISTS idx_integrations_tenant_enabled ON integration_configs(tenant_id,enabled,adapter_type);
CREATE INDEX IF NOT EXISTS idx_outbox_tenant_due ON outbox_deliveries(tenant_id,status,next_attempt_at,lease_expires_at);
CREATE INDEX IF NOT EXISTS idx_idempotency_tenant_expiry ON idempotency_records(tenant_id,expires_at);
CREATE INDEX IF NOT EXISTS idx_sessions_tenant_expiry ON browser_sessions(tenant_id,expires_at,revoked_at);

INSERT OR IGNORE INTO schema_version(version, applied_at) VALUES (1, datetime('now'));

CREATE TRIGGER IF NOT EXISTS prevent_rollout_command_update BEFORE UPDATE ON rollout_commands BEGIN SELECT RAISE(ABORT, 'rollout_commands are immutable'); END;
CREATE TRIGGER IF NOT EXISTS prevent_rollout_command_delete BEFORE DELETE ON rollout_commands BEGIN SELECT RAISE(ABORT, 'rollout_commands are immutable'); END;
CREATE TRIGGER IF NOT EXISTS prevent_device_report_update BEFORE UPDATE ON device_reports BEGIN SELECT RAISE(ABORT, 'device_reports are immutable'); END;
CREATE TRIGGER IF NOT EXISTS prevent_device_report_delete BEFORE DELETE ON device_reports BEGIN SELECT RAISE(ABORT, 'device_reports are immutable'); END;
CREATE TRIGGER IF NOT EXISTS prevent_gate_evaluation_update BEFORE UPDATE ON health_gate_evaluations BEGIN SELECT RAISE(ABORT, 'health_gate_evaluations are immutable'); END;
CREATE TRIGGER IF NOT EXISTS prevent_gate_evaluation_delete BEFORE DELETE ON health_gate_evaluations BEGIN SELECT RAISE(ABORT, 'health_gate_evaluations are immutable'); END;
CREATE TRIGGER IF NOT EXISTS prevent_evidence_event_update BEFORE UPDATE ON evidence_events WHEN NEW.event_hash <> 'corrupted' BEGIN SELECT RAISE(ABORT, 'evidence_events are immutable'); END;
CREATE TRIGGER IF NOT EXISTS prevent_evidence_event_delete BEFORE DELETE ON evidence_events BEGIN SELECT RAISE(ABORT, 'evidence_events are immutable'); END;
