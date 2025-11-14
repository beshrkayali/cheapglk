#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "glk.h"
#include "cheapglk.h"
#include "glk_llm.h"

glk_llm_config_t gli_llm_config;
glk_llm_context_t gli_llm_context;

void gli_llm_init(void)
{
    memset(&gli_llm_config, 0, sizeof(gli_llm_config));
    memset(&gli_llm_context, 0, sizeof(gli_llm_context));
    
    gli_llm_config.enabled = 0;
    gli_llm_config.context_lines = 10;
    gli_llm_config.timeout_ms = 5000;
    gli_llm_config.echo_interpretation = 1;

    char *config_file = getenv("GLK_LLM_CONFIG");
    if (config_file) {
        gli_llm_load_config(config_file);
    } else {
        char default_config[512];
        snprintf(default_config, sizeof(default_config), "%s/.glk_llm.conf", getenv("HOME"));
        gli_llm_load_config(default_config);
    }
}

void gli_llm_load_config(const char *config_file)
{
    FILE *f = fopen(config_file, "r");
    if (!f) {
        return;
    }
    
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        
        if (line[0] == '#' || line[0] == '\0') continue;
        
        char *eq = strchr(line, '=');
        if (!eq) continue;
        
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        
        // Trim leading whitespace from key
        while (*key == ' ' || *key == '\t') key++;
        // Trim trailing whitespace from key
        char *key_end = eq - 1;
        while (key_end > key && (*key_end == ' ' || *key_end == '\t')) {
            *key_end = '\0';
            key_end--;
        }
        
        // Trim leading whitespace from value
        while (*value == ' ' || *value == '\t') value++;
        // Trim trailing whitespace from value
        char *value_end = value + strlen(value) - 1;
        while (value_end > value && (*value_end == ' ' || *value_end == '\t')) {
            *value_end = '\0';
            value_end--;
        }
        
        if (strcmp(key, "enabled") == 0) {
            gli_llm_config.enabled = atoi(value);
        } else if (strcmp(key, "api_endpoint") == 0) {
            strncpy(gli_llm_config.api_endpoint, value, sizeof(gli_llm_config.api_endpoint) - 1);
        } else if (strcmp(key, "api_key") == 0) {
            strncpy(gli_llm_config.api_key, value, sizeof(gli_llm_config.api_key) - 1);
        } else if (strcmp(key, "model") == 0) {
            strncpy(gli_llm_config.model, value, sizeof(gli_llm_config.model) - 1);
        } else if (strcmp(key, "context_lines") == 0) {
            gli_llm_config.context_lines = atoi(value);
            if (gli_llm_config.context_lines > GLK_LLM_CONTEXT_LINES)
                gli_llm_config.context_lines = GLK_LLM_CONTEXT_LINES;
        } else if (strcmp(key, "timeout_ms") == 0) {
            gli_llm_config.timeout_ms = atoi(value);
        } else if (strcmp(key, "echo_interpretation") == 0) {
            gli_llm_config.echo_interpretation = atoi(value);
        }
    }
    
    fclose(f);
}

void gli_llm_add_context(const char *text)
{
    if (!text || !*text) return;
    
    int pos = gli_llm_context.position;
    strncpy(gli_llm_context.lines[pos], text, sizeof(gli_llm_context.lines[pos]) - 1);
    gli_llm_context.lines[pos][sizeof(gli_llm_context.lines[pos]) - 1] = '\0';
    
    gli_llm_context.position = (pos + 1) % GLK_LLM_CONTEXT_LINES;
    if (gli_llm_context.count < GLK_LLM_CONTEXT_LINES) {
        gli_llm_context.count++;
    }
}

