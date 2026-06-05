#include "fs.h"
#include "../drivers/terminal.h"
#include <stddef.h>

static fs_file_t files[FS_MAX_FILES];
static fs_dir_t  dirs[FS_MAX_DIRS];
static int current_dir = 0;

static int strlen_s(const char* s) {
    int i = 0; while (s[i]) i++; return i;
}

static void strcpy_s(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int strcmp_s(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a - *b;
}

void fs_init(void) {
    for (int i = 0; i < FS_MAX_FILES; i++) files[i].used = 0;
    for (int i = 0; i < FS_MAX_DIRS;  i++) dirs[i].used  = 0;
    dirs[0].used = 1;
    dirs[0].parent = 0;
    strcpy_s(dirs[0].name, "/", FS_MAX_FILENAME);
    current_dir = 0;
}

int fs_mkdir(const char* name) {
    for (int i = 0; i < FS_MAX_DIRS; i++) {
        if (dirs[i].used && dirs[i].parent == current_dir && strcmp_s(dirs[i].name, name) == 0) {
            terminal_writeline("mkdir: directory already exists");
            return -1;
        }
    }
    for (int i = 1; i < FS_MAX_DIRS; i++) {
        if (!dirs[i].used) {
            dirs[i].used = 1;
            dirs[i].parent = current_dir;
            strcpy_s(dirs[i].name, name, FS_MAX_FILENAME);
            return 0;
        }
    }
    terminal_writeline("mkdir: no space left");
    return -1;
}

int fs_touch(const char* name, const char* content) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && files[i].dir_index == current_dir && strcmp_s(files[i].name, name) == 0) {
            if (content) {
                strcpy_s(files[i].data, content, FS_MAX_FILESIZE);
                files[i].size = strlen_s(content);
            }
            return 0;
        }
    }
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used) {
            files[i].used = 1;
            files[i].dir_index = current_dir;
            strcpy_s(files[i].name, name, FS_MAX_FILENAME);
            if (content) {
                strcpy_s(files[i].data, content, FS_MAX_FILESIZE);
                files[i].size = strlen_s(content);
            } else {
                files[i].data[0] = '\0';
                files[i].size = 0;
            }
            return 0;
        }
    }
    terminal_writeline("touch: no space left");
    return -1;
}

int fs_cat(const char* name) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && files[i].dir_index == current_dir && strcmp_s(files[i].name, name) == 0) {
            terminal_writeline(files[i].data);
            return 0;
        }
    }
    terminal_writeline("cat: file not found");
    return -1;
}

void fs_ls(void) {
    int found = 0;
    terminal_setcolor(VGA_LIGHT_CYAN, VGA_BLACK);
    for (int i = 0; i < FS_MAX_DIRS; i++) {
        if (dirs[i].used && dirs[i].parent == current_dir && i != current_dir) {
            terminal_write("[DIR]  ");
            terminal_writeline(dirs[i].name);
            found = 1;
        }
    }
    terminal_setcolor(VGA_WHITE, VGA_BLACK);
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && files[i].dir_index == current_dir) {
            terminal_write("[FILE] ");
            terminal_writeline(files[i].name);
            found = 1;
        }
    }
    terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
    if (!found) terminal_writeline("(empty)");
}

int fs_rm(const char* name) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && files[i].dir_index == current_dir && strcmp_s(files[i].name, name) == 0) {
            files[i].used = 0;
            return 0;
        }
    }
    terminal_writeline("rm: file not found");
    return -1;
}

int fs_cd(const char* name) {
    if (strcmp_s(name, "..") == 0) {
        if (current_dir != 0) current_dir = dirs[current_dir].parent;
        return 0;
    }
    if (strcmp_s(name, "/") == 0) { current_dir = 0; return 0; }
    for (int i = 1; i < FS_MAX_DIRS; i++) {
        if (dirs[i].used && dirs[i].parent == current_dir && strcmp_s(dirs[i].name, name) == 0) {
            current_dir = i;
            return 0;
        }
    }
    terminal_writeline("cd: directory not found");
    return -1;
}

int fs_getdata(const char* name, char* out, int maxlen) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && files[i].dir_index == current_dir && strcmp_s(files[i].name, name) == 0) {
            int j = 0;
            while (j < maxlen-1 && files[i].data[j]) { out[j] = files[i].data[j]; j++; }
            out[j] = '\0';
            return j;
        }
    }
    return -1;
}

int fs_pwd(char* out) {
    if (current_dir == 0) { out[0]='/'; out[1]='\0'; return 0; }
    char parts[16][FS_MAX_FILENAME];
    int depth = 0, d = current_dir;
    while (d != 0 && depth < 16) {
        int i = 0;
        while (dirs[d].name[i] && i < FS_MAX_FILENAME-1) { parts[depth][i] = dirs[d].name[i]; i++; }
        parts[depth][i] = '\0';
        depth++;
        d = dirs[d].parent;
    }
    int pos = 0;
    for (int i = depth - 1; i >= 0; i--) {
        out[pos++] = '/';
        for (int j = 0; parts[i][j]; j++) out[pos++] = parts[i][j];
    }
    out[pos] = '\0';
    return 0;
}
