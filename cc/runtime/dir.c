/*
 * Concurrent-C Directory/Filesystem Runtime
 *
 * Cross-platform: POSIX (macOS, Linux, BSD) and Windows.
 */

#include <ccc/std/dir.cch>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#define PATH_SEP '\\'
#else
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fnmatch.h>
#define PATH_SEP '/'
#endif

/* ============================================================================
 * Platform-specific directory iterator
 * ============================================================================ */

struct CCDirIter {
    CCArena* arena;
#ifdef _WIN32
    HANDLE handle;
    WIN32_FIND_DATAA find_data;
    int first;
#else
    DIR* dir;
#endif
};

/* ============================================================================
 * Helpers
 * ============================================================================ */

/* Use cc_io_from_errno() from cc_io.cch for error conversion.
 * For EOF on directory iteration, use CC_IO_OTHER with os_code=0. */
#define CC_DIR_EOF_ERROR ((CCIoError){.kind = CC_IO_OTHER, .os_code = 0})

/* ============================================================================
 * Directory Iteration
 * ============================================================================ */

CCResultDirIterIoError cc_dir_open(CCArena* arena, const char* path) {
    if (!arena || !path) {
        return cc_err_CCResultDirIterIoError(cc_io_from_errno(EINVAL));
    }

    CCDirIter*     iter = cc_arena_alloc(arena, sizeof(CCDirIter), _Alignof(CCDirIter));
    if (!iter) {
        return cc_err_CCResultDirIterIoError(cc_io_from_errno(ENOMEM));
    }
    memset(iter, 0, sizeof(*iter));
    iter->arena = arena;

#ifdef _WIN32
    /* Windows: need pattern with wildcard */
    char pattern[MAX_PATH];
    size_t len = strlen(path);
    if (len + 3 >= MAX_PATH) {
        return cc_err_CCResultDirIterIoError(cc_io_from_errno(ENAMETOOLONG));
    }
    memcpy(pattern, path, len);
    if (len > 0 && path[len-1] != '\\' && path[len-1] != '/') {
        pattern[len++] = '\\';
    }
    pattern[len++] = '*';
    pattern[len] = '\0';

    iter->handle = FindFirstFileA(pattern, &iter->find_data);
    if (iter->handle == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        CCIoError e = {.kind = CC_IO_FILE_NOT_FOUND, .os_code = (int)err};
        return cc_err_CCResultDirIterIoError(e);
    }
    iter->first = 1;
#else
    iter->dir = opendir(path);
    if (!iter->dir) {
        return cc_err_CCResultDirIterIoError(cc_io_from_errno(errno));
    }
#endif

    return cc_ok_CCResultDirIterIoError(iter);
}

CCResultDirEntryIoError cc_dir_next(CCDirIter* iter, CCArena* arena) {
    CCDirEntry entry = {0};

    if (!iter || !arena) {
        return cc_err_CCResultDirEntryIoError(cc_io_from_errno(EINVAL));
    }

#ifdef _WIN32
    if (!iter->first) {
        if (!FindNextFileA(iter->handle, &iter->find_data)) {
            DWORD err = GetLastError();
            if (err == ERROR_NO_MORE_FILES) {
                return cc_err_CCResultDirEntryIoError(CC_DIR_EOF_ERROR);
            }
            CCIoError e = {.kind = CC_IO_OTHER, .os_code = (int)err};
            return cc_err_CCResultDirEntryIoError(e);
        }
    }
    iter->first = 0;

    /* Skip . and .. */
    while (strcmp(iter->find_data.cFileName, ".") == 0 ||
           strcmp(iter->find_data.cFileName, "..") == 0) {
        if (!FindNextFileA(iter->handle, &iter->find_data)) {
            return cc_err_CCResultDirEntryIoError(CC_DIR_EOF_ERROR);
        }
    }

    size_t name_len = strlen(iter->find_data.cFileName);
    char* name_copy = cc_arena_alloc(arena, name_len + 1, 1);
    if (!name_copy) {
        return cc_err_CCResultDirEntryIoError(cc_io_from_errno(ENOMEM));
    }
    memcpy(name_copy, iter->find_data.cFileName, name_len + 1);
    entry.name.ptr = name_copy;
    entry.name.len = name_len;

    if (iter->find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        entry.type = CC_DIRENT_DIR;
    } else if (iter->find_data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        entry.type = CC_DIRENT_SYMLINK;
    } else {
        entry.type = CC_DIRENT_FILE;
    }
#else
    struct dirent* de;
    while ((de = readdir(iter->dir)) != NULL) {
        /* Skip . and .. */
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        break;
    }

    if (!de) {
        return cc_err_CCResultDirEntryIoError(CC_DIR_EOF_ERROR);
    }

    size_t name_len = strlen(de->d_name);
    char* name_copy = cc_arena_alloc(arena, name_len + 1, 1);
    if (!name_copy) {
        return cc_err_CCResultDirEntryIoError(cc_io_from_errno(ENOMEM));
    }
    memcpy(name_copy, de->d_name, name_len + 1);
    entry.name.ptr = name_copy;
    entry.name.len = name_len;

#ifdef DT_DIR
    switch (de->d_type) {
        case DT_REG: entry.type = CC_DIRENT_FILE; break;
        case DT_DIR: entry.type = CC_DIRENT_DIR; break;
        case DT_LNK: entry.type = CC_DIRENT_SYMLINK; break;
        default:     entry.type = CC_DIRENT_OTHER; break;
    }
#else
    /* Fallback: stat the file */
    entry.type = CC_DIRENT_OTHER;
#endif
#endif

    return cc_ok_CCResultDirEntryIoError(entry);
}

