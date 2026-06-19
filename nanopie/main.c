// nanopie CLI - a thin frontend over the nanopie.h single-header library.
//
// build: cc -O2 -Wall -o nanopie main.c
// run:   ./nanopie                        # repl
//        ./nanopie examples/fib.py        # run file, exit
//        ./nanopie -i examples/fib.py     # run file then repl (like python -i)
//        ./nanopie -c 'print(1+2)'        # run a command string
//        ./nanopie -m examples.fib        # run a module by dotted name
//        ./nanopie -                      # read program from stdin
//        ./nanopie --print-ast examples/fib.py           # dump AST as s-exprs
//        ./nanopie --print-ast-and-imports examples/fib.py  # also dump imports

#include <stdio.h>
#include <string.h>

#define NANOPIE_IMPLEMENTATION
#include "nanopie.h"

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

int main(int argc, char **argv){
    Env global; env_init(&global);
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
