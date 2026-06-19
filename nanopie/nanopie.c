// nanopie - a tiny python interpreter
// fast startup, simple programs only. tree-walking evaluator.
// supports: ints, strings, f-strings ({x} and {x=}), variables,
// def/return, if/elif/else, while, for-in-range, + - * / // %,
// comparisons, print(), and `import dotted.name` to load .py files.
//
// build: cc -O2 -Wall -o nanopie nanopie.c
// run:   ./nanopie                        # repl
//        ./nanopie examples/fib.py        # run file, exit
//        ./nanopie -i examples/fib.py     # run file then repl (like python -i)
//        ./nanopie -c 'print(1+2)'        # run a command string
//        ./nanopie -m examples.fib        # run a module by dotted name
//        ./nanopie -                      # read program from stdin
//        ./nanopie --print-ast examples/fib.py           # dump AST as s-exprs
//        ./nanopie --print-ast-and-imports examples/fib.py  # also dump imports

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <setjmp.h>
#include <stdarg.h>

/* ---------- helpers ---------- */

static void *xmalloc(size_t n){ void*p=malloc(n); if(!p){ fprintf(stderr,"oom"); fputc(10,stderr); exit(1); } return p; }
static void *xrealloc(void*p,size_t n){ void*q=realloc(p,n); if(!q){ fprintf(stderr,"oom"); fputc(10,stderr); exit(1); } return q; }
static char *sclone(const char*s){ if(!s) return NULL; size_t n=strlen(s)+1; char*c=xmalloc(n); memcpy(c,s,n); return c; }
static char *sclone_n(const char*s, size_t n){ char*c=xmalloc(n+1); memcpy(c,s,n); c[n]=0; return c; }

static jmp_buf g_jmp;
static int g_can_longjmp = 0;

static void die(const char*fmt, ...){
    va_list ap; va_start(ap,fmt);
    fprintf(stderr,"nanopie: ");
    vfprintf(stderr,fmt,ap);
    fputc(10,stderr);
    va_end(ap);
    if(g_can_longjmp) longjmp(g_jmp,1);
    exit(1);
}

/* dynamic string builder */
typedef struct { char *data; size_t len, cap; } SB;
static void sb_init(SB*s){ s->data=NULL; s->len=s->cap=0; }
static void sb_grow(SB*s, size_t need){
    if(s->len+need+1 <= s->cap) return;
    size_t nc = s->cap? s->cap*2 : 16;
    while(nc < s->len+need+1) nc*=2;
    s->data = xrealloc(s->data, nc);
    s->cap = nc;
}
static void sb_appendn(SB*s, const char*p, size_t n){ sb_grow(s,n); memcpy(s->data+s->len,p,n); s->len+=n; s->data[s->len]=0; }
static void sb_append(SB*s, const char*p){ sb_appendn(s,p,strlen(p)); }
static void sb_appendc(SB*s, char c){ sb_grow(s,1); s->data[s->len++]=c; s->data[s->len]=0; }
static char *sb_str(SB*s){ if(!s->data){ char*r=xmalloc(1); r[0]=0; return r; } return sclone(s->data); }

static void rstrip(char*s){ size_t n=strlen(s); while(n>0 && (s[n-1]==' '||s[n-1]=='\t')) s[--n]=0; }
static void strip(char*s){ char*p=s; while(*p==' '||*p=='\t') p++; if(p!=s) memmove(s,p,strlen(p)+1); rstrip(s); }

/* ---------- values ---------- */

enum { V_INT, V_STR, V_NONE };
typedef struct { int type; long i; char *s; } Value;

static Value v_int(long i){ Value v; v.type=V_INT; v.i=i; v.s=NULL; return v; }
static Value v_str(char*s){ Value v; v.type=V_STR; v.i=0; v.s=s; return v; }
static Value v_none(void){ Value v; v.type=V_NONE; v.i=0; v.s=NULL; return v; }

static char *val_to_str(Value v){
    if(v.type==V_INT){ char buf[32]; sprintf(buf,"%ld",v.i); return sclone(buf); }
    if(v.type==V_STR) return sclone(v.s?v.s:"");
    return sclone("None");
}

static int truthy(Value v){
    if(v.type==V_INT) return v.i != 0;
    if(v.type==V_STR) return v.s && v.s[0] != 0;
    return 0;
}

/* ---------- environment ---------- */

typedef struct Binding { char *name; Value val; struct Binding *next; } Binding;
typedef struct { Binding *head; } Env;

static Binding* env_find(Env*e,const char*n){ for(Binding*b=e->head;b;b=b->next) if(strcmp(b->name,n)==0) return b; return NULL; }
static void env_set(Env*e,const char*n,Value v){ Binding*b=env_find(e,n); if(b){b->val=v;} else { b=xmalloc(sizeof *b); b->name=sclone(n); b->val=v; b->next=e->head; e->head=b; } }
static Value env_get(Env*e,const char*n){ Binding*b=env_find(e,n); if(!b) die("name '%s' is not defined", n); return b->val; }

/* ---------- ast ---------- */

typedef struct Node Node;
struct Node {
    int type;
    long ival;
    char *sval;
    char *name;
    char op[8];
    Node *a, *b;
    Node **list; int nlist;
    int debug;      /* fstring part debug flag */
    char *src;      /* fstring debug prefix (e.g. "i=") */
};

enum {
    N_NUM, N_STR, N_NAME, N_FSTR, N_BINOP, N_UNARY, N_COMPARE,
    N_ASSIGN, N_IF, N_WHILE, N_FOR, N_FUNCDEF, N_RETURN,
    N_EXPRSTMT, N_CALL, N_BLOCK, N_PASS, N_IMPORT
};

static Node* new_node(int t){ Node*n=xmalloc(sizeof *n); memset(n,0,sizeof *n); n->type=t; return n; }
static void node_push(Node*parent, Node*child){ parent->list=xrealloc(parent->list,(parent->nlist+1)*sizeof(Node*)); parent->list[parent->nlist++]=child; }

