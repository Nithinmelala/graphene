/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University */

/*
 * This file contains the main function of the PAL loader, which loads and processes environment,
 * arguments and manifest.
 */

#include <stdbool.h>

#include "api.h"
#include "elf/elf.h"
#include "pal.h"
#include "pal_debug.h"
#include "pal_defs.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "pal_rtld.h"
#include "sysdeps/generic/ldsodefs.h"
#include "toml.h"

PAL_CONTROL g_pal_control;

PAL_CONTROL* pal_control_addr(void) {
    return &g_pal_control;
}

struct pal_internal_state g_pal_state;

static void load_libraries(void) {
    int ret = 0;
    char* preload_str = NULL;

    if (!g_pal_state.manifest_root)
        return;

    /* FIXME: rewrite to use array-of-strings TOML syntax */
    /* string with preload libs: can be multiple URIs separated by commas */
    ret = toml_string_in(g_pal_state.manifest_root, "loader.preload", &preload_str);
    if (ret < 0)
        INIT_FAIL_MANIFEST(PAL_ERROR_DENIED, "Cannot parse \'loader.preload\'");

    if (!preload_str)
        return;

    size_t len = strlen(preload_str);
    if (len == 0)
        return;

    char* c = preload_str;
    char* library_name = c;
    for (;; c++) {
        if (*c == ',' || !*c) {
            if (c > library_name) {
                *c = 0;
                if ((ret = load_elf_object(library_name, OBJECT_PRELOAD)) < 0)
                    INIT_FAIL(-ret, "Unable to load preload library");
            }

            if (c == preload_str + len)
                break;

            library_name = c + 1;
        }
    }
}

/* This function leaks memory on failure (and this is non-trivial to fix), but the assumption is
 * that its failure finishes the execution of the whole process right away. */
static int insert_envs_from_manifest(const char*** envpp) {
    int ret;
    assert(envpp);

    if (!g_pal_state.manifest_root)
        return 0;

    toml_table_t* toml_loader = toml_table_in(g_pal_state.manifest_root, "loader");
    if (!toml_loader)
        return 0;

    toml_table_t* toml_envs = toml_table_in(toml_loader, "env");
    if (!toml_envs)
        return 0;

    ssize_t toml_envs_cnt = toml_table_nkval(toml_envs);
    if (toml_envs_cnt <= 0) {
        /* no env entries found in the manifest */
        return 0;
    }

    size_t orig_envs_cnt = 0;
    size_t overwrite_cnt = 0;
    for (const char** orig_env = *envpp; *orig_env; orig_env++, orig_envs_cnt++) {
        char* orig_env_key_end = strchr(*orig_env, '=');
        if (!orig_env_key_end)
            return -PAL_ERROR_INVAL;

        *orig_env_key_end = '\0';
        toml_raw_t toml_env_raw = toml_raw_in(toml_envs, *orig_env);
        if (toml_env_raw) {
            /* found the original-env key in manifest (i.e., loader.env.<key> exists) */
            overwrite_cnt++;
        }
        *orig_env_key_end = '=';
    }

    size_t total_envs_cnt = orig_envs_cnt + toml_envs_cnt - overwrite_cnt;
    const char** new_envp = calloc(total_envs_cnt + 1, sizeof(const char*));
    if (!new_envp)
        return -PAL_ERROR_NOMEM;

    /* For simplicity, allocate each env anew; this is suboptimal but happens only once. First
     * go through original envs and populate new_envp with only those that are not overwritten by
     * manifest envs. Then append all manifest envs to new_envp. */
    size_t idx = 0;
    for (const char** orig_env = *envpp; *orig_env; orig_env++) {
        char* orig_env_key_end = strchr(*orig_env, '=');

        *orig_env_key_end = '\0';
        toml_raw_t toml_env_raw = toml_raw_in(toml_envs, *orig_env);
        if (!toml_env_raw) {
            /* this original env is not found in manifest (i.e., not overwritten) */
            *orig_env_key_end = '=';
            new_envp[idx] = malloc_copy(*orig_env, strlen(*orig_env) + 1);
            if (!new_envp[idx]) {
                /* don't care about proper memory cleanup since will terminate anyway */
                return -PAL_ERROR_NOMEM;
            }
            idx++;
        }
        *orig_env_key_end = '=';
    }
    assert(idx < total_envs_cnt);

    for (ssize_t i = 0; i < toml_envs_cnt; i++) {
        const char* toml_env_key = toml_key_in(toml_envs, i);
        assert(toml_env_key);
        toml_raw_t toml_env_value_raw = toml_raw_in(toml_envs, toml_env_key);
        assert(toml_env_value_raw);

        char* toml_env_value = NULL;
        ret = toml_rtos(toml_env_value_raw, &toml_env_value);
        if (ret < 0) {
            /* don't care about proper memory cleanup since will terminate anyway */
            return -PAL_ERROR_NOMEM;
        }

        char* final_env = alloc_concat3(toml_env_key, strlen(toml_env_key), "=", 1, toml_env_value,
                                        strlen(toml_env_value));
        new_envp[idx++] = final_env;
        free(toml_env_value);
    }
    assert(idx == total_envs_cnt);

    *envpp = new_envp;
    return 0;
}

