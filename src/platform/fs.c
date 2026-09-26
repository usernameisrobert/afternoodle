/* Filesystem. No SDL dependency so tools/CLI can use it headless. */
#include "afndle/platform/platform.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(AF_OS_WINDOWS)
#  include <direct.h>
#  include <io.h>
#  include <windows.h>
#  define AF_SEP '\\'
#else
#  include <sys/time.h>
#  include <unistd.h>
#  define AF_SEP '/'
#endif

#include "afndle/core/log.h"

/* Normalises separators to '/' internally -- paths behave the same everywhere
 * and '/' is accepted by Windows APIs too. */
static void norm_sep(char* s) {
    for (; *s; s++)
        if (*s == '\\') *s = '/';
}

int af_fs_exists(const char *path) {
    if (!path) return 0;
    struct stat st;
    return stat(path, &st) == 0;
}

int af_fs_is_dir(const char *path) {
    if (!path) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (st.st_mode & S_IFMT) == S_IFDIR;
}

int af_fs_is_file(const char *path) {
    if (!path) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (st.st_mode & S_IFMT) == S_IFREG;
}

char *af_fs_read_file(const char *path, int64_t *out_len) {
    if (out_len) *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)af_malloc((size_t)size + 1);
    size_t got = fread(buf, 1, (size_t)size, f);
    buf[got] = '\0';
    fclose(f);
    if (out_len) *out_len = (int64_t)got;
    return buf;
}

int af_fs_write_file(const char *path, const void *data, int64_t len) {
    if (af_fs_is_dir(path)) return 0;
    /* Make sure the parent directory exists so save always just works. */
    char dir[1024];
    af_fs_dirname(path, dir, (int)sizeof(dir));
    if (dir[0]) af_fs_mkdirs(dir);
    FILE *f = fopen(path, "wb");
    if (!f) {
        AF_WARN("write failed: %s (%s)", path, strerror(errno));
        return 0;
    }
    size_t wrote = len > 0 ? fwrite(data, 1, (size_t)len, f) : 0;
    fclose(f);
    return wrote == (size_t)len;
}

int af_fs_append_file(const char *path, const void *data, int64_t len) {
    FILE *f = fopen(path, "ab");
    if (!f) return 0;
    size_t wrote = len > 0 ? fwrite(data, 1, (size_t)len, f) : 0;
    fclose(f);
    return wrote == (size_t)len;
}

int af_fs_mkdir(const char *path) {
    if (!path || !*path) return 0;
    if (af_fs_is_dir(path)) return 1;
#if defined(AF_OS_WINDOWS)
    int r = _mkdir(path);
#else
    int r = mkdir(path, 0777);
#endif
    return r == 0 || errno == EEXIST;
}

int af_fs_mkdirs(const char *path) {
    if (!path || !*path) return 0;
    char tmp[1024];
    af_str_cpy_max(af_str(path), tmp, (int)sizeof(tmp));
    norm_sep(tmp);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        af_fs_mkdir(tmp);
        *p = '/';
    }
    return af_fs_mkdir(tmp);
}

int af_fs_remove(const char *path) {
    if (af_fs_is_dir(path)) {
#if defined(AF_OS_WINDOWS)
        return _rmdir(path) == 0;
#else
        return rmdir(path) == 0;
#endif
    }
    return remove(path) == 0;
}

int af_fs_rename(const char *from, const char *to) { return rename(from, to) == 0; }

int64_t af_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (int64_t)st.st_size;
}

int64_t af_fs_modified_time(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (int64_t)st.st_mtime;
}

static int entry_cmp(const void *a, const void *b) {
    const AfDirEntry *x = (const AfDirEntry *)a, *y = (const AfDirEntry *)b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir; /* dirs first */
    int c = af_str_casecmp(af_str(x->name), y->name);
    return c;
}

