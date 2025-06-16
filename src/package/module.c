#include "package/module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <errno.h>

// Version operations
Version* version_parse(const char* version_str) {
    if (!version_str) return NULL;
    
    Version* version = calloc(1, sizeof(Version));
    if (!version) return NULL;
    
    // Parse semantic version format: major.minor.patch[-pre_release][+build_metadata]
    char* str_copy = string_duplicate(version_str);
    char* current = str_copy;
    
    // Parse major version
    char* dot = strchr(current, '.');
    if (!dot) {
        free(str_copy);
        free(version);
        return NULL;
    }
    *dot = '\0';
    version->major = atoi(current);
    current = dot + 1;
    
    // Parse minor version
    dot = strchr(current, '.');
    if (!dot) {
        free(str_copy);
        free(version);
        return NULL;
    }
    *dot = '\0';
    version->minor = atoi(current);
    current = dot + 1;
    
    // Parse patch version (may have pre-release or build metadata)
    char* dash = strchr(current, '-');
    char* plus = strchr(current, '+');
    
    // Find the end of patch version
    char* patch_end = current + strlen(current);
    if (dash && (!plus || dash < plus)) {
        patch_end = dash;
    } else if (plus && (!dash || plus < dash)) {
        patch_end = plus;
    }
    
    // Extract patch version
    size_t patch_len = patch_end - current;
    char patch_str[16];
    strncpy(patch_str, current, patch_len);
    patch_str[patch_len] = '\0';
    version->patch = atoi(patch_str);
    
    // Parse pre-release if present
    if (dash) {
        char* pre_start = dash + 1;
        char* pre_end = plus ? plus : str_copy + strlen(str_copy);
        size_t pre_len = pre_end - pre_start;
        version->pre_release = malloc(pre_len + 1);
        strncpy(version->pre_release, pre_start, pre_len);
        version->pre_release[pre_len] = '\0';
    }
    
    // Parse build metadata if present
    if (plus) {
        version->build_metadata = string_duplicate(plus + 1);
    }
    
    free(str_copy);
    return version;
}

void version_free(Version* version) {
    if (!version) return;
    free(version->pre_release);
    free(version->build_metadata);
    free(version);
}

char* version_to_string(const Version* version) {
    if (!version) return NULL;
    
    size_t buffer_size = 64;
    char* buffer = malloc(buffer_size);
    
    int written = snprintf(buffer, buffer_size, "%d.%d.%d", 
                          version->major, version->minor, version->patch);
    
    if (version->pre_release) {
        written += snprintf(buffer + written, buffer_size - written, "-%s", version->pre_release);
    }
    
    if (version->build_metadata) {
        written += snprintf(buffer + written, buffer_size - written, "+%s", version->build_metadata);
    }
    
    return buffer;
}

int version_compare(const Version* a, const Version* b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    
    // Compare major.minor.patch
    if (a->major != b->major) return a->major - b->major;
    if (a->minor != b->minor) return a->minor - b->minor;
    if (a->patch != b->patch) return a->patch - b->patch;
    
    // Handle pre-release versions (lower precedence than normal versions)
    if (!a->pre_release && b->pre_release) return 1;
    if (a->pre_release && !b->pre_release) return -1;
    if (!a->pre_release && !b->pre_release) return 0;
    
    return strcmp(a->pre_release, b->pre_release);
}

bool version_satisfies(const Version* version, const VersionConstraint* constraint) {
    if (!version || !constraint) return false;
    
    switch (constraint->type) {
        case VERSION_EXACT:
            return version_compare(version, constraint->min_version) == 0;
            
        case VERSION_COMPATIBLE: // ^1.2.3 allows >=1.2.3 <2.0.0
            if (constraint->min_version->major != version->major) return false;
            return version_compare(version, constraint->min_version) >= 0;
            
        case VERSION_TILDE: // ~1.2.3 allows >=1.2.3 <1.3.0
            if (constraint->min_version->major != version->major ||
                constraint->min_version->minor != version->minor) return false;
            return version_compare(version, constraint->min_version) >= 0;
            
        case VERSION_RANGE:
            return version_compare(version, constraint->min_version) >= 0 &&
                   version_compare(version, constraint->max_version) < 0;
            
        case VERSION_LATEST:
        case VERSION_ANY:
            return true;
    }
    
    return false;
}

