#ifndef GLK_LLM_H
#define GLK_LLM_H

#include "glk.h"

#define GLK_LLM_BUFFER_SIZE 4096
#define GLK_LLM_CONTEXT_LINES 20

typedef struct {
    int enabled;
    char api_endpoint[512];
    char api_key[256];
    char model[128];
    int context_lines;
    int timeout_ms;
    int echo_interpretation;
} glk_llm_config_t;

#define GLK_LLM_MAX_QUEUED_COMMANDS 10

typedef struct {
    char lines[GLK_LLM_CONTEXT_LINES][256];
    int count;
    int position;
    char last_user_input[256];
    char command_queue[GLK_LLM_MAX_QUEUED_COMMANDS][256];
    int queue_head;
    int queue_tail;
    int queue_count;
} glk_llm_context_t;

extern glk_llm_config_t gli_llm_config;
extern glk_llm_context_t gli_llm_context;

void gli_llm_init(void);
void gli_llm_load_config(const char *config_file);
void gli_llm_add_context(const char *text);
int gli_llm_process_input(const char *input, char *output, glui32 maxlen);
void gli_llm_check_and_suggest(void);
int gli_llm_generate_help(const char *user_input, char *output, size_t max_len);

#endif /* GLK_LLM_H */
