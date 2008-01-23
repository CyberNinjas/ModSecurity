
#include "msc_lua.h"

#include "apr_strings.h"

typedef struct {
    apr_array_header_t *parts;
    apr_pool_t *pool;
} msc_lua_dumpw_t;

typedef struct {
    msc_script *script;
    int index;
} msc_lua_dumpr_t;

/**
 *
 */
static const char* dump_reader(lua_State* L, void* user_data, size_t* size) {
    msc_lua_dumpr_t *dumpr = (msc_lua_dumpr_t *)user_data;

    /* Do we have more chunks to return? */
    if (dumpr->index == dumpr->script->parts->nelts) {
        return NULL;
    }

    /* Get one chunk. */
    msc_script_part *part = ((msc_script_part **)dumpr->script->parts->elts)[dumpr->index];
    *size = part->len;
    
    dumpr->index++;

    return part->data;
}

/**
 *
 */
static int dump_writer(lua_State *L, const void* data, size_t len, void* user_data) {
    msc_lua_dumpw_t *dump = (msc_lua_dumpw_t *)user_data;

    /* Allocate new part, copy the data into it. */
    msc_script_part *part = apr_palloc(dump->pool, sizeof(msc_script_part));
    part->data = apr_palloc(dump->pool, len);
    part->len = len;
    memcpy((void *)part->data, data, len);

    /* Then add it to the list of parsts. */
    *(const msc_script_part **)apr_array_push(dump->parts) = part;

    return 0;
}

/**
 *
 */
static int lua_restore(lua_State *L, msc_script *script) {
    msc_lua_dumpr_t dumpr;

    dumpr.script = script;
    dumpr.index = 0;

    return lua_load(L, dump_reader, &dumpr, script->name);
}

/**
 *
 */
char *lua_compile(msc_script **script, const char *filename, apr_pool_t *pool) {
    lua_State *L = NULL;
    msc_lua_dumpw_t dump;

    /* Initialise state. */
    L = lua_open();
    luaL_openlibs(L);

    /* Find script. */
    if (luaL_loadfile(L, filename)) {
        return apr_psprintf(pool, "ModSecurity: Failed to compile script %s: %s",
            filename, lua_tostring(L, -1));
    }

    /* Dump the script into binary form. */
    dump.pool = pool;
    dump.parts = apr_array_make(pool, 128, sizeof(msc_script_part *));

    lua_dump(L, dump_writer, &dump);

    (*script) = apr_pcalloc(pool, sizeof(msc_script));
    (*script)->name = filename;
    (*script)->parts = dump.parts;        
    
    /* Destroy state. */
    lua_close(L);

    return NULL;
}

/**
 *
 */
static int l_log(lua_State *L) {
    modsec_rec *msr = NULL;
    const char *text;
    int level;
    
    /* Retrieve parameters. */
    level = luaL_checknumber(L, 1);
    text = luaL_checkstring(L, 2);

    /* Retrieve msr. */
    lua_getglobal(L, "__msr");
    msr = (modsec_rec *)lua_topointer(L, -1);

    /* Log message. */
    if (msr != NULL) {
        msr_log(msr, level, "%s", text);
    }

    return 0;
}

/**
 *
 */
static apr_array_header_t *resolve_tfns(lua_State *L, int idx, modsec_rec *msr, apr_pool_t *mp) {
    apr_array_header_t *tfn_arr = NULL;
    msre_tfn_metadata *tfn = NULL;
    char *name = NULL;

    tfn_arr = apr_array_make(mp, 25, sizeof(msre_tfn_metadata *));
    if (tfn_arr == NULL) return NULL;

    if (lua_istable(L, idx)) { /* Is the second parameter an array? */
        int i, n = lua_objlen(L, idx);

        for(i = 1; i <= n; i++) {
            lua_rawgeti(L, idx, i);
            name = (char *)luaL_checkstring(L, -1);
            tfn = msre_engine_tfn_resolve(msr->modsecurity->msre, name);
            if (tfn == NULL) {
                msr_log(msr, 1, "SecRuleScript: Invalid transformation function: %s", name);
            } else {
                *(msre_tfn_metadata **)apr_array_push(tfn_arr) = tfn;
            }
        }
    } else if (lua_isstring(L, idx)) { /* The second parameter may be a simple string? */
        name = (char *)luaL_checkstring(L, idx);
        tfn = msre_engine_tfn_resolve(msr->modsecurity->msre, name);
        if (tfn == NULL) {
            msr_log(msr, 1, "SecRuleScript: Invalid transformation function: %s", name);
        } else {
            *(msre_tfn_metadata **)apr_array_push(tfn_arr) = tfn;
        }
    } else {
        msr_log(msr, 1, "SecRuleScript: Transformation parameter must be a transformation name or array of transformation names.");
        return NULL;
    }

    return tfn_arr;
}

/**
 *
 */
static int l_getvar(lua_State *L) {
    char *varname = NULL, *param = NULL;
    modsec_rec *msr = NULL;
    msre_rule *rule = NULL;
    char *my_error_msg = NULL;
    char *p1 = NULL;
    apr_array_header_t *tfn_arr = NULL;
    msre_var *vx = NULL;

    /* Retrieve parameters. */
    p1 = (char *)luaL_checkstring(L, 1);

    /* Retrieve msr. */
    lua_getglobal(L, "__msr");
    msr = (modsec_rec *)lua_topointer(L, -1);

    /* Retrieve rule. */
    lua_getglobal(L, "__rule");
    rule = (msre_rule *)lua_topointer(L, -1);

    /* Extract the variable name and its parameter from the script. */
    varname = apr_pstrdup(msr->msc_rule_mptmp, p1);
    param = strchr(varname, '.');
    if (param != NULL) {
        *param = '\0';
        param++;
    }

    /* Resolve variable. */
    msre_var *var = msre_create_var_ex(msr->msc_rule_mptmp, msr->modsecurity->msre,
        varname, param, msr, &my_error_msg);

    if (var == NULL) {
        msr_log(msr, 1, "%s", my_error_msg);
        
        lua_pushnil(L);

        return 0;
    }

    /* Resolve transformation functions. */
    tfn_arr = resolve_tfns(L, 2, msr, msr->msc_rule_mptmp);

    /* Generate variable. */
    vx = generate_single_var(msr, var, tfn_arr, rule, msr->msc_rule_mptmp);
    if (vx == NULL) {
        lua_pushnil(L);

        return 0;
    }

    /* Return variable value. */
    lua_pushlstring(L, vx->value, vx->value_len);            

    return 1;
}

