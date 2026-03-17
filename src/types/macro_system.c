#include "macros.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

// =============================================================================
// String Utilities
// =============================================================================

static char* str_dup(const char* s) {
    if (!s) return NULL;
    char* d = malloc(strlen(s) + 1);
    if (d) strcpy(d, s);
    return d;
}

static char* str_to_lower(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char* lower = malloc(len + 1);
    if (!lower) return NULL;
    for (size_t i = 0; i < len; i++) lower[i] = (char)tolower((unsigned char)s[i]);
    lower[len] = '\0';
    return lower;
}

static char* str_to_upper(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char* upper = malloc(len + 1);
    if (!upper) return NULL;
    for (size_t i = 0; i < len; i++) upper[i] = (char)toupper((unsigned char)s[i]);
    upper[len] = '\0';
    return upper;
}

// snake_case: "MyTypeName" -> "my_type_name"
static char* str_to_snake(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char* snake = malloc(len * 2 + 1);
    if (!snake) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (isupper((unsigned char)s[i]) && i > 0) {
            snake[j++] = '_';
        }
        snake[j++] = (char)tolower((unsigned char)s[i]);
    }
    snake[j] = '\0';
    return snake;
}

// =============================================================================
// Template String Filters
// =============================================================================

char* template_apply_filter(const char* value, const char* filter) {
    if (!value) return NULL;
    if (!filter || strlen(filter) == 0) return str_dup(value);

    if (strcmp(filter, "lowercase") == 0 || strcmp(filter, "lower") == 0) {
        return str_to_lower(value);
    }
    if (strcmp(filter, "uppercase") == 0 || strcmp(filter, "upper") == 0) {
        return str_to_upper(value);
    }
    if (strcmp(filter, "snake_case") == 0 || strcmp(filter, "snake") == 0) {
        return str_to_snake(value);
    }

    return str_dup(value);
}

// =============================================================================
// Template Substitution
// =============================================================================

char* template_substitute(const char* tmpl, const char* param, const char* value) {
    if (!tmpl || !param || !value) return str_dup(tmpl);

    // Build placeholder: {{param}}
    size_t param_len = strlen(param);
    size_t placeholder_len = param_len + 4; // {{ + param + }}
    char* placeholder = malloc(placeholder_len + 1);
    if (!placeholder) return str_dup(tmpl);
    snprintf(placeholder, placeholder_len + 1, "{{%s}}", param);

    // Count occurrences
    size_t count = 0;
    const char* p = tmpl;
    while ((p = strstr(p, placeholder)) != NULL) {
        count++;
        p += placeholder_len;
    }

    if (count == 0) {
        free(placeholder);
        return str_dup(tmpl);
    }

    // Build result
    size_t value_len = strlen(value);
    size_t tmpl_len = strlen(tmpl);
    size_t result_len = tmpl_len + count * (value_len - placeholder_len);
    char* result = malloc(result_len + 1);
    if (!result) { free(placeholder); return str_dup(tmpl); }

    size_t ri = 0;
    p = tmpl;
    while (*p) {
        if (strncmp(p, placeholder, placeholder_len) == 0) {
            memcpy(result + ri, value, value_len);
            ri += value_len;
            p += placeholder_len;
        } else {
            result[ri++] = *p++;
        }
    }
    result[ri] = '\0';

    free(placeholder);
    return result;
}

// =============================================================================
// Macro System Lifecycle
// =============================================================================

MacroSystem* macro_system_new(ComptimeInterpreter* comptime) {
    MacroSystem* ms = calloc(1, sizeof(MacroSystem));
    if (!ms) return NULL;

    ms->comptime = comptime;
    ms->max_expansion_depth = 64;
    ms->enforce_hygiene = true;
    ms->template_capacity = 16;
    ms->templates = calloc(ms->template_capacity, sizeof(TemplateMacro*));

    return ms;
}