/* ---------- functions ---------- */

typedef struct Func { char *name; char **params; int nparams; Node *body; struct Func *next; } Func;
static Func *funcs = NULL;
static Func* func_find(const char*n){ for(Func*f=funcs;f;f=f->next) if(strcmp(f->name,n)==0) return f; return NULL; }

/* ---------- lexer ---------- */

enum { TK_NEWLINE, TK_INDENT, TK_DEDENT, TK_NAME, TK_NUMBER, TK_STRING, TK_FSTR, TK_OP, TK_EOF };
typedef struct { int kind; char *text; long ival; char *sval; } Token;

typedef struct {
    const char *src;
    size_t pos, len;
    int no_indent;
    int bracket_depth;
    int indents[64]; int nind;
    int at_line_start;
    int line_has_content;
    Token *queue; int qhead, qcount, qcap;
    Token cur;
} Lexer;

static char peek(Lexer*L){ return L->pos < L->len ? L->src[L->pos] : 0; }
static char peek2(Lexer*L){ return L->pos+1 < L->len ? L->src[L->pos+1] : 0; }
static char peek3(Lexer*L){ return L->pos+2 < L->len ? L->src[L->pos+2] : 0; }
static void adv(Lexer*L){ if(L->pos < L->len) L->pos++; }

static void q_push(Lexer*L, int kind, const char*text, const char*sval, long ival){
    if(L->qhead >= L->qcount){ L->qhead=0; L->qcount=0; } /* compact when drained */
    if(L->qcount >= L->qcap){ L->qcap = L->qcap? L->qcap*2 : 16; L->queue=xrealloc(L->queue, L->qcap*sizeof(Token)); }
    Token *t = &L->queue[L->qcount++];
    t->kind=kind; t->text=text?sclone(text):NULL; t->sval=sval?sclone(sval):NULL; t->ival=ival;
}

static size_t count_indent(Lexer*L){
    size_t col=0;
    for(;;){
        char c=peek(L);
        if(c==' '){ adv(L); col++; }
        else if(c=='\t'){ adv(L); col = (col/8+1)*8; }
        else break;
    }
    return col;
}

static Token read_string(Lexer*L, int is_f);

static Token read_token(Lexer*L){
    Token t; memset(&t,0,sizeof t);
    char c = peek(L);
    if((c=='f'||c=='F') && (peek2(L)=='\''||peek2(L)=='"')) return read_string(L,1);
    if(isalpha((unsigned char)c)||c=='_'){
        size_t start=L->pos;
        while(isalnum((unsigned char)peek(L))||peek(L)=='_') adv(L);
        t.kind=TK_NAME; t.text=sclone_n(L->src+start, L->pos-start); return t;
    }
    if(isdigit((unsigned char)c)){
        size_t start=L->pos;
        while(isdigit((unsigned char)peek(L))) adv(L);
        char *tmp=sclone_n(L->src+start, L->pos-start);
        t.kind=TK_NUMBER; t.ival=atol(tmp); free(tmp); return t;
    }
    if(c=='\''||c=='"') return read_string(L,0);
    /* operators, longest first */
    char buf[3]={0,0,0};
    buf[0]=peek(L); buf[1]=peek2(L);
    if(!strcmp(buf,"==")||!strcmp(buf,"!=")||!strcmp(buf,"<=")||!strcmp(buf,">=")||!strcmp(buf,"//")||!strcmp(buf,"->")){
        adv(L); adv(L); t.kind=TK_OP; t.text=sclone(buf); return t;
    }
    const char *single="+-*/%()[]{}:,=<>.;";
    if(strchr(single,c)){ adv(L); buf[1]=0; t.kind=TK_OP; t.text=sclone(buf); return t; }
    die("unexpected character '%c' (0x%02x)", c? c:'?', (unsigned char)c);
    return t; /* unreachable */
}

static Token read_string(Lexer*L, int is_f){
    if(is_f) adv(L); /* consume f/F */
    char q = peek(L);
    if(q!='\'' && q!='"') die("expected string");
    adv(L);
    int triple = (peek(L)==q && peek2(L)==q);
    if(triple){ adv(L); adv(L); }
    SB sb; sb_init(&sb);
    for(;;){
        char c2=peek(L);
        if(c2==0) die("unterminated string");
        if(triple){
            if(c2==q && peek2(L)==q && peek3(L)==q){ adv(L);adv(L);adv(L); break; }
        } else {
            if(c2==q){ adv(L); break; }
            if(c2=='\n') die("unterminated string (newline)");
        }
        if(c2=='\\'){
            adv(L); char e=peek(L); adv(L);
            char out=e;
            switch(e){
                case 'n': out=10;  break;  /* \n */
                case 't': out=9;   break;  /* \t */
                case 'r': out=13;  break;  /* \r */
                case 92: out=92;   break;  /* \\ */
                case 39: out=39;   break;  /* \' */
                case 34: out=34;   break;  /* \" */
                case '0': out=0;   break;  /* \0 */
                default: out=e;    break;
            }
            sb_appendc(&sb, out);
        } else {
            sb_appendc(&sb, c2); adv(L);
        }
    }
    Token t; memset(&t,0,sizeof t);
    if(is_f){ t.kind=TK_FSTR; t.sval=sb_str(&sb); }
    else    { t.kind=TK_STRING; t.sval=sb_str(&sb); }
    return t;
}

