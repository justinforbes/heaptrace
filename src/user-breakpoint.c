#include "logging.h"
#include "util.h"
#include "user-breakpoint.h"


UserBreakpoint *USER_BREAKPOINT_HEAD = 0;

/* SECTION: TOKENIZATION */


static UserBreakpointToken *_create_token(char *s, size_t s_sz, UBPTType type, size_t i) {
    UserBreakpointToken *token = (UserBreakpointToken *)calloc(1, sizeof(UserBreakpointToken));
    token->type = type;
    token->value = calloc(1, s_sz + 2);
    strncpy(token->value, s, s_sz);
    token->i = i;
    //printf("... adding token \"%s\" of type=%d\n", token->value, token->type);
    return token;
}


static void _free_token_list(UserBreakpointToken *token) {
    while (token) {
        UserBreakpointToken *next_token = token->next;
        free(token->value);
        free(token);
        token = next_token;
    }
}


UserBreakpointToken *tokenize_user_breakpoint_str(char *breakpoint) {
    UserBreakpointToken *token_head = 0;

    char *start_ptr = breakpoint;
    uint in_token = 0;
    UserBreakpointToken *cur_token = 0;
    UserBreakpointToken *next_token = 0;

    debug("Tokenizing \"%s\"...\n", breakpoint);
    for (size_t i = 0; i < strlen(breakpoint) + 1; i++) {
        char *s = breakpoint + i;
        char c = *s;

        if (!in_token) {
            if (c == ' ' || c == '\t' || c == '\n') {
                // skip whitespace
                continue;
            } else {
                // start token
                start_ptr = s;
                in_token = 1;
            }
        }

        if (start_ptr != s) { // if in the middle of a token
            if (c == ':' || c == '+' || c == '-' || c == '=' || c == '\0' || c == ' ' || c == '\t' || c == '\n') {
                next_token = _create_token(start_ptr, s - start_ptr, UBPT_TYPE_IDENTIFIER, i - (s - start_ptr));
                if (!token_head) token_head = next_token;
                if (cur_token) cur_token->next = next_token;
                cur_token = next_token;
                start_ptr = s;
                in_token = 0;
            }
        }

        //if (!in_token) 
        {
            if (c == ':' || c == '+' || c == '-' || c == '=') {
                next_token = _create_token(start_ptr, 1, UBPT_TYPE_PUNCTUATOR, i - (s - start_ptr));
                if (!token_head) token_head = cur_token;
                if (cur_token) cur_token->next = next_token;
                cur_token = next_token;
                in_token = 0;
            }
        }
    }

    return token_head;
}


/* SECTION: TOKEN TO AST CONVERSION */


static void _expect_token(UserBreakpoint *ubp, UserBreakpointToken *cur_token, uint b, char *msg) {
    if (!b) {
        log("\n");
        fatal("invalid user breakpoint syntax\n");
        log(COLOR_ERROR "    %s\n", ubp->name);
        char *space_buf = calloc(1, cur_token->i + 2);
        memset(space_buf, ' ', cur_token->i);
        log(COLOR_ERROR "    %s^-- %s\n\n" COLOR_RESET, space_buf, msg);
        free(space_buf);
        exit(1);
    }
}


#define EXPECT(b, msg) _expect_token(ubp, cur_token, (b), (msg))
#define GET() EXPECT(cur_token && cur_token->next, "expected a token following this one"); cur_token = cur_token->next;
#define NEXT() (UserBreakpointToken *)(cur_token && cur_token->next ? cur_token->next : 0)


static UserBreakpointToken *_parse_token_expression(UserBreakpoint *ubp, UserBreakpointToken *cur_token) {
    ubp->address = (UserBreakpointAddress *)calloc(1, sizeof(UserBreakpointAddress));
    UserBreakpointAddress *cur_ubpa = ubp->address;
    cur_ubpa->operation = UBPA_OPERATION_ADD;

    EXPECT(!!cur_token, "expression missing");
    while (1) {
        if (cur_token->type == UBPT_TYPE_PUNCTUATOR) {
            // if this isn't the first run...
            if (ubp->address != cur_ubpa) {
                // save current ubpa
                UserBreakpointAddress *next_ubpa = (UserBreakpointAddress *)calloc(1, sizeof(UserBreakpointAddress));
                cur_ubpa->next_operation = next_ubpa;
                cur_ubpa = next_ubpa;
                cur_ubpa->operation = UBPA_OPERATION_ADD;
            }

            if (!strcmp(cur_token->value, "+")) {
            } else if (!strcmp(cur_token->value, "-")) {
                if (cur_ubpa->operation == UBPA_OPERATION_SUBTRACT) { // --
                    cur_ubpa->operation = UBPA_OPERATION_ADD;
                } else {
                    cur_ubpa->operation = UBPA_OPERATION_SUBTRACT; // +-
                }
            } else if (!strcmp(cur_token->value, ":")) {
                break;
            } else {
                EXPECT(0, "invalid operation");
            }

            EXPECT(!!NEXT(), "missing expression after operator");
        } else { // cur_token->type == UBPT_TYPE_IDENTIFIER
            if (is_uint(cur_token->value)) {
                int endp;
                cur_ubpa->address = str_to_uint64(cur_token->value);
            } else {
                cur_ubpa->symbol_name = strdup(cur_token->value);
            }
        }

        cur_token = cur_token->next;
        if (!cur_token) break; // make sure this while look runs at least once
    }

    return cur_token;
}