void cc_dir_close(CCDirIter* iter) {
    if (!iter) return;
#ifdef _WIN32
    if (iter->handle != INVALID_HANDLE_VALUE) {
        FindClose(iter->handle);
        iter->handle = INVALID_HANDLE_VALUE;
    }
#else
    if (iter->dir) {
        closedir(iter->dir);
        iter->dir = NULL;
    }
#endif
}

/* ============================================================================
 * Directory Operations
 * ============================================================================ */

bool cc_path_exists(const char* path) {
    if (!path) return false;
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

bool cc_path_is_dir(const char* path) {
    if (!path) return false;
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
#endif
}

bool cc_path_is_file(const char* path) {
    if (!path) return false;
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISREG(st.st_mode);
#endif
}

CCResultBoolIoError cc_dir_create(const char* path) {
    if (!path) {
        return cc_err_CCResultBoolIoError(cc_io_from_errno(EINVAL));
    }
#ifdef _WIN32
    if (CreateDirectoryA(path, NULL)) {
        return cc_ok_CCResultBoolIoError(true);
    }
    DWORD err = GetLastError();
    CCIoError e = {.kind = CC_IO_OTHER, .os_code = (int)err};
    return cc_err_CCResultBoolIoError(e);
#else
    if (mkdir(path, 0755) == 0) {
        return cc_ok_CCResultBoolIoError(true);
    }
    return cc_err_CCResultBoolIoError(cc_io_from_errno(errno));
#endif
}

CCResultBoolIoError cc_dir_create_all(const char* path) {
    if (!path) {
        return cc_err_CCResultBoolIoError(cc_io_from_errno(EINVAL));
    }

    /* Make a mutable copy */
    size_t len = strlen(path);
    char* buf = malloc(len + 1);
    if (!buf) {
        return cc_err_CCResultBoolIoError(cc_io_from_errno(ENOMEM));
    }
    memcpy(buf, path, len + 1);

    /* Create directories one level at a time */
    for (size_t i = 1; i <= len; i++) {
        if (buf[i] == PATH_SEP || buf[i] == '\0') {
            char saved = buf[i];
            buf[i] = '\0';

            if (!cc_path_exists(buf)) {
#ifdef _WIN32
                if (!CreateDirectoryA(buf, NULL)) {
                    DWORD err = GetLastError();
                    if (err != ERROR_ALREADY_EXISTS) {
                        free(buf);
                        CCIoError e = {.kind = CC_IO_OTHER, .os_code = (int)err};
                        return cc_err_CCResultBoolIoError(e);
                    }
                }
#else
                if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
                    int err = errno;
                    free(buf);
                    return cc_err_CCResultBoolIoError(cc_io_from_errno(err));
                }
#endif
            }

            buf[i] = saved;
        }
    }

    free(buf);
    return cc_ok_CCResultBoolIoError(true);
}

