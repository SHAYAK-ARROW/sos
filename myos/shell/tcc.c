/*
 * SOS Mini C Compiler (tcc.c)
 * Supports: int/char/void, pointers, arrays, structs,
 * malloc/calloc/free, printf/scanf,
 * if/else, while, for, do-while, switch,
 * functions, return, break, continue
 * Error reporting with line numbers
 */

#include "tcc.h"
#include "../drivers/terminal.h"
#include "../drivers/keyboard.h"
#include <stddef.h>

/* ===== Memory allocator (simple bump allocator) ===== */
static unsigned char heap[65536]; /* 64KB heap */
static unsigned int  heap_top = 0;

typedef struct MemBlock {
    unsigned int size;
    int          free;
    struct MemBlock* next;
} MemBlock;

static MemBlock* heap_head = 0;

static void heap_init(void){
    heap_top = 0;
    heap_head = 0;
}

static void* tcc_malloc(unsigned int size){
    if(size==0) return 0;
    size = (size+3)&~3; /* align to 4 bytes */
    /* search free block */
    MemBlock* b = heap_head;
    while(b){
        if(b->free && b->size >= size){ b->free=0; return (void*)(b+1); }
        b=b->next;
    }
    /* new block */
    unsigned int need = sizeof(MemBlock)+size;
    if(heap_top+need > sizeof(heap)) return 0;
    MemBlock* nb = (MemBlock*)(heap+heap_top);
    nb->size = size; nb->free = 0; nb->next = heap_head;
    heap_head = nb;
    heap_top += need;
    return (void*)(nb+1);
}

static void* tcc_calloc(unsigned int n, unsigned int size){
    unsigned int total = n*size;
    void* p = tcc_malloc(total);
    if(p){ unsigned char* b=(unsigned char*)p; for(unsigned int i=0;i<total;i++) b[i]=0; }
    return p;
}

static void tcc_free(void* ptr){
    if(!ptr) return;
    MemBlock* b = ((MemBlock*)ptr)-1;
    b->free = 1;
}

/* ===== Helpers ===== */
static int t_strlen(const char* s){ int i=0; while(s[i]) i++; return i; }
static int t_strcmp(const char* a,const char* b){ while(*a&&*b&&*a==*b){a++;b++;} return *a-*b; }
static int t_strncmp(const char* a,const char* b,int n){ for(int i=0;i<n;i++) if(a[i]!=b[i]) return a[i]-b[i]; return 0; }
static void t_strcpy(char* d,const char* s){ int i=0; while((d[i]=s[i])) i++; }
static int t_isspace(char c){ return c==' '||c=='\t'||c=='\n'||c=='\r'; }
static int t_isdigit(char c){ return c>='0'&&c<='9'; }
static int t_isalpha(char c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; }
static int t_isalnum(char c){ return t_isalpha(c)||t_isdigit(c); }
static int t_isxdigit(char c){ return t_isdigit(c)||(c>='a'&&c<='f')||(c>='A'&&c<='F'); }

static void int_to_str(int v, char* buf){
    if(v<0){*buf++='-';v=-v;}
    char tmp[16];int i=0;
    if(!v){tmp[i++]='0';}
    while(v){tmp[i++]='0'+(v%10);v/=10;}
    for(int j=i-1;j>=0;j--)*buf++=tmp[j];
    *buf=0;
}
static int str_to_int(const char* s){
    int neg=0,v=0;
    if(*s=='-'){neg=1;s++;}
    while(t_isdigit(*s)) v=v*10+(*s++)-'0';
    return neg?-v:v;
}

/* ===== Error reporting ===== */
static int cur_line;
static int had_error;

static void tcc_error(const char* msg, const char* extra){
    terminal_setcolor(12, 0); /* red */
    terminal_write("Error at line ");
    char buf[16]; int_to_str(cur_line, buf);
    terminal_write(buf);
    terminal_write(": ");
    terminal_write(msg);
    if(extra){ terminal_write(" '"); terminal_write(extra); terminal_write("'"); }
    terminal_putchar('\n');
    terminal_setcolor(7, 0);
    had_error = 1;
}

static void tcc_warn(const char* msg){
    terminal_setcolor(14, 0); /* yellow */
    terminal_write("Warning line ");
    char buf[16]; int_to_str(cur_line, buf);
    terminal_write(buf); terminal_write(": ");
    terminal_writeline(msg);
    terminal_setcolor(7, 0);
}

/* ===== Tokens ===== */
#define T_INT      1
#define T_CHAR     2
#define T_VOID     3
#define T_IF       4
#define T_ELSE     5
#define T_WHILE    6
#define T_FOR      7
#define T_DO       8
#define T_RETURN   9
#define T_BREAK   10
#define T_CONTINUE 11
#define T_SWITCH  12
#define T_CASE    13
#define T_DEFAULT 14
#define T_STRUCT  15
#define T_SIZEOF  16
#define T_IDENT   20
#define T_NUM     21
#define T_STR     22
#define T_CHAR_LIT 23
#define T_PLUS    30
#define T_MINUS   31
#define T_STAR    32
#define T_SLASH   33
#define T_MOD     34
#define T_AMP     35
#define T_EQ      36  /* == */
#define T_NEQ     37
#define T_LT      38
#define T_GT      39
#define T_LE      40
#define T_GE      41
#define T_ASSIGN  42  /* = */
#define T_PLUSEQ  43
#define T_MINUSEQ 44
#define T_STAREQ  45
#define T_SLASHEQ 46
#define T_INC     47  /* ++ */
#define T_DEC     48  /* -- */
#define T_AND     49  /* && */
#define T_OR      50  /* || */
#define T_NOT     51  /* ! */
#define T_TILDE   52  /* ~ */
#define T_LSHIFT  53  /* << */
#define T_RSHIFT  54  /* >> */
#define T_BITOR   55  /* | */
#define T_BITXOR  56  /* ^ */
#define T_SEMI    60
#define T_COLON   61
#define T_COMMA   62
#define T_DOT     63
#define T_ARROW   64  /* -> */
#define T_LPAREN  70
#define T_RPAREN  71
#define T_LBRACE  72
#define T_RBRACE  73
#define T_LBRACKET 74
#define T_RBRACKET 75
#define T_EOF     99