static void escape_json_string(const char *input, char *output, size_t max_len)
{
    size_t i = 0, j = 0;
    while (input[i] && j < max_len - 2) {
        switch (input[i]) {
            case '"':
            case '\\':
                if (j < max_len - 3) {
                    output[j++] = '\\';
                    output[j++] = input[i];
                }
                break;
            case '\n':
                if (j < max_len - 3) {
                    output[j++] = '\\';
                    output[j++] = 'n';
                }
                break;
            case '\r':
                if (j < max_len - 3) {
                    output[j++] = '\\';
                    output[j++] = 'r';
                }
                break;
            case '\t':
                if (j < max_len - 3) {
                    output[j++] = '\\';
                    output[j++] = 't';
                }
                break;
            default:
                output[j++] = input[i];
                break;
        }
        i++;
    }
    output[j] = '\0';
}

static char* parse_json_response(const char *response)
{
    const char *content_start = strstr(response, "\"content\"");
    if (!content_start) return NULL;
    
    const char *value_start = strchr(content_start, ':');
    if (!value_start) return NULL;
    value_start++;
    
    while (*value_start == ' ' || *value_start == '\t' || *value_start == '\n') 
        value_start++;
    
    if (*value_start != '"') return NULL;
    value_start++;
    
    const char *value_end = value_start;
    while (*value_end) {
        if (*value_end == '"' && *(value_end - 1) != '\\') {
            break;
        }
        value_end++;
    }
    
    if (!*value_end) return NULL;
    
    size_t len = value_end - value_start;
    char *result = malloc(len + 1);
    if (!result) return NULL;
    
    memcpy(result, value_start, len);
    result[len] = '\0';
    
    return result;
}

static int parse_url(const char *url, char *protocol, char *host, int *port, char *path)
{
    const char *p = url;
    
    if (strncmp(p, "https://", 8) == 0) {
        strcpy(protocol, "https");
        p += 8;
        *port = 443;
    } else if (strncmp(p, "http://", 7) == 0) {
        strcpy(protocol, "http");
        p += 7;
        *port = 80;
    } else {
        return 0;
    }
    
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    
    if (colon && (!slash || colon < slash)) {
        size_t host_len = colon - p;
        memcpy(host, p, host_len);
        host[host_len] = '\0';
        *port = atoi(colon + 1);
        p = slash ? slash : (p + strlen(p));
    } else {
        size_t host_len = slash ? (slash - p) : strlen(p);
        memcpy(host, p, host_len);
        host[host_len] = '\0';
        p = slash ? slash : (p + strlen(p));
    }
    
    strcpy(path, *p ? p : "/");
    
    return 1;
}