CCResultBoolIoError cc_dir_remove(const char* path) {
    if (!path) {
        return cc_err_CCResultBoolIoError(cc_io_from_errno(EINVAL));
    }
#ifdef _WIN32
    if (RemoveDirectoryA(path)) {
        return cc_ok_CCResultBoolIoError(true);
    }
    DWORD err = GetLastError();
    CCIoError e = {.kind = CC_IO_OTHER, .os_code = (int)err};
    return cc_err_CCResultBoolIoError(e);
#else
    if (rmdir(path) == 0) {
        return cc_ok_CCResultBoolIoError(true);
    }
    return cc_err_CCResultBoolIoError(cc_io_from_errno(errno));
#endif
}

CCResultBoolIoError cc_file_remove(const char* path) {
    if (!path) {
        return cc_err_CCResultBoolIoError(cc_io_from_errno(EINVAL));
    }
#ifdef _WIN32
    if (DeleteFileA(path)) {
        return cc_ok_CCResultBoolIoError(true);
    }
    DWORD err = GetLastError();
    CCIoError e = {.kind = CC_IO_OTHER, .os_code = (int)err};
    return cc_err_CCResultBoolIoError(e);
#else
    if (unlink(path) == 0) {
        return cc_ok_CCResultBoolIoError(true);
    }
    return cc_err_CCResultBoolIoError(cc_io_from_errno(errno));
#endif
}

CCSlice cc_dir_cwd(CCArena* arena) {
    CCSlice result = {0};
    if (!arena) return result;

    char buf[4096];
#ifdef _WIN32
    DWORD len = GetCurrentDirectoryA(sizeof(buf), buf);
    if (len == 0 || len >= sizeof(buf)) return result;
#else
    if (!getcwd(buf, sizeof(buf))) return result;
    size_t len = strlen(buf);
#endif

    char* copy = cc_arena_alloc(arena, len + 1, 1);
    if (!copy) return result;
    memcpy(copy, buf, len);
    copy[len] = '\0';

    result.ptr = copy;
    result.len = len;
    return result;
}

CCResultBoolIoError cc_dir_chdir(const char* path) {
    if (!path) {
        return cc_err_CCResultBoolIoError(cc_io_from_errno(EINVAL));
    }
#ifdef _WIN32
    if (SetCurrentDirectoryA(path)) {
        return cc_ok_CCResultBoolIoError(true);
    }
    DWORD err = GetLastError();
    CCIoError e = {.kind = CC_IO_OTHER, .os_code = (int)err};
    return cc_err_CCResultBoolIoError(e);
#else
    if (chdir(path) == 0) {
        return cc_ok_CCResultBoolIoError(true);
    }
    return cc_err_CCResultBoolIoError(cc_io_from_errno(errno));
#endif
}

/* ============================================================================
 * Glob Pattern Matching
 * ============================================================================ */

bool cc_glob_match(const char* pattern, const char* name) {
    if (!pattern || !name) return false;

#ifdef _WIN32
    /* Simple glob matching for Windows */
    const char* p = pattern;
    const char* n = name;

    while (*p && *n) {
        if (*p == '*') {
            p++;
            if (*p == '\0') return true;  /* Trailing * matches all */
            /* Try matching rest of pattern at each position */
            while (*n) {
                if (cc_glob_match(p, n)) return true;
                n++;
            }
            return false;
        } else if (*p == '?') {
            p++;
            n++;
        } else if (*p == *n) {
            p++;
            n++;
        } else {
            return false;
        }
    }

    /* Handle trailing * in pattern */
    while (*p == '*') p++;
    return (*p == '\0' && *n == '\0');
#else
    return fnmatch(pattern, name, 0) == 0;
#endif
}

/* Helper: add path to glob result */
static void glob_add(CCGlobResult* result, CCArena* arena, const char* path, size_t len) {
    if (result->count >= result->capacity) {
        size_t new_cap = result->capacity ? result->capacity * 2 : 16;
        CCSlice* new_paths = cc_arena_alloc(arena, new_cap * sizeof(CCSlice), _Alignof(CCSlice));
        if (!new_paths) return;
        if (result->paths && result->count > 0) {
            memcpy(new_paths, result->paths, result->count * sizeof(CCSlice));
        }
        result->paths = new_paths;
        result->capacity = new_cap;
    }

    char* copy = cc_arena_alloc(arena, len + 1, 1);
    if (!copy) return;
    memcpy(copy, path, len);
    copy[len] = '\0';

    result->paths[result->count].ptr = copy;
    result->paths[result->count].len = len;
    result->count++;
}