typedef struct {
    int type;
    int ival;
    char sval[128];
} Tok;

static const char* src_ptr;
static Tok curtok;

static void skip_ws(void){
    while(*src_ptr){
        if(*src_ptr=='\n'){ cur_line++; src_ptr++; continue; }
        if(t_isspace(*src_ptr)){ src_ptr++; continue; }
        /* preprocessor directive - skip entire line */
        if(src_ptr[0]=='#'){
            while(*src_ptr&&*src_ptr!='\n') src_ptr++;
            continue;
        }
        /* single line comment */
        if(src_ptr[0]=='/'&&src_ptr[1]=='/'){
            while(*src_ptr&&*src_ptr!='\n') src_ptr++;
            continue;
        }
        /* multi line comment */
        if(src_ptr[0]=='/'&&src_ptr[1]=='*'){
            src_ptr+=2;
            while(*src_ptr&&!(src_ptr[0]=='*'&&src_ptr[1]=='/')){
                if(*src_ptr=='\n') cur_line++;
                src_ptr++;
            }
            if(*src_ptr) src_ptr+=2;
            continue;
        }
        break;
    }
}

static void next_tok(void){
    skip_ws();
    if(!*src_ptr){ curtok.type=T_EOF; return; }
    char c=*src_ptr;

    /* String literal */
    if(c=='"'){
        src_ptr++; int i=0;
        while(*src_ptr&&*src_ptr!='"'&&i<126){
            if(*src_ptr=='\\'){
                src_ptr++;
                if(*src_ptr=='n') curtok.sval[i++]='\n';
                else if(*src_ptr=='t') curtok.sval[i++]='\t';
                else if(*src_ptr=='0') curtok.sval[i++]='\0';
                else curtok.sval[i++]=*src_ptr;
                src_ptr++;
            } else { curtok.sval[i++]=*src_ptr++; }
        }
        curtok.sval[i]=0; if(*src_ptr=='"') src_ptr++;
        curtok.type=T_STR; return;
    }
    /* Char literal */
    if(c=='\''){
        src_ptr++;
        char ch=*src_ptr++;
        if(ch=='\\'){
            if(*src_ptr=='n') ch='\n';
            else if(*src_ptr=='t') ch='\t';
            else ch=*src_ptr;
            src_ptr++;
        }
        if(*src_ptr=='\'') src_ptr++;
        curtok.type=T_CHAR_LIT; curtok.ival=ch; return;
    }
    /* Number */
    if(t_isdigit(c)){
        int v=0;
        if(c=='0'&&(src_ptr[1]=='x'||src_ptr[1]=='X')){
            src_ptr+=2;
            while(t_isxdigit(*src_ptr)){
                char hc=*src_ptr++;
                int hv=t_isdigit(hc)?hc-'0':(hc>='a'?hc-'a'+10:hc-'A'+10);
                v=v*16+hv;
            }
        } else {
            while(t_isdigit(*src_ptr)) v=v*10+(*src_ptr++)-'0';
        }
        curtok.type=T_NUM; curtok.ival=v; return;
    }
    /* Identifier / keyword */
    if(t_isalpha(c)){
        int i=0; while(t_isalnum(*src_ptr)&&i<63) curtok.sval[i++]=*src_ptr++;
        curtok.sval[i]=0;
        if(t_strcmp(curtok.sval,"int"     )==0) curtok.type=T_INT;
        else if(t_strcmp(curtok.sval,"char"    )==0) curtok.type=T_CHAR;
        else if(t_strcmp(curtok.sval,"void"    )==0) curtok.type=T_VOID;
        else if(t_strcmp(curtok.sval,"if"      )==0) curtok.type=T_IF;
        else if(t_strcmp(curtok.sval,"else"    )==0) curtok.type=T_ELSE;
        else if(t_strcmp(curtok.sval,"while"   )==0) curtok.type=T_WHILE;
        else if(t_strcmp(curtok.sval,"for"     )==0) curtok.type=T_FOR;
        else if(t_strcmp(curtok.sval,"do"      )==0) curtok.type=T_DO;
        else if(t_strcmp(curtok.sval,"return"  )==0) curtok.type=T_RETURN;
        else if(t_strcmp(curtok.sval,"break"   )==0) curtok.type=T_BREAK;
        else if(t_strcmp(curtok.sval,"continue")==0) curtok.type=T_CONTINUE;
        else if(t_strcmp(curtok.sval,"switch"  )==0) curtok.type=T_SWITCH;
        else if(t_strcmp(curtok.sval,"case"    )==0) curtok.type=T_CASE;
        else if(t_strcmp(curtok.sval,"default" )==0) curtok.type=T_DEFAULT;
        else if(t_strcmp(curtok.sval,"struct"  )==0) curtok.type=T_STRUCT;
        else if(t_strcmp(curtok.sval,"sizeof"  )==0) curtok.type=T_SIZEOF;
        else curtok.type=T_IDENT;
        return;
    }
    src_ptr++;
    switch(c){
        case '+': 
            if(*src_ptr=='+'){ src_ptr++; curtok.type=T_INC; }
            else if(*src_ptr=='='){ src_ptr++; curtok.type=T_PLUSEQ; }
            else { curtok.type=T_PLUS; } 
            break;
        case '-': 
            if(*src_ptr=='-'){ src_ptr++; curtok.type=T_DEC; }
            else if(*src_ptr=='='){ src_ptr++; curtok.type=T_MINUSEQ; }
            else if(*src_ptr=='>'){ src_ptr++; curtok.type=T_ARROW; }
            else { curtok.type=T_MINUS; } 
            break;
        case '*': 
            if(*src_ptr=='='){ src_ptr++; curtok.type=T_STAREQ; }
            else { curtok.type=T_STAR; } 
            break;
        case '/': 
            if(*src_ptr=='='){ src_ptr++; curtok.type=T_SLASHEQ; }
            else { curtok.type=T_SLASH; } 
            break;
        case '%': 
            curtok.type=T_MOD; 
            break;
        case '&': 
            if(*src_ptr=='&'){ src_ptr++; curtok.type=T_AND; }
            else { curtok.type=T_AMP; } 
            break;
        case '|': 
            if(*src_ptr=='|'){ src_ptr++; curtok.type=T_OR; }
            else { curtok.type=T_BITOR; } 
            break;
        case '^': 
            curtok.type=T_BITXOR; 
            break;
        case '~': 
            curtok.type=T_TILDE; 
            break;
        case '!': 
            if(*src_ptr=='='){ src_ptr++; curtok.type=T_NEQ; }
            else { curtok.type=T_NOT; } 
            break;
        case '<': 
            if(*src_ptr=='='){ src_ptr++; curtok.type=T_LE; }
            else if(*src_ptr=='<'){ src_ptr++; curtok.type=T_LSHIFT; }
            else { curtok.type=T_LT; } 
            break;
        case '>': 
            if(*src_ptr=='='){ src_ptr++; curtok.type=T_GE; }
            else if(*src_ptr=='>'){ src_ptr++; curtok.type=T_RSHIFT; }
            else { curtok.type=T_GT; } 
            break;
        case '=': 
            if(*src_ptr=='='){ src_ptr++; curtok.type=T_EQ; }
            else { curtok.type=T_ASSIGN; } 
            break;
        case ';': curtok.type=T_SEMI; break;
        case ':': curtok.type=T_COLON; break;
        case ',': curtok.type=T_COMMA; break;
        case '.': curtok.type=T_DOT; break;
        case '(': curtok.type=T_LPAREN; break;
        case ')': curtok.type=T_RPAREN; break;
        case '{': curtok.type=T_LBRACE; break;
        case '}': curtok.type=T_RBRACE; break;
        case '[': curtok.type=T_LBRACKET; break;
        case ']': curtok.type=T_RBRACKET; break;
        default:  curtok.type=T_EOF; break;
    }
}