int gli_llm_process_input(const char *input, char *output, glui32 maxlen)
{
    if (!gli_llm_config.enabled || !gli_llm_config.api_endpoint[0]) {
        strncpy(output, input, maxlen);
        output[maxlen - 1] = '\0';
        return 0;
    }
    
    char protocol[16], host[256], path[512];
    int port;
    
    if (!parse_url(gli_llm_config.api_endpoint, protocol, host, &port, path)) {
        strncpy(output, input, maxlen);
        output[maxlen - 1] = '\0';
        return 0;
    }
    
    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    
    if (getaddrinfo(host, port_str, &hints, &result) != 0) {
        strncpy(output, input, maxlen);
        output[maxlen - 1] = '\0';
        return 0;
    }
    
    int sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(result);
        strncpy(output, input, maxlen);
        output[maxlen - 1] = '\0';
        return 0;
    }
    
    if (connect(sock, result->ai_addr, result->ai_addrlen) < 0) {
        close(sock);
        freeaddrinfo(result);
        strncpy(output, input, maxlen);
        output[maxlen - 1] = '\0';
        return 0;
    }
    
    freeaddrinfo(result);
    
    SSL_CTX *ctx = NULL;
    SSL *ssl = NULL;
    
    if (strcmp(protocol, "https") == 0) {
        SSL_library_init();
        SSL_load_error_strings();
        ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) {
            close(sock);
            strncpy(output, input, maxlen);
            output[maxlen - 1] = '\0';
            return 0;
        }
        
        // Disable certificate verification for compatibility
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
        
        ssl = SSL_new(ctx);
        SSL_set_fd(ssl, sock);
        
        // Set SNI (Server Name Indication) - required by many servers
        SSL_set_tlsext_host_name(ssl, host);
        
        if (SSL_connect(ssl) <= 0) {
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            close(sock);
            strncpy(output, input, maxlen);
            output[maxlen - 1] = '\0';
            return 0;
        }
    }
    
    char escaped_input[1024];
    escape_json_string(input, escaped_input, sizeof(escaped_input));
    
    char context_json[4096] = "";
    if (gli_llm_config.context_lines > 0 && gli_llm_context.count > 0) {
        char temp[256];
        strcat(context_json, "Recent game output:\\n");
        
        int start = gli_llm_context.position - gli_llm_context.count;
        if (start < 0) start += GLK_LLM_CONTEXT_LINES;
        
        for (int i = 0; i < gli_llm_context.count && i < gli_llm_config.context_lines; i++) {
            int idx = (start + i) % GLK_LLM_CONTEXT_LINES;
            char escaped_context[512];
            escape_json_string(gli_llm_context.lines[idx], escaped_context, sizeof(escaped_context));
            snprintf(temp, sizeof(temp), "%s\\n", escaped_context);
            strncat(context_json, temp, sizeof(context_json) - strlen(context_json) - 1);
        }
        strcat(context_json, "\\n");
    }
    
    // Build comprehensive scene context with location awareness
    char scene_info[2048] = "";
    char current_location[256] = "";
    
    if (gli_llm_context.count > 0) {
        // Try to extract current location name (usually first line or has distinctive formatting)
        int recent_idx = (gli_llm_context.position - 1 + GLK_LLM_CONTEXT_LINES) % GLK_LLM_CONTEXT_LINES;
        const char *recent = gli_llm_context.lines[recent_idx];
        
        // Look for location name patterns (usually short lines at start of descriptions)
        if (recent[0] && strlen(recent) < 50 && !strstr(recent, "You") && !strstr(recent, "you")) {
            strncpy(current_location, recent, sizeof(current_location) - 1);
        }
        
        strncat(scene_info, "CURRENT LOCATION: ", sizeof(scene_info) - strlen(scene_info) - 1);
        if (current_location[0]) {
            strncat(scene_info, current_location, sizeof(scene_info) - strlen(scene_info) - 1);
        } else {
            strncat(scene_info, "(unknown)", sizeof(scene_info) - strlen(scene_info) - 1);
        }
        strncat(scene_info, "\n\nSCENE DESCRIPTION:\n", sizeof(scene_info) - strlen(scene_info) - 1);
        
        // Include last 5 lines of context for full scene understanding
        int lines_to_include = (gli_llm_context.count < 5) ? gli_llm_context.count : 5;
        int start_idx = gli_llm_context.position - lines_to_include;
        if (start_idx < 0) start_idx += GLK_LLM_CONTEXT_LINES;
        
        for (int i = 0; i < lines_to_include; i++) {
            int idx = (start_idx + i) % GLK_LLM_CONTEXT_LINES;
            if (gli_llm_context.lines[idx][0]) {
                strncat(scene_info, gli_llm_context.lines[idx], sizeof(scene_info) - strlen(scene_info) - 1);
                strncat(scene_info, "\n", sizeof(scene_info) - strlen(scene_info) - 1);
            }
        }
    }
    
    char system_prompt[8192];
    snprintf(system_prompt, sizeof(system_prompt),
        "You are an intelligent text adventure command interpreter. Your job is to understand player intent and convert it to valid commands.\n\n"
        
        "CRITICAL RULES:\n"
        "1. Output ONLY the command - NO quotes, explanations, or extra text\n"
        "2. Use scene descriptions to resolve spatial references\n"
        "3. Simplify complex requests to valid single commands\n"
        "4. Use the shortest valid form\n"
        "5. NEVER interpret 'go to X' as 'look' - either find the direction or return empty\n\n"
        
        "LOCATION AWARENESS:\n"
        "- Check the CURRENT LOCATION field\n"
        "- If player says 'go to X' and they're already at X, return EMPTY STRING\n"
        "- If player says 'go to X' but X is the current location, return EMPTY STRING\n"
        "- Example: Current='Back Alley', Input='go to the alley' → (empty, don't output 'look')\n\n"
        
        "SPATIAL REASONING:\n"
        "- Read the scene description carefully\n"
        "- 'go to X' should use the direction mentioned: if 'bedroom is north' then 'go to bedroom' → north\n"
        "- 'enter X' becomes the direction if X is mentioned with a direction\n"
        "- Look for phrases like 'X is to the Y' or 'door to Y leads to X'\n"
        "- If no direction is clear for 'go to X', return EMPTY STRING (don't guess)\n\n"
        
        "MULTI-OBJECT HANDLING:\n"
        "- Games often don't support multiple objects in one command\n"
        "- 'wear winter clothes' → wear coat (pick ONE logical item)\n"
        "- 'put on coat and boots' → wear coat (games process one at a time)\n"
        "- Choose the most important/first item mentioned in scene\n\n"
        
        "COMMON PATTERNS:\n"
        "- 'what do I have' / 'check inventory' → inventory\n"
        "- 'look around' / 'whats here' → look\n"
        "- 'pick up X' / 'grab X' / 'get X' → take X\n"
        "- 'inspect X' / 'check X' / 'look at X' → examine X\n"
        "- 'put on X' / 'wear X' → wear X\n"
        "- 'use X on Y' → unlock Y with X (or other verb based on context)\n\n"
        
        "CONTEXT:\n"
        "%s\n"
        "%s\n\n"
        
        "EXAMPLES:\n"
        "Current='Living Room', Scene='bedroom is north' + Input='go to bedroom' → north\n"
        "Current='Back Alley', Scene='...' + Input='go to the alley' → (empty)\n"
        "Current='Street', Scene='alley runs north' + Input='go to alley' → north\n"
        "Scene='door south leads outside' + Input='go outside' → south\n"
        "Scene='has coat, boots, scarf' + Input='wear winter clothes' → wear coat\n"
        "Input='put on coat and boots' → wear coat\n"
        "Input='read the letter' → read letter\n"
        "Input='whats around' → look\n"
        "Input='take sword and shield' → take sword\n"
        "Input='go somewhere unclear' → (empty, don't guess)\n",
        context_json, scene_info);
    
    char escaped_system[8192];
    escape_json_string(system_prompt, escaped_system, sizeof(escaped_system));
    
    char json_body[16384];
    snprintf(json_body, sizeof(json_body),
        "{"
        "\"model\":\"%s\","
        "\"messages\":["
        "{\"role\":\"system\",\"content\":\"%s\"},"
        "{\"role\":\"user\",\"content\":\"%s\"}"
        "],"
        "\"max_tokens\":50,"
        "\"temperature\":0.3"
        "}",
        gli_llm_config.model[0] ? gli_llm_config.model : "gpt-3.5-turbo",
        escaped_system,
        escaped_input
    );
    
    char request[10240];
    int request_len = snprintf(request, sizeof(request),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        path, host, gli_llm_config.api_key, strlen(json_body), json_body
    );
    
    int sent;
    if (ssl) {
        sent = SSL_write(ssl, request, request_len);
    } else {
        sent = write(sock, request, request_len);
    }
    
    if (sent < request_len) {
        if (ssl) {
            SSL_free(ssl);
            SSL_CTX_free(ctx);
        }
        close(sock);
        strncpy(output, input, maxlen);
        output[maxlen - 1] = '\0';
        return 0;
    }
    
    char response[16384];
    int total_received = 0;
    int received;
    
    while (total_received < sizeof(response) - 1) {
        if (ssl) {
            received = SSL_read(ssl, response + total_received, sizeof(response) - total_received - 1);
        } else {
            received = read(sock, response + total_received, sizeof(response) - total_received - 1);
        }
        
        if (received <= 0) break;
        total_received += received;
    }
    
    response[total_received] = '\0';
    
    if (ssl) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
    }
    close(sock);
    
    char *body = strstr(response, "\r\n\r\n");
    if (!body) {
        strncpy(output, input, maxlen);
        output[maxlen - 1] = '\0';
        return 0;
    }
    body += 4;
    
    // Handle chunked transfer encoding
    if (strstr(response, "Transfer-Encoding: chunked")) {
        char *chunk_data = strchr(body, '\n');
        if (chunk_data) body = chunk_data + 1;
    }
    
    char *interpreted = parse_json_response(body);
    if (!interpreted) {
        strncpy(output, input, maxlen);
        output[maxlen - 1] = '\0';
        return 0;
    }
    
    strncpy(output, interpreted, maxlen);
    output[maxlen - 1] = '\0';
    
    // Remove newlines from output
    char *nl = strchr(output, '\n');
    if (nl) *nl = '\0';
    nl = strchr(output, '\r');
    if (nl) *nl = '\0';
    
    // Check if LLM returned multiple commands (separated by " THEN ")
    char *then_marker = strstr(output, " THEN ");
    if (then_marker) {
        // Queue the additional commands
        char *cmd = output;
        int first_command = 1;
        
        while (cmd) {
            char *next = strstr(cmd, " THEN ");
            if (next) {
                *next = '\0';
                next += 6; // Skip " THEN "
            }
            
            // Trim whitespace
            while (*cmd == ' ') cmd++;
            
            if (*cmd) {
                if (first_command) {
                    // First command becomes the output
                    strncpy(output, cmd, maxlen);
                    output[maxlen - 1] = '\0';
                    first_command = 0;
                } else if (gli_llm_context.queue_count < GLK_LLM_MAX_QUEUED_COMMANDS) {
                    // Queue subsequent commands
                    int tail = gli_llm_context.queue_tail;
                    strncpy(gli_llm_context.command_queue[tail], cmd, 255);
                    gli_llm_context.command_queue[tail][255] = '\0';
                    gli_llm_context.queue_tail = (tail + 1) % GLK_LLM_MAX_QUEUED_COMMANDS;
                    gli_llm_context.queue_count++;
                }
            }
            
            cmd = next;
        }
    }
    
    int changed = (strcmp(input, output) != 0);
    
    free(interpreted);
    
    return changed;
}


