#include "cinterp.h"
#include "../drivers/terminal.h"
#include "../drivers/keyboard.h"
#include <stddef.h>

/* ===== Helpers ===== */
static int ci_isspace(char c){ return c==' '||c=='\t'||c=='\n'||c=='\r'; }
static int ci_isdigit(char c){ return c>='0'&&c<='9'; }
static int ci_isalpha(char c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; }
static int ci_isalnum(char c){ return ci_isalpha(c)||ci_isdigit(c); }

static int ci_strlen(const char* s){ int i=0; while(s[i]) i++; return i; }
static int ci_strcmp(const char* a, const char* b){
    while(*a&&*b&&*a==*b){a++;b++;} return *a-*b;
}
static void ci_strcpy(char* d, const char* s){ int i=0; while((d[i]=s[i])) i++; }
static void ci_strncpy(char* d, const char* s, int n){ int i=0; for(;i<n&&s[i];i++) d[i]=s[i]; d[i]=0; }

static void int_to_str(int v, char* buf){
    if(v<0){ *buf++='-'; v=-v; }
    char tmp[16]; int i=0;
    if(v==0){ tmp[i++]='0'; }
    while(v>0){ tmp[i++]='0'+(v%10); v/=10; }
    for(int j=i-1;j>=0;j--) *buf++=tmp[j];
    *buf=0;
}
static int str_to_int(const char* s){
    int neg=0, v=0;
    if(*s=='-'){neg=1;s++;}
    while(ci_isdigit(*s)) v=v*10+(*s++)-'0';
    return neg?-v:v;
}

/* ===== Variables ===== */
#define MAX_VARS 32
#define VAR_NAME 32
typedef struct { char name[VAR_NAME]; int value; } Var;
static Var vars[MAX_VARS];
static int var_count;

static void vars_clear(void){ var_count=0; }
static int* var_get(const char* name){
    for(int i=0;i<var_count;i++)
        if(ci_strcmp(vars[i].name,name)==0) return &vars[i].value;
    return 0;
}
static void var_set(const char* name, int val){
    for(int i=0;i<var_count;i++){
        if(ci_strcmp(vars[i].name,name)==0){ vars[i].value=val; return; }
    }
    if(var_count<MAX_VARS){
        ci_strcpy(vars[var_count].name, name);
        vars[var_count].value=val;
        var_count++;
    }
}

/* ===== Tokenizer ===== */
#define TOK_NUM   1
#define TOK_IDENT 2
#define TOK_PLUS  3
#define TOK_MINUS 4
#define TOK_MUL   5
#define TOK_DIV   6
#define TOK_MOD   7
#define TOK_EQ    8   /* == */
#define TOK_NEQ   9   /* != */
#define TOK_LT   10
#define TOK_GT   11
#define TOK_LE   12
#define TOK_GE   13
#define TOK_ASSIGN 14 /* = */
#define TOK_SEMI  15
#define TOK_LPAREN 16
#define TOK_RPAREN 17
#define TOK_LBRACE 18
#define TOK_RBRACE 19
#define TOK_COMMA  20
#define TOK_STR    21
#define TOK_EOF    22
#define TOK_AND    23 /* && */
#define TOK_OR     24 /* || */

typedef struct {
    int type;
    int ival;
    char sval[128];
} Token;

static const char* src;
static Token cur;

static void skip_ws(void){
    while(*src && (ci_isspace(*src)||(src[0]=='/'&&src[1]=='/')))  {
        if(src[0]=='/'&&src[1]=='/') while(*src&&*src!='\n') src++;
        else src++;
    }
}

static void next_token(void){
    skip_ws();
    if(!*src){ cur.type=TOK_EOF; return; }
    char c=*src;

    if(c=='"'){
        src++; int i=0;
        while(*src&&*src!='"'&&i<127) cur.sval[i++]=*src++;
        cur.sval[i]=0; if(*src=='"') src++;
        cur.type=TOK_STR; return;
    }
    if(ci_isdigit(c)||( c=='-'&&ci_isdigit(src[1]) )){
        int neg=0; if(c=='-'){neg=1;src++;}
        int v=0; while(ci_isdigit(*src)) v=v*10+(*src++)-'0';
        cur.type=TOK_NUM; cur.ival=neg?-v:v; return;
    }
    if(ci_isalpha(c)){
        int i=0; while(ci_isalnum(*src)&&i<31) cur.sval[i++]=*src++;
        cur.sval[i]=0; cur.type=TOK_IDENT; return;
    }
    src++;
    switch(c){
        case '+': cur.type=TOK_PLUS; break;
        case '-': cur.type=TOK_MINUS; break;
        case '*': cur.type=TOK_MUL; break;
        case '/': cur.type=TOK_DIV; break;
        case '%': cur.type=TOK_MOD; break;
        case ';': cur.type=TOK_SEMI; break;
        case '(': cur.type=TOK_LPAREN; break;
        case ')': cur.type=TOK_RPAREN; break;
        case '{': cur.type=TOK_LBRACE; break;
        case '}': cur.type=TOK_RBRACE; break;
        case ',': cur.type=TOK_COMMA; break;
        case '<': if(*src=='='){ src++; cur.type=TOK_LE; } else cur.type=TOK_LT; break;
        case '>': if(*src=='='){ src++; cur.type=TOK_GE; } else cur.type=TOK_GT; break;
        case '=': if(*src=='='){ src++; cur.type=TOK_EQ; } else cur.type=TOK_ASSIGN; break;
        case '!': if(*src=='='){ src++; cur.type=TOK_NEQ; } break;
        case '&': if(*src=='&'){ src++; cur.type=TOK_AND; } break;
        case '|': if(*src=='|'){ src++; cur.type=TOK_OR;  } break;
        default: cur.type=TOK_EOF; break;
    }
}