UserBreakpoint *create_user_breakpoint(char *name) {
    UserBreakpointToken *token_head = tokenize_user_breakpoint_str(name);
    UserBreakpoint *ubp = (UserBreakpoint *)calloc(1, sizeof(UserBreakpoint));
    ubp->name = strdup(name);
    ubp->when = UBP_WHEN_BEFORE;
    ubp->count = 1;
    
    UserBreakpointToken *cur_token = token_head;
    enum action {ACTION_WHAT, ACTION_ADDRESS, ACTION_COUNT} cur_action = ACTION_WHAT;
    while (cur_token) {
        if (cur_action == ACTION_WHAT) {
            EXPECT(cur_token->type == UBPT_TYPE_IDENTIFIER, "expected an identifier");
            if (!strcmp(cur_token->value, "address") || !strcmp(cur_token->value, "addr")) {
                ubp->what = UBP_WHAT_ADDRESS;
                goto expect_address;
            } else if (!strcmp(cur_token->value, "oid") || !strcmp(cur_token->value, "operation") || !strcmp(cur_token->value, "number")) {
                ubp->what = UBP_WHAT_OID;
                goto expect_address;
            } else if (!strcmp(cur_token->value, "segfault") || !strcmp(cur_token->value, "sigsegv") || !strcmp(cur_token->value, "segv")) {
                ubp->what = UBP_WHAT_SEGFAULT;
                break;
            } else if (!strcmp(cur_token->value, "main") || !strcmp(cur_token->value, "entry") || !strcmp(cur_token->value, "start") || !strcmp(cur_token->value, "_entry")) {
                ubp->what = UBP_WHAT_ENTRY;
                break;
            } else {
                EXPECT(0, "unknown 'what': please choose one of [oid, address, segfault, entry]");
            }
        } else if (cur_action == ACTION_ADDRESS) {
            cur_token = _parse_token_expression(ubp, cur_token);
            cur_action = ACTION_COUNT;
        } else if (cur_action == ACTION_COUNT) {
            EXPECT(cur_token->type == UBPT_TYPE_IDENTIFIER, "expected identifier 'count'");
            ubp->count = (size_t)str_to_uint64(cur_token->value);
            break; // if you're adding &&/AND support, start by setting cur_action here
            // actually tbh just call self again...
        }

        cur_token = NEXT();
        continue;

expect_address:;
        cur_action = ACTION_ADDRESS;
        GET();
        EXPECT(cur_token->type == UBPT_TYPE_PUNCTUATOR && !strcmp(cur_token->value, "="), "unexpected token");
        GET();
        continue;
    }

    _free_token_list(token_head);
    return ubp;
}


void insert_user_breakpoint(UserBreakpoint *ubp) {
    if (!USER_BREAKPOINT_HEAD) {
        USER_BREAKPOINT_HEAD = ubp;
    } else {
        UserBreakpoint *cur_ubp = USER_BREAKPOINT_HEAD;
        while(cur_ubp->next) { cur_ubp = cur_ubp->next; }
        cur_ubp->next = ubp;
    }
}


static void free_address_list(UserBreakpointAddress *ubpa) {
    if (!ubpa) return;
    free(ubpa->symbol_name);
    free_address_list(ubpa);
}


void free_user_breakpoints() {
    UserBreakpoint *cur_ubp = USER_BREAKPOINT_HEAD;
    while (cur_ubp) {
        free_address_list(cur_ubp->address);
        free(cur_ubp->name);
        cur_ubp = cur_ubp->next;
    }
}


/* SECTION: CHECKING BREAKPOINTS */


static inline uint _is_reference_constant(char *name) {
    // TODO: check if we need "binary", "target", "glibc", etc
    if (!strcmp(name, "bin") || !strcmp(name, "libc")) {
        return 1;
    }

    return 0;
}


size_t count_symbol_references(char **se_names) {
    size_t count = 0;
    UserBreakpoint cur_ubp = USER_BREAKPOINT_HEAD;
    while (cur_ubp) {
        UserBreakpointAddress cur_ubpa = cur_ubp->address;
        while (cur_ubpa) {
            if (cur_ubpa->symbol_name) {
                if (!_is_reference_constant(cur_ubpa->symbol_name)) {
                    if (se_names) {
                        se_names[count] = strdup(cur_ubpa->symbol_name);
                    }
                    count++;
                }
            }
            cur_ubpa = cur_ubpa->next_operation;
        }
        cur_ubp = cur_ubp->next;
    }
    return count;
}