// Version constraint operations
VersionConstraint* version_constraint_parse(const char* constraint_str) {
    if (!constraint_str) return NULL;
    
    VersionConstraint* constraint = calloc(1, sizeof(VersionConstraint));
    if (!constraint) return NULL;
    
    // Determine constraint type and parse accordingly
    if (strcmp(constraint_str, "*") == 0 || strcmp(constraint_str, "any") == 0) {
        constraint->type = VERSION_ANY;
    } else if (strcmp(constraint_str, "latest") == 0) {
        constraint->type = VERSION_LATEST;
    } else if (constraint_str[0] == '^') {
        constraint->type = VERSION_COMPATIBLE;
        constraint->min_version = version_parse(constraint_str + 1);
    } else if (constraint_str[0] == '~') {
        constraint->type = VERSION_TILDE;
        constraint->min_version = version_parse(constraint_str + 1);
    } else if (strstr(constraint_str, ">=") || strstr(constraint_str, "<=") || strstr(constraint_str, ">") || strstr(constraint_str, "<")) {
        constraint->type = VERSION_RANGE;
        // TODO: Parse complex range expressions
        // For now, simplified implementation
        constraint->min_version = version_parse("0.0.0");
        constraint->max_version = version_parse("999.999.999");
    } else {
        constraint->type = VERSION_EXACT;
        constraint->min_version = version_parse(constraint_str);
    }
    
    return constraint;
}

void version_constraint_free(VersionConstraint* constraint) {
    if (!constraint) return;
    version_free(constraint->min_version);
    version_free(constraint->max_version);
    free(constraint);
}

// Dependency operations
Dependency* dependency_create(const char* name, const char* constraint_str) {
    if (!name) return NULL;
    
    Dependency* dep = calloc(1, sizeof(Dependency));
    if (!dep) return NULL;
    
    dep->name = string_duplicate(name);
    if (constraint_str) {
        dep->constraint = version_constraint_parse(constraint_str);
    } else {
        dep->constraint = version_constraint_parse("*");
    }
    
    return dep;
}

void dependency_free(Dependency* dep) {
    if (!dep) return;
    
    free(dep->name);
    free(dep->source);
    version_constraint_free(dep->constraint);
    string_array_free(dep->features, dep->feature_count);
    free(dep);
}

Dependency* dependency_list_add(Dependency* list, Dependency* dep) {
    if (!dep) return list;
    
    dep->next = list;
    return dep;
}

Dependency* dependency_list_find(Dependency* list, const char* name) {
    if (!name) return NULL;
    
    for (Dependency* current = list; current; current = current->next) {
        if (strcmp(current->name, name) == 0) {
            return current;
        }
    }
    return NULL;
}

void dependency_list_free(Dependency* list) {
    while (list) {
        Dependency* next = list->next;
        dependency_free(list);
        list = next;
    }
}

// Module operations
Module* module_create(const char* name, const char* version_str) {
    if (!name || !version_str) return NULL;
    
    Module* module = calloc(1, sizeof(Module));
    if (!module) return NULL;
    
    module->name = string_duplicate(name);
    module->version = version_parse(version_str);
    module->created_at = time(NULL);
    module->updated_at = module->created_at;
    
    return module;
}

void module_free(Module* module) {
    if (!module) return;
    
    free(module->name);
    version_free(module->version);
    free(module->description);
    free(module->homepage);
    free(module->repository);
    free(module->license);
    string_array_free(module->authors, module->author_count);
    string_array_free(module->keywords, module->keyword_count);
    
    dependency_list_free(module->dependencies);
    dependency_list_free(module->dev_dependencies);
    dependency_list_free(module->build_dependencies);
    
    free(module->build_script);
    string_array_free(module->source_dirs, module->source_dir_count);
    string_array_free(module->exclude_patterns, module->exclude_pattern_count);
    
    free(module->main_file);
    string_array_free(module->public_exports, module->export_count);
    string_array_free(module->private_symbols, module->private_symbol_count);
    
    string_array_free(module->features, module->feature_count);
    string_array_free(module->default_features, module->default_feature_count);
    
    free(module->minimum_goo_version);
    free(module->checksum);
    free(module->root_path);
    free(module->cache_path);
    
    if (module->workspace_members) {
        for (size_t i = 0; i < module->workspace_member_count; i++) {
            module_free(module->workspace_members[i]);
        }
        free(module->workspace_members);
    }
    
    free(module);
}

