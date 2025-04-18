/*
 * Copyright (c) 2024 燕几（北京）科技有限公司
 *
 * Rustica (runtime) is licensed under Mulan PSL v2. You can use this
 * software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *
 *              https://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES
 * OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
 * TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 *
 * See the Mulan PSL v2 for more details.
 */

CREATE SCHEMA rustica;

CREATE TABLE rustica.modules(
    name text PRIMARY KEY,
    byte_code bytea NOT NULL,
    bin_code bytea NOT NULL,
    heap_types int[] NOT NULL
);

CREATE TABLE rustica.queries(
    module text NOT NULL,  -- 0
    index int NOT NULL, -- 1
    sql text NOT NULL,  -- 2

    arg_type bigint NOT NULL,  -- 3
    arg_oids oid[] NOT NULL,  -- 4
    arg_field_types bigint[] NOT NULL,  -- 5
    arg_field_fn int[] NOT NULL,  -- 6

    ret_type bigint[] NOT NULL,  -- 7
    ret_oids oid[] NOT NULL,  -- 8
    ret_field_types bigint[] NOT NULL,  -- 9
    ret_field_fn int[] NOT NULL,  -- 10

    PRIMARY KEY (module, index),
    FOREIGN KEY (module) REFERENCES rustica.modules(name)
);

CREATE TYPE rustica.compile_result AS (
    bin_code bytea,
    heap_types int[],
    queries rustica.queries[]
);

CREATE TYPE rustica.tid_oid AS (
    tid uuid,
    oid oid
);

CREATE FUNCTION rustica.compile_wasm(bytea, rustica.tid_oid[])
    RETURNS rustica.compile_result
    AS 'MODULE_PATHNAME'
    LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION rustica.invalidate_module_cache() RETURNS TRIGGER AS $$
    BEGIN
        IF TG_OP = 'DELETE' THEN
            PERFORM pg_notify('rustica_module_cache_invalidation', OLD.name);
        ELSE
            PERFORM pg_notify('rustica_module_cache_invalidation', NEW.name);
        END IF;
        RETURN NULL;
    END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER module_change
    AFTER INSERT OR UPDATE OR DELETE ON rustica.modules
    FOR EACH ROW EXECUTE FUNCTION rustica.invalidate_module_cache();