static Token lex_next_token(Lexer*L){
    for(;;){
        if(L->qhead < L->qcount) return L->queue[L->qhead++];
        if(L->at_line_start && L->bracket_depth==0 && !L->no_indent){
            for(;;){
                size_t col = count_indent(L);
                char c = peek(L);
                if(c=='\n'){ adv(L); continue; }              /* blank line */
                if(c=='#'){ while(peek(L)&&peek(L)!='\n') adv(L); continue; } /* comment-only */
                if(c==0){                                     /* EOF at line start */
                    if(L->line_has_content){ q_push(L,TK_NEWLINE,NULL,NULL,0); L->line_has_content=0; }
                    while(L->nind>1){ L->nind--; q_push(L,TK_DEDENT,NULL,NULL,0); }
                    q_push(L,TK_EOF,NULL,NULL,0);
                    break;
                }
                if(col > L->indents[L->nind-1]){
                    L->indents[L->nind++] = (int)col;
                    q_push(L,TK_INDENT,NULL,NULL,0);
                } else if(col < L->indents[L->nind-1]){
                    while(L->nind>1 && col < L->indents[L->nind-1]){
                        L->nind--;
                        q_push(L,TK_DEDENT,NULL,NULL,0);
                    }
                    if(col != L->indents[L->nind-1]) die("indentation error");
                }
                L->at_line_start = 0;
                L->line_has_content = 0;
                break;
            }
            continue;
        }
        /* skip spaces (and newlines when no_indent) */
        char c = peek(L);
        while(c==' '||c=='\t'||(L->no_indent && c=='\n')){ adv(L); c=peek(L); }
        if(c==0){
            if(!L->no_indent){
                if(L->line_has_content){ q_push(L,TK_NEWLINE,NULL,NULL,0); L->line_has_content=0; }
                while(L->nind>1){ L->nind--; q_push(L,TK_DEDENT,NULL,NULL,0); }
            }
            q_push(L,TK_EOF,NULL,NULL,0);
            return L->queue[L->qhead++];
        }
        if(c=='\n'){
            adv(L);
            if(L->bracket_depth>0) continue;
            if(L->no_indent) continue;
            if(L->line_has_content){ q_push(L,TK_NEWLINE,NULL,NULL,0); L->line_has_content=0; }
            L->at_line_start = 1;
            continue;
        }
        if(c=='#'){
            while(peek(L)&&peek(L)!='\n') adv(L);
            continue;
        }
        Token t = read_token(L);
        L->line_has_content = 1;
        q_push(L, t.kind, t.text, t.sval, t.ival);
        if(t.kind==TK_OP){
            if(!strcmp(t.text,"(")||!strcmp(t.text,"[")||!strcmp(t.text,"{")) L->bracket_depth++;
            else if(!strcmp(t.text,")")||!strcmp(t.text,"]")||!strcmp(t.text,"}")){ if(L->bracket_depth>0) L->bracket_depth--; }
        }
        return L->queue[L->qhead++];
    }
}

static void lex_init(Lexer*L, const char*src, int no_indent){
    memset(L,0,sizeof *L);
    L->src=src; L->len=strlen(src);
    L->no_indent=no_indent;
    L->nind=1; L->indents[0]=0;
    L->at_line_start=1;
}
static void lex_advance(Lexer*L){ L->cur = lex_next_token(L); }

static int is_op(Lexer*L, const char*s){ return L->cur.kind==TK_OP && strcmp(L->cur.text,s)==0; }
static int is_kw(Lexer*L, const char*s){ return L->cur.kind==TK_NAME && strcmp(L->cur.text,s)==0; }
static void expect_op(Lexer*L, const char*s){ if(!is_op(L,s)) die("expected '%s'", s); lex_advance(L); }
static void expect_kw(Lexer*L, const char*s){ if(!is_kw(L,s)) die("expected '%s'", s); lex_advance(L); }
static void expect_kind(Lexer*L, int k, const char*what){ if(L->cur.kind!=k) die("expected %s", what); lex_advance(L); }

/* ---------- parser ---------- */

static Node *parse_expr(Lexer*L);
static Node *parse_stmt(Lexer*L);
static Node *parse_import(Lexer*L);

static Node *make_str(const char*s){ Node*n=new_node(N_STR); n->sval=sclone(s); return n; }

static Node *parse_subexpr(const char *s){
    Lexer sub; lex_init(&sub, s, 1); lex_advance(&sub);
    Node *e = parse_expr(&sub);
    if(sub.cur.kind != TK_EOF) die("trailing tokens in fstring expression");
    return e;
}

static Node *parse_fstring(const char *content){
    Node *node = new_node(N_FSTR);
    SB lit; sb_init(&lit);
    size_t i=0, n=strlen(content);
    while(i<n){
        char c = content[i];
        if(c=='{'){
            if(i+1<n && content[i+1]=='{'){ sb_appendc(&lit,'{'); i+=2; continue; }
            size_t j=i+1;
            while(j<n && content[j]!='}') j++;
            if(j>=n) die("unterminated fstring field");
            char *raw = sclone_n(content+i+1, j-(i+1));
            char *colon = strchr(raw, ':');
            if(colon){ *colon=0; }  /* drop format spec */
            rstrip(raw);
            int debug=0; char *prefix=NULL; char *parse_src=raw;
            size_t rl=strlen(raw);
            if(rl>0 && raw[rl-1]=='='){
                char prev = (rl>=2)? raw[rl-2] : 0;
                if(prev!='=' && prev!='!' && prev!='<' && prev!='>' && prev!=':' && prev!='='){
                    debug=1; prefix=sclone(raw);          /* e.g. "i=" or " i = " */
                    parse_src = sclone_n(raw, rl-1);
                    strip(parse_src);
                }
            } else {
                parse_src = sclone(raw);
                strip(parse_src);
            }
            if(lit.len>0){ node_push(node, make_str(sb_str(&lit))); sb_init(&lit); }
            Node *en = parse_subexpr(parse_src);
            en->debug = debug;
            en->src = prefix;
            node_push(node, en);
            i = j+1;
            continue;
        }
        if(c=='}'){
            if(i+1<n && content[i+1]=='}'){ sb_appendc(&lit,'}'); i+=2; continue; }
            die("single '}' in fstring");
        }
        sb_appendc(&lit, c); i++;
    }
    if(lit.len>0) node_push(node, make_str(sb_str(&lit)));
    return node;
}