static void set_debug_type(void) {
    int ret = 0;

    if (!g_pal_state.manifest_root)
        return;

    char* debug_type = NULL;
    ret = toml_string_in(g_pal_state.manifest_root, "loader.debug_type", &debug_type);
    if (ret < 0)
        INIT_FAIL_MANIFEST(PAL_ERROR_DENIED, "Cannot parse \'loader.debug_type\'");

    if (!debug_type)
        return;

    PAL_HANDLE handle = NULL;

    if (!strcmp(debug_type, "inline")) {
        ret = _DkStreamOpen(&handle, URI_PREFIX_DEV "tty", PAL_ACCESS_WRONLY, 0, 0, 0);
    } else if (!strcmp(debug_type, "file")) {
        char* debug_file = NULL;
        ret = toml_string_in(g_pal_state.manifest_root, "loader.debug_file", &debug_file);
        if (ret < 0 || !debug_file)
            INIT_FAIL_MANIFEST(PAL_ERROR_DENIED, "Cannot find/parse \'loader.debug_file\'");

        ret = _DkStreamOpen(&handle, debug_file, PAL_ACCESS_WRONLY, PAL_SHARE_OWNER_W,
                            PAL_CREATE_TRY, 0);
        free(debug_file);
    } else if (!strcmp(debug_type, "none")) {
        ret = 0;
    } else {
        INIT_FAIL_MANIFEST(PAL_ERROR_INVAL, "Unknown \'loader.debug_type\' "
                           "(allowed: `inline`, `file`, `none`)");
    }

    free(debug_type);

    if (ret < 0)
        INIT_FAIL(-ret, "Cannot open debug stream");

    g_pal_control.debug_stream = handle;
}

/* Loads a file containing a concatenation of C-strings. The resulting array of pointers is
 * NULL-terminated. All C-strings inside it reside in a single malloc-ed buffer starting at
 * (*res)[0].
 */
static int load_cstring_array(const char* uri, const char*** res) {
    PAL_HANDLE hdl;
    PAL_STREAM_ATTR attr;
    char* buf = NULL;
    const char** array = NULL;
    int ret;

    ret = _DkStreamOpen(&hdl, uri, PAL_ACCESS_RDONLY, 0, 0, 0);
    if (ret < 0)
        return ret;
    ret = _DkStreamAttributesQueryByHandle(hdl, &attr);
    if (ret < 0)
        goto out_fail;
    size_t file_size = attr.pending_size;
    buf = malloc(file_size);
    if (!buf) {
        ret = -PAL_ERROR_NOMEM;
        goto out_fail;
    }
    ret = _DkStreamRead(hdl, 0, file_size, buf, NULL, 0);
    if (ret < 0)
        goto out_fail;
    if (file_size > 0 && buf[file_size - 1] != '\0') {
        ret = -PAL_ERROR_INVAL;
        goto out_fail;
    }

    size_t count = 0;
    for (size_t i = 0; i < file_size; i++)
        if (buf[i] == '\0')
            count++;
    array = malloc(sizeof(char*) * (count + 1));
    if (!array) {
        ret = -PAL_ERROR_NOMEM;
        goto out_fail;
    }
    array[count] = NULL;
    if (file_size > 0) {
        const char** argv_it = array;
        *(argv_it++) = buf;
        for (size_t i = 0; i < file_size - 1; i++)
            if (buf[i] == '\0')
                *(argv_it++) = buf + i + 1;
    }
    *res = array;
    return _DkObjectClose(hdl);

out_fail:
    (void)_DkObjectClose(hdl);
    free(buf);
    free(array);
    return ret;
}