/* Helper: recursive glob */
static void glob_recurse(CCArena* arena, CCGlobResult* result,
                         const char* dir, const char* pattern, int recursive);

static void glob_dir(CCArena* arena, CCGlobResult* result,
                     const char* dir, const char* pattern, int recursive) {
    CCResultDirIterIoError iter_res = cc_dir_open(arena, dir);
    if (iter_res.is_err) return;

    CCDirIter* iter = iter_res.ok;
    while (1) {
        CCResultDirEntryIoError entry_res = cc_dir_next(iter, arena);
        if (entry_res.is_err) break;

        CCDirEntry entry = entry_res.ok;
        const char* name = entry.name.ptr;

        /* Build full path */
        size_t dir_len = strlen(dir);
        size_t name_len = entry.name.len;
        size_t path_len = dir_len + 1 + name_len;
        char* full_path = cc_arena_alloc(arena, path_len + 1, 1);
        if (!full_path) continue;

        memcpy(full_path, dir, dir_len);
        full_path[dir_len] = PATH_SEP;
        memcpy(full_path + dir_len + 1, name, name_len);
        full_path[path_len] = '\0';

        if (entry.type == CC_DIRENT_DIR) {
            if (recursive) {
                glob_recurse(arena, result, full_path, pattern, recursive);
            }
        } else {
            if (cc_glob_match(pattern, name)) {
                glob_add(result, arena, full_path, path_len);
            }
        }
    }

    cc_dir_close(iter);
}

static void glob_recurse(CCArena* arena, CCGlobResult* result,
                         const char* dir, const char* pattern, int recursive) {
    glob_dir(arena, result, dir, pattern, recursive);

    /* Recurse into subdirectories */
    CCResultDirIterIoError iter_res = cc_dir_open(arena, dir);
    if (iter_res.is_err) return;

    CCDirIter* iter = iter_res.ok;
    while (1) {
        CCResultDirEntryIoError entry_res = cc_dir_next(iter, arena);
        if (entry_res.is_err) break;

        CCDirEntry entry = entry_res.ok;
        if (entry.type != CC_DIRENT_DIR) continue;

        size_t dir_len = strlen(dir);
        size_t name_len = entry.name.len;
        size_t path_len = dir_len + 1 + name_len;
        char* subdir = cc_arena_alloc(arena, path_len + 1, 1);
        if (!subdir) continue;

        memcpy(subdir, dir, dir_len);
        subdir[dir_len] = PATH_SEP;
        memcpy(subdir + dir_len + 1, entry.name.ptr, name_len);
        subdir[path_len] = '\0';

        glob_recurse(arena, result, subdir, pattern, recursive);
    }

    cc_dir_close(iter);
}

CCGlobResult cc_glob(CCArena* arena, const char* pattern) {
    CCGlobResult result = {0};
    if (!arena || !pattern) return result;

    /* Check for ** (recursive) */
    int recursive = (strstr(pattern, "**") != NULL);

    /* Find directory and file pattern parts */
    const char* last_sep = strrchr(pattern, PATH_SEP);
#ifndef _WIN32
    if (!last_sep) last_sep = strrchr(pattern, '/');
#endif

    char dir[4096];
    const char* file_pattern;

    if (last_sep) {
        size_t dir_len = (size_t)(last_sep - pattern);
        if (dir_len >= sizeof(dir)) return result;
        memcpy(dir, pattern, dir_len);
        dir[dir_len] = '\0';
        file_pattern = last_sep + 1;

        /* Handle ** in directory part */
        char* star_star = strstr(dir, "**");
        if (star_star) {
            *star_star = '\0';  /* Truncate at ** */
            if (dir[0] == '\0') strcpy(dir, ".");
            recursive = 1;
        }
    } else {
        strcpy(dir, ".");
        file_pattern = pattern;
    }

    if (recursive) {
        glob_recurse(arena, &result, dir, file_pattern, 1);
    } else {
        glob_dir(arena, &result, dir, file_pattern, 0);
    }

    return result;
}