Module* module_load_from_file(const char* filepath) {
    if (!filepath || !file_exists(filepath)) return NULL;
    
    char* content = read_file_contents(filepath);
    if (!content) return NULL;
    
    Module* module = NULL;
    if (strstr(filepath, ".toml")) {
        module = module_parse_toml(content);
    } else if (strstr(filepath, ".json")) {
        module = module_parse_json(content);
    }
    
    free(content);
    return module;
}

bool module_save_to_file(const Module* module, const char* filepath) {
    if (!module || !filepath) return false;
    
    char* content = NULL;
    if (strstr(filepath, ".toml")) {
        content = module_to_toml(module);
    } else if (strstr(filepath, ".json")) {
        content = module_to_json(module);
    }
    
    if (!content) return false;
    
    bool result = write_file_contents(filepath, content);
    free(content);
    return result;
}

Module* module_load_from_directory(const char* directory) {
    if (!directory || !directory_exists(directory)) return NULL;
    
    // Try to find goo.toml, then goo.json
    char filepath[1024];
    
    snprintf(filepath, sizeof(filepath), "%s/goo.toml", directory);
    if (file_exists(filepath)) {
        Module* module = module_load_from_file(filepath);
        if (module) {
            module->root_path = string_duplicate(directory);
        }
        return module;
    }
    
    snprintf(filepath, sizeof(filepath), "%s/goo.json", directory);
    if (file_exists(filepath)) {
        Module* module = module_load_from_file(filepath);
        if (module) {
            module->root_path = string_duplicate(directory);
        }
        return module;
    }
    
    return NULL;
}

bool module_validate(const Module* module, char** error_message) {
    if (!module) {
        if (error_message) *error_message = string_duplicate("Module is null");
        return false;
    }
    
    if (!module->name || strlen(module->name) == 0) {
        if (error_message) *error_message = string_duplicate("Module name is required");
        return false;
    }
    
    if (!module->version) {
        if (error_message) *error_message = string_duplicate("Module version is required");
        return false;
    }
    
    // Validate name format (lowercase, alphanumeric, dashes, underscores)
    for (const char* c = module->name; *c; c++) {
        if (!isalnum(*c) && *c != '-' && *c != '_') {
            if (error_message) {
                char* msg = malloc(256);
                snprintf(msg, 256, "Invalid character '%c' in module name", *c);
                *error_message = msg;
            }
            return false;
        }
    }
    
    return true;
}

// Simplified TOML parsing (basic implementation)
Module* module_parse_toml(const char* toml_content) {
    if (!toml_content) return NULL;
    
    Module* module = calloc(1, sizeof(Module));
    if (!module) return NULL;
    
    // Very basic TOML parsing - in a real implementation, use a proper TOML library
    char* content_copy = string_duplicate(toml_content);
    char* line = strtok(content_copy, "\n");
    
    while (line) {
        // Skip empty lines and comments
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '\0' || *line == '#') {
            line = strtok(NULL, "\n");
            continue;
        }
        
        // Parse key = value pairs
        char* equals = strchr(line, '=');
        if (equals) {
            *equals = '\0';
            char* key = line;
            char* value = equals + 1;
            
            // Trim whitespace
            while (*key == ' ' || *key == '\t') key++;
            while (*value == ' ' || *value == '\t') value++;
            
            // Remove quotes from value
            if (*value == '"') {
                value++;
                char* end_quote = strrchr(value, '"');
                if (end_quote) *end_quote = '\0';
            }
            
            // Parse known keys
            if (strcmp(key, "name") == 0) {
                module->name = string_duplicate(value);
            } else if (strcmp(key, "version") == 0) {
                module->version = version_parse(value);
            } else if (strcmp(key, "description") == 0) {
                module->description = string_duplicate(value);
            } else if (strcmp(key, "license") == 0) {
                module->license = string_duplicate(value);
            }
        }
        
        line = strtok(NULL, "\n");
    }
    
    free(content_copy);
    
    // Set default values
    if (!module->name) module->name = string_duplicate("unnamed");
    if (!module->version) module->version = version_parse("0.1.0");
    module->created_at = time(NULL);
    module->updated_at = module->created_at;
    
    return module;
}

