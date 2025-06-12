#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Channel type compatibility and pattern checking

// Check if two channel patterns are compatible for communication
int channel_patterns_compatible(ChannelPattern pattern1, ChannelPattern pattern2) {
    // Compatible patterns for communication
    switch (pattern1) {
        case CHAN_PATTERN_BASIC:
            // Basic channels can communicate with any pattern
            return 1;
            
        case CHAN_PATTERN_PUB:
            // Publishers can only send to subscribers
            return pattern2 == CHAN_PATTERN_SUB || pattern2 == CHAN_PATTERN_BASIC;
            
        case CHAN_PATTERN_SUB:
            // Subscribers can only receive from publishers
            return pattern2 == CHAN_PATTERN_PUB || pattern2 == CHAN_PATTERN_BASIC;
            
        case CHAN_PATTERN_REQ:
            // Request channels connect to reply channels
            return pattern2 == CHAN_PATTERN_REP || pattern2 == CHAN_PATTERN_BASIC;
            
        case CHAN_PATTERN_REP:
            // Reply channels connect to request channels
            return pattern2 == CHAN_PATTERN_REQ || pattern2 == CHAN_PATTERN_BASIC;
            
        case CHAN_PATTERN_PUSH:
            // Push channels connect to pull channels
            return pattern2 == CHAN_PATTERN_PULL || pattern2 == CHAN_PATTERN_BASIC;
            
        case CHAN_PATTERN_PULL:
            // Pull channels connect to push channels
            return pattern2 == CHAN_PATTERN_PUSH || pattern2 == CHAN_PATTERN_BASIC;
    }
    
    return 0;
}

// Get the communication direction for a channel pattern
typedef enum {
    CHANNEL_SEND_ONLY,
    CHANNEL_RECV_ONLY,
    CHANNEL_BIDIRECTIONAL
} ChannelDirection;

ChannelDirection get_channel_direction(ChannelPattern pattern) {
    switch (pattern) {
        case CHAN_PATTERN_PUB:
        case CHAN_PATTERN_PUSH:
            return CHANNEL_SEND_ONLY;
            
        case CHAN_PATTERN_SUB:
        case CHAN_PATTERN_PULL:
            return CHANNEL_RECV_ONLY;
            
        case CHAN_PATTERN_BASIC:
        case CHAN_PATTERN_REQ:
        case CHAN_PATTERN_REP:
        default:
            return CHANNEL_BIDIRECTIONAL;
    }
}

// Check if a channel operation is valid for the channel type
int check_channel_operation_valid(TypeChecker* checker, Type* channel_type, int is_send, Position pos) {
    if (!checker || !channel_type || channel_type->kind != TYPE_CHANNEL) {
        type_error(checker, pos, "Channel operation on non-channel type");
        return 0;
    }
    
    ChannelPattern pattern = channel_type->data.channel.pattern;
    ChannelDirection direction = get_channel_direction(pattern);
    
    if (is_send) {
        // Sending operation
        if (direction == CHANNEL_RECV_ONLY) {
            type_error(checker, pos,
                      "Cannot send on receive-only channel with pattern %s",
                      channel_pattern_string(pattern));
            return 0;
        }
    } else {
        // Receiving operation
        if (direction == CHANNEL_SEND_ONLY) {
            type_error(checker, pos,
                      "Cannot receive from send-only channel with pattern %s",
                      channel_pattern_string(pattern));
            return 0;
        }
    }
    
    return 1;
}

// Check channel send operation (ch <- value)
Type* type_check_channel_send(TypeChecker* checker, ASTNode* channel_expr, ASTNode* value_expr, Position pos) {
    if (!checker || !channel_expr || !value_expr) return NULL;
    
    Type* channel_type = type_check_expression(checker, channel_expr);
    Type* value_type = type_check_expression(checker, value_expr);
    
    if (!channel_type || !value_type) return NULL;
    
    // Check that left side is a channel
    if (channel_type->kind != TYPE_CHANNEL) {
        type_error(checker, pos, "Cannot send to non-channel type %s", type_to_string(channel_type));
        return NULL;
    }
    
    // Check channel operation validity
    if (!check_channel_operation_valid(checker, channel_type, 1, pos)) {
        return NULL;
    }
    
    // Check value type compatibility
    Type* element_type = channel_type->data.channel.element_type;
    if (!type_compatible(value_type, element_type)) {
        type_error(checker, pos,
                  "Cannot send %s to channel of %s",
                  type_to_string(value_type), type_to_string(element_type));
        return NULL;
    }
    
    // Channel send returns void
    return type_checker_get_builtin(checker, TYPE_VOID);
}

// Check channel receive operation (<-ch)
Type* type_check_channel_receive(TypeChecker* checker, ASTNode* channel_expr, Position pos) {
    if (!checker || !channel_expr) return NULL;
    
    Type* channel_type = type_check_expression(checker, channel_expr);
    if (!channel_type) return NULL;
    
    // Check that operand is a channel
    if (channel_type->kind != TYPE_CHANNEL) {
        type_error(checker, pos, "Cannot receive from non-channel type %s", type_to_string(channel_type));
        return NULL;
    }
    
    // Check channel operation validity
    if (!check_channel_operation_valid(checker, channel_type, 0, pos)) {
        return NULL;
    }
    
    // Channel receive returns the element type
    return channel_type->data.channel.element_type;
}