/**
 *
 */
static int l_getvars(lua_State *L) {
    const apr_array_header_t *tarr;
    const apr_table_entry_t *telts;
    apr_table_t *vartable = NULL;
    apr_array_header_t *tfn_arr = NULL;
    char *varname = NULL, *param = NULL;
    modsec_rec *msr = NULL;
    msre_rule *rule = NULL;
    msre_var *vartemplate = NULL;
    char *my_error_msg = NULL;
    char *p1 = NULL;
    int i;

    /* Retrieve parameters. */
    p1 = (char *)luaL_checkstring(L, 1);

    /* Retrieve msr. */
    lua_getglobal(L, "__msr");
    msr = (modsec_rec *)lua_topointer(L, -1);

    /* Retrieve rule. */
    lua_getglobal(L, "__rule");
    rule = (msre_rule *)lua_topointer(L, -1);

    /* Extract the variable name and its parameter from the script. */
    varname = apr_pstrdup(msr->msc_rule_mptmp, p1);
    param = strchr(varname, '.');
    if (param != NULL) {
        *param = '\0';
        param++;
    }

    /* Resolve transformation functions. */
    tfn_arr = resolve_tfns(L, 2, msr, msr->msc_rule_mptmp);

    lua_newtable(L);

    /* Resolve variable. */
    vartemplate = msre_create_var_ex(msr->msc_rule_mptmp, msr->modsecurity->msre,
        varname, param, msr, &my_error_msg);

    if (vartemplate == NULL) {
        msr_log(msr, 1, "%s", my_error_msg);

        /* Returning empty table. */
        return 1;
    }

    vartable = generate_multi_var(msr, vartemplate, tfn_arr, rule, msr->msc_rule_mptmp);

    tarr = apr_table_elts(vartable);
    telts = (const apr_table_entry_t*)tarr->elts;
    for (i = 0; i < tarr->nelts; i++) {
        msre_var *var = (msre_var *)telts[i].val;

        lua_pushnumber(L, i + 1); /* Index is not zero-based. */

        lua_newtable(L); /* Per-parameter table. */

        lua_pushstring(L, "name");
        lua_pushlstring(L, var->name, strlen(var->name));
        lua_settable(L, -3);

        lua_pushstring(L, "value");
        lua_pushlstring(L, var->value, var->value_len);
        lua_settable(L, -3);

        lua_settable(L, -3); /* Push one parameter into the results table. */    
    }

    return 1;
}

static const struct luaL_Reg mylib[] = {
    { "log", l_log },
    { "getvar", l_getvar },
    { "getvars", l_getvars },
    { NULL, NULL }
};

/**
 *
 */
int lua_execute(msc_script *script, char *param, modsec_rec *msr, msre_rule *rule, char **error_msg) {
    apr_time_t time_before;
    lua_State *L = NULL;
    int rc;

    if (error_msg == NULL) return -1;
    *error_msg = NULL;

    if (msr->txcfg->debuglog_level >= 8) {
        msr_log(msr, 8, "Lua: Executing script: %s", script->name);
    }

    time_before = apr_time_now();

    /* Create new state. */
    L = lua_open();

    luaL_openlibs(L);

    /* Associate msr with the state. */
    lua_pushlightuserdata(L, (void *)msr);
    lua_setglobal(L, "__msr");

    /* Associate rule with the state. */
    if (rule != NULL) {
        lua_pushlightuserdata(L, (void *)rule);
        lua_setglobal(L, "__rule");
    }

    /* Register functions. */
    luaL_register(L, "m", mylib);

    rc = lua_restore(L, script);
    if (rc) {
        *error_msg = apr_psprintf(msr->mp, "Lua: Failed to restore script with %i.", rc);
        return -1;
    }

    /* Execute the chunk so that the functions are defined. */
    lua_pcall(L, 0, 0, 0);

    /* Execute main() */
    lua_getglobal(L, "main");

    /* Put the parameter on the stack. */
    if (param != NULL) {
        lua_pushlstring(L, param, strlen(param));
    }

    if (lua_pcall(L, ((param != NULL) ? 1 : 0), 1, 0)) {
        *error_msg = apr_psprintf(msr->mp, "Lua: Script execution failed: %s", lua_tostring(L, -1));
        return -1;
    }

    /* Get the response from the script. */
    *error_msg = (char *)lua_tostring(L, -1);
    if (*error_msg != NULL) {
        *error_msg = apr_pstrdup(msr->mp, *error_msg);
    }

    /* Destroy state. */
    lua_pop(L, 1);
    lua_close(L);

    /* Returns status code to caller. */
    if (msr->txcfg->debuglog_level >= 8) {
        msr_log(msr, 8, "Lua: Script completed in %" APR_TIME_T_FMT " usec, returning: %s.",
            (apr_time_now() - time_before), *error_msg);
    }

    return ((*error_msg != NULL) ? RULE_MATCH : RULE_NO_MATCH);
}