static Node *parse_atom(Lexer*L){
    if(L->cur.kind==TK_NUMBER){ Node*n=new_node(N_NUM); n->ival=L->cur.ival; lex_advance(L); return n; }
    if(L->cur.kind==TK_STRING){ Node*n=new_node(N_STR); n->sval=sclone(L->cur.sval); lex_advance(L); return n; }
    if(L->cur.kind==TK_FSTR){ Node*n=parse_fstring(L->cur.sval); lex_advance(L); return n; }
    if(L->cur.kind==TK_NAME){ Node*n=new_node(N_NAME); n->name=sclone(L->cur.text); lex_advance(L); return n; }
    if(is_op(L,"(")){ lex_advance(L); Node*e=parse_expr(L); expect_op(L,")"); return e; }
    die("unexpected token in expression");
    return NULL;
}

static Node *parse_call(Lexer*L){
    Node *a = parse_atom(L);
    while(is_op(L,"(")){
        lex_advance(L);
        Node *call = new_node(N_CALL);
        call->a = a;
        if(!is_op(L,")")){
            node_push(call, parse_expr(L));
            while(is_op(L,",")){ lex_advance(L); if(is_op(L,")")) break; node_push(call, parse_expr(L)); }
        }
        expect_op(L,")");
        a = call;
    }
    return a;
}

static Node *parse_unary(Lexer*L){
    if(is_op(L,"-")){ lex_advance(L); Node*u=new_node(N_UNARY); strcpy(u->op,"-"); u->a=parse_unary(L); return u; }
    if(is_op(L,"+")){ lex_advance(L); return parse_unary(L); }
    return parse_call(L);
}

static Node *parse_mul(Lexer*L){
    Node *l = parse_unary(L);
    while(is_op(L,"*")||is_op(L,"/")||is_op(L,"//")||is_op(L,"%")){
        char op[8]; strcpy(op, L->cur.text); lex_advance(L);
        Node *r = parse_unary(L);
        Node *n = new_node(N_BINOP); strcpy(n->op, op); n->a=l; n->b=r; l=n;
    }
    return l;
}

static Node *parse_add(Lexer*L){
    Node *l = parse_mul(L);
    while(is_op(L,"+")||is_op(L,"-")){
        char op[8]; strcpy(op, L->cur.text); lex_advance(L);
        Node *r = parse_mul(L);
        Node *n = new_node(N_BINOP); strcpy(n->op, op); n->a=l; n->b=r; l=n;
    }
    return l;
}

static Node *parse_cmp(Lexer*L){
    Node *l = parse_add(L);
    if(is_op(L,"<")||is_op(L,">")||is_op(L,"<=")||is_op(L,">=")||is_op(L,"==")||is_op(L,"!=")){
        char op[8]; strcpy(op, L->cur.text); lex_advance(L);
        Node *r = parse_add(L);
        Node *n = new_node(N_COMPARE); strcpy(n->op, op); n->a=l; n->b=r; return n;
    }
    return l;
}

static Node *parse_expr(Lexer*L){ return parse_cmp(L); }

static Node *parse_block(Lexer*L);

static Node *parse_if_body(Lexer*L){
    Node *test = parse_expr(L);
    expect_op(L,":");
    Node *then = parse_block(L);
    Node *node = new_node(N_IF);
    node->a = test; node->b = then;
    if(is_kw(L,"elif")){
        lex_advance(L);
        node_push(node, parse_if_body(L));
    } else if(is_kw(L,"else")){
        lex_advance(L);
        expect_op(L,":");
        node_push(node, parse_block(L));
    }
    return node;
}

static Node *parse_simple_stmt(Lexer*L){
    if(is_kw(L,"return")){
        lex_advance(L);
        Node *n = new_node(N_RETURN);
        if(L->cur.kind!=TK_NEWLINE && L->cur.kind!=TK_EOF && L->cur.kind!=TK_DEDENT && !is_op(L,";"))
            n->a = parse_expr(L);
        return n;
    }
    if(is_kw(L,"import")) return parse_import(L);
    if(is_kw(L,"pass")){ lex_advance(L); return new_node(N_PASS); }
    if(is_kw(L,"elif")||is_kw(L,"else")) die("unexpected '%s'", L->cur.text);
    Node *e = parse_expr(L);
    if(is_op(L,"=")){
        lex_advance(L);
        Node *val = parse_expr(L);
        if(e->type != N_NAME) die("bad assignment target");
        Node *n = new_node(N_ASSIGN); n->a=e; n->b=val; return n;
    }
    Node *n = new_node(N_EXPRSTMT); n->a=e; return n;
}

static Node *parse_simple_stmts(Lexer*L){
    Node *first = parse_simple_stmt(L);
    if(!is_op(L,";")) return first;
    Node *blk = new_node(N_BLOCK);
    node_push(blk, first);
    while(is_op(L,";")){
        lex_advance(L);
        if(L->cur.kind==TK_NEWLINE || L->cur.kind==TK_EOF) break;  /* trailing ';' */
        node_push(blk, parse_simple_stmt(L));
    }
    return blk;
}

static Node *parse_import(Lexer*L){
    lex_advance(L); /* 'import' */
    SB sb; sb_init(&sb);
    if(L->cur.kind!=TK_NAME) die("expected module name after import");
    sb_append(&sb, L->cur.text); lex_advance(L);
    while(is_op(L,".")){
        lex_advance(L);
        if(L->cur.kind!=TK_NAME) die("expected name after '.'");
        sb_append(&sb, ".");
        sb_append(&sb, L->cur.text);
        lex_advance(L);
    }
    Node *n = new_node(N_IMPORT);
    n->name = sb_str(&sb);
    return n;
}

static Node *parse_funcdef(Lexer*L){
    lex_advance(L); /* 'def' */
    if(L->cur.kind!=TK_NAME) die("expected function name");
    Node *n = new_node(N_FUNCDEF);
    n->name = sclone(L->cur.text);
    lex_advance(L);
    expect_op(L,"(");
    if(!is_op(L,")")){
        for(;;){
            if(L->cur.kind!=TK_NAME) die("expected parameter name");
            Node *p = new_node(N_NAME); p->name=sclone(L->cur.text);
            node_push(n, p);
            lex_advance(L);
            if(is_op(L,",")){ lex_advance(L); continue; }
            break;
        }
    }
    expect_op(L,")");
    expect_op(L,":");
    n->b = parse_block(L);
    return n;
}

