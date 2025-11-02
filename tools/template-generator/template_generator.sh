#!/bin/bash

# Template Generator for Goo Projects
# Generates project scaffolding from predefined templates

set -e

# Configuration
TEMPLATES_DIR="$(dirname "$0")/../../templates"
SCRIPT_DIR="$(dirname "$0")"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m' # No Color

# Helper functions
log_info() {
    echo -e "${BLUE}ℹ️  $1${NC}"
}

log_success() {
    echo -e "${GREEN}✅ $1${NC}"
}

log_warning() {
    echo -e "${YELLOW}⚠️  $1${NC}"
}

log_error() {
    echo -e "${RED}❌ $1${NC}"
}

log_header() {
    echo -e "${CYAN}${BOLD}🚀 $1${NC}"
}

# Show usage information
show_usage() {
    echo -e "${BOLD}Goo Template Generator${NC}"
    echo ""
    echo "Usage: $0 [command] [options]"
    echo ""
    echo "Commands:"
    echo "  list                List available templates"
    echo "  generate <template> Generate project from template"
    echo "  info <template>     Show template information"
    echo "  validate <template> Validate template configuration"
    echo ""
    echo "Options:"
    echo "  -n, --name <name>   Project name"
    echo "  -o, --output <dir>  Output directory (default: current directory)"
    echo "  -i, --interactive   Interactive mode for template variables"
    echo "  -v, --verbose       Verbose output"
    echo "  -h, --help          Show this help"
    echo ""
    echo "Examples:"
    echo "  $0 list"
    echo "  $0 generate console-app -n my-app"
    echo "  $0 generate web-app -n my-web-app -i"
    echo "  $0 info microservice"
}

