#include "shell.h"
#include "cinterp.h"
#include "tcc.h"
#include "../drivers/terminal.h"
#include "../drivers/keyboard.h"
#include "../fs/fs.h"
#include "../fs/fat16.h"
#include <stddef.h>

#define CMD_BUF 256

/* ===== Helpers ===== */
static int strlen_s(const char* s) { int i=0; while(s[i]) i++; return i; }
static int strcmp_s(const char* a, const char* b) {
    while(*a && *b && *a==*b){a++;b++;} return *a-*b;
}
static void strcpy_s(char* d, const char* s){ int i=0; while((d[i]=s[i])) i++; }

static inline void outw_s(unsigned short port, unsigned short val) {
    __asm__ volatile ("outw %0, %1" :: "a"(val), "Nd"(port));
}

/* ===== Command History ===== */
#define HISTORY_MAX 16
static char history[HISTORY_MAX][CMD_BUF];
static int  history_count = 0;
static int  history_idx   = 0;

static void history_add(const char* cmd) {
    if(!*cmd) return;
    strcpy_s(history[history_count % HISTORY_MAX], cmd);
    history_count++;
    history_idx = history_count;
}

/* ===== Clipboard ===== */
static char clipboard[512];

static void line_clear_display(char* buf, int* pos, int* len){
    while(*pos>0){ terminal_putchar('\b'); (*pos)--; }
    for(int i=0;i<*len;i++) terminal_putchar(' ');
    for(int i=0;i<*len;i++) terminal_putchar('\b');
    buf[0]='\0'; *len=0; *pos=0;
}

static void line_redraw(char* buf, int* pos, int* len, const char* newstr){
    line_clear_display(buf, pos, len);
    strcpy_s(buf, newstr);
    *len = strlen_s(buf);
    *pos = *len;
    terminal_write(buf);
}

/* Extended readline with direct ASCII support */
static void shell_readline(char* buf, int maxlen){
    int pos=0, len=0;
    buf[0]='\0';

    while(1){
        char c = keyboard_getchar();
        if(c == 0) continue;
        terminal_putchar(c);

        if(c == '\n'){
            terminal_putchar('\n');
            buf[len]='\0';
            break;
        }
        if(c == '\b'){
            if(pos > 0){
                pos--;
                for(int i=pos; i<len-1; i++) buf[i]=buf[i+1];
                len--; buf[len]='\0';
                terminal_putchar('\b');
                for(int i=pos; i<len; i++) terminal_putchar(buf[i]);
                terminal_putchar(' ');
                for(int i=pos; i<len+1; i++) terminal_putchar('\b');
            }
            continue;
        }
        if(len < maxlen-1){
            for(int i=len; i>pos; i--) buf[i]=buf[i-1];
            buf[pos]=c; pos++; len++; buf[len]='\0';
            for(int i=pos-1; i<len; i++) terminal_putchar(buf[i]);
            for(int i=pos; i<len; i++) terminal_putchar('\b');
        }
    }
}

static void int_to_str2(int v, char* buf){
    if(v<0){*buf++='-';v=-v;}
    char tmp[16];int i=0;
    if(!v){tmp[i++]='0';}
    while(v){tmp[i++]='0'+(v%10);v/=10;}
    for(int j=i-1;j>=0;j--)*buf++=tmp[j];
    *buf=0;
}

/* ===== Code Editor ===== */
#define EDITOR_LINES 20
#define EDITOR_COLS  78
#define EDITOR_LINE_LEN 128

static char ed_lines[EDITOR_LINES][EDITOR_LINE_LEN];
static int  ed_nlines;
static int  ed_row, ed_col;
static char ed_fname[64];

static void editor_draw(void){
    terminal_clear();
    terminal_setcolor(VGA_BLACK, VGA_LIGHT_GREY);
    terminal_write(" SOS Editor | ");
    terminal_write(ed_fname);
    terminal_write(" | Type normally. Press '`' (backtick) to Save & Exit ");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
    terminal_putchar('\n');

    for(int i=0;i<ed_nlines;i++){
        terminal_setcolor(VGA_DARK_GREY, VGA_BLACK);
        char num[5];
        num[0]='0'+(i+1)/10; num[1]='0'+(i+1)%10; num[2]=':'; num[3]=' '; num[4]=0;
        terminal_write(num);

        if(i==ed_row){
            terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
            int ln=strlen_s(ed_lines[i]);
            for(int j=0;j<ln;j++){
                if(j==ed_col){
                    terminal_setcolor(VGA_BLACK, VGA_LIGHT_GREY);
                    terminal_putchar(ed_lines[i][j]?ed_lines[i][j]:' ');
                    terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
                } else {
                    terminal_putchar(ed_lines[i][j]);
                }
            }
            if(ed_col>=ln){
                terminal_setcolor(VGA_BLACK, VGA_LIGHT_GREY);
                terminal_putchar(' ');
                terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
            }
            terminal_putchar('\n');
        } else {
            terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
            terminal_writeline(ed_lines[i]);
        }
    }
    terminal_setcolor(VGA_LIGHT_GREEN, VGA_BLACK);
    terminal_write("Line:");
    char r[8]; int_to_str2(ed_row+1, r); terminal_write(r);
    terminal_write(" Col:");
    char c2[8]; int_to_str2(ed_col, c2); terminal_write(c2);
    terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
}