void macro_system_free(MacroSystem* ms) {
    if (!ms) return;

    for (size_t i = 0; i < ms->template_count; i++) {
        template_macro_free(ms->templates[i]);
    }
    free(ms->templates);
    free(ms);
}

// =============================================================================
// Template Macros
// =============================================================================

TemplateMacro* template_macro_new(const char* name, const char* body) {
    TemplateMacro* tmpl = calloc(1, sizeof(TemplateMacro));
    if (!tmpl) return NULL;

    tmpl->name = str_dup(name);
    tmpl->body_template = str_dup(body);

    return tmpl;
}

void template_macro_free(TemplateMacro* tmpl) {
    if (!tmpl) return;
    free(tmpl->name);
    free(tmpl->body_template);

    TemplateParam* p = tmpl->params;
    while (p) {
        TemplateParam* next = p->next;
        free(p->name);
        free(p->filter);
        free(p);
        p = next;
    }

    free(tmpl);
}

void macro_system_register_template(MacroSystem* ms, TemplateMacro* tmpl) {
    if (!ms || !tmpl) return;

    if (ms->template_count >= ms->template_capacity) {
        size_t new_cap = ms->template_capacity * 2;
        TemplateMacro** tmp = realloc(ms->templates, new_cap * sizeof(TemplateMacro*));
        if (!tmp) return;
        ms->templates = tmp;
        ms->template_capacity = new_cap;
    }

    ms->templates[ms->template_count++] = tmpl;
}

// =============================================================================
// Macro Expansion
// =============================================================================

MacroExpansion* macro_expansion_new(Position site, const char* macro_name) {
    MacroExpansion* exp = calloc(1, sizeof(MacroExpansion));
    if (!exp) return NULL;

    exp->expansion_site = site;
    exp->macro_name = macro_name;
    exp->node_capacity = 8;
    exp->generated_nodes = calloc(exp->node_capacity, sizeof(ASTNode*));

    return exp;
}

void macro_expansion_free(MacroExpansion* exp) {
    if (!exp) return;

    for (size_t i = 0; i < exp->node_count; i++) {
        ast_node_free(exp->generated_nodes[i]);
    }
    free(exp->generated_nodes);
    free(exp);
}

void macro_expansion_add_node(MacroExpansion* exp, ASTNode* node) {
    if (!exp || !node) return;

    if (exp->node_count >= exp->node_capacity) {
        size_t new_cap = exp->node_capacity * 2;
        ASTNode** tmp = realloc(exp->generated_nodes, new_cap * sizeof(ASTNode*));
        if (!tmp) return;
        exp->generated_nodes = tmp;
        exp->node_capacity = new_cap;
    }

    exp->generated_nodes[exp->node_count++] = node;
}

// =============================================================================
// Derive Macros
// =============================================================================

DeriveSpec* derive_parse_annotation(const char* args) {
    if (!args || strlen(args) == 0) return NULL;

    DeriveSpec* head = NULL;
    DeriveSpec* tail = NULL;

    // Parse comma-separated list: "Debug, Clone, Serialize"
    char* copy = str_dup(args);
    char* token = strtok(copy, ", ");

    while (token) {
        // Skip whitespace
        while (isspace((unsigned char)*token)) token++;

        DeriveSpec* spec = calloc(1, sizeof(DeriveSpec));
        if (!spec) break;

        if (strcmp(token, "Debug") == 0)            spec->kind = DERIVE_DEBUG;
        else if (strcmp(token, "Clone") == 0)       spec->kind = DERIVE_CLONE;
        else if (strcmp(token, "Hash") == 0)        spec->kind = DERIVE_HASH;
        else if (strcmp(token, "PartialEq") == 0 ||
                 strcmp(token, "Eq") == 0 ||
                 strcmp(token, "Equal") == 0)        spec->kind = DERIVE_EQUAL;
        else if (strcmp(token, "Serialize") == 0)   spec->kind = DERIVE_SERIALIZE;
        else if (strcmp(token, "Deserialize") == 0) spec->kind = DERIVE_DESERIALIZE;
        else if (strcmp(token, "Default") == 0)     spec->kind = DERIVE_DEFAULT;
        else {
            spec->kind = DERIVE_CUSTOM;
            spec->custom_name = str_dup(token);
        }

        if (!head) { head = spec; tail = spec; }
        else { tail->next = spec; tail = spec; }

        token = strtok(NULL, ", ");
    }

    free(copy);
    return head;
}

