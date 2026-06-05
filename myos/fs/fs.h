#ifndef FS_H
#define FS_H

#include <stdint.h>

#define FS_MAX_FILES     64
#define FS_MAX_DIRS      16
#define FS_MAX_FILENAME  32
#define FS_MAX_FILESIZE  512
#define FS_MAX_PATH      128

typedef struct {
    char name[FS_MAX_FILENAME];
    char data[FS_MAX_FILESIZE];
    int  size;
    int  used;
    int  dir_index;  /* which directory this belongs to */
} fs_file_t;

typedef struct {
    char name[FS_MAX_FILENAME];
    int  used;
    int  parent;
} fs_dir_t;

void fs_init(void);
int  fs_mkdir(const char* name);
int  fs_touch(const char* name, const char* content);
int  fs_cat(const char* name);
void fs_ls(void);
int  fs_rm(const char* name);
int  fs_getdata(const char* name, char* out, int maxlen);
int  fs_pwd(char* out);
int  fs_cd(const char* name);

#endif