static void expect(int type, const char* what){
    if(curtok.type!=type){ tcc_error("expected", what); }
    else next_tok();
}

/* ===== Value system ===== */
#define VAL_INT  0
#define VAL_PTR  1
#define VAL_STR  2

typedef struct {
    int type;   /* VAL_INT/PTR/STR */
    int ival;
    char* sval;
    int* pval; /* pointer to int */
} Val;

static Val make_int(int v){ Val r; r.type=VAL_INT; r.ival=v; r.sval=0; r.pval=0; return r; }
static Val make_str(char* s){ Val r; r.type=VAL_STR; r.ival=0; r.sval=s; r.pval=0; return r; }
static Val make_ptr(int* p){ Val r; r.type=VAL_PTR; r.ival=0; r.sval=0; r.pval=p; return r; }
static int val_int(Val v){ if(v.type==VAL_PTR) return v.pval?(int)(unsigned long)v.pval:0; return v.ival; }

/* ===== Variables ===== */
#define MAX_VARS   64
#define MAX_ARRAYS 16
#define VARNAME    48

typedef struct { char name[VARNAME]; int value; int is_ptr; int* ptr; } Var;
typedef struct { char name[VARNAME]; int* data; int size; } Arr;

/* Stack frames for functions */
#define MAX_FRAMES 8
#define MAX_FRAME_VARS 32
typedef struct {
    Var vars[MAX_FRAME_VARS];
    int nvar;
} Frame;

static Frame frames[MAX_FRAMES];
static int   frame_top = 0;

static Frame* cur_frame(void){ return &frames[frame_top]; }

static void frame_push(void){ if(frame_top<MAX_FRAMES-1){ frame_top++; frames[frame_top].nvar=0; } }
static void frame_pop(void){ if(frame_top>0) frame_top--; }

static int* var_addr(const char* name){
    /* search from top frame down */
    for(int f=frame_top;f>=0;f--){
        Frame* fr=&frames[f];
        for(int i=0;i<fr->nvar;i++)
            if(t_strcmp(fr->vars[i].name,name)==0) return &fr->vars[i].value;
    }
    return 0;
}

static void var_set(const char* name, int val){
    for(int f=frame_top;f>=0;f--){
        Frame* fr=&frames[f];
        for(int i=0;i<fr->nvar;i++)
            if(t_strcmp(fr->vars[i].name,name)==0){ fr->vars[i].value=val; return; }
    }
    /* create in current frame */
    Frame* fr=cur_frame();
    if(fr->nvar<MAX_FRAME_VARS){
        t_strcpy(fr->vars[fr->nvar].name,name);
        fr->vars[fr->nvar].value=val;
        fr->vars[fr->nvar].is_ptr=0;
        fr->vars[fr->nvar].ptr=0;
        fr->nvar++;
    }
}

/* Arrays */
static Arr arrays[MAX_ARRAYS];
static int n_arrays=0;

static Arr* arr_find(const char* name){
    for(int i=0;i<n_arrays;i++) if(t_strcmp(arrays[i].name,name)==0) return &arrays[i];
    return 0;
}
static Arr* arr_create(const char* name, int size){
    if(n_arrays>=MAX_ARRAYS) return 0;
    t_strcpy(arrays[n_arrays].name,name);
    arrays[n_arrays].data=(int*)tcc_calloc(size,sizeof(int));
    arrays[n_arrays].size=size;
    return &arrays[n_arrays++];
}

/* ===== Functions ===== */
#define MAX_FUNCS 16
typedef struct {
    char name[VARNAME];
    const char* body_start;  /* points into source */
    const char* params[8];
    char param_names[8][VARNAME];
    int  nparam;
} Func;