static Node *parse_stmt(Lexer*L){
    if(is_kw(L,"def")) return parse_funcdef(L);
    if(is_kw(L,"if")){ lex_advance(L); return parse_if_body(L); }
    if(is_kw(L,"while")){
        lex_advance(L);
        Node *test=parse_expr(L); expect_op(L,":");
        Node *n=new_node(N_WHILE); n->a=test; n->b=parse_block(L); return n;
    }
    if(is_kw(L,"for")){
        lex_advance(L);
        if(L->cur.kind!=TK_NAME) die("expected loop variable");
        Node *n=new_node(N_FOR);
        n->name=sclone(L->cur.text); lex_advance(L);
        expect_kw(L,"in");
        n->a=parse_expr(L); expect_op(L,":");
        n->b=parse_block(L);
        return n;
    }
    return parse_simple_stmts(L);
}

static Node *parse_block(Lexer*L){
    Node *blk = new_node(N_BLOCK);
    if(is_op(L,":")||L->cur.kind==TK_NEWLINE){
        /* shouldn't hit ':' here; expect NEWLINE then INDENT */
    }
    if(L->cur.kind==TK_NEWLINE){
        lex_advance(L);
        expect_kind(L,TK_INDENT,"indent");
        while(L->cur.kind != TK_DEDENT){
            if(L->cur.kind==TK_NEWLINE){ lex_advance(L); continue; }
            if(L->cur.kind==TK_EOF) die("unexpected EOF in block");
            node_push(blk, parse_stmt(L));
        }
        expect_kind(L,TK_DEDENT,"dedent");
        return blk;
    }
    /* inline single simple statement */
    node_push(blk, parse_simple_stmts(L));
    return blk;
}

/* parse an entire source string into a block of top-level statements */
static Node *parse_source(const char *code){
    Lexer L; lex_init(&L, code, 0); lex_advance(&L);
    Node *blk = new_node(N_BLOCK);
    while(L.cur.kind != TK_EOF){
        if(L.cur.kind==TK_NEWLINE){ lex_advance(&L); continue; }
        node_push(blk, parse_stmt(&L));
    }
    return blk;
}

/* ---------- ast printer (s-expressions) ---------- */

static void print_indent(int n){ for(int i=0;i<n;i++) fputs("  ", stdout); }

static void print_str_escaped(const char *s){
    fputc('"', stdout);
    if(!s){ fputc('"', stdout); return; }
    for(const char*p=s; *p; p++){
        unsigned char c = (unsigned char)*p;
        if(c=='"' || c=='\\'){ fputc('\\', stdout); fputc(c, stdout); }
        else if(c=='\n'){ fputs("\\n", stdout); }
        else if(c=='\t'){ fputs("\\t", stdout); }
        else if(c=='\r'){ fputs("\\r", stdout); }
        else if(c < 0x20){ printf("\\x%02x", c); }
        else fputc(c, stdout);
    }
    fputc('"', stdout);
}

static void print_ast(Node *n, int ind){
    if(!n){ fputs("nil", stdout); return; }
    switch(n->type){
    case N_NUM:  printf("%ld", n->ival); break;
    case N_STR:  print_str_escaped(n->sval); break;
    case N_NAME: fputs(n->name, stdout); break;
    case N_FSTR:
        fputs("(fstr", stdout);
        for(int i=0;i<n->nlist;i++){
            Node *p = n->list[i];
            fputc(' ', stdout);
            if(p->type==N_STR) print_str_escaped(p->sval);
            else if(p->debug){
                fputs("(debug ", stdout); print_str_escaped(p->src);
                fputc(' ', stdout); print_ast(p, ind); fputc(')', stdout);
            } else print_ast(p, ind);
        }
        fputc(')', stdout);
        break;
    case N_BINOP:
        printf("(%s ", n->op); print_ast(n->a, ind);
        fputc(' ', stdout); print_ast(n->b, ind); fputc(')', stdout);
        break;
    case N_UNARY:
        printf("(%s ", n->op); print_ast(n->a, ind); fputc(')', stdout);
        break;
    case N_COMPARE:
        printf("(%s ", n->op); print_ast(n->a, ind);
        fputc(' ', stdout); print_ast(n->b, ind); fputc(')', stdout);
        break;
    case N_ASSIGN:
        fputs("(= ", stdout); print_ast(n->a, ind);
        fputc(' ', stdout); print_ast(n->b, ind); fputc(')', stdout);
        break;
    case N_EXPRSTMT:
        print_ast(n->a, ind); break;
    case N_PASS:
        fputs("(pass)", stdout); break;
    case N_RETURN:
        if(n->a){ fputs("(return ", stdout); print_ast(n->a, ind); fputc(')', stdout); }
        else fputs("(return)", stdout);
        break;
    case N_CALL:
        fputs("(call ", stdout); print_ast(n->a, ind);
        for(int i=0;i<n->nlist;i++){ fputc(' ', stdout); print_ast(n->list[i], ind); }
        fputc(')', stdout);
        break;
    case N_IMPORT:
        printf("(import %s)", n->name); break;
    case N_IF:
        fputs("(if ", stdout); print_ast(n->a, ind); fputc(' ', stdout); print_ast(n->b, ind);
        if(n->nlist>0){ fputc(' ', stdout); print_ast(n->list[0], ind); }
        fputc(')', stdout);
        break;
    case N_WHILE:
        fputs("(while ", stdout); print_ast(n->a, ind); fputc(' ', stdout); print_ast(n->b, ind); fputc(')', stdout);
        break;
    case N_FOR:
        printf("(for %s ", n->name); print_ast(n->a, ind);
        fputc(' ', stdout); print_ast(n->b, ind); fputc(')', stdout);
        break;
    case N_FUNCDEF:
        fputs("(def ", stdout); fputs(n->name, stdout); fputs(" (", stdout);
        for(int i=0;i<n->nlist;i++){ if(i>0) fputc(' ', stdout); fputs(n->list[i]->name, stdout); }
        fputs(") ", stdout); print_ast(n->b, ind); fputc(')', stdout);
        break;
    case N_BLOCK:
        fputs("(block", stdout);
        for(int i=0;i<n->nlist;i++){
            fputs("\n", stdout); print_indent(ind+1); print_ast(n->list[i], ind+1);
        }
        fputc(')', stdout);
        break;
    default:
        fputs("(?)", stdout); break;
    }
}