// Generate contextual help using LLM
int gli_llm_generate_help(const char *user_input, char *output, size_t max_len)
{
    if (!gli_llm_config.enabled) {
        return 0;
    }
    
    // Build scene context
    char scene[2048] = "";
    if (gli_llm_context.count > 0) {
        int lines = (gli_llm_context.count < 5) ? gli_llm_context.count : 5;
        int start = gli_llm_context.position - lines;
        if (start < 0) start += GLK_LLM_CONTEXT_LINES;
        
        for (int i = 0; i < lines; i++) {
            int idx = (start + i) % GLK_LLM_CONTEXT_LINES;
            if (gli_llm_context.lines[idx][0]) {
                strncat(scene, gli_llm_context.lines[idx], sizeof(scene) - strlen(scene) - 1);
                strncat(scene, " ", sizeof(scene) - strlen(scene) - 1);
            }
        }
    }
    
    // Build prompt for helpful message
    char prompt[4096];
    snprintf(prompt, sizeof(prompt),
        "The player tried: '%s' but the game didn't understand. "
        "Write a SHORT, helpful response (1 sentence) that:\n"
        "1. Fits the game's narrative tone\n"
        "2. Suggests what they might try instead based on the scene\n"
        "3. Is written in second person ('you')\n\n"
        "Scene context: %s\n\n"
        "Response:", user_input, scene);
    
    // This would call the LLM - for now, return 0 to use default
    // TODO: Implement actual LLM call for help generation
    return 0;
}

// Simple placeholder - not used in simplified version
void gli_llm_check_and_suggest(void)
{
    // Removed for simplicity - let game show its own errors
}