static Func funcs[MAX_FUNCS];
static int  n_funcs=0;

static Func* func_find(const char* name){
    for(int i=0;i<n_funcs;i++) if(t_strcmp(funcs[i].name,name)==0) return &funcs[i];
    return 0;
}

/* ===== Control flow ===== */
static int do_return=0;
static int return_val=0;
static int do_break=0;
static int do_continue=0;

/* ===== Forward declarations ===== */
static Val parse_expr(void);
static void parse_stmt(void);
static void parse_block(void);
static Val parse_assign(void);

/* ===== printf implementation ===== */
static void tcc_printf(const char* fmt, Val* args, int nargs){
    int ai=0;
    for(int i=0;fmt[i];i++){
        if(fmt[i]!='%'){ terminal_putchar(fmt[i]); continue; }
        i++;
        if(!fmt[i]) break;
        if(fmt[i]=='d'||fmt[i]=='i'){
            int v=(ai<nargs)?val_int(args[ai++]):0;
            char buf[16]; int_to_str(v,buf); terminal_write(buf);
        } else if(fmt[i]=='c'){
            int v=(ai<nargs)?val_int(args[ai++]):0;
            terminal_putchar((char)v);
        } else if(fmt[i]=='s'){
            if(ai<nargs){
                Val a=args[ai++];
                if(a.type==VAL_STR&&a.sval) terminal_write(a.sval);
                else { char buf[16]; int_to_str(val_int(a),buf); terminal_write(buf); }
            }
        } else if(fmt[i]=='p'){
            int v=(ai<nargs)?val_int(args[ai++]):0;
            terminal_write("0x"); char buf[16]; int_to_str(v,buf); terminal_write(buf);
        } else if(fmt[i]=='%'){
            terminal_putchar('%');
        } else if(fmt[i]=='x'||fmt[i]=='X'){
            int v=(ai<nargs)?val_int(args[ai++]):0;
            char buf[16]; char hex[]="0123456789abcdef";
            int pos=0; unsigned int uv=(unsigned int)v;
            if(!uv) buf[pos++]='0';
            else { char tmp[8]; int ti=0; while(uv){tmp[ti++]=hex[uv&0xF];uv>>=4;} for(int j=ti-1;j>=0;j--) buf[pos++]=tmp[j]; }
            buf[pos]=0; terminal_write(buf);
        } else {
            terminal_putchar('%'); terminal_putchar(fmt[i]);
        }
    }
}