/* 'pal_main' must be called by the host-specific bootloader */
noreturn void pal_main(PAL_NUM instance_id,        /* current instance id */
                       PAL_HANDLE manifest_handle, /* manifest handle if opened */
                       PAL_HANDLE exec_handle,     /* executable handle if opened */
                       PAL_PTR exec_loaded_addr,   /* executable addr if loaded */
                       PAL_HANDLE parent_process,  /* parent process if it's a child */
                       PAL_HANDLE first_thread,    /* first thread handle */
                       PAL_STR* arguments,         /* application arguments */
                       PAL_STR* environments       /* environment variables */) {
    g_pal_state.instance_id = instance_id;
    g_pal_state.alloc_align = _DkGetAllocationAlignment();
    assert(IS_POWER_OF_2(g_pal_state.alloc_align));

    init_slab_mgr(g_pal_state.alloc_align);

    g_pal_state.parent_process = parent_process;

    char uri_buf[URI_MAX];
    char* manifest_uri = NULL;
    char* exec_uri = NULL;
    ssize_t ret;

    if (exec_handle) {
        ret = _DkStreamGetName(exec_handle, uri_buf, URI_MAX);
        if (ret < 0)
            INIT_FAIL(-ret, "Cannot get executable name");

        exec_uri = malloc_copy(uri_buf, ret + 1);
    }

    if (manifest_handle) {
        ret = _DkStreamGetName(manifest_handle, uri_buf, URI_MAX);
        if (ret < 0)
            INIT_FAIL(-ret, "Cannot get manifest name");

        manifest_uri = malloc_copy(uri_buf, ret + 1);
    } else {
        if (!exec_handle)
            INIT_FAIL(PAL_ERROR_INVAL, "Must have manifest or executable");

        /* try opening "<execname>.manifest" */
        size_t len = sizeof(uri_buf);
        ret = get_norm_path(exec_uri, uri_buf, &len);
        if (ret < 0) {
            INIT_FAIL(-ret, "Cannot normalize exec_uri");
        }

        strcpy_static(uri_buf + len, ".manifest", sizeof(uri_buf) - len);
        ret = _DkStreamOpen(&manifest_handle, uri_buf, PAL_ACCESS_RDONLY, 0, 0, 0);
        if (ret) {
            /* try opening "file:manifest" */
            manifest_uri = URI_PREFIX_FILE "manifest";
            ret = _DkStreamOpen(&manifest_handle, manifest_uri, PAL_ACCESS_RDONLY, 0, 0, 0);
            if (ret) {
                INIT_FAIL(PAL_ERROR_DENIED, "Cannot find manifest file");
            }
        }
    }

    /* load manifest if there is one */
    if (!g_pal_state.manifest_root && manifest_handle) {
        PAL_STREAM_ATTR attr;
        ret = _DkStreamAttributesQueryByHandle(manifest_handle, &attr);
        if (ret < 0)
            INIT_FAIL(-ret, "Cannot open manifest file");

        void* cfg_addr = NULL;
        int cfg_size = attr.pending_size;

        ret = _DkStreamMap(manifest_handle, &cfg_addr, PAL_PROT_READ, 0, ALLOC_ALIGN_UP(cfg_size));
        if (ret < 0)
            INIT_FAIL(-ret, "Cannot open manifest file");

        char errbuf[256];
        g_pal_state.manifest_root = toml_parse(cfg_addr, errbuf, sizeof(errbuf));
        if (!g_pal_state.manifest_root)
            INIT_FAIL_MANIFEST(PAL_ERROR_DENIED, errbuf);
    }

    if (g_pal_state.manifest_root) {
        char* dummy_exec_str = NULL;
        ret = toml_string_in(g_pal_state.manifest_root, "loader.exec", &dummy_exec_str);
        if (ret < 0 || dummy_exec_str)
            INIT_FAIL(PAL_ERROR_INVAL, "loader.exec is not supported anymore. Please update your "
                                       "manifest according to the current documentation.");
    }

    /* try to find an executable with the name matching the manifest name */
    if (!exec_handle && manifest_uri) {
        size_t exec_uri_len;
        bool success = false;
        // try ".manifest"
        if (strendswith(manifest_uri, ".manifest")) {
            exec_uri_len = strlen(manifest_uri) - 9;
            success = true;
        } else if (strendswith(manifest_uri, ".manifest.sgx")) {
            exec_uri_len = strlen(manifest_uri) - 13;
            success = true;
        }

        if (success) {
            exec_uri = alloc_concat(manifest_uri, exec_uri_len, NULL, 0);
            if (!exec_uri)
                INIT_FAIL(PAL_ERROR_NOMEM, "Cannot allocate URI buf");
            ret = _DkStreamOpen(&exec_handle, exec_uri, PAL_ACCESS_RDONLY, 0, 0, 0);
            if (ret < 0)
                INIT_FAIL(PAL_ERROR_INVAL, "Cannot open the executable");
        }
    }

    /* must be an ELF */
    if (exec_handle) {
        if (exec_loaded_addr) {
            if (!has_elf_magic(exec_loaded_addr, sizeof(ElfW(Ehdr))))
                INIT_FAIL(PAL_ERROR_INVAL, "Executable is not an ELF binary");
        } else {
            if (!is_elf_object(exec_handle))
                INIT_FAIL(PAL_ERROR_INVAL, "Executable is not an ELF binary");
        }
    }

    g_pal_state.manifest        = manifest_uri;
    g_pal_state.manifest_handle = manifest_handle;
    g_pal_state.exec            = exec_uri;
    g_pal_state.exec_handle     = exec_handle;

    bool disable_aslr = false;
    if (g_pal_state.manifest_root) {
        int64_t disable_aslr_int64;
        ret = toml_int_in(g_pal_state.manifest_root, "loader.insecure__disable_aslr",
                          /*defaultval=*/0, &disable_aslr_int64);
        if (ret < 0 || (disable_aslr_int64 != 0 && disable_aslr_int64 != 1)) {
            INIT_FAIL_MANIFEST(PAL_ERROR_DENIED, "Cannot parse \'loader.insecure__disable_aslr\' "
                                                 "(the value must be 0 or 1)");
        }
        disable_aslr = !!disable_aslr_int64;
    }

    /* Load argv */
    /* TODO: Add an option to specify argv inline in the manifest. This requires an upgrade to the
     * manifest syntax. See https://github.com/oscarlab/graphene/issues/870 (Use YAML or TOML syntax
     * for manifests). 'loader.argv0_override' won't be needed after implementing this feature and
     * resolving https://github.com/oscarlab/graphene/issues/1053 (RFC: graphene invocation).
     */
    bool argv0_overridden = false;
    if (g_pal_state.manifest_root) {
        char* argv0_override = NULL;
        ret = toml_string_in(g_pal_state.manifest_root, "loader.argv0_override", &argv0_override);
        if (ret < 0)
            INIT_FAIL_MANIFEST(PAL_ERROR_DENIED, "Cannot parse \'loader.argv0_override\'");

        if (argv0_override) {
            argv0_overridden = true;
            if (!arguments[0]) {
                arguments = malloc(sizeof(const char*) * 2);
                if (!arguments)
                    INIT_FAIL(PAL_ERROR_NOMEM, "malloc() failed");
                arguments[1] = NULL;
            }
            arguments[0] = argv0_override;
        }
    }

    int64_t use_cmdline_argv = 0;
    if (g_pal_state.manifest_root) {
        ret = toml_int_in(g_pal_state.manifest_root, "loader.insecure__use_cmdline_argv",
                          /*defaultval=*/0, &use_cmdline_argv);
        if (ret < 0 || (use_cmdline_argv != 0 && use_cmdline_argv != 1)) {
            INIT_FAIL_MANIFEST(PAL_ERROR_DENIED, "Cannot parse "
                               "\'loader.insecure__use_cmdline_argv\' (the value must be 0 or 1)");
        }
    }

    if (use_cmdline_argv) {
        printf("WARNING: Using insecure argv source. Don't use this configuration in "
               "production!\n");
    } else {
        char* argv_src_file = NULL;

        if (g_pal_state.manifest_root) {
            ret = toml_string_in(g_pal_state.manifest_root, "loader.argv_src_file", &argv_src_file);
            if (ret < 0)
                INIT_FAIL_MANIFEST(PAL_ERROR_DENIED, "Cannot parse \'loader.argv_src_file\'");
        }

        if (argv_src_file) {
            /* Load argv from a file and discard cmdline argv. We trust the file contents (this can
             * be achieved using protected or trusted files). */
            if (arguments[0] && arguments[1])
                printf("Discarding cmdline arguments (%s %s [...]) because loader.argv_src_file "
                        "was specified in the manifest.\n", arguments[0], arguments[1]);

            ret = load_cstring_array(argv_src_file, &arguments);
            if (ret < 0)
                INIT_FAIL(-ret, "Cannot load arguments from \'loader.argv_src_file\'");

            free(argv_src_file);
        } else if (!argv0_overridden || (arguments[0] && arguments[1])) {
            INIT_FAIL(PAL_ERROR_INVAL, "argv handling wasn't configured in the manifest, but "
                      "cmdline arguments were specified.");
        }
    }

    int64_t use_host_env = 0;
    if (g_pal_state.manifest_root) {
        ret = toml_int_in(g_pal_state.manifest_root, "loader.insecure__use_host_env",
                          /*defaultval=*/0, &use_host_env);
        if (ret < 0 || (use_host_env != 0 && use_host_env != 1)) {
            INIT_FAIL_MANIFEST(PAL_ERROR_DENIED, "Cannot parse "
                               "\'loader.insecure__use_host_env\' (the value must be 0 or 1)");
        }
    }

    if (use_host_env) {
        printf("WARNING: Forwarding host environment variables to the app is enabled. Don't use "
               "this configuration in production!\n");
    } else {
        environments = malloc(sizeof(*environments));
        if (!environments)
            INIT_FAIL(PAL_ERROR_NOMEM, "Out of memory");
        environments[0] = NULL;
    }

    char* env_src_file = NULL;
    if (g_pal_state.manifest_root) {
        ret = toml_string_in(g_pal_state.manifest_root, "loader.env_src_file", &env_src_file);
        if (ret < 0)
            INIT_FAIL_MANIFEST(PAL_ERROR_DENIED, "Cannot parse \'loader.env_src_file\'");
    }

    if (use_host_env && env_src_file)
        INIT_FAIL(PAL_ERROR_INVAL, "Wrong manifest configuration - cannot use "
                  "loader.insecure__use_host_env and loader.env_src_file at the same time.");

    if (env_src_file) {
        /* Insert environment variables from a file. We trust the file contents (this can be
         * achieved using protected or trusted files). */
        ret = load_cstring_array(env_src_file, &environments);
        if (ret < 0)
            INIT_FAIL(-ret, "Cannot load environment variables from \'loader.env_src_file\'");
        free(env_src_file);
    }


    // TODO: Envs from file should be able to override ones from the manifest, but current
    // code makes this hard to implement.
    ret = insert_envs_from_manifest(&environments);
    if (ret < 0)
        INIT_FAIL(-ret, "Inserting environment variables from the manifest failed");

    load_libraries();

    if (exec_handle) {
        if (exec_loaded_addr) {
            ret = add_elf_object(exec_loaded_addr, exec_handle, OBJECT_EXEC);
        } else {
            ret = load_elf_object_by_handle(exec_handle, OBJECT_EXEC);
        }

        if (ret < 0)
            INIT_FAIL(-ret, pal_strerror(ret));
    }

    set_debug_type();

    g_pal_control.host_type       = XSTRINGIFY(HOST_TYPE);
    g_pal_control.process_id      = _DkGetProcessId();
    g_pal_control.host_id         = _DkGetHostId();
    g_pal_control.manifest_handle = manifest_handle;
    g_pal_control.executable      = exec_uri;
    g_pal_control.parent_process  = parent_process;
    g_pal_control.first_thread    = first_thread;
    g_pal_control.disable_aslr    = disable_aslr;

    _DkGetAvailableUserAddressRange(&g_pal_control.user_address.start,
                                    &g_pal_control.user_address.end);

    g_pal_control.alloc_align = g_pal_state.alloc_align;

    if (_DkGetCPUInfo(&g_pal_control.cpu_info) < 0) {
        goto out_fail;
    }
    g_pal_control.mem_info.mem_total = _DkMemoryQuota();

    /* Now we will start the execution */
    start_execution(arguments, environments);

out_fail:
    /* We wish we will never reached here */
    INIT_FAIL(PAL_ERROR_DENIED, "unexpected termination");
}