AfDirEntry *af_fs_list_dir(const char *path, int *out_count) {
    if (out_count) *out_count = 0;
    if (!path) return NULL;
    DIR *d = opendir(path);
    if (!d) return NULL;

    int cap = 32, count = 0;
    AfDirEntry *list = (AfDirEntry *)af_malloc(sizeof(AfDirEntry) * (size_t)cap);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (count == cap) {
            cap *= 2;
            list = (AfDirEntry *)af_realloc(list, sizeof(AfDirEntry) * (size_t)cap);
        }
        AfDirEntry *de = &list[count];
        memset(de, 0, sizeof(*de));
        af_str_cpy_max(af_str(e->d_name), de->name, (int)sizeof(de->name));
        char full[1024];
        af_fs_path_join(path, e->d_name, full, (int)sizeof(full));
        de->is_dir = af_fs_is_dir(full);
        de->size = af_file_size(full);
        de->modified = af_fs_modified_time(full);
        count++;
    }
    closedir(d);
    if (count == 0) { af_free(list); return NULL; }
    qsort(list, (size_t)count, sizeof(AfDirEntry), entry_cmp);
    if (out_count) *out_count = count;
    return list;
}

void af_fs_dirname(const char *path, char *out, int cap) {
    if (cap <= 0) return;
    out[0] = '\0';
    if (!path) return;
    char tmp[1024];
    af_str_cpy_max(af_str(path), tmp, (int)sizeof(tmp));
    norm_sep(tmp);
    char *slash = strrchr(tmp, '/');
    if (!slash) { out[0] = '.'; out[1] = '\0'; return; }
    if (slash == tmp) { out[0] = '/'; out[1] = '\0'; return; }
    *slash = '\0';
    af_str_cpy_max(af_str(tmp), out, cap);
}

void af_fs_basename(const char *path, char *out, int cap) {
    if (cap <= 0) return;
    out[0] = '\0';
    if (!path) return;
    char tmp[1024];
    af_str_cpy_max(af_str(path), tmp, (int)sizeof(tmp));
    norm_sep(tmp);
    const char *slash = strrchr(tmp, '/');
    af_str_cpy_max(af_str(slash ? slash + 1 : tmp), out, cap);
}

void af_fs_strip_extension(const char *path, char *out, int cap) {
    af_fs_basename(path, out, cap);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = '\0';
}

const char *af_fs_extension(const char *path) {
    if (!path) return "";
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *base = path;
    if (slash && (!bslash || slash > bslash)) base = slash + 1;
    if (bslash && (!slash || bslash > slash)) base = bslash + 1;
    const char *dot = strrchr(base, '.');
    return (dot && dot != base) ? dot : "";
}

void af_fs_path_join(const char *a, const char *b, char *out, int cap) {
    if (cap <= 0) return;
    out[0] = '\0';
    if (!a || !*a) { af_str_cpy_max(af_str(b ? b : ""), out, cap); norm_sep(out); return; }
    if (!b || !*b) { af_str_cpy_max(af_str(a), out, cap); norm_sep(out); return; }
    if (af_fs_is_absolute(b)) { af_str_cpy_max(af_str(b), out, cap); norm_sep(out); return; }

    int alen = (int)strlen(a);
    af_str_cpy_max(af_str(a), out, cap);
    norm_sep(out);
    while (alen > 1 && out[alen - 1] == '/') out[--alen] = '\0';
    int olen = (int)strlen(out);
    if (olen + 1 < cap) {
        out[olen] = '/';
        out[olen + 1] = '\0';
        af_str_cpy_max(af_str(b), out + olen + 1, cap - olen - 1);
        norm_sep(out);
    }
}

/* Collapses "//" and "./" and resolves "..". Keeps a leading "/" and, on
 * Windows, a "C:/" drive prefix. */