/* ===== Expression parser ===== */
static Val parse_primary(void){
    /* sizeof */
    if(curtok.type==T_SIZEOF){
        next_tok();
        expect(T_LPAREN,"(");
        /* skip type or expr */
        int depth=1;
        while(curtok.type!=T_EOF&&depth>0){
            if(curtok.type==T_LPAREN) depth++;
            else if(curtok.type==T_RPAREN) depth--;
            if(depth>0) next_tok(); else break;
        }
        expect(T_RPAREN,")");
        return make_int(4); /* all types are 4 bytes in our system */
    }
    /* Number */
    if(curtok.type==T_NUM){ int v=curtok.ival; next_tok(); return make_int(v); }
    /* Char literal */
    if(curtok.type==T_CHAR_LIT){ int v=curtok.ival; next_tok(); return make_int(v); }
    /* String */
    if(curtok.type==T_STR){
        char* s=(char*)tcc_malloc(t_strlen(curtok.sval)+1);
        if(s) t_strcpy(s,curtok.sval); else s="";
        next_tok();
        return make_str(s);
    }
    /* Unary minus */
    if(curtok.type==T_MINUS){ next_tok(); Val v=parse_primary(); return make_int(-val_int(v)); }
    /* Unary not */
    if(curtok.type==T_NOT){ next_tok(); Val v=parse_primary(); return make_int(!val_int(v)); }
    /* Bitwise not */
    if(curtok.type==T_TILDE){ next_tok(); Val v=parse_primary(); return make_int(~val_int(v)); }
    /* Address-of */
    if(curtok.type==T_AMP){
        next_tok();
        if(curtok.type==T_IDENT){
            char name[VARNAME]; t_strcpy(name,curtok.sval); next_tok();
            int* addr=var_addr(name);
            return make_int(addr?(int)(unsigned long)addr:0);
        }
        return make_int(0);
    }
    /* Dereference */
    if(curtok.type==T_STAR){
        next_tok();
        Val v=parse_primary();
        int* p=(int*)(unsigned long)val_int(v);
        return p?make_int(*p):make_int(0);
    }
    /* Pre-increment/decrement */
    if(curtok.type==T_INC||curtok.type==T_DEC){
        int is_inc=(curtok.type==T_INC); next_tok();
        if(curtok.type==T_IDENT){
            char name[VARNAME]; t_strcpy(name,curtok.sval); next_tok();
            int* addr=var_addr(name);
            if(addr){ *addr+=is_inc?1:-1; return make_int(*addr); }
        }
        return make_int(0);
    }
    /* Parenthesized expression */
    if(curtok.type==T_LPAREN){
        /* cast? */
        next_tok();
        Val v=parse_expr();
        expect(T_RPAREN,")");
        return v;
    }
    /* Identifier */
    if(curtok.type==T_IDENT){
        char name[VARNAME]; t_strcpy(name,curtok.sval); next_tok();

        /* Function call */
        if(curtok.type==T_LPAREN){
            next_tok();
            Val args[8]; int nargs=0;
            /* collect args */
            while(curtok.type!=T_RPAREN&&curtok.type!=T_EOF&&nargs<8){
                args[nargs++]=parse_assign();
                if(curtok.type==T_COMMA) next_tok();
            }
            expect(T_RPAREN,")");

            /* Built-in functions */
            if(t_strcmp(name,"printf")==0){
                if(nargs>0){
                    const char* fmt=0;
                    if(args[0].type==VAL_STR) fmt=args[0].sval;
                    if(fmt) tcc_printf(fmt, args+1, nargs-1);
                    else { char buf[16]; int_to_str(val_int(args[0]),buf); terminal_write(buf); }
                }
                return make_int(0);
            }
            if(t_strcmp(name,"puts")==0){
                if(nargs>0&&args[0].type==VAL_STR) terminal_writeline(args[0].sval);
                return make_int(0);
            }
            if(t_strcmp(name,"putchar")==0){
                if(nargs>0) terminal_putchar((char)val_int(args[0]));
                return make_int(0);
            }
            if(t_strcmp(name,"scanf")==0){
                /* scanf("%d",&var) */
                char buf[32]; keyboard_readline(buf,32);
                if(nargs>=2&&args[1].type==VAL_INT){
                    int* p=(int*)(unsigned long)val_int(args[1]);
                    if(p) *p=str_to_int(buf);
                }
                return make_int(0);
            }
            if(t_strcmp(name,"getchar")==0){
                extern char keyboard_getchar(void);
                return make_int(keyboard_getchar());
            }
            if(t_strcmp(name,"malloc")==0){
                int sz=nargs>0?val_int(args[0]):0;
                void* p=tcc_malloc(sz);
                return make_int((int)(unsigned long)p);
            }
            if(t_strcmp(name,"calloc")==0){
                int n2=nargs>0?val_int(args[0]):0;
                int sz=nargs>1?val_int(args[1]):0;
                void* p=tcc_calloc(n2,sz);
                return make_int((int)(unsigned long)p);
            }
            if(t_strcmp(name,"free")==0){
                if(nargs>0){ void* p=(void*)(unsigned long)val_int(args[0]); tcc_free(p); }
                return make_int(0);
            }
            if(t_strcmp(name,"abs")==0){
                int v=nargs>0?val_int(args[0]):0;
                return make_int(v<0?-v:v);
            }
            if(t_strcmp(name,"strlen")==0){
                if(nargs>0&&args[0].type==VAL_STR&&args[0].sval)
                    return make_int(t_strlen(args[0].sval));
                return make_int(0);
            }
            if(t_strcmp(name,"strcmp")==0){
                if(nargs>=2&&args[0].type==VAL_STR&&args[1].type==VAL_STR)
                    return make_int(t_strcmp(args[0].sval,args[1].sval));
                return make_int(0);
            }

            /* User function call */
            Func* fn=func_find(name);
            if(fn){
                const char* saved_src=src_ptr;
                Tok saved_tok=curtok;
                int saved_line=cur_line;
                int saved_ret=do_return; do_return=0;

                frame_push();
                /* bind params */
                for(int i=0;i<fn->nparam&&i<nargs;i++)
                    var_set(fn->param_names[i], val_int(args[i]));

                src_ptr=fn->body_start;
                next_tok();
                parse_block();

                int ret=return_val; do_return=saved_ret;
                frame_pop();
                src_ptr=saved_src; curtok=saved_tok; cur_line=saved_line;
                return make_int(ret);
            }
            tcc_warn(name); /* undefined function */
            return make_int(0);
        }

        /* Array access */
        if(curtok.type==T_LBRACKET){
            next_tok();
            int idx=val_int(parse_expr());
            expect(T_RBRACKET,"]");
            Arr* a=arr_find(name);
            if(a&&idx>=0&&idx<a->size) return make_int(a->data[idx]);
            tcc_error("array out of bounds or not found",name);
            return make_int(0);
        }

        /* Post-increment/decrement */
        if(curtok.type==T_INC||curtok.type==T_DEC){
            int is_inc=(curtok.type==T_INC); next_tok();
            int* addr=var_addr(name);
            if(addr){ int old=*addr; *addr+=is_inc?1:-1; return make_int(old); }
            return make_int(0);
        }

        /* Regular variable */
        int* addr=var_addr(name);
        if(addr) return make_int(*addr);
        tcc_error("undefined variable",name);
        return make_int(0);
    }
    tcc_error("unexpected token","");
    next_tok();
    return make_int(0);
}

static Val parse_muldiv(void){
    Val v=parse_primary();
    while((curtok.type==T_STAR||curtok.type==T_SLASH||curtok.type==T_MOD)&&!had_error){
        int op=curtok.type; next_tok(); Val r=parse_primary();
        int ri=val_int(r);
        if(op==T_STAR) v=make_int(val_int(v)*ri);
        else if(op==T_SLASH) v=make_int(ri?val_int(v)/ri:0);
        else v=make_int(ri?val_int(v)%ri:0);
    }
    return v;
}
static Val parse_addsub(void){
    Val v=parse_muldiv();
    while((curtok.type==T_PLUS||curtok.type==T_MINUS)&&!had_error){
        int op=curtok.type; next_tok(); Val r=parse_muldiv();
        v=make_int(op==T_PLUS?val_int(v)+val_int(r):val_int(v)-val_int(r));
    }
    return v;
}
static Val parse_shift(void){
    Val v=parse_addsub();
    while((curtok.type==T_LSHIFT||curtok.type==T_RSHIFT)&&!had_error){
        int op=curtok.type; next_tok(); Val r=parse_addsub();
        v=make_int(op==T_LSHIFT?val_int(v)<<val_int(r):val_int(v)>>val_int(r));
    }
    return v;
}
static Val parse_cmp(void){
    Val v=parse_shift();
    while((curtok.type==T_LT||curtok.type==T_GT||curtok.type==T_LE||
           curtok.type==T_GE||curtok.type==T_EQ||curtok.type==T_NEQ)&&!had_error){
        int op=curtok.type; next_tok(); Val r=parse_shift();
        int a=val_int(v),b=val_int(r);
        if(op==T_LT) v=make_int(a<b); else if(op==T_GT) v=make_int(a>b);
        else if(op==T_LE) v=make_int(a<=b); else if(op==T_GE) v=make_int(a>=b);
        else if(op==T_EQ) v=make_int(a==b); else v=make_int(a!=b);
    }
    return v;
}
static Val parse_bitand(void){ Val v=parse_cmp(); while(curtok.type==T_AMP&&!had_error){ next_tok(); Val r=parse_cmp(); v=make_int(val_int(v)&val_int(r)); } return v; }
static Val parse_bitxor(void){ Val v=parse_bitand(); while(curtok.type==T_BITXOR&&!had_error){ next_tok(); Val r=parse_bitand(); v=make_int(val_int(v)^val_int(r)); } return v; }
static Val parse_bitor(void){ Val v=parse_bitxor(); while(curtok.type==T_BITOR&&!had_error){ next_tok(); Val r=parse_bitxor(); v=make_int(val_int(v)|val_int(r)); } return v; }
static Val parse_logand(void){ Val v=parse_bitor(); while(curtok.type==T_AND&&!had_error){ next_tok(); Val r=parse_bitor(); v=make_int(val_int(v)&&val_int(r)); } return v; }
static Val parse_logor(void){ Val v=parse_logand(); while(curtok.type==T_OR&&!had_error){ next_tok(); Val r=parse_logand(); v=make_int(val_int(v)||val_int(r)); } return v; }