static void print_program(Node *blk){
    for(int i=0;i<blk->nlist;i++){
        if(i>0) fputc('\n', stdout);
        print_ast(blk->list[i], 0);
    }
    fputc('\n', stdout);
}

/* ---------- evaluator ---------- */

static int g_returning = 0;
static Value g_retval;

static int eval_block(Node *blk, Env *e);
static Value eval(Node *n, Env *e);
static void load_module(const char *mod, Env *env);

static long py_floordiv(long a, long b){
    if(b==0) die("integer division by zero");
    long q = a / b;
    if((a % b != 0) && ((a<0) != (b<0))) q--;
    return q;
}
static long py_mod(long a, long b){
    if(b==0) die("modulo by zero");
    return a - py_floordiv(a,b)*b;
}

static long eval_int(Node *n, Env *e){
    Value v = eval(n, e);
    if(v.type != V_INT) die("expected integer");
    return v.i;
}

static Value call_user_func(Func *f, Value *args, int nargs){
    if(nargs != f->nparams) die("'%s' expects %d args, got %d", f->name, f->nparams, nargs);
    Env local; local.head = NULL;
    for(int i=0;i<nargs;i++) env_set(&local, f->params[i], args[i]);
    int saved_ret = g_returning; Value saved_val = g_retval;
    g_returning = 0;
    eval_block(f->body, &local);
    Value r = g_returning ? g_retval : v_none();
    g_returning = saved_ret; g_retval = saved_val;
    return r;
}

static void print_args(Node **args, int n, Env *e){
    for(int i=0;i<n;i++){
        if(i>0) fputc(' ', stdout);
        Value v = eval(args[i], e);
        char *s = val_to_str(v);
        fputs(s, stdout);
    }
    fputc(10, stdout);
}

static Value eval(Node *n, Env *e){
    switch(n->type){
    case N_NUM: return v_int(n->ival);
    case N_STR: return v_str(sclone(n->sval));
    case N_NAME: return env_get(e, n->name);
    case N_FSTR: {
        SB sb; sb_init(&sb);
        for(int i=0;i<n->nlist;i++){
            Node *p = n->list[i];
            if(p->type==N_STR){ sb_append(&sb, p->sval); }
            else{
                Value ev = eval(p, e);
                char *s = val_to_str(ev);
                if(p->debug){ sb_append(&sb, p->src); sb_append(&sb, s); }
                else sb_append(&sb, s);
            }
        }
        return v_str(sb_str(&sb));
    }
    case N_UNARY: {
        Value v = eval(n->a, e);
        if(v.type!=V_INT) die("unary minus on non-int");
        if(!strcmp(n->op,"-")) return v_int(-v.i);
        return v;
    }
    case N_BINOP: {
        long l = eval_int(n->a, e), r = eval_int(n->b, e);
        if(!strcmp(n->op,"+")) return v_int(l + r);
        if(!strcmp(n->op,"-")) return v_int(l - r);
        if(!strcmp(n->op,"*")) return v_int(l * r);
        if(!strcmp(n->op,"//")) return v_int(py_floordiv(l,r));
        if(!strcmp(n->op,"%")) return v_int(py_mod(l,r));
        if(!strcmp(n->op,"/")) return v_int(py_floordiv(l,r)); /* no floats; floor */
        die("unknown operator '%s'", n->op);
        return v_none();
    }
    case N_COMPARE: {
        long l = eval_int(n->a, e), r = eval_int(n->b, e);
        int res;
        if(!strcmp(n->op,"<")) res = l < r;
        else if(!strcmp(n->op,">")) res = l > r;
        else if(!strcmp(n->op,"<=")) res = l <= r;
        else if(!strcmp(n->op,">=")) res = l >= r;
        else if(!strcmp(n->op,"==")) res = l == r;
        else if(!strcmp(n->op,"!=")) res = l != r;
        else { die("unknown comparison '%s'", n->op); res=0; }
        return v_int(res);
    }
    case N_CALL: {
        Node *callee = n->a;
        if(callee->type != N_NAME) die("only named function calls supported");
        const char *fname = callee->name;
        if(!strcmp(fname,"print")){ print_args(n->list, n->nlist, e); return v_none(); }
        if(!strcmp(fname,"range")) die("range() only allowed in for-loops");
        Func *f = func_find(fname);
        if(!f) die("unknown function '%s'", fname);
        if(n->nlist > 16) die("too many arguments");
        Value args[16];
        for(int i=0;i<n->nlist;i++) args[i] = eval(n->list[i], e);
        return call_user_func(f, args, n->nlist);
    }
    default:
        die("eval: not an expression (node type %d)", n->type);
        return v_none();
    }
}

