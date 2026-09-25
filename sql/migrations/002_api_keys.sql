CREATE TABLE api_keys (
    id uuid PRIMARY KEY,
    name text NOT NULL,
    token_hash text NOT NULL UNIQUE,
    token_last4 text NOT NULL,
    status text NOT NULL CHECK (status IN ('enabled', 'disabled')),
    created_at timestamptz NOT NULL DEFAULT now(),
    disabled_at timestamptz,
    CHECK ((status = 'disabled') = (disabled_at IS NOT NULL))
);
CREATE INDEX api_keys_created ON api_keys (created_at DESC, id DESC);
CREATE INDEX usage_records_key_cursor ON usage_records (key_name, finished_at DESC, ledger_id DESC);