# List available templates
list_templates() {
    log_header "Available Templates"
    echo ""
    
    if [ ! -d "$TEMPLATES_DIR" ]; then
        log_error "Templates directory not found: $TEMPLATES_DIR"
        return 1
    fi
    
    for template_dir in "$TEMPLATES_DIR"/*; do
        if [ -d "$template_dir" ]; then
            template_name=$(basename "$template_dir")
            template_json="$template_dir/template.json"
            
            if [ -f "$template_json" ]; then
                # Extract template info using jq if available, fallback to basic parsing
                if command -v jq >/dev/null 2>&1; then
                    name=$(jq -r '.name' "$template_json" 2>/dev/null || echo "$template_name")
                    description=$(jq -r '.description' "$template_json" 2>/dev/null || echo "No description")
                    category=$(jq -r '.category' "$template_json" 2>/dev/null || echo "general")
                else
                    name="$template_name"
                    description="Template: $template_name"
                    category="general"
                fi
                
                echo -e "${BOLD}$template_name${NC} - $name"
                echo -e "  📝 $description"
                echo -e "  🏷️  Category: $category"
                echo ""
            else
                log_warning "Template $template_name missing template.json"
            fi
        fi
    done
}

# Show template information
show_template_info() {
    local template_name="$1"
    local template_dir="$TEMPLATES_DIR/$template_name"
    local template_json="$template_dir/template.json"
    
    if [ ! -f "$template_json" ]; then
        log_error "Template not found: $template_name"
        return 1
    fi
    
    log_header "Template Information: $template_name"
    echo ""
    
    if command -v jq >/dev/null 2>&1; then
        # Use jq for nice formatting
        echo -e "${BOLD}Name:${NC} $(jq -r '.name' "$template_json")"
        echo -e "${BOLD}Description:${NC} $(jq -r '.description' "$template_json")"
        echo -e "${BOLD}Category:${NC} $(jq -r '.category' "$template_json")"
        echo -e "${BOLD}Version:${NC} $(jq -r '.version' "$template_json")"
        echo -e "${BOLD}Author:${NC} $(jq -r '.author' "$template_json")"
        
        echo ""
        echo -e "${BOLD}Features:${NC}"
        jq -r '.features[]' "$template_json" | sed 's/^/  • /'
        
        echo ""
        echo -e "${BOLD}Variables:${NC}"
        jq -r '.variables[] | "  • \(.name) (\(.type)) - \(.description)"' "$template_json"
        
        echo ""
        echo -e "${BOLD}Requirements:${NC}"
        jq -r '.requirements | to_entries[] | "  • \(.key): \(.value)"' "$template_json"
    else
        # Fallback without jq
        echo "Template: $template_name"
        echo "Location: $template_dir"
        echo ""
        log_info "Install 'jq' for detailed template information"
    fi
}

# Validate template
validate_template() {
    local template_name="$1"
    local template_dir="$TEMPLATES_DIR/$template_name"
    local template_json="$template_dir/template.json"
    
    log_header "Validating Template: $template_name"
    echo ""
    
    if [ ! -d "$template_dir" ]; then
        log_error "Template directory not found: $template_dir"
        return 1
    fi
    
    if [ ! -f "$template_json" ]; then
        log_error "template.json not found in $template_dir"
        return 1
    fi
    
    # Validate JSON syntax
    if command -v jq >/dev/null 2>&1; then
        if ! jq empty "$template_json" >/dev/null 2>&1; then
            log_error "Invalid JSON syntax in template.json"
            return 1
        fi
        log_success "JSON syntax is valid"
    else
        log_warning "Install 'jq' for JSON validation"
    fi
    
    # Check required fields
    local required_fields=("name" "description" "version" "structure")
    for field in "${required_fields[@]}"; do
        if command -v jq >/dev/null 2>&1; then
            if [ "$(jq -r ".$field" "$template_json")" = "null" ]; then
                log_error "Missing required field: $field"
                return 1
            else
                log_success "Field '$field' is present"
            fi
        fi
    done
    
    log_success "Template validation passed"
}

# Process template variables
process_variables() {
    local template_json="$1"
    local output_dir="$2"
    local interactive="$3"
    
    if [ ! -f "$template_json" ]; then
        return 1
    fi
    
    # Create variables file for substitution
    local vars_file="$output_dir/.template_vars"
    
    if command -v jq >/dev/null 2>&1; then
        # Extract variables from template
        local variables
        variables=$(jq -r '.variables[]?' "$template_json" 2>/dev/null || echo "")
        
        if [ -n "$variables" ]; then
            echo "# Template Variables" > "$vars_file"
            
            # Process each variable
            while IFS= read -r var_json; do
                if [ -n "$var_json" ] && [ "$var_json" != "null" ]; then
                    local var_name var_desc var_type var_required var_default
                    
                    var_name=$(echo "$var_json" | jq -r '.name')
                    var_desc=$(echo "$var_json" | jq -r '.description')
                    var_type=$(echo "$var_json" | jq -r '.type')
                    var_required=$(echo "$var_json" | jq -r '.required')
                    var_default=$(echo "$var_json" | jq -r '.default')
                    
                    local value="$var_default"
                    
                    if [ "$interactive" = "true" ]; then
                        echo ""
                        echo -e "${BOLD}$var_name${NC} ($var_type)"
                        echo "  $var_desc"
                        if [ "$var_default" != "null" ]; then
                            echo "  Default: $var_default"
                        fi
                        
                        if [ "$var_required" = "true" ]; then
                            echo -n "  Value (required): "
                        else
                            echo -n "  Value [press enter for default]: "
                        fi
                        
                        read -r user_input
                        
                        if [ -n "$user_input" ]; then
                            value="$user_input"
                        elif [ "$var_required" = "true" ] && [ "$var_default" = "null" ]; then
                            log_error "Required variable '$var_name' cannot be empty"
                            return 1
                        fi
                    fi
                    
                    # Add to variables file
                    echo "$var_name=$value" >> "$vars_file"
                fi
            done < <(jq -c '.variables[]?' "$template_json" 2>/dev/null || echo "")
        fi
    fi
    
    echo "$vars_file"
}

# Substitute variables in files
substitute_variables() {
    local vars_file="$1"
    local target_dir="$2"
    
    if [ ! -f "$vars_file" ]; then
        return 0
    fi
    
    # Source the variables
    source "$vars_file"
    
    # Find all text files and substitute variables
    find "$target_dir" -type f \( -name "*.goo" -o -name "*.json" -o -name "*.yaml" -o -name "*.yml" -o -name "*.md" -o -name "*.txt" -o -name "*.sh" -o -name "Dockerfile" -o -name "*.proto" \) -exec bash -c '
        for file; do
            # Use envsubst if available, otherwise use sed
            if command -v envsubst >/dev/null 2>&1; then
                envsubst < "$file" > "$file.tmp" && mv "$file.tmp" "$file"
            else
                # Basic variable substitution with sed
                while IFS="=" read -r var value; do
                    if [ -n "$var" ] && [ "${var:0:1}" != "#" ]; then
                        sed -i.bak "s/{{$var}}/$value/g" "$file" 2>/dev/null || true
                        rm -f "$file.bak"
                    fi
                done < "'"$vars_file"'"
            fi
        done
    ' bash {} +
    
    # Clean up
    rm -f "$vars_file"
}

# Execute hooks
execute_hooks() {
    local template_json="$1"
    local hook_type="$2"
    local output_dir="$3"
    
    if [ ! -f "$template_json" ]; then
        return 0
    fi
    
    if command -v jq >/dev/null 2>&1; then
        local hooks
        hooks=$(jq -r ".hooks.$hook_type[]?" "$template_json" 2>/dev/null || echo "")
        
        if [ -n "$hooks" ]; then
            log_info "Executing $hook_type hooks..."
            
            while IFS= read -r hook_cmd; do
                if [ -n "$hook_cmd" ] && [ "$hook_cmd" != "null" ]; then
                    log_info "Running: $hook_cmd"
                    cd "$output_dir" && eval "$hook_cmd"
                fi
            done <<< "$hooks"
        fi
    fi
}

# Copy template files
copy_template_files() {
    local template_dir="$1"
    local output_dir="$2"
    
    # Copy all files except template.json
    find "$template_dir" -type f ! -name "template.json" -exec bash -c '
        for file; do
            rel_path="${file#'"$template_dir"'/}"
            target_dir="'"$output_dir"'/$(dirname "$rel_path")"
            mkdir -p "$target_dir"
            cp "$file" "'"$output_dir"'/$rel_path"
        done
    ' bash {} +
}

# Generate project from template
generate_project() {
    local template_name="$1"
    local project_name="$2"
    local output_dir="$3"
    local interactive="$4"
    
    local template_dir="$TEMPLATES_DIR/$template_name"
    local template_json="$template_dir/template.json"
    
    if [ ! -f "$template_json" ]; then
        log_error "Template not found: $template_name"
        return 1
    fi
    
    # Set project name if provided
    if [ -n "$project_name" ]; then
        export PROJECT_NAME="$project_name"
        export SERVICE_NAME="$project_name"  # For microservice template
    fi
    
    local full_output_dir="$output_dir/$project_name"
    
    log_header "Generating Project from Template: $template_name"
    echo ""
    
    # Create output directory
    if [ -d "$full_output_dir" ]; then
        log_warning "Directory $full_output_dir already exists"
        echo -n "Overwrite? [y/N]: "
        read -r confirm
        if [ "$confirm" != "y" ] && [ "$confirm" != "Y" ]; then
            log_info "Cancelled"
            return 0
        fi
        rm -rf "$full_output_dir"
    fi
    
    mkdir -p "$full_output_dir"
    
    # Execute pre-generation hooks
    execute_hooks "$template_json" "pre_generate" "$output_dir"
    
    # Process template variables
    local vars_file
    vars_file=$(process_variables "$template_json" "$full_output_dir" "$interactive")
    
    # Copy template files
    log_info "Copying template files..."
    copy_template_files "$template_dir" "$full_output_dir"
    
    # Substitute variables in files
    if [ -f "$vars_file" ]; then
        log_info "Substituting template variables..."
        substitute_variables "$vars_file" "$full_output_dir"
    fi
    
    # Execute post-generation hooks
    execute_hooks "$template_json" "post_generate" "$output_dir"
    
    log_success "Project generated successfully!"
    echo ""
    echo "Project location: $full_output_dir"
    echo ""
    
    # Show next steps if available
    if command -v jq >/dev/null 2>&1; then
        local features
        features=$(jq -r '.features[]?' "$template_json" 2>/dev/null || echo "")
        if [ -n "$features" ]; then
            echo -e "${BOLD}Included Features:${NC}"
            while IFS= read -r feature; do
                if [ -n "$feature" ] && [ "$feature" != "null" ]; then
                    echo "  • $feature"
                fi
            done <<< "$features"
        fi
    fi
}

# Main function
main() {
    local command=""
    local template_name=""
    local project_name=""
    local output_dir="."
    local interactive=false
    local verbose=false
    
    # Parse command line arguments
    while [[ $# -gt 0 ]]; do
        case $1 in
            list)
                command="list"
                shift
                ;;
            generate)
                command="generate"
                template_name="$2"
                shift 2
                ;;
            info)
                command="info"
                template_name="$2"
                shift 2
                ;;
            validate)
                command="validate"
                template_name="$2"
                shift 2
                ;;
            -n|--name)
                project_name="$2"
                shift 2
                ;;
            -o|--output)
                output_dir="$2"
                shift 2
                ;;
            -i|--interactive)
                interactive=true
                shift
                ;;
            -v|--verbose)
                verbose=true
                shift
                ;;
            -h|--help)
                show_usage
                exit 0
                ;;
            *)
                log_error "Unknown option: $1"
                show_usage
                exit 1
                ;;
        esac
    done
    
    # Execute command
    case $command in
        list)
            list_templates
            ;;
        generate)
            if [ -z "$template_name" ]; then
                log_error "Template name required for generate command"
                show_usage
                exit 1
            fi
            if [ -z "$project_name" ]; then
                log_error "Project name required (-n/--name)"
                show_usage
                exit 1
            fi
            generate_project "$template_name" "$project_name" "$output_dir" "$interactive"
            ;;
        info)
            if [ -z "$template_name" ]; then
                log_error "Template name required for info command"
                show_usage
                exit 1
            fi
            show_template_info "$template_name"
            ;;
        validate)
            if [ -z "$template_name" ]; then
                log_error "Template name required for validate command"
                show_usage
                exit 1
            fi
            validate_template "$template_name"
            ;;
        *)
            show_usage
            ;;
    esac
}

# Run main function
main "$@"