// Check channel assignment compatibility
int check_channel_assignment_compatibility(TypeChecker* checker, Type* target_type, Type* source_type, Position pos) {
    if (!checker || !target_type || !source_type) return 0;
    
    if (target_type->kind != TYPE_CHANNEL || source_type->kind != TYPE_CHANNEL) {
        return type_compatible(source_type, target_type);
    }
    
    // Both are channels - check element type and pattern compatibility
    Type* target_element = target_type->data.channel.element_type;
    Type* source_element = source_type->data.channel.element_type;
    
    if (!type_equals(target_element, source_element)) {
        type_error(checker, pos,
                  "Channel element type mismatch: expected %s, got %s",
                  type_to_string(target_element), type_to_string(source_element));
        return 0;
    }
    
    ChannelPattern target_pattern = target_type->data.channel.pattern;
    ChannelPattern source_pattern = source_type->data.channel.pattern;
    
    if (!channel_patterns_compatible(target_pattern, source_pattern)) {
        type_error(checker, pos,
                  "Incompatible channel patterns: cannot assign %s channel to %s channel",
                  channel_pattern_string(source_pattern),
                  channel_pattern_string(target_pattern));
        return 0;
    }
    
    return 1;
}

// Check select statement case compatibility
int check_select_case_compatibility(TypeChecker* checker, ASTNode* case_expr, Position pos __attribute__((unused))) {
    if (!checker || !case_expr) return 0;
    
    // TODO: Implement select statement case checking
    // Each case should be a channel operation (send or receive)
    
    return 1;
}

// Helper function to get string representation of channel pattern
const char* channel_pattern_string(ChannelPattern pattern) {
    switch (pattern) {
        case CHAN_PATTERN_BASIC: return "basic";
        case CHAN_PATTERN_PUB: return "pub";
        case CHAN_PATTERN_SUB: return "sub";
        case CHAN_PATTERN_REQ: return "req";
        case CHAN_PATTERN_REP: return "rep";
        case CHAN_PATTERN_PUSH: return "push";
        case CHAN_PATTERN_PULL: return "pull";
        default: return "unknown";
    }
}

// Check if a channel type allows buffering
int channel_allows_buffering(ChannelPattern pattern) {
    switch (pattern) {
        case CHAN_PATTERN_BASIC:
        case CHAN_PATTERN_PUSH:
        case CHAN_PATTERN_PULL:
            return 1;  // These patterns can be buffered
            
        case CHAN_PATTERN_PUB:
        case CHAN_PATTERN_SUB:
        case CHAN_PATTERN_REQ:
        case CHAN_PATTERN_REP:
            return 0;  // These patterns are typically unbuffered
    }
    
    return 1;
}

// Check channel buffer size constraints
int check_channel_buffer_constraints(TypeChecker* checker, Type* channel_type, int buffer_size, Position pos) {
    if (!checker || !channel_type || channel_type->kind != TYPE_CHANNEL) return 0;
    
    ChannelPattern pattern = channel_type->data.channel.pattern;
    
    if (buffer_size > 0 && !channel_allows_buffering(pattern)) {
        type_warning(checker, pos,
                    "Channel pattern %s typically does not use buffering",
                    channel_pattern_string(pattern));
    }
    
    if (buffer_size < 0) {
        type_error(checker, pos, "Channel buffer size cannot be negative");
        return 0;
    }
    
    return 1;
}

// Check channel endpoint configuration for network patterns
int check_channel_endpoint(TypeChecker* checker, Type* channel_type, const char* endpoint, Position pos) {
    if (!checker || !channel_type || channel_type->kind != TYPE_CHANNEL) return 0;
    
    ChannelPattern pattern = channel_type->data.channel.pattern;
    
    // Network patterns require endpoints
    if ((pattern == CHAN_PATTERN_PUB || pattern == CHAN_PATTERN_SUB ||
         pattern == CHAN_PATTERN_REQ || pattern == CHAN_PATTERN_REP ||
         pattern == CHAN_PATTERN_PUSH || pattern == CHAN_PATTERN_PULL) && !endpoint) {
        type_warning(checker, pos,
                    "Channel pattern %s typically requires network endpoint configuration",
                    channel_pattern_string(pattern));
    }
    
    // Basic channels shouldn't have endpoints
    if (pattern == CHAN_PATTERN_BASIC && endpoint) {
        type_warning(checker, pos,
                    "Basic channels do not need network endpoints");
    }
    
    return 1;
}

// Validate channel type creation
int validate_channel_type_creation(TypeChecker* checker, Type* element_type, ChannelPattern pattern, 
                                 const char* endpoint, int buffer_size, Position pos) {
    if (!checker || !element_type) return 0;
    
    // Check if element type is valid for channels
    if (element_type->kind == TYPE_VOID) {
        type_error(checker, pos, "Channel element type cannot be void");
        return 0;
    }
    
    // Create temporary channel type for validation
    Type* channel_type = type_channel(element_type, pattern);
    if (!channel_type) return 0;
    
    int result = check_channel_buffer_constraints(checker, channel_type, buffer_size, pos) &&
                 check_channel_endpoint(checker, channel_type, endpoint, pos);
    
    type_free(channel_type);
    return result;
}