void derive_spec_free(DeriveSpec* spec) {
    while (spec) {
        DeriveSpec* next = spec->next;
        free((void*)spec->custom_name);
        free(spec);
        spec = next;
    }
}

// Generate a debug string function for a struct
static ASTNode* derive_debug_for_struct(const char* struct_name) {
    // Generate: func <StructName>_debug(self: *<StructName>) string { ... }
    // For now, create a function declaration node
    char func_name[256];
    snprintf(func_name, sizeof(func_name), "%s_debug", struct_name);

    Position pos = {0};
    FuncDeclNode* func = ast_func_decl_new(func_name, pos);
    func->body = ast_node_new(AST_BLOCK_STMT, pos);

    return (ASTNode*)func;
}

// Generate a clone function for a struct
static ASTNode* derive_clone_for_struct(const char* struct_name) {
    char func_name[256];
    snprintf(func_name, sizeof(func_name), "%s_clone", struct_name);

    Position pos = {0};
    FuncDeclNode* func = ast_func_decl_new(func_name, pos);
    func->body = ast_node_new(AST_BLOCK_STMT, pos);

    return (ASTNode*)func;
}

// Generate a hash function for a struct
static ASTNode* derive_hash_for_struct(const char* struct_name) {
    char func_name[256];
    snprintf(func_name, sizeof(func_name), "%s_hash", struct_name);

    Position pos = {0};
    FuncDeclNode* func = ast_func_decl_new(func_name, pos);
    func->body = ast_node_new(AST_BLOCK_STMT, pos);

    return (ASTNode*)func;
}

// Generate an equality function for a struct
static ASTNode* derive_equal_for_struct(const char* struct_name) {
    char func_name[256];
    snprintf(func_name, sizeof(func_name), "%s_equal", struct_name);

    Position pos = {0};
    FuncDeclNode* func = ast_func_decl_new(func_name, pos);
    func->body = ast_node_new(AST_BLOCK_STMT, pos);

    return (ASTNode*)func;
}

MacroExpansion* derive_expand(MacroSystem* ms, DeriveSpec* spec, ASTNode* target_struct) {
    if (!ms || !spec || !target_struct) return NULL;

    // Get struct name from the target
    const char* struct_name = NULL;
    if (target_struct->type == AST_TYPE_DECL) {
        // Type declaration wrapping a struct
        struct_name = "Unknown";
    }

    if (!struct_name) struct_name = "Unknown";

    Position pos = target_struct->pos;
    MacroExpansion* expansion = macro_expansion_new(pos, "derive");

    for (DeriveSpec* s = spec; s; s = s->next) {
        ASTNode* generated = NULL;

        switch (s->kind) {
            case DERIVE_DEBUG:
                generated = derive_debug_for_struct(struct_name);
                break;
            case DERIVE_CLONE:
                generated = derive_clone_for_struct(struct_name);
                break;
            case DERIVE_HASH:
                generated = derive_hash_for_struct(struct_name);
                break;
            case DERIVE_EQUAL:
                generated = derive_equal_for_struct(struct_name);
                break;
            case DERIVE_SERIALIZE:
            case DERIVE_DESERIALIZE:
            case DERIVE_DEFAULT:
            case DERIVE_CUSTOM:
                // TODO: implement remaining derives
                break;
        }

        if (generated) {
            macro_expansion_add_node(expansion, generated);
            ms->stats.derives_expanded++;
            ms->stats.nodes_generated++;
        }
    }

    return expansion;
}