static void cmd_edit(char* fname){
    if(!*fname){ terminal_writeline("edit: missing filename"); return; }
    strcpy_s(ed_fname, fname);
    ed_nlines=1; ed_row=0; ed_col=0;

    static char tmp[4096];
    int r = -1;
    if(fat_ready()) r = fat_read(fname, tmp, 4096);
    if(r < 0) r = fs_getdata(fname, tmp, 4096);
    if(r>0){
        int li=0, ci=0; ed_nlines=0;
        for(int i=0;tmp[i]&&li<EDITOR_LINES;i++){
            if(tmp[i]=='\n'||ci>=EDITOR_LINE_LEN-1){
                ed_lines[li][ci]='\0'; li++; ci=0;
            } else {
                ed_lines[li][ci++]=tmp[i];
            }
        }
        if(ci>0){ ed_lines[li][ci]='\0'; li++; }
        ed_nlines=li>0?li:1;
    } else {
        ed_lines[0][0]='\0'; ed_nlines=1;
    }

    editor_draw();

    while(1){
        char c = keyboard_getchar();
        if(!c) continue;

        if(c == '`') {
            static char out[4096]; int op=0;
            for(int i=0;i<ed_nlines;i++){
                for(int j=0;ed_lines[i][j];j++) out[op++]=ed_lines[i][j];
                out[op++]='\n';
            }
            out[op]='\0';
            extern int fs_touch(const char*, const char*);
            if(fat_ready()) fat_write(ed_fname, out, op);
            else fs_touch(ed_fname, out);
            terminal_clear();
            terminal_setcolor(VGA_LIGHT_GREEN, VGA_BLACK);
            terminal_writeline("File saved.");
            terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
            return;
        }

        if(c=='\n'){
            if(ed_nlines<EDITOR_LINES-1){
                for(int i=ed_nlines;i>ed_row+1;i--) strcpy_s(ed_lines[i], ed_lines[i-1]);
                ed_lines[ed_row+1][0]='\0';
                strcpy_s(ed_lines[ed_row+1], ed_lines[ed_row]+ed_col);
                ed_lines[ed_row][ed_col]='\0';
                ed_nlines++; ed_row++; ed_col=0;
            }
        } else if(c=='\b'){
            if(ed_col>0){
                int len2=strlen_s(ed_lines[ed_row]);
                for(int i=ed_col-1;i<len2-1;i++) ed_lines[ed_row][i]=ed_lines[ed_row][i+1];
                ed_lines[ed_row][len2-1]='\0'; ed_col--;
            } else if(ed_row>0){
                int plen=strlen_s(ed_lines[ed_row-1]);
                int clen2=strlen_s(ed_lines[ed_row]);
                if(plen+clen2<EDITOR_LINE_LEN-1){
                    for(int i=0;i<clen2;i++) ed_lines[ed_row-1][plen+i]=ed_lines[ed_row][i];
                    ed_lines[ed_row-1][plen+clen2]='\0';
                    for(int i=ed_row;i<ed_nlines-1;i++) strcpy_s(ed_lines[i],ed_lines[i+1]);
                    ed_nlines--; ed_row--; ed_col=plen;
                }
            }
        } else {
            int len2=strlen_s(ed_lines[ed_row]);
            if(len2<EDITOR_LINE_LEN-1){
                for(int i=len2;i>=ed_col;i--) ed_lines[ed_row][i+1]=ed_lines[ed_row][i];
                ed_lines[ed_row][ed_col]=c; ed_col++;
            }
        }
        editor_draw();
    }
}

static void print_prompt(void) {
    char pwd[128]; fs_pwd(pwd);
    terminal_setcolor(VGA_LIGHT_GREEN, VGA_BLACK);
    terminal_write("root@sos:");
    terminal_setcolor(VGA_LIGHT_CYAN, VGA_BLACK);
    terminal_write(pwd);
    terminal_setcolor(VGA_WHITE, VGA_BLACK);
    terminal_write("# ");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
}