void af_fs_normalize(const char *in, char *out, int cap) {
    if (cap <= 0) return;
    out[0] = '\0';
    if (!in) return;
    char tmp[1024];
    af_str_cpy_max(af_str(in), tmp, (int)sizeof(tmp));
    norm_sep(tmp);

    char *segs[128];
    int nseg = 0;
    int absolute = tmp[0] == '/';
    /* Drive prefix, if any. */
    int start = 0;
    if (tmp[0] && tmp[1] == ':') { segs[nseg++] = tmp; start = 2; if (tmp[2] == '/') start = 3; }

    char *p = tmp + start;
    char *tok = p;
    for (;;) {
        if (*p == '/' || *p == '\0') {
            int len = (int)(p - tok);
            if (len == 0) { /* skip */ }
            else if (len == 1 && tok[0] == '.') { /* skip */ }
            else if (len == 2 && tok[0] == '.' && tok[1] == '.') {
                if (nseg > (absolute ? 1 : 0) && strcmp(segs[nseg - 1], "..") != 0) nseg--;
                else if (!absolute && nseg < 128) segs[nseg++] = tok;
            } else if (nseg < 128) {
                segs[nseg++] = tok;
            }
            if (*p == '\0') break;
            *p = '\0';
            tok = p + 1;
        }
        p++;
    }

    int olen = 0;
    for (int i = 0; i < nseg; i++) {
        int len = (int)strlen(segs[i]);
        if (i > 0 || (!absolute && i == 0)) { if (olen + 1 < cap) out[olen++] = '/'; }
        for (int k = 0; k < len && olen + 1 < cap; k++) out[olen++] = segs[i][k];
    }
    out[olen < cap ? olen : cap - 1] = '\0';
    if (olen == 0 && cap > 1) { out[0] = absolute ? '/' : '.'; out[1] = '\0'; }
}

int af_fs_is_absolute(const char *path) {
    if (!path || !*path) return 0;
    if (path[0] == '/' || path[0] == '\\') return 1;
    if (path[0] && path[1] == ':' && (path[2] == '/' || path[2] == '\\')) return 1;
    return 0;
}

void af_fs_absolute(const char *path, char *out, int cap) {
    if (cap <= 0) return;
    if (af_fs_is_absolute(path)) { af_fs_normalize(path, out, cap); return; }
    char cwd[1024];
    af_fs_get_cwd(cwd, (int)sizeof(cwd));
    char joined[2048];
    af_fs_path_join(cwd, path ? path : "", joined, (int)sizeof(joined));
    af_fs_normalize(joined, out, cap);
}

void af_fs_exe_dir(char *out, int cap) {
    if (cap <= 0) return;
    out[0] = '\0';
#if defined(AF_OS_WINDOWS)
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)sizeof(buf));
    if (n > 0) {
        af_fs_dirname(buf, out, cap);
        return;
    }
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        af_fs_dirname(buf, out, cap);
        return;
    }
#endif
    af_fs_get_cwd(out, cap);
}

/* Returns 0 on success, so a failed chdir is visible instead of silent. */
int af_fs_set_cwd(const char *path) {
    if (!path) return 0;
#if defined(AF_OS_WINDOWS)
    return _chdir(path);
#else
    return chdir(path);
#endif
}

void af_fs_get_cwd(char *out, int cap) {
    if (cap <= 0) return;
    out[0] = '\0';
    char buf[1024];
#if defined(AF_OS_WINDOWS)
    if (!_getcwd(buf, (int)sizeof(buf))) return;
#else
    if (!getcwd(buf, sizeof(buf))) return;
#endif
    af_fs_normalize(buf, out, cap);
}

void af_fs_user_config_dir(const char *app, char *out, int cap) {
    if (cap <= 0) return;
    const char *base = NULL;
    char joined[1024];
#if defined(AF_OS_WINDOWS)
    base = getenv("APPDATA");
    if (base) af_fs_path_join(base, app ? app : "afternoodle", joined, (int)sizeof(joined));
    else af_str_cpy_max(af_str("."), joined, (int)sizeof(joined));
#else
    base = getenv("XDG_CONFIG_HOME");
    if (base && *base) af_fs_path_join(base, app ? app : "afternoodle", joined, (int)sizeof(joined));
    else {
        const char *home = getenv("HOME");
        if (home && *home) {
            char sub[512];
            snprintf(sub, sizeof(sub), ".config/%s", app ? app : "afternoodle");
            af_fs_path_join(home, sub, joined, (int)sizeof(joined));
        } else {
            af_str_cpy_max(af_str("."), joined, (int)sizeof(joined));
        }
    }
#endif
    af_fs_mkdirs(joined);
    af_fs_normalize(joined, out, cap);
}