/* ===== Parser / Evaluator ===== */
static int parse_expr(void);
static void parse_stmt(int execute);
static void parse_block(int execute);

static int parse_primary(void){
    if(cur.type==TOK_NUM){ int v=cur.ival; next_token(); return v; }
    if(cur.type==TOK_IDENT){
        char name[32]; ci_strcpy(name, cur.sval); next_token();
        /* function call? */
        if(cur.type==TOK_LPAREN){
            next_token(); /* skip ( */
            int arg=0;
            if(cur.type!=TOK_RPAREN) arg=parse_expr();
            if(cur.type==TOK_RPAREN) next_token();
            /* built-in functions */
            if(ci_strcmp(name,"abs")==0) return arg<0?-arg:arg;
            return 0;
        }
        int* v=var_get(name); return v?*v:0;
    }
    if(cur.type==TOK_LPAREN){
        next_token(); int v=parse_expr();
        if(cur.type==TOK_RPAREN) next_token();
        return v;
    }
    if(cur.type==TOK_MINUS){ next_token(); return -parse_primary(); }
    return 0;
}

static int parse_muldiv(void){
    int v=parse_primary();
    while(cur.type==TOK_MUL||cur.type==TOK_DIV||cur.type==TOK_MOD){
        int op=cur.type; next_token(); int r=parse_primary();
        if(op==TOK_MUL) v*=r;
        else if(op==TOK_DIV) v=(r?v/r:0);
        else v=(r?v%r:0);
    }
    return v;
}

static int parse_addsub(void){
    int v=parse_muldiv();
    while(cur.type==TOK_PLUS||cur.type==TOK_MINUS){
        int op=cur.type; next_token(); int r=parse_muldiv();
        v=(op==TOK_PLUS)?v+r:v-r;
    }
    return v;
}

static int parse_cmp(void){
    int v=parse_addsub();
    while(cur.type==TOK_LT||cur.type==TOK_GT||cur.type==TOK_LE||
          cur.type==TOK_GE||cur.type==TOK_EQ||cur.type==TOK_NEQ){
        int op=cur.type; next_token(); int r=parse_addsub();
        if(op==TOK_LT)  v=v<r;
        else if(op==TOK_GT)  v=v>r;
        else if(op==TOK_LE)  v=v<=r;
        else if(op==TOK_GE)  v=v>=r;
        else if(op==TOK_EQ)  v=v==r;
        else                  v=v!=r;
    }
    return v;
}

static int parse_expr(void){
    int v=parse_cmp();
    while(cur.type==TOK_AND||cur.type==TOK_OR){
        int op=cur.type; next_token(); int r=parse_cmp();
        v=(op==TOK_AND)?(v&&r):(v||r);
    }
    return v;
}

/* Skip a block without executing */
static void skip_block(void){
    if(cur.type==TOK_LBRACE){
        next_token(); int depth=1;
        while(*src && depth>0){
            if(cur.type==TOK_LBRACE) depth++;
            else if(cur.type==TOK_RBRACE) depth--;
            if(depth>0) next_token(); else break;
        }
        if(cur.type==TOK_RBRACE) next_token();
    } else {
        parse_stmt(0);
    }
}

static void parse_block(int execute){
    if(cur.type==TOK_LBRACE){
        next_token();
        while(cur.type!=TOK_RBRACE && cur.type!=TOK_EOF)
            parse_stmt(execute);
        if(cur.type==TOK_RBRACE) next_token();
    } else {
        parse_stmt(execute);
    }
}

static void cmd_print_val(int val){
    char buf[32]; int_to_str(val, buf);
    terminal_writeline(buf);
}