static int eval_stmt(Node *n, Env *e){
    switch(n->type){
    case N_FUNCDEF: {
        Func *f = xmalloc(sizeof *f);
        f->name = sclone(n->name);
        f->nparams = n->nlist;
        f->params = xmalloc((n->nlist+1)*sizeof(char*));
        for(int i=0;i<n->nlist;i++) f->params[i] = sclone(n->list[i]->name);
        f->body = n->b;
        f->next = funcs; funcs = f;
        return 0;
    }
    case N_RETURN:
        g_retval = n->a ? eval(n->a, e) : v_none();
        g_returning = 1;
        return 1;
    case N_ASSIGN: {
        Value v = eval(n->b, e);
        env_set(e, n->a->name, v);
        return 0;
    }
    case N_EXPRSTMT:
        eval(n->a, e);
        return 0;
    case N_PASS:
        return 0;
    case N_IMPORT:
        load_module(n->name, e);
        return 0;
    case N_IF:
        if(truthy(eval(n->a, e))) return eval_block(n->b, e);
        if(n->nlist > 0){
            Node *els = n->list[0];
            if(els->type==N_IF) return eval_stmt(els, e);
            return eval_block(els, e);
        }
        return 0;
    case N_WHILE:
        while(truthy(eval(n->a, e))){
            if(eval_block(n->b, e)) return 1;
        }
        return 0;
    case N_FOR: {
        Node *iter = n->a;
        if(iter->type!=N_CALL || iter->a->type!=N_NAME || strcmp(iter->a->name,"range")!=0)
            die("for-loops only support range(...)");
        int na = iter->nlist;
        long start=0, stop=0, step=1;
        if(na==1) stop = eval_int(iter->list[0], e);
        else if(na==2){ start=eval_int(iter->list[0],e); stop=eval_int(iter->list[1],e); }
        else if(na==3){ start=eval_int(iter->list[0],e); stop=eval_int(iter->list[1],e); step=eval_int(iter->list[2],e); }
        else die("range() takes 1-3 args");
        if(step==0) die("range() step is zero");
        for(long i=start; step>0 ? i<stop : i>stop; i+=step){
            env_set(e, n->name, v_int(i));
            if(eval_block(n->b, e)) return 1;
        }
        return 0;
    }
    case N_BLOCK:
        return eval_block(n, e);
    default:
        die("eval_stmt: unknown statement (node type %d)", n->type);
        return 0;
    }
}

static int eval_block(Node *blk, Env *e){
    for(int i=0;i<blk->nlist;i++){
        if(eval_stmt(blk->list[i], e)) return 1;
    }
    return 0;
}

/* ---------- file loading ---------- */