void fill_symbol_references(HeaptraceContext *ctx) {
    ProcMapsEntry *bin_pme = pme_walk(ctx->pme_head, PROCELF_TYPE_BINARY);
    ASSERT(bin_pme, "cannot find binary base address");

    UserBreakpoint cur_ubp = USER_BREAKPOINT_HEAD;
    while (cur_ubp) {
        UserBreakpointAddress cur_ubpa = cur_ubp->address;
        while (cur_ubpa) {
            if (cur_ubpa->symbol_name) {
                if (!_is_reference_constant(cur_ubpa->symbol_name)) {
                    uint resolved = 0;
                    SymbolEntry *cur_se = ctx->se_head;
                    while (cur_se) {
                        if (!strcmp(cur_ubpa->symbol_name, cur_se->name)) {
                            // found the symbol!
                            cur_ubpa->symbol_name = 0;
                            if (cur_se->type != SE_TYPE_STATIC) {
                                warn("user breakpoint \"%s\" references symbol %s which is a dynamic symbol. Only static symbols are currently supported.\n", cur_ubp->name, cur_se->name);
                            } else {
                                cur_ubpa->address = bin_pme->base + cur_se->offset;
                            }
                            resolved = 1;
                            break;
                        }
                        cur_se = cur_se->next;
                    }

                    if (!resolved) {
                        warn("user breakpoint \"%s\" references %s but the symbol could not be resolved. Will assume symbol %s=0x0\n", cur_ubp->name, cur_ubpa->symbol_name, cur_ubpa->symbol_name);
                        cur_ubpa->symbol_name = 0;
                        cur_ubpa->address;
                    }
                } else {
                    if (!(ctx->is_dynamic) && !strcmp(cur_ubpa->symbol_name, "libc")) {
                        warn("user breakpoint \"%s\" references %s but target binary is statically linked\n", cur_ubp->name, cur_ubpa->symbol_name);
                    }
                }
            }
            cur_ubpa = cur_ubpa->next_operation;
        }
        cur_ubp = cur_ubp->next;
    }
}


static inline uint64_t _resolve_symbol(HeaptraceContext *ctx) {
    // TODO: optimize! maybe fill in the "address" and NULL the "symbol_name" 
    // when symbols are resolved at runtime. First make an array of all sym 
    // names and merge with se_names
    if (!strcmp(ctx->target_at_entry, "entry")) {}
}


static inline uint _check_breakpoint_logic(HeaptraceContext *ctx, UserBreakpoint *ubp, UBPWhen when) {
    if (ubp->when != when) return 0;
    if (ubp->what == UBP_WHAT_OID) return ctx->h_oid == ubp->oid;
    else if (ctx->what == UBP_WHAT_SEGFAULT) return ctx->h_state == PROCESS_STATE_SEGFAULT;
    else if (ctx->what == UBP_WHAT_ENTRY) return ctx->h_state == PROCESS_STATE_ENTRY;
    else {
        ASSERT(!!ubp->address, "what=address, but address is NULL. Please report this along with your command line arguments.");
        uint64_t ptr = 0;
        UserBreakpointAddress *cur_ubpa = ubp->address;
        while (cur_ubpa) {
            uint64_t cur_ptr = cur_ubpa->address;
            ASSERT(!(cur_ubpa->symbol_name), "unable to check user breakpoint \"%s\"; symbol \"%s\" not resolved");
            // NOTE: this assertion is gonna fail if we use `entry`!

            if (cur_ubpa->operation == UBPA_OPERATION_ADD) {
                ptr += cur_ptr;
            } else if (cur_ubpa->operation == UBPA_OPERATION_SUBTRACT) {
                ptr -= cur_ptr;
            } else {
                ASSERT(0, "unknown operation %d. Please report this!", cur_ubpa-operation);
            }

            cur_ubpa = cur_ubpa->next;
        }
    }
}


void check_should_break(HeaptraceContext *ctx, UBPWhen when) {
    UserBreakpoint cur_ubp = USER_BREAKPOINT_HEAD;
    while (cur_ubp) {
        if (_check_breakpoint_logic(ctx, cur_ubp, when)) {
            if (++(cur_ubp->h_i) >= cur_ubp->count) {
                // ok, we've hit this breakpoint enough to call gdb!
                log(COLOR_ERROR "    [   PROCESS PAUSED   ]\n");
                log(COLOR_ERROR "    |   * attaching GDB via: " COLOR_ERROR_BOLD "%s -p %d\n" COLOR_RESET, OPT_GDB_PATH, ctx->pid);

                // launch gdb
                _remove_breakpoints(ctx, BREAKPOINT_OPTS_ALL); // TODO/XXX: use end_debugger
                PTRACE(PTRACE_DETACH, ctx->pid, NULL, SIGSTOP);

                char buf[10+1];
                snprintf(buf, 10, "%u", ctx->pid);
                char *args[] = {OPT_GDB_PATH, "-p", buf, NULL};
                if (execv(args[0], args) == -1) {
                    ASSERT(0, "failed to execute debugger %s: %s (errno %d)", args[0], strerror(errno), errno);
                }
            }
        }
        cur_ubp = cur_ubp->next;
    }
}