static void parse_stmt(int execute){
    if(cur.type==TOK_EOF) return;

    /* int x = expr; */
    if(cur.type==TOK_IDENT && ci_strcmp(cur.sval,"int")==0){
        next_token();
        if(cur.type!=TOK_IDENT){ terminal_writeline("error: expected variable name"); return; }
        char name[32]; ci_strcpy(name, cur.sval); next_token();
        int val=0;
        if(cur.type==TOK_ASSIGN){ next_token(); val=parse_expr(); }
        if(execute) var_set(name, val);
        if(cur.type==TOK_SEMI) next_token();
        return;
    }

    /* print(...) or print "str" or print expr */
    if(cur.type==TOK_IDENT && ci_strcmp(cur.sval,"print")==0){
        next_token();
        if(cur.type==TOK_LPAREN) next_token();
        if(cur.type==TOK_STR){
            if(execute) terminal_writeline(cur.sval);
            next_token();
        } else {
            int val=parse_expr();
            if(execute) cmd_print_val(val);
        }
        if(cur.type==TOK_RPAREN) next_token();
        if(cur.type==TOK_SEMI) next_token();
        return;
    }

    /* scan(varname) — read int from keyboard */
    if(cur.type==TOK_IDENT && ci_strcmp(cur.sval,"scan")==0){
        next_token();
        if(cur.type==TOK_LPAREN) next_token();
        char name[32]="";
        if(cur.type==TOK_IDENT){ ci_strcpy(name,cur.sval); next_token(); }
        if(cur.type==TOK_RPAREN) next_token();
        if(cur.type==TOK_SEMI) next_token();
        if(execute && name[0]){
            char buf[32]; keyboard_readline(buf,32);
            var_set(name, str_to_int(buf));
        }
        return;
    }

    /* if (cond) { } else { } */
    if(cur.type==TOK_IDENT && ci_strcmp(cur.sval,"if")==0){
        next_token();
        if(cur.type==TOK_LPAREN) next_token();
        int cond=execute?parse_expr():( parse_expr(),0 );
        if(cur.type==TOK_RPAREN) next_token();
        parse_block(execute && cond);
        if(cur.type==TOK_IDENT && ci_strcmp(cur.sval,"else")==0){
            next_token();
            parse_block(execute && !cond);
        }
        return;
    }

    /* while (cond) { } */
    if(cur.type==TOK_IDENT && ci_strcmp(cur.sval,"while")==0){
        next_token();
        if(cur.type==TOK_LPAREN) next_token();
        const char* cond_start=src-1; /* save position before expr */
        /* We need to re-evaluate cond each loop — save src position */
        /* Simple approach: save src pointer before condition */
        Token saved_cur=cur;
        const char* saved_src=src;
        int cond=execute?parse_expr():0;
        if(cur.type==TOK_RPAREN) next_token();
        const char* body_start=src;
        Token body_cur=cur;
        (void)cond_start; (void)saved_cur; (void)saved_src;

        if(!execute){ skip_block(); return; }

        int limit=10000; /* prevent infinite loop */
        while(cond && limit-->0){
            src=body_start; cur=body_cur;
            parse_block(1);
            /* re-evaluate condition */
            src=saved_src; cur=saved_cur;
            cond=parse_expr();
            if(cur.type==TOK_RPAREN) next_token();
        }
        /* skip past body */
        src=body_start; cur=body_cur;
        skip_block();
        return;
    }

    /* for (init; cond; inc) { } */
    if(cur.type==TOK_IDENT && ci_strcmp(cur.sval,"for")==0){
        next_token();
        if(cur.type==TOK_LPAREN) next_token();
        /* init */
        parse_stmt(execute);
        /* cond */
        const char* cond_src=src; Token cond_tok=cur;
        int cond=execute?parse_expr():0;
        if(cur.type==TOK_SEMI) next_token();
        /* inc */
        const char* inc_src=src; Token inc_tok=cur;
        /* skip inc expression */
        parse_expr(); /* just to advance */
        if(cur.type==TOK_RPAREN) next_token();
        const char* body_src=src; Token body_tok=cur;

        if(!execute){ skip_block(); return; }

        int limit=10000;
        while(cond && limit-->0){
            src=body_src; cur=body_tok;
            parse_block(1);
            /* inc */
            src=inc_src; cur=inc_tok;
            parse_expr();
            /* cond */
            src=cond_src; cur=cond_tok;
            cond=parse_expr();
            if(cur.type==TOK_SEMI) next_token();
        }
        src=body_src; cur=body_tok;
        skip_block();
        return;
    }

    /* assignment: x = expr; or x += expr; */
    if(cur.type==TOK_IDENT){
        char name[32]; ci_strcpy(name, cur.sval); next_token();
        if(cur.type==TOK_ASSIGN){
            next_token(); int val=parse_expr();
            if(execute) var_set(name, val);
            if(cur.type==TOK_SEMI) next_token();
            return;
        }
        /* expression statement (function call etc) */
        if(cur.type==TOK_SEMI){ next_token(); return; }
        /* skip */
        if(cur.type==TOK_SEMI) next_token();
        return;
    }

    /* skip unknown */
    next_token();
}

void cinterp_run(const char* code){
    vars_clear();
    src=code;
    next_token();
    while(cur.type!=TOK_EOF)
        parse_stmt(1);
}