static void cmd_help(void) {
    terminal_setcolor(VGA_LIGHT_CYAN, VGA_BLACK);
    terminal_writeline("==== SOS Project Commands ====");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
    terminal_writeline("  help              - show this help");
    terminal_writeline("  clear             - clear screen");
    terminal_writeline("  ls                - list files");
    terminal_writeline("  pwd               - print directory");
    terminal_writeline("  cd <dir>          - change directory");
    terminal_writeline("  mkdir <name>      - make directory");
    terminal_writeline("  touch <name>      - create file");
    terminal_writeline("  write <name>      - write file (. to end)");
    terminal_writeline("  edit <name>       - open code editor");
    terminal_writeline("  cat <name>        - show file");
    terminal_writeline("  rm <name>         - delete file");
    terminal_writeline("  run <name>        - run C script (interpreter)");
    terminal_writeline("  tcc <name>        - compile & run C program");
    terminal_writeline("  uname             - system info");
    terminal_writeline("  reboot            - reboot");
    terminal_writeline("  shutdown          - shutdown");
}

static void cmd_uname(void) {
    terminal_setcolor(VGA_LIGHT_GREEN, VGA_BLACK);
    terminal_writeline("SOS Project v1.0 by SHAYAK_ARROW | Built with AI");
    terminal_writeline("32-bit x86 | C Interpreter | Code Editor");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
}

static void cmd_reboot(void) {
    terminal_writeline("Rebooting...");
    __asm__ volatile (
        "cli\n"
        "1: inb $0x64, %%al\n"
        "testb $0x02, %%al\n"
        "jnz 1b\n"
        "movb $0xFE, %%al\n"
        "outb %%al, $0x64\n"
        "hlt\n" ::: "eax"
    );
}

static void cmd_shutdown(void) {
    terminal_writeline("Shutting down...");
    outw_s(0x0604, 0x2000);
    outw_s(0xB004, 0x0000);
    outw_s(0x4004, 0x3400);
    __asm__ volatile (
        "movw $0x5301, %%ax\n" "xorw %%bx, %%bx\n" "int $0x15\n"
        "movw $0x530E, %%ax\n" "movw $0x0102, %%cx\n" "int $0x15\n"
        "movw $0x5307, %%ax\n" "movw $0x0001, %%bx\n" "movw $0x0003, %%cx\n"
        "int $0x15\n" ::: "eax","ebx","ecx"
    );
    terminal_writeline("Power off manually.");
    __asm__ volatile ("cli; hlt");
}

static void cmd_write(char* fname) {
    if(!*fname){ terminal_writeline("write: missing filename"); return; }
    terminal_setcolor(VGA_LIGHT_CYAN, VGA_BLACK);
    terminal_write("Writing '"); terminal_write(fname);
    terminal_writeline("' ? type lines, end with '.' alone:");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
    static char buf[4096]; int pos=0;
    char line[256];
    while(1){
        terminal_write("> ");
        shell_readline(line, 256);
        if(strcmp_s(line,".")==0) break;
        int len=strlen_s(line);
        if(pos+len+1<4095){
            for(int i=0;line[i];i++) buf[pos++]=line[i];
            buf[pos++]='\n';
        }
    }
    buf[pos]='\0';
    if(fat_ready()) fat_write(fname, buf, pos);
    else fs_touch(fname, buf);
    terminal_setcolor(VGA_LIGHT_GREEN, VGA_BLACK);
    terminal_writeline("Saved!");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
}

static void cmd_run(char* fname) {
    if(!*fname){ terminal_writeline("run: missing filename"); return; }
    static char code[4096];
    int r = -1;
    if(fat_ready()) r = fat_read(fname, code, 4096);
    if(r < 0) r = fs_getdata(fname, code, 4096);
    if(r < 0){ terminal_write("run: not found: "); terminal_writeline(fname); return; }
    terminal_setcolor(VGA_LIGHT_GREEN, VGA_BLACK);
    terminal_write("Running: "); terminal_writeline(fname);
    terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
    cinterp_run(code);
}

static void cmd_run_tcc(char* fname) {
    if(!*fname){ terminal_writeline("tcc: missing filename"); return; }
    static char code[8192];
    int r = -1;
    if(fat_ready()) r = fat_read(fname, code, 8192);
    if(r < 0) r = fs_getdata(fname, code, 8192);
    if(r < 0){ terminal_write("tcc: file not found: "); terminal_writeline(fname); return; }
    terminal_setcolor(VGA_LIGHT_GREEN, VGA_BLACK);
    terminal_write("Compiling & running: "); terminal_writeline(fname);
    terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
    tcc_run(code);
}

