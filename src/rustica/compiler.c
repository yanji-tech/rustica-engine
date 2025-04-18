/*
 * Copyright (c) 2024-present 燕几（北京）科技有限公司
 *
 * Rustica Engine is licensed under Mulan PSL v2. You can use this
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

#include "postgres.h"
#include "funcapi.h"
#include "nodes/nodeFuncs.h"
#include "parser/parser.h"
#include "tcop/tcopprot.h"
#include "tcop/pquery.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"
#include "varatt.h"

#include "wasm_runtime_common.h"
#include "aot_export.h"

#if WASM_ENABLE_DEBUG_AOT != 0
#include "storage/fd.h"
#include "dwarf_extractor.h"
#endif

#include "rustica/compiler.h"
#include "rustica/datatypes.h"
#include "rustica/utils.h"
#include "rustica/wamr.h"

typedef enum wasm_to_pg_fn {
    wasm_i32_to_pg_bool,
    wasm_i32_to_pg_int4,
    wasm_i64_to_pg_int8,
    wasm_f32_to_pg_float4,
    wasm_f64_to_pg_float8,
    wasm_externref_to_datum_obj,
    wasm_i32_array_to_pg_int2_array,
    wasm_i32_array_to_pg_int4_array,
} wasm_to_pg_fn;

typedef enum pg_to_wasm_fn {
    pg_bool_to_wasm_i32,
    pg_int4_to_wasm_i32,
    pg_int8_to_wasm_i64,
    pg_float4_to_wasm_f32,
    pg_float8_to_wasm_f64,
    pg_datum_to_wasm_obj,
    pg_int2_array_to_wasm_i32_array,
    pg_int4_array_to_wasm_i32_array,
} pg_to_wasm_fn;

static Datum
compile_aot(
#if WASM_ENABLE_DEBUG_AOT != 0
    bytea *wasm,
#endif
    wasm_module_t module);

static void
run_and_compile(wasm_module_t module,
                Oid query_oid,
                Datum *heap_types,
                Datum *queries);

static Datum
compile_queries(CommonHeapTypes *heap_types,
                Oid query_oid,
                wasm_func_type_t func_type,
                wasm_module_t module);

static void
compile_query_type(wasm_module_t module,
                   wasm_ref_type_t ref_type,
                   Datum *query_attrs);

static void
compile_query(CommonHeapTypes *heap_types,
              wasm_exec_env_t exec_env,
              Datum *query_attrs,
              wasm_ref_type_t ref_type,
              wasm_struct_obj_t query);

static wasm_to_pg_fn
wasm_to_pg(wasm_exec_env_t exec_env,
           CommonHeapTypes *heap_types,
           wasm_ref_type_t ref_type,
           Oid pg_type);

static pg_to_wasm_fn
pg_to_wasm(wasm_exec_env_t exec_env,
           CommonHeapTypes *heap_types,
           Oid pg_type,
           wasm_ref_type_t ref_type);

static bool
validate_moonbit_array(wasm_ref_type_t ref_type,
                       wasm_module_t module,
                       wasm_ref_type_t *rv,
                       wasm_ref_type_t *fixed_array,
                       bool nullable);

static List *
describe_query_results(char *sql, Oid *argtypes, int nargs);

static Datum
compile_aot(
#if WASM_ENABLE_DEBUG_AOT != 0
    bytea *wasm,
#endif
    wasm_module_t module) {
    AOTCompOption option = { .opt_level = 3,
                             .size_level = 3,
                             .output_format = AOT_FORMAT_FILE,
                             .bounds_checks = 2,
                             .stack_bounds_checks = 2,
                             .enable_simd = true,
                             .enable_bulk_memory = true,
                             .enable_aux_stack_frame = true,
                             .enable_gc = true,
                             .target_arch = "x86_64" };
    aot_comp_data_t comp_data =
        aot_create_comp_data(module, option.target_arch, option.enable_gc);
    if (!comp_data)
        ereport(ERROR,
                errmsg("could not create compilation data: %s",
                       aot_get_last_error()));
#if WASM_ENABLE_DEBUG_AOT != 0
    File file = OpenTemporaryFile(false);
    int32 wasm_size = VARSIZE_ANY_EXHDR(wasm);
    int nbytes = FileWrite(file, VARDATA_ANY(wasm), wasm_size, 0, 0);
    if (nbytes != wasm_size) {
        if (nbytes < 0)
            ereport(ERROR,
                    (errcode_for_file_access(),
                     errmsg("could not write temporary file: %m")));
        ereport(
            ERROR,
            (errcode(ERRCODE_DISK_FULL),
             errmsg("could not write temporary file: wrote only %d of %d bytes",
                    nbytes,
                    wasm_size)));
    }
    if (!create_dwarf_extractor(comp_data, FilePathName(file)))
        ereport(ERROR,
                errmsg("could not create dwarf extractor: %s",
                       aot_get_last_error()));
    FileClose(file);
#endif
    aot_comp_context_t comp_ctx = aot_create_comp_context(comp_data, &option);
    if (!comp_ctx)
        ereport(ERROR,
                errmsg("could not create compilation context: %s",
                       aot_get_last_error()));
    if (!aot_compile_wasm(comp_ctx))
        ereport(
            ERROR,
            errmsg("failed to compile wasm module: %s", aot_get_last_error()));

    aot_obj_data_t obj_data = aot_obj_data_create(comp_ctx);
    if (!obj_data)
        ereport(
            ERROR,
            errmsg("could not create object data: %s", aot_get_last_error()));
    uint32_t aot_file_size =
        aot_get_aot_file_size(comp_ctx, comp_data, obj_data);
    bytea *rv = (bytea *)palloc(aot_file_size + VARHDRSZ);
    SET_VARSIZE(rv, aot_file_size + VARHDRSZ);
    if (!aot_emit_aot_file_buf_ex(comp_ctx,
                                  comp_data,
                                  obj_data,
                                  (uint8 *)VARDATA(rv),
                                  aot_file_size))
        ereport(ERROR,
                errmsg("Failed to emit aot file: %s", aot_get_last_error()));
    aot_obj_data_destroy(obj_data);
    PG_RETURN_POINTER(rv);
}

Datum
rst_compile(PG_FUNCTION_ARGS) {
    // Load the input WASM module
    DECLARE_ERROR_BUF(128);
    bytea *wasm = PG_GETARG_BYTEA_P(0);
    int32 wasm_size = VARSIZE_ANY_EXHDR(wasm);

    ArrayType *tid_oid_map = PG_GETARG_ARRAYTYPE_P(1);
    Oid tid_oid_type = ARR_ELEMTYPE(tid_oid_map);
    Datum *map_datums;
    deconstruct_array(tid_oid_map,
                      tid_oid_type,
                      get_typlen(tid_oid_type),
                      false,
                      'd',
                      &map_datums,
                      NULL,
                      &tid_map_len);
    if (tid_map_len > 0) {
        tid_map = (TidOid *)palloc(sizeof(TidOid) * tid_map_len);
        for (int i = 0; i < tid_map_len; i++) {
            HeapTupleHeader pair = DatumGetHeapTupleHeader(map_datums[i]);
            bool isnull;
            tid_map[i].tid = DatumGetUUIDP(GetAttributeByNum(pair, 1, &isnull));
            Assert(!isnull);
            tid_map[i].oid = getBaseType(
                DatumGetObjectId(GetAttributeByNum(pair, 2, &isnull)));
            Assert(!isnull);
        }
    }

    TupleDesc rv_tupdesc;
    Datum rv[3] = { 0 };
    wasm_module_t module = NULL;

    // The backdoor API may create nested WAMR runtime, so stash the parent env
    wasm_exec_env_t prev_exec_env = wasm_runtime_get_exec_env_tls();
    wasm_runtime_set_exec_env_tls(NULL);

    PG_TRY();
    {
        module = wasm_runtime_load((uint8 *)VARDATA_ANY(wasm),
                                   wasm_size,
                                   ERROR_BUF_PARAMS);
        if (!module)
            ereport(ERROR, errmsg("failed to load WASM module: %s", ERROR_BUF));

        // Find type descriptors needed to build the result
#ifdef USE_ASSERT_CHECKING
        TypeFuncClass rv_cls =
#endif
            get_call_result_type(fcinfo, NULL, &rv_tupdesc);
        Assert(rv_cls == TYPEFUNC_COMPOSITE);
        Assert(rv_tupdesc->natts == 3);
        Assert(TupleDescAttr(rv_tupdesc, 0)->atttypid == BYTEAOID);
        Assert(get_element_type(TupleDescAttr(rv_tupdesc, 1)->atttypid)
               == INT4OID);
        Oid query_oid =
            get_element_type(TupleDescAttr(rv_tupdesc, 2)->atttypid);
        Assert(query_oid != InvalidOid);

        // Compile AOT binary and query plans
        rv[0] = compile_aot(
#if WASM_ENABLE_DEBUG_AOT != 0
            wasm,
#endif
            module);
        run_and_compile(module, query_oid, &rv[1], &rv[2]);
    }
    PG_FINALLY();
    {
        if (module != NULL)
            wasm_runtime_unregister_and_unload(module);
        pfree(tid_map);
        tid_map = NULL;
        tid_map_len = 0;
        wasm_runtime_set_exec_env_tls(prev_exec_env);
    }
    PG_END_TRY();

    bool isnull[3] = { 0 };
    // If no queries are found, mark the 3rd field (queries) as NULL
    if (!rv[2])
        isnull[2] = 1;
    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(rv_tupdesc, rv, isnull)));
}

static void
run_and_compile(wasm_module_t module,
                Oid query_oid,
                Datum *heap_types_out,
                Datum *queries_out) {
    // Compile for exported functions
    CommonHeapTypes heap_types;
    memset(&heap_types, -1, sizeof(heap_types));
    wasm_func_type_t func_type;
    if ((func_type = wasm_module_lookup_exported_func(module, "get_queries")))
        *queries_out =
            compile_queries(&heap_types, query_oid, func_type, module);

    // Construct result array of heap_types
    int size = sizeof(CommonHeapTypes) / sizeof(int32_t);
    Datum array[size];
    int32_t *ptr = (int32_t *)&heap_types;
    for (int i = 0; i < size; i++) {
        array[i] = Int32GetDatum(ptr[i]);
    }
    int dims[1] = { size };
    int lbs[1] = { 1 };
    *heap_types_out = PointerGetDatum(construct_md_array(array,
                                                         NULL,
                                                         1,
                                                         dims,
                                                         lbs,
                                                         INT4OID,
                                                         sizeof(int32_t),
                                                         true,
                                                         'i'));
}

static Datum
compile_queries(CommonHeapTypes *heap_types,
                Oid query_oid,
                wasm_func_type_t func_type,
                wasm_module_t module) {
    // Validate the signature of get_queries()
    if (wasm_func_type_get_param_count(func_type) != 1)
        ereport(ERROR, errmsg("get_queries() must take exactly 1 argument"));
    if (wasm_func_type_get_param_type(func_type, 0).value_type
        != VALUE_TYPE_I32)
        ereport(ERROR,
                errmsg("get_queries() must take a boolean/i32 argument"));
    if (wasm_func_type_get_result_count(func_type) != 1)
        ereport(ERROR, errmsg("get_queries() must return exactly 1 value"));
    wasm_struct_type_t queries_type = wasm_ref_type_get_referred_struct(
        wasm_func_type_get_result_type(func_type, 0),
        module,
        false);
    if (!queries_type)
        ereport(ERROR,
                errmsg("get_queries() must return a non-nullable struct"));
    uint32 nqueries = wasm_struct_type_get_field_count(queries_type);
    if (!nqueries)
        PG_RETURN_POINTER(construct_empty_array(query_oid));
    if (nqueries > 65535)
        ereport(ERROR, errmsg("get_queries() returned too many queries"));

    // Trap to clean up WAMR instance
    DECLARE_ERROR_BUF(128);
    Datum queries_array[nqueries];
    wasm_module_inst_t instance = NULL;
    wasm_exec_env_t exec_env = NULL;
    TupleDesc query_tupdesc = NULL;
    PG_TRY();
    {
        // Call get_queries(populate_args_oids=True) with a temporary instance
        if (!(instance = wasm_runtime_instantiate(module,
                                                  64 * 1024,
                                                  256 * 1024,
                                                  ERROR_BUF_PARAMS)))
            ereport(ERROR,
                    errmsg("cannot instantiate WASM module: %s", ERROR_BUF));
        if (!(exec_env = wasm_exec_env_create(instance, 64 * 1024)))
            ereport(ERROR, errmsg("cannot create execution environment"));
        wasm_function_inst_t get_queries_func =
            wasm_runtime_lookup_function(instance, "get_queries");
        Assert(get_queries_func != NULL);
        wasm_val_t val;
        wasm_val_t args[1] = { { .kind = WASM_I32, .of.i32 = 1 } };
        if (!wasm_runtime_call_wasm_a(exec_env,
                                      get_queries_func,
                                      1,
                                      &val,
                                      1,
                                      args))
            ereport(ERROR, errmsg("failed to call get_queries()"));
        Assert(val.kind == WASM_EXTERNREF);
        wasm_struct_obj_t queries = (wasm_struct_obj_t)val.of.ref;
        Assert(nqueries == wasm_struct_obj_get_field_count(queries));

        // Compile each query
        query_tupdesc = lookup_rowtype_tupdesc(query_oid, -1);
        for (uint32 q = 0; q < nqueries; q++) {
            Datum query_attrs[12];

            // Compile the query type first
            wasm_ref_type_t query_ref_type =
                wasm_struct_type_get_field_type(queries_type, q, NULL);
            compile_query_type(module, query_ref_type, query_attrs);

            // Compile the actual query object
            wasm_value_t value;
            wasm_struct_obj_get_field(queries, q, false, &value);
            Assert(wasm_obj_is_struct_obj(value.gc_obj));
            wasm_struct_obj_t query = (wasm_struct_obj_t)value.gc_obj;
            value.i64 = q;
            wasm_struct_obj_set_field(query, 0, &value);
            compile_query(heap_types,
                          exec_env,
                          query_attrs,
                          query_ref_type,
                          query);

            // Construct a query tuple
            bool isnull[sizeof(query_attrs) / sizeof(Datum)] = { false };
            queries_array[q] = HeapTupleGetDatum(
                heap_form_tuple(query_tupdesc, query_attrs, isnull));
        }
    }
    PG_FINALLY();
    {
        if (query_tupdesc)
            ReleaseTupleDesc(query_tupdesc);
        if (exec_env)
            wasm_exec_env_destroy(exec_env);
        if (instance)
            wasm_runtime_deinstantiate(instance);
    }
    PG_END_TRY();

    // Compose the queries array datum and return
    int dims[1] = { (int)nqueries };
    int lbs[1] = { 1 };
    int16 typlen;
    bool typbyval;
    char typalign;
    get_typlenbyvalalign(query_oid, &typlen, &typbyval, &typalign);
    PG_RETURN_ARRAYTYPE_P(construct_md_array(queries_array,
                                             NULL,
                                             1,
                                             dims,
                                             lbs,
                                             query_oid,
                                             typlen,
                                             typbyval,
                                             typalign));
}

static void
compile_query_type(wasm_module_t module,
                   wasm_ref_type_t ref_type,
                   Datum *query_attrs) {
    // Each Query must be a struct of at least 5 fields
    wasm_struct_type_t query_type =
        wasm_ref_type_get_referred_struct(ref_type, module, false);
    if (!query_type)
        ereport(ERROR, errmsg("Query must be a non-nullable struct"));
    if (wasm_struct_type_get_field_count(query_type) < 5)
        ereport(ERROR, errmsg("Query must have at least 5 fields"));

    // first field: shared query index to execute the query
    ref_type = wasm_struct_type_get_field_type(query_type, 0, NULL);
    if (ref_type.value_type != VALUE_TYPE_I64
        && ref_type.value_type != VALUE_TYPE_I32)
        ereport(ERROR, errmsg("first field of Query must be an integer"));

    // second field: the SQL text in bytes
    ref_type = wasm_struct_type_get_field_type(query_type, 1, NULL);
    if (!wasm_ref_type_is_ref_extern(ref_type))
        ereport(ERROR, errmsg("second field of Query must be bytes"));

    // third field: MoonBit array of query argument OIDs
    ref_type = wasm_struct_type_get_field_type(query_type, 2, NULL);
    if (!validate_moonbit_array(ref_type, module, &ref_type, NULL, false))
        ereport(ERROR, errmsg("third field of Query must be an array"));
    if (ref_type.value_type != VALUE_TYPE_I32)
        ereport(ERROR, errmsg("third field of Query must be an array of i32"));

    // forth field: nullable struct or MoonBit Unit for query arguments
    uint32 nargs;
    wasm_struct_type_t arg_struct_type = NULL;
    ref_type = wasm_struct_type_get_field_type(query_type, 3, NULL);
    if (ref_type.value_type == VALUE_TYPE_I32) {
        // This is MoonBit Unit
        nargs = 0;
    }
    else if ((arg_struct_type =
                  wasm_ref_type_get_referred_struct(ref_type, module, true))) {
        nargs = wasm_struct_type_get_field_count(arg_struct_type);
    }
    else
        ereport(
            ERROR,
            errmsg("forth field of Query must be a nullable struct or Unit"));
    if (nargs > 65535)
        ereport(ERROR, errmsg("too many arguments (max: 65535)"));
    query_attrs[3] = Int64GetDatum(*(int64 *)&ref_type);

    // Build arg_field_types array
    Datum arg_field_types[nargs];
    for (int i = 0; i < nargs; i++) {
        ref_type = wasm_struct_type_get_field_type(arg_struct_type, i, NULL);
        arg_field_types[i] = Int64GetDatum(*(int64 *)&ref_type);
    }
    int dims[1] = { (int)nargs };
    int lbs[1] = { 1 };
    query_attrs[5] = PointerGetDatum(construct_md_array(arg_field_types,
                                                        NULL,
                                                        1,
                                                        dims,
                                                        lbs,
                                                        INT8OID,
                                                        sizeof(int64),
                                                        true,
                                                        'i'));

    // fifth field: boxed optional struct or MoonBit Unit for result
    ref_type = wasm_struct_type_get_field_type(query_type, 4, NULL);
    wasm_struct_type_t ref_box =
        wasm_ref_type_get_referred_struct(ref_type, module, false);
    if (!ref_box)
        ereport(ERROR, errmsg("fifth field of Query must be a result box"));
    if (wasm_struct_type_get_field_count(ref_box) != 1)
        ereport(ERROR, errmsg("the result box must have exactly 1 field"));
    ref_type = wasm_struct_type_get_field_type(ref_box, 0, NULL);
    uint32 nattrs;
    wasm_struct_type_t ret_struct_type;
    if (ref_type.value_type == VALUE_TYPE_I32) {
        // This is MoonBit Unit
        nattrs = 0;
    }
    else if ((ret_struct_type =
                  wasm_ref_type_get_referred_struct(ref_type, module, true))) {
        nattrs = wasm_struct_type_get_field_count(ret_struct_type);
    }
    else
        ereport(ERROR,
                errmsg("fifth field of Query must be a box of nullable struct "
                       "or Unit"));
    Datum ret_type[1] = { Int64GetDatum(*(int64 *)&ref_type) };

    // Build ret_type array
    dims[0] = 1;
    query_attrs[7] = PointerGetDatum(construct_md_array(ret_type,
                                                        NULL,
                                                        1,
                                                        dims,
                                                        lbs,
                                                        INT8OID,
                                                        sizeof(int64),
                                                        true,
                                                        'i'));

    // Build ret_field_types array
    Datum ret_field_types[nattrs];
    for (int i = 0; i < nattrs; i++) {
        ref_type = wasm_struct_type_get_field_type(ret_struct_type, i, NULL);
        ret_field_types[i] = Int64GetDatum(*(int64 *)&ref_type);
    }
    dims[0] = (int)nattrs;
    query_attrs[9] = PointerGetDatum(construct_md_array(ret_field_types,
                                                        NULL,
                                                        1,
                                                        dims,
                                                        lbs,
                                                        INT8OID,
                                                        sizeof(int64),
                                                        true,
                                                        'i'));
}

static void
compile_query(CommonHeapTypes *heap_types,
              wasm_exec_env_t exec_env,
              Datum *query_attrs,
              wasm_ref_type_t ref_type,
              wasm_struct_obj_t query) {
    wasm_value_t value;

    // 0. module: text = ''
    // text *e = (text *)palloc(1 + varhdrsz_short);
    // set_varsize_short(e, 1 + varhdrsz_short);
    // memcpy(VARDATA_SHORT(e), "\0", 1);
    query_attrs[0] = PointerGetDatum("\3\0");

    // 1. index: int = idx: Int?
    wasm_struct_obj_get_field(query, 0, false, &value);
    query_attrs[1] = Int32GetDatum(value.i32);

    // 2. sql: text = sql: Bytes
    wasm_struct_obj_get_field(query, 1, false, &value);
    text *sql_datum = (text *)DatumGetPointer(
        wasm_externref_obj_get_datum(value.gc_obj, BYTEAOID));
    char *sql = VARDATA_ANY(sql_datum);
    int sql_len = VARSIZE_ANY_EXHDR(sql_datum);
    if (sql[sql_len - 1] != '\0') {
        sql_datum = (text *)palloc(sql_len + 1 + VARHDRSZ);
        SET_VARSIZE(sql_datum, sql_len + 1 + VARHDRSZ);
        memcpy(VARDATA(sql_datum), sql, sql_len);
        sql = VARDATA(sql_datum);
        sql[sql_len] = '\0';
    }
    query_attrs[2] = PointerGetDatum(sql_datum);

    // Re-read the compiled array arg_field_types
    int nargs;
    wasm_ref_type_t *arg_field_types;
    deconstruct_array(DatumGetArrayTypeP(query_attrs[5]),
                      INT8OID,
                      sizeof(int64),
                      true,
                      'i',
                      (Datum **)&arg_field_types,
                      NULL,
                      &nargs);

    // 4. arg_oids: oid[] = args_oids: Array[Int] { buf: array(i32), len: Int }
    wasm_struct_obj_get_field(query, 2, false, &value);
    Assert(wasm_obj_is_struct_obj(value.gc_obj));
    wasm_struct_obj_t args_oids_array = (wasm_struct_obj_t)value.gc_obj;
    wasm_struct_obj_get_field(args_oids_array, 1, false, &value);
    if (value.i32 != nargs)
        ereport(ERROR, errmsg("given %d OIDs but expect %d", value.i32, nargs));
    wasm_struct_obj_get_field(args_oids_array, 0, false, &value);
    Assert(wasm_obj_is_array_obj(value.gc_obj));
    Oid *argtypes =
        wasm_array_obj_first_elem_addr((wasm_array_obj_t)value.gc_obj);
    Datum args_array[nargs];
    for (int i = 0; i < nargs; i++) {
        args_array[i] = Int32GetDatum((int32)argtypes[i]);
    }
    int dims[1] = { nargs };
    int lbs[1] = { 1 };
    query_attrs[4] = PointerGetDatum(construct_md_array(args_array,
                                                        NULL,
                                                        1,
                                                        dims,
                                                        lbs,
                                                        INT4OID,
                                                        sizeof(int32_t),
                                                        true,
                                                        'i'));

    // 6. arg_field_fn: int[]
    for (int i = 0; i < nargs; i++) {
        wasm_to_pg_fn fn =
            wasm_to_pg(exec_env, heap_types, arg_field_types[i], argtypes[i]);
        args_array[i] = Int32GetDatum(fn);
    }
    pfree(arg_field_types);
    query_attrs[6] = PointerGetDatum(construct_md_array(args_array,
                                                        NULL,
                                                        1,
                                                        dims,
                                                        lbs,
                                                        INT4OID,
                                                        sizeof(int32_t),
                                                        true,
                                                        'i'));

    // Re-read the compiled array ret_field_types
    int nattrs;
    wasm_ref_type_t *ret_field_types;
    deconstruct_array(DatumGetArrayTypeP(query_attrs[9]),
                      INT8OID,
                      sizeof(int64),
                      true,
                      'i',
                      (Datum **)&ret_field_types,
                      NULL,
                      &nattrs);

    // 8. ret_oids: oid[]
    List *target_list = describe_query_results(sql, argtypes, nargs);
    if (nattrs != list_length(target_list))
        ereport(ERROR,
                errmsg("given %d OIDs but expect %d",
                       nattrs,
                       list_length(target_list)));
    Datum ret_array[nattrs];
    ListCell *target_cell;
    int i = 0;
    foreach (target_cell, target_list) {
        TargetEntry *target = (TargetEntry *)lfirst(target_cell);
        if (!target->resjunk) {
            Oid ret_field_type = exprType((Node *)target->expr);
            ret_array[i] = Int32GetDatum((int32)ret_field_type);
            i++;
        }
    }
    dims[0] = nattrs;
    query_attrs[8] = PointerGetDatum(construct_md_array(ret_array,
                                                        NULL,
                                                        1,
                                                        dims,
                                                        lbs,
                                                        INT4OID,
                                                        sizeof(int32_t),
                                                        true,
                                                        'i'));

    // 10. ret_field_fn: int[]
    for (i = 0; i < nattrs; i++) {
        int fn = pg_to_wasm(exec_env,
                            heap_types,
                            DatumGetInt32(ret_array[i]),
                            ret_field_types[i]);
        ret_array[i] = Int32GetDatum(fn);
    }
    pfree(ret_field_types);
    query_attrs[10] = PointerGetDatum(construct_md_array(ret_array,
                                                         NULL,
                                                         1,
                                                         dims,
                                                         lbs,
                                                         INT4OID,
                                                         sizeof(int32_t),
                                                         true,
                                                         'i'));
}

static wasm_to_pg_fn
wasm_to_pg(wasm_exec_env_t exec_env,
           CommonHeapTypes *heap_types,
           wasm_ref_type_t ref_type,
           Oid pg_type) {
    switch (pg_type) {
        case BOOLOID:
            if (ref_type.value_type == VALUE_TYPE_I32)
                return wasm_i32_to_pg_bool;
            break;

        case INT4OID:
        case DATEOID:
            if (ref_type.value_type == VALUE_TYPE_I32)
                return wasm_i32_to_pg_int4;
            break;

        case INT8OID:
            switch (ref_type.value_type) {
                case VALUE_TYPE_I64:
                    return wasm_i64_to_pg_int8;

                case VALUE_TYPE_I32:
                    return wasm_i32_to_pg_int4;
            }
            break;

        case TIMESTAMPOID:
        case TIMESTAMPTZOID:
        case TIMEOID:
            if (ref_type.value_type == VALUE_TYPE_I64)
                return wasm_i64_to_pg_int8;
            break;

        case FLOAT4OID:
            if (ref_type.value_type == VALUE_TYPE_F32)
                return wasm_f32_to_pg_float4;
            break;

        case FLOAT8OID:
            if (ref_type.value_type == VALUE_TYPE_F64)
                return wasm_f64_to_pg_float8;
            break;

        case TEXTOID:
        case VARCHAROID:
        case UUIDOID:
        case JSONOID:
        case JSONBOID:
        case BYTEAOID:
        case INTERVALOID:
            if (wasm_is_reftype_externref(ref_type.value_type)
                || ref_type.heap_type == HEAP_TYPE_EXTERN)
                return wasm_externref_to_datum_obj;
            break;

        case INT2ARRAYOID:
        case INT4ARRAYOID:
            if (validate_moonbit_array(ref_type,
                                       wasm_exec_env_get_module(exec_env),
                                       &ref_type,
                                       NULL,
                                       false))
                if (ref_type.value_type == VALUE_TYPE_I32)
                    return pg_type == INT2ARRAYOID
                               ? wasm_i32_array_to_pg_int2_array
                               : wasm_i32_array_to_pg_int4_array;
            break;

        default:
            break;
    }
    ereport(ERROR,
            errmsg("cannot cast WASM \"%s\" into PG \"%s\"",
                   wasm_ref_type_repr(heap_types, ref_type),
                   get_rel_name(pg_type)));
}

static pg_to_wasm_fn
pg_to_wasm(wasm_exec_env_t exec_env,
           CommonHeapTypes *heap_types,
           Oid pg_type,
           wasm_ref_type_t ref_type) {
    switch (pg_type) {
        case BOOLOID:
            if (ref_type.value_type == VALUE_TYPE_I32)
                return pg_bool_to_wasm_i32;
            break;

        case INT4OID:
        case DATEOID:
            if (ref_type.value_type == VALUE_TYPE_I32)
                return pg_int4_to_wasm_i32;
            break;

        case INT8OID:
        case TIMESTAMPOID:
        case TIMESTAMPTZOID:
        case TIMEOID:
            if (ref_type.value_type == VALUE_TYPE_I64)
                return pg_int8_to_wasm_i64;
            break;

        case FLOAT4OID:
            if (ref_type.value_type == VALUE_TYPE_F32)
                return pg_float4_to_wasm_f32;
            break;

        case FLOAT8OID:
            if (ref_type.value_type == VALUE_TYPE_F64)
                return pg_float8_to_wasm_f64;
            break;

        case TEXTOID:
        case VARCHAROID:
        case UUIDOID:
        case JSONOID:
        case JSONBOID:
        case BYTEAOID:
        case INTERVALOID:
            if (wasm_is_reftype_externref(ref_type.value_type)
                || ref_type.heap_type == HEAP_TYPE_EXTERN)
                return pg_datum_to_wasm_obj;
            break;

        case INT2ARRAYOID:
        case INT4ARRAYOID:
            if (validate_moonbit_array(ref_type,
                                       wasm_exec_env_get_module(exec_env),
                                       &ref_type,
                                       NULL,
                                       false))
                if (ref_type.value_type == VALUE_TYPE_I32)
                    return pg_type == INT2ARRAYOID
                               ? pg_int2_array_to_wasm_i32_array
                               : pg_int4_array_to_wasm_i32_array;
            break;

        default:
            break;
    }
    ereport(ERROR,
            errmsg("cannot cast PG \"%s\" into WASM \"%s\"",
                   get_rel_name(pg_type),
                   wasm_ref_type_repr(heap_types, ref_type)));
}

static bool
validate_moonbit_array(wasm_ref_type_t ref_type,
                       wasm_module_t module,
                       wasm_ref_type_t *rv,
                       wasm_ref_type_t *fixed_array,
                       bool nullable) {
    // MoonBit Array is a struct of 2 fields
    wasm_struct_type_t array_struct =
        wasm_ref_type_get_referred_struct(ref_type, module, nullable);
    if (!array_struct)
        return false;
    if (wasm_struct_type_get_field_count(array_struct) != 2)
        return false;

    // First field: WASM array of the actual elements
    ref_type = wasm_struct_type_get_field_type(array_struct, 0, NULL);
    if (fixed_array)
        *fixed_array = ref_type;
    wasm_array_type_t array_type =
        wasm_ref_type_get_referred_array(ref_type, module, false);
    if (!array_type)
        return false;

    // Second field: i32 of the actual length
    ref_type = wasm_struct_type_get_field_type(array_struct, 1, NULL);
    if (ref_type.value_type != VALUE_TYPE_I32)
        return false;

    // Return the ref type of the actual element
    *rv = wasm_array_type_get_elem_type(array_type, NULL);
    return true;
}

static List *
describe_query_results(char *sql, Oid *argtypes, int nargs) {
    List *parsetree_list = raw_parser(sql, RAW_PARSE_DEFAULT);
    if (list_length(parsetree_list) != 1)
        ereport(ERROR,
                errmsg("expect exactly 1 SQL statement, found %d",
                       list_length(parsetree_list)));
    RawStmt *parsetree = (RawStmt *)linitial(parsetree_list);
    List *querytree_list = pg_analyze_and_rewrite_fixedparams(parsetree,
                                                              sql,
                                                              argtypes,
                                                              nargs,
                                                              NULL);
    Query *stmt;
    ListCell *cell;
    switch (ChoosePortalStrategy(querytree_list)) {
        case PORTAL_ONE_SELECT:
        case PORTAL_ONE_MOD_WITH:
            stmt = linitial_node(Query, querytree_list);
            return stmt->targetList;

        case PORTAL_ONE_RETURNING:
            foreach (cell, querytree_list) {
                stmt = lfirst_node(Query, cell);
                if (stmt->canSetTag) {
                    return stmt->returningList;
                }
            }
            break;

        case PORTAL_UTIL_SELECT:
            ereport(ERROR, errmsg("utilities SQL is not allowed"));

        case PORTAL_MULTI_QUERY:
            break;
    }
    return NULL;
}