static Val parse_assign(void){
    Val v=parse_logor();
    /* ternary */
    if(curtok.type==T_COLON+1){ /* ? — we'll handle it differently */
        return v;
    }
    return v;
}

static Val parse_expr(void){
    /* Check for assignment: ident = expr or ident[idx] = expr */
    const char* saved=src_ptr;
    Tok saved_tok=curtok;
    int saved_line2=cur_line;

    if(curtok.type==T_IDENT){
        char name[VARNAME]; t_strcpy(name,curtok.sval); next_tok();

        /* array assign */
        if(curtok.type==T_LBRACKET){
            next_tok();
            int idx=val_int(parse_expr());
            expect(T_RBRACKET,"]");
            if(curtok.type==T_ASSIGN){
                next_tok();
                Val rhs=parse_expr();
                Arr* a=arr_find(name);
                if(a&&idx>=0&&idx<a->size) a->data[idx]=val_int(rhs);
                else tcc_error("array out of bounds",name);
                return rhs;
            }
            /* not assignment - restore and re-parse */
            src_ptr=saved; curtok=saved_tok; cur_line=saved_line2;
            return parse_assign();
        }

        /* deref assign: *ptr = val handled in regular expr */
        if(curtok.type==T_ASSIGN){
            next_tok(); Val rhs=parse_expr();
            int* addr=var_addr(name);
            if(addr) *addr=val_int(rhs);
            else { var_set(name,val_int(rhs)); }
            return rhs;
        }
        if(curtok.type==T_PLUSEQ){ next_tok(); Val rhs=parse_expr(); int* a=var_addr(name); if(a) *a+=val_int(rhs); return make_int(a?*a:0); }
        if(curtok.type==T_MINUSEQ){ next_tok(); Val rhs=parse_expr(); int* a=var_addr(name); if(a) *a-=val_int(rhs); return make_int(a?*a:0); }
        if(curtok.type==T_STAREQ){ next_tok(); Val rhs=parse_expr(); int* a=var_addr(name); if(a) *a*=val_int(rhs); return make_int(a?*a:0); }
        if(curtok.type==T_SLASHEQ){ next_tok(); Val rhs=parse_expr(); int* a=var_addr(name); int rv=val_int(rhs); if(a&&rv) *a/=rv; return make_int(a?*a:0); }

        /* restore and parse normally */
        src_ptr=saved; curtok=saved_tok; cur_line=saved_line2;
    }
    /* dereference assign: *ptr = val */
    if(curtok.type==T_STAR){
        const char* s2=src_ptr; Tok t2=curtok; int l2=cur_line;
        next_tok();
        if(curtok.type==T_IDENT){
            char name[VARNAME]; t_strcpy(name,curtok.sval); next_tok();
            if(curtok.type==T_ASSIGN){
                next_tok(); Val rhs=parse_expr();
                int* addr=var_addr(name);
                if(addr){ int* p=(int*)(unsigned long)(*addr); if(p) *p=val_int(rhs); }
                return rhs;
            }
        }
        src_ptr=s2; curtok=t2; cur_line=l2;
    }
    return parse_assign();
}

/* ===== Statement parser ===== */
static void skip_block_or_stmt(void){
    if(curtok.type==T_LBRACE){
        int depth=1; next_tok();
        while(curtok.type!=T_EOF&&depth>0){
            if(curtok.type==T_LBRACE) depth++;
            else if(curtok.type==T_RBRACE) depth--;
            if(depth>0) next_tok(); else break;
        }
        if(curtok.type==T_RBRACE) next_tok();
    } else {
        while(curtok.type!=T_SEMI&&curtok.type!=T_EOF) next_tok();
        if(curtok.type==T_SEMI) next_tok();
    }
}

static void parse_block(void){
    if(curtok.type==T_LBRACE){
        next_tok();
        while(curtok.type!=T_RBRACE&&curtok.type!=T_EOF&&!had_error&&!do_return&&!do_break&&!do_continue)
            parse_stmt();
        if(curtok.type==T_RBRACE) next_tok();
    } else {
        parse_stmt();
    }
}

