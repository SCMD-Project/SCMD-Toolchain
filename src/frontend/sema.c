#include "scmd/sema.h"
#include "scmd/common.h"

#include <stdlib.h>
#include <string.h>

typedef struct Sym { const char *name; ScmdTypeKind type; } Sym;
typedef struct SymVec { Sym *items; size_t len, cap; } SymVec;
typedef struct ArrSym { const char *name; ScmdTypeKind type; size_t len; } ArrSym;
typedef struct ArrVec { ArrSym *items; size_t len, cap; } ArrVec;
typedef struct NameVec { const char **items; size_t len, cap; } NameVec;

static void sv_push(SymVec *v,const char *name,ScmdTypeKind type){ if(v->len==v->cap){size_t nc=v->cap?v->cap*2u:8u;v->items=(Sym*)realloc(v->items,nc*sizeof(*v->items));v->cap=nc;}v->items[v->len++]=(Sym){name,type}; }
static int sv_find(const SymVec *v,const char *name){for(size_t i=0;i<v->len;++i)if(strcmp(v->items[i].name,name)==0)return(int)i;return-1;}
static ScmdTypeKind sv_type(const SymVec *v,const char *name){int i=sv_find(v,name);return i<0?SCMD_TYPE_UNKNOWN:v->items[(size_t)i].type;}
static void av_push(ArrVec *v,const char *name,ScmdTypeKind type,size_t len){if(v->len==v->cap){size_t nc=v->cap?v->cap*2u:8u;v->items=(ArrSym*)realloc(v->items,nc*sizeof(*v->items));v->cap=nc;}v->items[v->len++]=(ArrSym){name,type,len};}
static int av_find(const ArrVec *v,const char *name){for(size_t i=0;i<v->len;++i)if(strcmp(v->items[i].name,name)==0)return(int)i;return-1;}
static ScmdTypeKind av_type(const ArrVec *v,const char *name){int i=av_find(v,name);return i<0?SCMD_TYPE_UNKNOWN:v->items[(size_t)i].type;}
static void nv_push(NameVec *v,const char *name){if(v->len==v->cap){size_t nc=v->cap?v->cap*2u:8u;v->items=(const char**)realloc(v->items,nc*sizeof(*v->items));v->cap=nc;}v->items[v->len++]=name;}
static int nv_find(const NameVec *v,const char *name){for(size_t i=0;i<v->len;++i)if(strcmp(v->items[i],name)==0)return(int)i;return-1;}
static const char *type_name(ScmdTypeKind t){return t==SCMD_TYPE_BOOL?"bool":(t==SCMD_TYPE_U8?"u8":"unknown");}

static bool console_name_equal(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) {
        unsigned char x=(unsigned char)*a, y=(unsigned char)*b;
        if(x>='A'&&x<='Z')x=(unsigned char)(x+('a'-'A'));
        if(y>='A'&&y<='Z')y=(unsigned char)(y+('a'-'A'));
        if(x!=y)return false;
    }
    return *a==*b;
}