static char *read_file(const char *path){
    FILE *f = fopen(path, "rb");
    if(!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(sz < 0){ fclose(f); return NULL; }
    char *buf = xmalloc((size_t)sz + 1);
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = 0;
    fclose(f);
    return buf;
}

static void run_source(const char *code, Env *env, int repl);

static char *module_to_path(const char *mod){
    char *p = sclone(mod);
    for(char*q=p; *q; q++) if(*q=='.') *q='/';
    size_t n = strlen(p);
    char *path = xmalloc(n + 4);
    sprintf(path, "%s.py", p);
    free(p);
    return path;
}

static void load_module(const char *mod, Env *env){
    char *path = module_to_path(mod);
    char *code = read_file(path);
    if(!code){ die("cannot open module '%s' (%s)", mod, path); free(path); return; }
    free(path);
    run_source(code, env, 0);
}

/* ---------- ast dump with imports ---------- */

static char *g_visited_paths[256];
static int g_nvisited = 0;
static char *g_imports[256];
static int g_nimports = 0;

static int already_visited(const char *path){
    for(int i=0;i<g_nvisited;i++) if(strcmp(g_visited_paths[i], path)==0) return 1;
    return 0;
}

static void collect_imports(Node *n){
    if(!n) return;
    if(n->type==N_IMPORT){
        for(int i=0;i<g_nimports;i++) if(strcmp(g_imports[i], n->name)==0) return;
        if(g_nimports < 256) g_imports[g_nimports++] = n->name;
        return;
    }
    if(n->a) collect_imports(n->a);
    if(n->b) collect_imports(n->b);
    for(int i=0;i<n->nlist;i++) collect_imports(n->list[i]);
}

static void print_ast_and_imports(const char *code, const char *origin, const char *self_path){
    if(self_path && !already_visited(self_path) && g_nvisited < 256)
        g_visited_paths[g_nvisited++] = sclone(self_path);
    Node *blk = parse_source(code);
    if(origin) printf("; module: %s\n", origin);
    print_program(blk);
    int start = g_nimports;
    collect_imports(blk);
    int end = g_nimports;
    for(int i=start; i<end; i++){
        char *path = module_to_path(g_imports[i]);
        if(already_visited(path)){ free(path); continue; }
        char *sub = read_file(path);
        if(sub){
            fputc('\n', stdout);
            print_ast_and_imports(sub, g_imports[i], path);
            free(sub);
        } else {
            fprintf(stderr, "nanopie: cannot open module '%s'\n", g_imports[i]);
        }
        free(path);
    }
}

static void repl_echo(Env *env, Node *expr){
    Value v = eval(expr, env);
    if(!g_returning && v.type != V_NONE){
        char *s = val_to_str(v);
        fputs(s, stdout);
        fputc(10, stdout);
    }
}

static void run_source(const char *code, Env *env, int repl){
    Lexer L; lex_init(&L, code, 0); lex_advance(&L);
    while(L.cur.kind != TK_EOF){
        if(L.cur.kind==TK_NEWLINE){ lex_advance(&L); continue; }
        Node *stmt = parse_stmt(&L);
        g_returning = 0;
        if(!repl){ eval_stmt(stmt, env); continue; }
        if(stmt->type==N_EXPRSTMT){ repl_echo(env, stmt->a); continue; }
        if(stmt->type==N_BLOCK && stmt->nlist>0 &&
           stmt->list[stmt->nlist-1]->type==N_EXPRSTMT){
            for(int i=0; i<stmt->nlist-1; i++){
                eval_stmt(stmt->list[i], env);
                if(g_returning) break;
            }
            if(!g_returning) repl_echo(env, stmt->list[stmt->nlist-1]->a);
            continue;
        }
        eval_stmt(stmt, env);
    }
}

/* ---------- repl ---------- */

static char *read_repl_input(void){
    char line[4096];
    fputs(">>> ", stdout); fflush(stdout);
    if(!fgets(line, sizeof line, stdin)) return NULL;
    size_t len = strlen(line);
    while(len>0 && (line[len-1]=='\n'||line[len-1]=='\r')) line[--len]=0;
    if(line[0]==0) return sclone("");
    if(len>0 && line[len-1]==':'){
        SB sb; sb_init(&sb);
        sb_append(&sb, line); sb_appendc(&sb, '\n');
        for(;;){
            fputs("... ", stdout); fflush(stdout);
            if(!fgets(line, sizeof line, stdin)) break;
            len = strlen(line);
            while(len>0 && (line[len-1]=='\n'||line[len-1]=='\r')) line[--len]=0;
            if(line[0]==0) break;  /* blank line ends block */
            sb_append(&sb, line); sb_appendc(&sb, '\n');
        }
        return sb_str(&sb);
    }
    return sclone(line);
}

static void repl(Env *env){
    g_can_longjmp = 1;
    for(;;){
        if(setjmp(g_jmp) != 0){ continue; }
        char *input = read_repl_input();
        if(input==NULL){ fputc(10, stdout); break; }
        if(input[0]==0){ free(input); continue; }
        run_source(input, env, 1);
        free(input);
    }
}

/* ---------- main ---------- */

static void print_version(void){ fputs("nanopie 0.1\n", stdout); }

static void print_help(void){
    fputs(
"usage: nanopie [option ...] [file]\n"
"\n"
"options:\n"
"  -c <cmd>    execute the program passed as a string\n"
"  -m <mod>    run a module by dotted name (e.g. examples.fib)\n"
"  -i          run then drop into the interactive REPL (like python -i)\n"
"  -           read the program from stdin\n"
"  --print-ast              parse, dump the AST as s-expressions, and exit\n"
"  --print-ast-and-imports  like --print-ast, also dump imported modules\n"
"  -V          print version and exit\n"
"  -h, --help  print this help and exit\n"
"  --          stop processing options; next arg is the file\n"
"\n"
"no arguments  start the REPL\n",
    stdout);
}

static char *read_stdin_all(void){
    SB sb; sb_init(&sb);
    char buf[4096];
    size_t n;
    while((n = fread(buf, 1, sizeof buf, stdin)) > 0) sb_appendn(&sb, buf, n);
    return sb_str(&sb);
}

int main(int argc, char **argv){
    Env global; global.head = NULL;
    env_set(&global, "True", v_int(1));
    env_set(&global, "False", v_int(0));
    env_set(&global, "None", v_none());

    int force_interactive = 0;
    int dump_ast = 0;          /* 0=no, 1=--print-ast, 2=--print-ast-and-imports */
    const char *cmd = NULL;     /* -c */
    const char *mod = NULL;     /* -m */
    int read_std = 0;           /* -  */
    const char *file = NULL;    /* positional */
    int have_source = 0;        /* one of cmd/mod/stdin/file given */

    for(int i=1; i<argc; i++){
        if(!strcmp(argv[i], "-i")){ force_interactive = 1; continue; }
        if(!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")){ print_help(); return 0; }
        if(!strcmp(argv[i], "-V") || !strcmp(argv[i], "--version")){ print_version(); return 0; }
        if(!strcmp(argv[i], "--print-ast")){ dump_ast = 1; continue; }
        if(!strcmp(argv[i], "--print-ast-and-imports")){ dump_ast = 2; continue; }
        if(!strcmp(argv[i], "-c")){
            if(++i >= argc) die("argument expected for -c");
            if(have_source) die("only one of -c/-m/-/file allowed");
            cmd = argv[i]; have_source = 1; continue;
        }
        if(!strcmp(argv[i], "-m")){
            if(++i >= argc) die("argument expected for -m");
            if(have_source) die("only one of -c/-m/-/file allowed");
            mod = argv[i]; have_source = 1; continue;
        }
        if(!strcmp(argv[i], "-")){
            if(have_source) die("only one of -c/-m/-/file allowed");
            read_std = 1; have_source = 1; continue;
        }
        if(!strcmp(argv[i], "--")){
            if(++i < argc){
                if(have_source) die("only one of -c/-m/-/file allowed");
                file = argv[i]; have_source = 1;
            }
            continue;
        }
        if(argv[i][0]=='-' && argv[i][1] != '\0') die("unknown option '%s'", argv[i]);
        if(have_source) die("only one of -c/-m/-/file allowed");
        file = argv[i]; have_source = 1;
    }

    if(dump_ast && !have_source) die("--print-ast requires a source (-c, -m, -, or file)");

    /* resolve the source string and an origin label for module headers */
    const char *source = NULL;
    char *source_alloc = NULL;
    const char *origin = NULL;
    if(cmd){ source = cmd; origin = "<command>"; }
    else if(mod){
        char *path = module_to_path(mod);
        source_alloc = read_file(path);
        if(!source_alloc){ fprintf(stderr,"nanopie: cannot open module '%s' (%s)\n", mod, path); free(path); return 1; }
        free(path);
        source = source_alloc; origin = mod;
    } else if(read_std){
        source_alloc = read_stdin_all();
        source = source_alloc; origin = "<stdin>";
    } else if(file){
        source_alloc = read_file(file);
        if(!source_alloc){ fprintf(stderr,"nanopie: cannot open '%s'\n", file); return 1; }
        source = source_alloc; origin = file;
    }

    if(dump_ast == 1){
        Node *blk = parse_source(source);
        print_program(blk);
    } else if(dump_ast == 2){
        char *self = NULL;
        if(mod) self = module_to_path(mod);   /* -m: root can be re-imported */
        else if(file) self = sclone(file);    /* file: root can be re-imported */
        print_ast_and_imports(source, origin, self);
        free(self);
    } else if(source){
        run_source(source, &global, 0);
    }
    free(source_alloc);

    if(!dump_ast && (force_interactive || !have_source)) repl(&global);
    return 0;
}