static void parse_stmt(void){
    if(curtok.type==T_EOF||had_error) return;

    /* Variable declaration: int/char x [= expr] [, y ...] ; */
    if(curtok.type==T_INT||curtok.type==T_CHAR||curtok.type==T_VOID){
        int is_ptr=0;
        next_tok();
        if(curtok.type==T_STAR){ is_ptr=1; next_tok(); }
        while(1){
            if(curtok.type!=T_IDENT){ tcc_error("expected variable name",""); return; }
            char name[VARNAME]; t_strcpy(name,curtok.sval); next_tok();
            /* array? */
            if(curtok.type==T_LBRACKET){
                next_tok();
                int sz=4;
                if(curtok.type==T_NUM){ sz=curtok.ival; next_tok(); }
                expect(T_RBRACKET,"]");
                arr_create(name,sz);
                /* optional initializer */
                if(curtok.type==T_ASSIGN){
                    next_tok();
                    if(curtok.type==T_LBRACE){
                        next_tok(); int idx2=0;
                        Arr* a=arr_find(name);
                        while(curtok.type!=T_RBRACE&&curtok.type!=T_EOF){
                            Val v=parse_expr();
                            if(a&&idx2<a->size) a->data[idx2]=val_int(v);
                            idx2++;
                            if(curtok.type==T_COMMA) next_tok();
                        }
                        expect(T_RBRACE,"}");
                    }
                }
            } else {
                int val=0;
                if(curtok.type==T_ASSIGN){ next_tok(); val=val_int(parse_expr()); }
                var_set(name, val);
                if(is_ptr){
                    /* mark as pointer - store as int */
                }
            }
            if(curtok.type!=T_COMMA) break;
            next_tok();
        }
        if(curtok.type==T_SEMI) next_tok();
        return;
    }

    /* if */
    if(curtok.type==T_IF){
        next_tok(); expect(T_LPAREN,"(");
        int cond=val_int(parse_expr());
        expect(T_RPAREN,")");
        if(cond){ parse_block(); } else { skip_block_or_stmt(); }
        if(curtok.type==T_ELSE){
            next_tok();
            if(!cond){ parse_block(); } else { skip_block_or_stmt(); }
        }
        return;
    }

    /* while */
    if(curtok.type==T_WHILE){
        next_tok(); expect(T_LPAREN,"(");
        const char* cond_src=src_ptr; Tok cond_tok=curtok; int cond_line=cur_line;
        int cond=val_int(parse_expr());
        expect(T_RPAREN,")");
        const char* body_src=src_ptr; Tok body_tok=curtok; int body_line=cur_line;
        if(!cond){ skip_block_or_stmt(); return; }
        int limit=100000;
        while(cond&&limit-->0&&!had_error&&!do_return&&!do_break){
            src_ptr=body_src; curtok=body_tok; cur_line=body_line;
            do_continue=0;
            parse_block();
            if(do_continue){ do_continue=0; }
            src_ptr=cond_src; curtok=cond_tok; cur_line=cond_line;
            cond=val_int(parse_expr());
            expect(T_RPAREN,")");
        }
        if(limit<=0) tcc_warn("while loop limit reached");
        do_break=0;
        src_ptr=body_src; curtok=body_tok; cur_line=body_line;
        skip_block_or_stmt();
        return;
    }

    /* do-while */
    if(curtok.type==T_DO){
        next_tok();
        const char* body_src=src_ptr; Tok body_tok=curtok; int body_line=cur_line;
        int limit=100000;
        do {
            src_ptr=body_src; curtok=body_tok; cur_line=body_line;
            do_continue=0;
            parse_block();
            if(do_continue) do_continue=0;
            if(curtok.type==T_WHILE) next_tok();
            expect(T_LPAREN,"(");
            int cond=val_int(parse_expr());
            expect(T_RPAREN,")");
            if(curtok.type==T_SEMI) next_tok();
            if(!cond||do_break||had_error||do_return) break;
            limit--;
        } while(limit>0);
        do_break=0;
        return;
    }

    /* for */
    if(curtok.type==T_FOR){
        next_tok(); expect(T_LPAREN,"(");
        /* init */
        if(curtok.type!=T_SEMI) parse_stmt(); else next_tok();
        const char* cond_src=src_ptr; Tok cond_tok=curtok; int cond_line=cur_line;
        int cond=1;
        if(curtok.type!=T_SEMI) cond=val_int(parse_expr());
        expect(T_SEMI,";");
        const char* inc_src=src_ptr; Tok inc_tok=curtok; int inc_line=cur_line;
        /* skip inc */
        while(curtok.type!=T_RPAREN&&curtok.type!=T_EOF) { parse_expr(); if(curtok.type==T_COMMA) next_tok(); else break; }
        expect(T_RPAREN,")");
        const char* body_src=src_ptr; Tok body_tok=curtok; int body_line=cur_line;
        if(!cond){ skip_block_or_stmt(); return; }
        int limit=100000;
        while(cond&&limit-->0&&!had_error&&!do_return&&!do_break){
            src_ptr=body_src; curtok=body_tok; cur_line=body_line;
            do_continue=0;
            parse_block();
            if(do_continue) do_continue=0;
            /* inc */
            src_ptr=inc_src; curtok=inc_tok; cur_line=inc_line;
            while(curtok.type!=T_RPAREN&&curtok.type!=T_EOF){ parse_expr(); if(curtok.type==T_COMMA) next_tok(); else break; }
            /* cond */
            src_ptr=cond_src; curtok=cond_tok; cur_line=cond_line;
            if(curtok.type!=T_SEMI) cond=val_int(parse_expr());
            else cond=1;
        }
        if(limit<=0) tcc_warn("for loop limit reached");
        do_break=0;
        src_ptr=body_src; curtok=body_tok; cur_line=body_line;
        skip_block_or_stmt();
        return;
    }

    /* switch */
    if(curtok.type==T_SWITCH){
        next_tok(); expect(T_LPAREN,"(");
        int sval=val_int(parse_expr());
        expect(T_RPAREN,")");
        expect(T_LBRACE,"{");
        int matched=0;
        while(curtok.type!=T_RBRACE&&curtok.type!=T_EOF&&!had_error&&!do_return&&!do_break){
            if(curtok.type==T_CASE){
                next_tok();
                int cv=val_int(parse_expr());
                expect(T_COLON,":");
                if(cv==sval||matched){
                    matched=1;
                    while(curtok.type!=T_CASE&&curtok.type!=T_DEFAULT&&
                          curtok.type!=T_RBRACE&&curtok.type!=T_EOF&&!do_break&&!do_return&&!had_error)
                        parse_stmt();
                } else {
                    while(curtok.type!=T_CASE&&curtok.type!=T_DEFAULT&&
                          curtok.type!=T_RBRACE&&curtok.type!=T_EOF)
                        next_tok();
                }
            } else if(curtok.type==T_DEFAULT){
                next_tok(); expect(T_COLON,":");
                if(!matched){
                    while(curtok.type!=T_RBRACE&&curtok.type!=T_EOF&&!do_break&&!do_return&&!had_error)
                        parse_stmt();
                }
            } else {
                parse_stmt();
            }
        }
        do_break=0;
        if(curtok.type==T_RBRACE) next_tok();
        return;
    }

    /* return */
    if(curtok.type==T_RETURN){
        next_tok();
        if(curtok.type!=T_SEMI) return_val=val_int(parse_expr());
        else return_val=0;
        if(curtok.type==T_SEMI) next_tok();
        do_return=1;
        return;
    }

    /* break */
    if(curtok.type==T_BREAK){ next_tok(); if(curtok.type==T_SEMI) next_tok(); do_break=1; return; }
    /* continue */
    if(curtok.type==T_CONTINUE){ next_tok(); if(curtok.type==T_SEMI) next_tok(); do_continue=1; return; }

    /* block */
    if(curtok.type==T_LBRACE){ parse_block(); return; }

    /* expression statement */
    if(curtok.type!=T_SEMI) parse_expr();
    if(curtok.type==T_SEMI) next_tok();
}