char* module_to_toml(const Module* module) {
    if (!module) return NULL;
    
    size_t buffer_size = 4096;
    char* buffer = malloc(buffer_size);
    size_t written = 0;
    
    written += snprintf(buffer + written, buffer_size - written,
        "[package]\n"
        "name = \"%s\"\n",
        module->name ? module->name : "unnamed");
    
    if (module->version) {
        char* version_str = version_to_string(module->version);
        written += snprintf(buffer + written, buffer_size - written,
            "version = \"%s\"\n", version_str);
        free(version_str);
    }
    
    if (module->description) {
        written += snprintf(buffer + written, buffer_size - written,
            "description = \"%s\"\n", module->description);
    }
    
    if (module->license) {
        written += snprintf(buffer + written, buffer_size - written,
            "license = \"%s\"\n", module->license);
    }
    
    // Add dependencies section
    if (module->dependencies) {
        written += snprintf(buffer + written, buffer_size - written, "\n[dependencies]\n");
        for (Dependency* dep = module->dependencies; dep; dep = dep->next) {
            char* constraint_str = version_constraint_to_string(dep->constraint);
            written += snprintf(buffer + written, buffer_size - written,
                "%s = \"%s\"\n", dep->name, constraint_str ? constraint_str : "*");
            free(constraint_str);
        }
    }
    
    return buffer;
}

// Placeholder implementations
Module* module_parse_json(const char* json_content) {
    // TODO: Implement JSON parsing
    return NULL;
}

char* module_to_json(const Module* module) {
    // TODO: Implement JSON serialization
    return NULL;
}

char* version_constraint_to_string(const VersionConstraint* constraint) {
    if (!constraint) return string_duplicate("*");
    
    switch (constraint->type) {
        case VERSION_ANY:
            return string_duplicate("*");
        case VERSION_LATEST:
            return string_duplicate("latest");
        case VERSION_EXACT:
            return version_to_string(constraint->min_version);
        case VERSION_COMPATIBLE:
            if (constraint->min_version) {
                char* version_str = version_to_string(constraint->min_version);
                char* result = malloc(strlen(version_str) + 2);
                sprintf(result, "^%s", version_str);
                free(version_str);
                return result;
            }
            break;
        case VERSION_TILDE:
            if (constraint->min_version) {
                char* version_str = version_to_string(constraint->min_version);
                char* result = malloc(strlen(version_str) + 2);
                sprintf(result, "~%s", version_str);
                free(version_str);
                return result;
            }
            break;
        case VERSION_RANGE:
            // TODO: Implement range string representation
            return string_duplicate("*");
    }
    
    return string_duplicate("*");
}

// Utility functions
char* string_duplicate(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* copy = malloc(len + 1);
    if (copy) strcpy(copy, str);
    return copy;
}

char** string_array_create(size_t count) {
    return calloc(count, sizeof(char*));
}

void string_array_free(char** array, size_t count) {
    if (!array) return;
    for (size_t i = 0; i < count; i++) {
        free(array[i]);
    }
    free(array);
}

bool file_exists(const char* path) {
    if (!path) return false;
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool directory_exists(const char* path) {
    if (!path) return false;
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

char* read_file_contents(const char* path) {
    if (!path) return NULL;
    
    FILE* file = fopen(path, "r");
    if (!file) return NULL;
    
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    char* content = malloc(size + 1);
    if (!content) {
        fclose(file);
        return NULL;
    }
    
    fread(content, 1, size, file);
    content[size] = '\0';
    fclose(file);
    
    return content;
}

bool write_file_contents(const char* path, const char* content) {
    if (!path || !content) return false;
    
    FILE* file = fopen(path, "w");
    if (!file) return false;
    
    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, file);
    fclose(file);
    
    return written == len;
}