static void skip_spaces(char** p){ while(**p==' ') (*p)++; }

void shell_run(void) {
    char buf[CMD_BUF];

    terminal_setcolor(VGA_LIGHT_CYAN, VGA_BLACK);
    terminal_writeline(" ____  ___  ____    ____  ____   ___       _ _____ ____ _____");
    terminal_writeline("/ ___|/ _ \\/ ___|  |  _ \\|  _ \\ / _ \\     | | ____/ ___|_   _|");
    terminal_writeline("\\___ \\ | | \\___ \\  | |_) | |_) | | | |  _ | |  _|| |     | |");
    terminal_writeline(" ___) | |_| |___) | |  __/|  _ <| |_| | | |_| | |__| |___  | |");
    terminal_writeline("|____/ \\___/|____/  |_|   |_| \\_\\\\___/   \\___/|_____\\____| |_|");
    terminal_setcolor(VGA_WHITE, VGA_BLACK);
    terminal_writeline("");
    terminal_writeline("  SOS Project v1.0  |  by SHAYAK_ARROW  |  Built with AI");
    terminal_writeline("  Type 'help' for commands");
    terminal_writeline("");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);

    while(1){
        print_prompt();
        shell_readline(buf, CMD_BUF);
        char* p=buf; skip_spaces(&p);
        if(!*p) continue;
        history_add(p);

        char* cmd=p;
        while(*p && *p!=' ') p++;
        if(*p==' '){ *p='\0'; p++; }
        skip_spaces(&p);
        char* args=p;

        if     (strcmp_s(cmd,"help"    )==0) cmd_help();
        else if(strcmp_s(cmd,"clear"   )==0) terminal_clear();
        else if(strcmp_s(cmd,"ls"      )==0){
            if(fat_ready()){
                FatEntry entries[32]; int n=fat_list("",entries,32);
                for(int i=0;i<n;i++){
                    terminal_setcolor(entries[i].is_dir?VGA_LIGHT_CYAN:VGA_WHITE, VGA_BLACK);
                    terminal_write(entries[i].is_dir?"[DIR]  ":"[FILE] ");
                    terminal_write(entries[i].name);
                    if(!entries[i].is_dir){
                        terminal_write("  (");
                        char sz[16]; int_to_str2(entries[i].size,sz);
                        terminal_write(sz); terminal_write(" bytes)");
                    }
                    terminal_putchar('\n');
                }
                terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
                if(n==0) terminal_writeline("(empty)");
            } else fs_ls();
        }
        else if(strcmp_s(cmd,"pwd"     )==0){ char pwd[128]; fs_pwd(pwd); terminal_writeline(pwd); }
        else if(strcmp_s(cmd,"cd"      )==0) fs_cd(args);
        else if(strcmp_s(cmd,"mkdir"   )==0){
            if(!*args) terminal_writeline("mkdir: missing name");
            else { if(fat_ready()) fat_mkdir(args); else fs_mkdir(args); }
        }
        else if(strcmp_s(cmd,"touch"   )==0){
            if(!*args) terminal_writeline("touch: missing name");
            else { if(fat_ready()) fat_write(args,"",0); else fs_touch(args,0); }
        }
        else if(strcmp_s(cmd,"write"   )==0) cmd_write(args);
        else if(strcmp_s(cmd,"edit"    )==0) cmd_edit(args);
        else if(strcmp_s(cmd,"cat"     )==0){
            if(!*args){ terminal_writeline("cat: missing filename"); }
            else {
                static char catbuf[4096]; int r=-1;
                if(fat_ready()) r=fat_read(args,catbuf,4096);
                if(r<0) fs_cat(args);
                else terminal_write(catbuf);
            }
        }
        else if(strcmp_s(cmd,"rm"      )==0){
            if(!*args) terminal_writeline("rm: missing filename");
            else { if(fat_ready()) fat_delete(args); else fs_rm(args); }
        }
        else if(strcmp_s(cmd,"run"      )==0) cmd_run(args);
        else if(strcmp_s(cmd,"tcc"      )==0) cmd_run_tcc(args);
        else if(strcmp_s(cmd,"uname"   )==0) cmd_uname();
        else if(strcmp_s(cmd,"reboot"  )==0) cmd_reboot();
        else if(strcmp_s(cmd,"shutdown")==0) cmd_shutdown();
        else if(strcmp_s(cmd,"halt"    )==0){ terminal_writeline("Halting..."); __asm__ volatile("cli;hlt"); }
        else { terminal_write(cmd); terminal_writeline(": command not found. Type 'help'"); }
    }
}