/* ===== Top-level: scan for function definitions ===== */
static void scan_functions(void){
    const char* saved=src_ptr; Tok saved_tok=curtok; int saved_line=cur_line;
    /* scan for: type name ( params ) { */
    while(curtok.type!=T_EOF&&!had_error){
        if(curtok.type==T_INT||curtok.type==T_CHAR||curtok.type==T_VOID){
            next_tok();
            if(curtok.type==T_STAR) next_tok();
            if(curtok.type!=T_IDENT){ next_tok(); continue; }
            char name[VARNAME]; t_strcpy(name,curtok.sval); next_tok();
            if(curtok.type==T_LPAREN){
                next_tok();
                /* collect param names */
                char pnames[8][VARNAME]; int np=0;
                while(curtok.type!=T_RPAREN&&curtok.type!=T_EOF){
                    if(curtok.type==T_INT||curtok.type==T_CHAR||curtok.type==T_VOID) next_tok();
                    if(curtok.type==T_STAR) next_tok();
                    if(curtok.type==T_IDENT&&np<8){ t_strcpy(pnames[np++],curtok.sval); next_tok(); }
                    if(curtok.type==T_COMMA) next_tok();
                }
                expect(T_RPAREN,")");
                if(curtok.type==T_LBRACE&&n_funcs<MAX_FUNCS){
                    /* register function */
                    t_strcpy(funcs[n_funcs].name,name);
                    funcs[n_funcs].body_start=src_ptr;
                    funcs[n_funcs].nparam=np;
                    for(int i=0;i<np;i++) t_strcpy(funcs[n_funcs].param_names[i],pnames[i]);
                    n_funcs++;
                    /* skip body */
                    int depth=1; next_tok();
                    while(curtok.type!=T_EOF&&depth>0){
                        if(curtok.type==T_LBRACE) depth++;
                        else if(curtok.type==T_RBRACE) depth--;
                        if(depth>0) next_tok(); else break;
                    }
                    if(curtok.type==T_RBRACE) next_tok();
                }
            }
        } else {
            next_tok();
        }
    }
    src_ptr=saved; curtok=saved_tok; cur_line=saved_line;
}

/* ===== Entry point ===== */
void tcc_run(const char* code){
    heap_init();
    n_arrays=0; n_funcs=0;
    frame_top=0; frames[0].nvar=0;
    do_return=0; do_break=0; do_continue=0;
    return_val=0; had_error=0; cur_line=1;

    src_ptr=code;
    next_tok();

    /* First pass: register all functions */
    scan_functions();
    had_error=0; /* reset errors from scan */

    /* Second pass: execute top-level and main() */
    src_ptr=code; next_tok(); cur_line=1;

    /* Skip function definitions, execute globals */
    while(curtok.type!=T_EOF&&!had_error){
        if(curtok.type==T_INT||curtok.type==T_CHAR||curtok.type==T_VOID){
            const char* save=src_ptr; Tok st=curtok; int sl=cur_line;
            next_tok();
            if(curtok.type==T_STAR) next_tok();
            if(curtok.type==T_IDENT){
                char name[VARNAME]; t_strcpy(name,curtok.sval); next_tok();
                if(curtok.type==T_LPAREN){
                    /* function def - skip */
                    while(curtok.type!=T_LBRACE&&curtok.type!=T_EOF) next_tok();
                    if(curtok.type==T_LBRACE){
                        int depth=1; next_tok();
                        while(depth>0&&curtok.type!=T_EOF){
                            if(curtok.type==T_LBRACE) depth++;
                            else if(curtok.type==T_RBRACE) depth--;
                            if(depth>0) next_tok(); else break;
                        }
                        if(curtok.type==T_RBRACE) next_tok();
                    }
                    continue;
                }
                /* global variable */
                src_ptr=save; curtok=st; cur_line=sl;
                parse_stmt();
                continue;
            }
            src_ptr=save; curtok=st; cur_line=sl;
            next_tok();
        } else {
            next_tok();
        }
    }

    /* Call main() */
    Func* main_fn=func_find("main");
    if(main_fn){
        do_return=0; return_val=0;
        frame_push();
        src_ptr=main_fn->body_start;
        next_tok(); cur_line=1;
        parse_block();
        frame_pop();
        terminal_setcolor(10,0);
        terminal_write("\nProcess exited with code ");
        char buf[16]; int_to_str(return_val,buf);
        terminal_writeline(buf);
        terminal_setcolor(7,0);
    } else {
        tcc_warn("no main() found");
    }
}