static bool export_name_safe(const char *name){
    if(!name||!*name)return false;
    const unsigned char *p=(const unsigned char*)name;
    if(!((*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')||*p=='_'))return false;
    for(++p;*p;++p)if(!((*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='_'))return false;
    return true;
}
static bool export_name_reserved(const char *name){
    static const char *const reserved[]={
        "alias","clear","clearall","echo","echoln","exec","exec_async","execifexists","help",
        "hideconsole","showconsole","incrementvar","multvar","quit","exit","kill","version","say","say_team","setinfo","sleep","toggle"
    };
    for(size_t i=0;i<sizeof(reserved)/sizeof(reserved[0]);++i)if(console_name_equal(name,reserved[i]))return true;
    return false;
}

static ScmdTypeKind check_expr(ScmdExpr *e,const SymVec *locals,const SymVec *globals,const ArrVec *arrays,const NameVec *functions,const char *path,int *errors){
    if(!e)return SCMD_TYPE_UNKNOWN;
    ScmdTypeKind t=SCMD_TYPE_UNKNOWN;
    switch(e->kind){
        case EXPR_BOOL:t=SCMD_TYPE_BOOL;break;
        case EXPR_INT:
            if(e->as.integer>255u){scmd_error_at(path,e->line,e->col,"integer literal %llu does not fit in u8",(unsigned long long)e->as.integer);(*errors)++;}
            t=SCMD_TYPE_U8;break;
        case EXPR_IDENT:
            t=sv_type(locals,e->as.name); if(t==SCMD_TYPE_UNKNOWN)t=sv_type(globals,e->as.name);
            if(t==SCMD_TYPE_UNKNOWN){scmd_error_at(path,e->line,e->col,"unknown variable '%s'",e->as.name);(*errors)++;}
            break;
        case EXPR_INDEX:{
            t=av_type(arrays,e->as.index.name);
            if(t==SCMD_TYPE_UNKNOWN){scmd_error_at(path,e->line,e->col,"unknown array '%s'",e->as.index.name);(*errors)++;break;}
            ScmdTypeKind it=check_expr(e->as.index.index,locals,globals,arrays,functions,path,errors);
            if(it!=SCMD_TYPE_U8){scmd_error_at(path,e->line,e->col,"array index must be u8, found %s",type_name(it));(*errors)++;}
            break;}
        case EXPR_CALL:
            if(nv_find(functions,e->as.call.name)<0){scmd_error_at(path,e->line,e->col,"unknown function '%s'",e->as.call.name);(*errors)++;}
            else scmd_error_at(path,e->line,e->col,"function calls cannot be used as expressions yet");
            if(e->as.call.arg_count){scmd_error_at(path,e->line,e->col,"function parameters are not implemented yet");(*errors)++;}
            t=SCMD_TYPE_UNKNOWN;break;
        case EXPR_UNARY:{
            ScmdTypeKind a=check_expr(e->as.unary.value,locals,globals,arrays,functions,path,errors);
            if(e->as.unary.op==UNARY_NOT){if(a!=SCMD_TYPE_BOOL){scmd_error_at(path,e->line,e->col,"operator ! expects bool, found %s",type_name(a));(*errors)++;}t=SCMD_TYPE_BOOL;}
            else {if(a!=SCMD_TYPE_U8){scmd_error_at(path,e->line,e->col,"unary integer operator expects u8, found %s",type_name(a));(*errors)++;}t=SCMD_TYPE_U8;}
            break;}
        case EXPR_BINARY:{
            ScmdTypeKind a=check_expr(e->as.binary.lhs,locals,globals,arrays,functions,path,errors);
            ScmdTypeKind b=check_expr(e->as.binary.rhs,locals,globals,arrays,functions,path,errors);
            ScmdBinaryOp op=e->as.binary.op;
            if(op==BIN_LOGICAL_AND||op==BIN_LOGICAL_OR){
                if(a!=SCMD_TYPE_BOOL||b!=SCMD_TYPE_BOOL){scmd_error_at(path,e->line,e->col,"logical operator expects bool operands, found %s and %s",type_name(a),type_name(b));(*errors)++;}t=SCMD_TYPE_BOOL;
            } else if(op==BIN_EQ||op==BIN_NEQ){
                if(a!=b||a==SCMD_TYPE_UNKNOWN){scmd_error_at(path,e->line,e->col,"comparison requires operands of the same type, found %s and %s",type_name(a),type_name(b));(*errors)++;}t=SCMD_TYPE_BOOL;
            } else if(op==BIN_LT||op==BIN_LE||op==BIN_GT||op==BIN_GE){
                if(a!=SCMD_TYPE_U8||b!=SCMD_TYPE_U8){scmd_error_at(path,e->line,e->col,"ordered comparison expects u8 operands, found %s and %s",type_name(a),type_name(b));(*errors)++;}t=SCMD_TYPE_BOOL;
            } else if(op==BIN_BIT_XOR && a==SCMD_TYPE_BOOL && b==SCMD_TYPE_BOOL){
                t=SCMD_TYPE_BOOL;
            } else {
                if(a!=SCMD_TYPE_U8||b!=SCMD_TYPE_U8){scmd_error_at(path,e->line,e->col,"integer/bitwise operator expects u8 operands, found %s and %s",type_name(a),type_name(b));(*errors)++;}t=SCMD_TYPE_U8;
            }
            break;}
    }
    e->inferred_type=t; return t;
}

static void collect_locals(ScmdStmt *s,SymVec *locals,const SymVec *globals,const ArrVec *arrays,const NameVec *functions,const char *path,int *errors){
    for(;s;s=s->next){
        if(s->kind==STMT_VAR_DECL){
            ScmdTypeKind it=check_expr(s->as.var_decl.init,locals,globals,arrays,functions,path,errors);
            ScmdTypeKind rt=s->as.var_decl.declared_type==SCMD_TYPE_UNKNOWN?it:s->as.var_decl.declared_type;
            if(s->as.var_decl.declared_type!=SCMD_TYPE_UNKNOWN && it!=SCMD_TYPE_UNKNOWN && it!=s->as.var_decl.declared_type){
                scmd_error_at(path,s->line,s->col,"initializer type %s does not match declared type %s",type_name(it),type_name(s->as.var_decl.declared_type));(*errors)++;
            }
            s->as.var_decl.resolved_type=rt;
            if(sv_find(locals,s->as.var_decl.name)>=0){scmd_error_at(path,s->line,s->col,"duplicate local '%s'",s->as.var_decl.name);(*errors)++;}
            else sv_push(locals,s->as.var_decl.name,rt);
        } else if(s->kind==STMT_IF){
            if(s->as.if_stmt.then_block)collect_locals(s->as.if_stmt.then_block->as.block_scope.first,locals,globals,arrays,functions,path,errors);
            if(s->as.if_stmt.else_block)collect_locals(s->as.if_stmt.else_block->as.block_scope.first,locals,globals,arrays,functions,path,errors);
        } else if(s->kind==STMT_WHILE){ if(s->as.while_stmt.body)collect_locals(s->as.while_stmt.body->as.block_scope.first,locals,globals,arrays,functions,path,errors); }
        else if(s->kind==STMT_FOR){
            if(s->as.for_stmt.init)collect_locals(s->as.for_stmt.init,locals,globals,arrays,functions,path,errors);
            if(s->as.for_stmt.body)collect_locals(s->as.for_stmt.body->as.block_scope.first,locals,globals,arrays,functions,path,errors);
        } else if(s->kind==STMT_BLOCK_SCOPE)collect_locals(s->as.block_scope.first,locals,globals,arrays,functions,path,errors);
    }
}

static void check_stmt(ScmdStmt *s,const SymVec *locals,const SymVec *globals,const ArrVec *arrays,const NameVec *functions,const NameVec *blocks,
                       const char *current_fn,const char *path,int *errors,unsigned char *edges,size_t fn_count,bool in_named_block,const NameVec *records,int nested_depth){
    int cur_index=current_fn?nv_find(functions,current_fn):-1;
    for(;s;s=s->next){
        switch(s->kind){
            case STMT_VAR_DECL: break; /* collected and typed earlier */
            case STMT_ASSIGN:{
                ScmdTypeKind lhs=sv_type(locals,s->as.assign.name); if(lhs==SCMD_TYPE_UNKNOWN)lhs=sv_type(globals,s->as.assign.name);
                if(lhs==SCMD_TYPE_UNKNOWN){scmd_error_at(path,s->line,s->col,"assignment to unknown variable '%s'",s->as.assign.name);(*errors)++;}
                ScmdTypeKind rhs=check_expr(s->as.assign.value,locals,globals,arrays,functions,path,errors);
                if(lhs!=SCMD_TYPE_UNKNOWN&&rhs!=SCMD_TYPE_UNKNOWN&&lhs!=rhs){scmd_error_at(path,s->line,s->col,"cannot assign %s to %s '%s'",type_name(rhs),type_name(lhs),s->as.assign.name);(*errors)++;}
                if(s->as.assign.op!=ASSIGN_SET && lhs!=SCMD_TYPE_U8){scmd_error_at(path,s->line,s->col,"compound assignment currently requires u8");(*errors)++;}
                break;}
            case STMT_ARRAY_ASSIGN:{
                ScmdTypeKind lhs=av_type(arrays,s->as.array_assign.name);
                if(lhs==SCMD_TYPE_UNKNOWN){scmd_error_at(path,s->line,s->col,"assignment to unknown array '%s'",s->as.array_assign.name);(*errors)++;}
                ScmdTypeKind ix=check_expr(s->as.array_assign.index,locals,globals,arrays,functions,path,errors);
                if(ix!=SCMD_TYPE_U8){scmd_error_at(path,s->line,s->col,"array index must be u8, found %s",type_name(ix));(*errors)++;}
                ScmdTypeKind rhs=check_expr(s->as.array_assign.value,locals,globals,arrays,functions,path,errors);
                if(lhs!=SCMD_TYPE_UNKNOWN&&rhs!=SCMD_TYPE_UNKNOWN&&lhs!=rhs){scmd_error_at(path,s->line,s->col,"cannot assign %s to %s array '%s'",type_name(rhs),type_name(lhs),s->as.array_assign.name);(*errors)++;}
                if(s->as.array_assign.op!=ASSIGN_SET){scmd_error_at(path,s->line,s->col,"compound array assignment is not implemented yet; use array[index] = value");(*errors)++;}
                break;}
            case STMT_IF:{ScmdTypeKind c=check_expr(s->as.if_stmt.cond,locals,globals,arrays,functions,path,errors);if(c!=SCMD_TYPE_BOOL){scmd_error_at(path,s->line,s->col,"if condition must be bool, found %s",type_name(c));(*errors)++;}
                if(s->as.if_stmt.then_block)check_stmt(s->as.if_stmt.then_block->as.block_scope.first,locals,globals,arrays,functions,blocks,current_fn,path,errors,edges,fn_count,in_named_block,records,nested_depth+1);
                if(s->as.if_stmt.else_block)check_stmt(s->as.if_stmt.else_block->as.block_scope.first,locals,globals,arrays,functions,blocks,current_fn,path,errors,edges,fn_count,in_named_block,records,nested_depth+1);break;}
            case STMT_WHILE:{ScmdTypeKind c=check_expr(s->as.while_stmt.cond,locals,globals,arrays,functions,path,errors);if(c!=SCMD_TYPE_BOOL){scmd_error_at(path,s->line,s->col,"while condition must be bool, found %s",type_name(c));(*errors)++;}
                if(s->as.while_stmt.body)check_stmt(s->as.while_stmt.body->as.block_scope.first,locals,globals,arrays,functions,blocks,current_fn,path,errors,edges,fn_count,in_named_block,records,nested_depth+1);break;}
            case STMT_FOR:{if(s->as.for_stmt.init)check_stmt(s->as.for_stmt.init,locals,globals,arrays,functions,blocks,current_fn,path,errors,edges,fn_count,in_named_block,records,nested_depth+1);if(s->as.for_stmt.cond){ScmdTypeKind c=check_expr(s->as.for_stmt.cond,locals,globals,arrays,functions,path,errors);if(c!=SCMD_TYPE_BOOL){scmd_error_at(path,s->line,s->col,"for condition must be bool, found %s",type_name(c));(*errors)++;}}
                if(s->as.for_stmt.step)check_stmt(s->as.for_stmt.step,locals,globals,arrays,functions,blocks,current_fn,path,errors,edges,fn_count,in_named_block,records,nested_depth+1);
                if(s->as.for_stmt.body)check_stmt(s->as.for_stmt.body->as.block_scope.first,locals,globals,arrays,functions,blocks,current_fn,path,errors,edges,fn_count,in_named_block,records,nested_depth+1);break;}
            case STMT_CALL:{int to=nv_find(functions,s->as.call.name);if(to<0){scmd_error_at(path,s->line,s->col,"unknown function '%s'",s->as.call.name);(*errors)++;}else if(cur_index>=0)edges[(size_t)cur_index*fn_count+(size_t)to]=1;
                if(s->as.call.arg_count){scmd_error_at(path,s->line,s->col,"function parameters are not implemented yet");(*errors)++;}break;}
            case STMT_RETURN: if(s->as.return_stmt.value){scmd_error_at(path,s->line,s->col,"return values are reserved but not implemented yet");(*errors)++;} break;
            case STMT_BLOCK_SCOPE: check_stmt(s->as.block_scope.first,locals,globals,arrays,functions,blocks,current_fn,path,errors,edges,fn_count,in_named_block,records,nested_depth+1); break;
            case STMT_RECORD:
                if(!in_named_block){scmd_error_at(path,s->line,s->col,"record is only valid inside block </ />");(*errors)++;}
                else if(nested_depth>0){scmd_error_at(path,s->line,s->col,"record must be at the top level of block");(*errors)++;}
                break;
            case STMT_JUMP:
                if(!in_named_block){scmd_error_at(path,s->line,s->col,"jump is only valid inside block </ />");(*errors)++;}
                else if(!records||nv_find(records,s->as.jump.record_name)<0){scmd_error_at(path,s->line,s->col,"unknown record '%s'",s->as.jump.record_name);(*errors)++;}
                break;
            case STMT_BLOCK_CALL:{
                if(nv_find(blocks,s->as.block_call.block_name)<0){scmd_error_at(path,s->line,s->col,"unknown block '%s'",s->as.block_call.block_name);(*errors)++;}
                if(s->as.block_call.kind==BLOCK_CALL_RUN_UNTIL||s->as.block_call.kind==BLOCK_CALL_RUN_RANGE){scmd_error_at(path,s->line,s->col,"runUntil/runRange are reserved but not lowered by the CS2 backend yet");(*errors)++;}
                break;}
            case STMT_BUILTIN: case STMT_WAIT: break;
        }
    }
}

static int dfs_cycle(size_t u,size_t n,const unsigned char *edges,unsigned char *state){state[u]=1;for(size_t v=0;v<n;++v){if(!edges[u*n+v])continue;if(state[v]==1)return 1;if(state[v]==0&&dfs_cycle(v,n,edges,state))return 1;}state[u]=2;return 0;}

bool scmd_sema_check(const char *path,ScmdProgram *program){
    int errors=0; SymVec globals={0}; ArrVec arrays={0}; NameVec functions={0},blocks={0};
    for(ScmdGlobal *g=program->globals;g;g=g->next){
        if(g->is_array){
            if(g->array_len==0||!g->array_init_values){scmd_error_at(path,g->line,g->col,"array '%s' was not prepared by compile-time pass",g->name);errors++;continue;}
            ScmdTypeKind it=check_expr(g->init,&(SymVec){0},&globals,&arrays,&functions,path,&errors);
            ScmdTypeKind rt=g->declared_type;
            if(it!=SCMD_TYPE_UNKNOWN&&it!=rt){scmd_error_at(path,g->line,g->col,"array initializer type %s does not match element type %s",type_name(it),type_name(rt));errors++;}
            g->resolved_type=rt;
            if(sv_find(&globals,g->name)>=0||av_find(&arrays,g->name)>=0){scmd_error_at(path,g->line,g->col,"duplicate global '%s'",g->name);errors++;}else av_push(&arrays,g->name,rt,g->array_len);
        } else {
            ScmdTypeKind it=check_expr(g->init,&(SymVec){0},&globals,&arrays,&functions,path,&errors); ScmdTypeKind rt=g->declared_type==SCMD_TYPE_UNKNOWN?it:g->declared_type;
            if(g->declared_type!=SCMD_TYPE_UNKNOWN&&it!=SCMD_TYPE_UNKNOWN&&g->declared_type!=it){scmd_error_at(path,g->line,g->col,"initializer type %s does not match declared type %s",type_name(it),type_name(g->declared_type));errors++;}
            g->resolved_type=rt; if(sv_find(&globals,g->name)>=0||av_find(&arrays,g->name)>=0){scmd_error_at(path,g->line,g->col,"duplicate global '%s'",g->name);errors++;}else sv_push(&globals,g->name,rt);
        }
    }
    for(ScmdFunction *f=program->functions;f;f=f->next){
        if(nv_find(&functions,f->name)>=0){scmd_error_at(path,f->line,f->col,"duplicate function '%s'",f->name);errors++;}
        else nv_push(&functions,f->name);
        if(f->exported){
            if(strlen(f->name)>31u){scmd_error_at(path,f->line,f->col,"exported function name '%s' exceeds the 31-byte console alias limit",f->name);errors++;}
            for(ScmdFunction *prev=program->functions;prev!=f;prev=prev->next){
                if(prev->exported&&console_name_equal(prev->name,f->name)){
                    scmd_error_at(path,f->line,f->col,"exported console name '%s' collides case-insensitively with '%s'",f->name,prev->name);errors++;break;
                }
            }
            if(!export_name_safe(f->name)){scmd_error_at(path,f->line,f->col,"exported function name '%s' must be an ASCII console identifier",f->name);errors++;}
            else if(export_name_reserved(f->name)){scmd_error_at(path,f->line,f->col,"exported function name '%s' collides with a CS2/SCMD builtin",f->name);errors++;}
        }
    }
    for(ScmdBlock *b=program->blocks;b;b=b->next){if(nv_find(&blocks,b->name)>=0){scmd_error_at(path,b->line,b->col,"duplicate block '%s'",b->name);errors++;}else nv_push(&blocks,b->name);}
    if(nv_find(&functions,"main")<0){scmd_error_at(path,1,1,"missing entry function 'main'");errors++;}
    size_t fn_count=functions.len; unsigned char *edges=(unsigned char*)calloc(fn_count*fn_count,1);
    for(ScmdFunction *f=program->functions;f;f=f->next){SymVec locals={0};collect_locals(f->body,&locals,&globals,&arrays,&functions,path,&errors);check_stmt(f->body,&locals,&globals,&arrays,&functions,&blocks,f->name,path,&errors,edges,fn_count,false,NULL,0);free(locals.items);}
    for(ScmdBlock *b=program->blocks;b;b=b->next){
        NameVec records={0}; for(ScmdStmt *s=b->body;s;s=s->next)if(s->kind==STMT_RECORD){if(nv_find(&records,s->as.record.name)>=0){scmd_error_at(path,s->line,s->col,"duplicate record '%s' in block '%s'",s->as.record.name,b->name);errors++;}else nv_push(&records,s->as.record.name);}
        SymVec no_locals={0}; check_stmt(b->body,&no_locals,&globals,&arrays,&functions,&blocks,NULL,path,&errors,edges,fn_count,true,&records,0); free(records.items);
    }
    unsigned char *state=(unsigned char*)calloc(fn_count,1);int cyclic=0;for(size_t i=0;i<fn_count;++i)if(!state[i]&&dfs_cycle(i,fn_count,edges,state)){cyclic=1;break;}
    if(cyclic){scmd_error_at(path,1,1,"recursive function calls are not supported yet");errors++;}
    free(edges);free(state);free(globals.items);free(arrays.items);free(functions.items);free(blocks.items);return errors==0;
}
