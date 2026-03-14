#include <ccc/std/command.h>

#include <errno.h>
#include <string.h>

static int cc_command_push_raw(CCCommand *cmd, CCSlice arg) {
    size_t old_len;
    size_t offset;

    if (!cmd || !cmd->arena || !cmd->storage.arena || !cmd->offsets.arena) return -1;

    old_len = cmd->storage.len;
    offset = old_len;

    if (Vec_size_t_push(&cmd->offsets, offset) != 0) return -1;
    if (!cc_string_push_slice(&cmd->storage, arg)) {
        cmd->offsets.len--;
        return -1;
    }
    if (!cc_string_push_char(&cmd->storage, '\0')) {
        cmd->storage.len = old_len;
        if (cmd->storage.data) cmd->storage.data[old_len] = '\0';
        cmd->offsets.len--;
        return -1;
    }
    return 0;
}

CCCommand cc_command_new(CCArena *arena, const char *program) {
    CCCommand cmd = {0};
    cmd.arena = arena;
    cmd.storage = cc_string_new(arena);
    cmd.offsets = Vec_size_t_init(arena, 0);

    if (!arena || !cmd.storage.arena || !cmd.offsets.arena) {
        CCCommand empty = {0};
        return empty;
    }

    if (program && !cc_command_arg(&cmd, program)) {
        CCCommand empty = {0};
        return empty;
    }
    return cmd;
}

size_t cc_command_argc(const CCCommand *cmd) {
    return cmd ? cmd->offsets.len : 0;
}

const char *cc_command_get(const CCCommand *cmd, size_t idx) {
    if (!cmd || idx >= cmd->offsets.len || !cmd->storage.data || !cmd->offsets.data) return NULL;
    return cmd->storage.data + cmd->offsets.data[idx];
}

const char *cc_command_program(const CCCommand *cmd) {
    return cc_command_get(cmd, 0);
}

CCCommand *cc_command_arg(CCCommand *cmd, const char *arg) {
    if (!arg) return cmd;
    return cc_command_arg_slice(cmd, cc_slice_from_buffer((void *)arg, strlen(arg)));
}

CCCommand *cc_command_arg_slice(CCCommand *cmd, CCSlice arg) {
    return cc_command_push_raw(cmd, arg) == 0 ? cmd : NULL;
}

CCCommand *cc_command_arg_i64(CCCommand *cmd, int64_t value) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", (long long)value);
    if (n < 0) return NULL;
    return cc_command_arg_slice(cmd, cc_slice_from_buffer(buf, (size_t)n));
}

const char **cc_command_argv(CCCommand *cmd) {
    const char **argv;
    size_t argc;
    size_t i;

    if (!cmd || !cmd->arena || !cmd->storage.data || !cmd->offsets.data) return NULL;

    argc = cmd->offsets.len;
    argv = (const char**)cc_arena_alloc(cmd->arena, (argc + 1) * sizeof(const char*), _Alignof(const char*));
    if (!argv) return NULL;

    for (i = 0; i < argc; ++i) {
        argv[i] = cmd->storage.data + cmd->offsets.data[i];
    }
    argv[argc] = NULL;
    return argv;
}

CCProcessConfig cc_command_process_config(CCCommand *cmd) {
    CCProcessConfig cfg = {0};
    cfg.program = cc_command_program(cmd);
    cfg.args = cc_command_argv(cmd);
    return cfg;
}

CCResult_CCProcess_CCIoError cc_command_spawn(CCCommand *cmd) {
    CCProcessConfig cfg = cc_command_process_config(cmd);
    if (!cfg.program || !cfg.args) {
        return cc_err_CCResult_CCProcess_CCIoError(cc_io_from_errno(EINVAL));
    }
    return cc_process_spawn(&cfg);
}

CCResult_CCProcessOutput_CCIoError cc_command_run(CCCommand *cmd, CCArena *arena) {
    CCProcessConfig cfg = cc_command_process_config(cmd);
    if (!arena || !cfg.program || !cfg.args) {
        return cc_err_CCResult_CCProcessOutput_CCIoError(cc_io_from_errno(EINVAL));
    }
    return cc_process_run(arena, cfg.program, cfg.args);
}

CCResult_CCProcessOutput_CCIoError cc_command_output(CCCommand *cmd, CCArena *arena) {
    return cc_command_run(cmd, arena);
}

CCResult_int_CCIoError cc_command_status(CCCommand *cmd) {
    CCProcessConfig cfg = cc_command_process_config(cmd);
    CCResult_CCProcess_CCIoError spawn_res;
    CCResult_CCProcessStatus_CCIoError wait_res;
    CCProcess proc;
    CCProcessStatus status;

    if (!cfg.program || !cfg.args) {
        return cc_err_CCResult_int_CCIoError(cc_io_from_errno(EINVAL));
    }

    spawn_res = cc_process_spawn(&cfg);
    if (cc_is_err(spawn_res)) {
        return cc_err_CCResult_int_CCIoError(cc_unwrap_err(spawn_res));
    }

    proc = cc_unwrap(spawn_res);
    wait_res = cc_process_wait(&proc);
    if (cc_is_err(wait_res)) {
        return cc_err_CCResult_int_CCIoError(cc_unwrap_err(wait_res));
    }

    status = cc_unwrap(wait_res);
    return cc_ok_CCResult_int_CCIoError(status.exit_code);
}
