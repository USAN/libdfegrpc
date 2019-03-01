#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/types.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "libdfegrpc.h"

#define LOG_DEBUG   DF_LOG_LEVEL_DEBUG, __FILE__, __LINE__, __PRETTY_FUNCTION__
#define LOG_INFO    DF_LOG_LEVEL_INFO, __FILE__, __LINE__, __PRETTY_FUNCTION__
#define LOG_WARNING DF_LOG_LEVEL_WARNING, __FILE__, __LINE__, __PRETTY_FUNCTION__
#define LOG_ERROR   DF_LOG_LEVEL_ERROR, __FILE__, __LINE__, __PRETTY_FUNCTION__

static void vtest_log(enum dialogflow_log_level level, const char *file, int line, const char *function, const char *fmt, va_list args)
{
    const char *levels[] = { "DEBUG", "INFO", "WARNING", "ERROR" };

    if (level < DF_LOG_LEVEL_DEBUG || level > DF_LOG_LEVEL_ERROR) {
        level = DF_LOG_LEVEL_ERROR;
    }

    printf("[%s] (%s:%d) ", levels[level], file, line);
    vprintf(fmt, args);
}

static void test_log(enum dialogflow_log_level level, const char *file, int line, const char *function, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vtest_log(level, file, line, function, fmt, args);
    va_end(args);
}

static void test_call_log(void *user_data, const char *event, size_t log_data_size, const struct dialogflow_log_data *log_data)
{
    size_t i;
    printf("CALL LOG: event=%s", event);
    for (i = 0; i < log_data_size; i++) {
        if (log_data[i].value_type == dialogflow_log_data_value_type_string) {
            printf(" %s=%s", log_data[i].name, (const char *) log_data[i].value);
        } else if (log_data[i].value_type == dialogflow_log_data_value_type_array_of_string) {
            size_t j;
            for (j = 0; j < log_data[i].value_count; j++) {
                printf(" %s[%d]=%s", log_data[i].name, (int)j, ((const char **) log_data[i].value)[j]);
            }
        }
    }
    printf("\n");
}

/* program keyfile projectid audiofile */
int main(int argc, char *argv[])
{
    char keybuffer[4096];
    const char *keyfile = NULL;
    const char *projectid = NULL;
    const char *audiofile = NULL;
    const char *event = NULL;
    const char *hints[10] = { "\0" };
    int single_utterance = 0;
    size_t hints_count = 0;
    int count = 1;

    struct dialogflow_session *session;
    int i = 0;
    char c;

    while ((c = getopt(argc, argv, "k:p:a:e:h:n:s")) != -1) {
        switch (c) {
            case 'k':
                keyfile = optarg;
                break;
            case 'p':
                projectid = optarg;
                break;
            case 'a':
                audiofile = optarg;
                break;
            case 'e':
                event = optarg;
                break;
            case 'n':
                count = atoi(optarg);
                break;
            case 'h':
                if (hints_count >= 10) {
                    test_log(LOG_ERROR, "Too many hints\n");
                    return 2;
                }
                hints[hints_count++] = optarg;
                break;
            case 's':
                single_utterance++;
                break;
            case '?':
            default:
                test_log(LOG_ERROR, "Error gathering options\n");
                return 5;
        }
    }

    if (!keyfile) {
        test_log(LOG_ERROR, "Keyfile is required\n");
        return 6;
    }
    if (!projectid) {
        test_log(LOG_ERROR, "Project ID is required\n");
        return 7;
    }
    if (!audiofile && !event) {
        test_log(LOG_ERROR, "One of event or audio file is required\n");
        return 8;
    }

    if (strchr(keyfile, '{')) {
        test_log(LOG_DEBUG, "Key file detected as an actual key\n");
    } else {
        FILE *f;
        test_log(LOG_DEBUG, "Loading key data from %s\n", keyfile);
        f = fopen(keyfile, "r");
        if (f) {
            size_t read = fread(keybuffer, sizeof(char), sizeof(keybuffer), f);
            keybuffer[read] = '\0';
            if (ferror(f)) {
                test_log(LOG_ERROR, "Error reading %s -- %d\n", keyfile, errno);
                fclose(f);
                return 10;
            } else if (!feof(f)) {
                test_log(LOG_WARNING, "May have read partial key from %s -- need to expand the buffer.\n", keyfile);
            }
            fclose(f);
            keyfile = keybuffer;
        } else {
            test_log(LOG_ERROR, "Unable to open %s -- %d\n", keyfile, errno);
            return 10;
        }
    }

    if (df_init(vtest_log, test_call_log)) {
        test_log(LOG_ERROR, "Failure initializing library\n");
        return 20;
    }

    while (count-- > 0) {

        session = df_create_session(NULL);
        if (session == NULL) {
            test_log(LOG_ERROR, "Failed to create client session\n");
            return 25;
        }
        df_set_auth_key(session, keyfile);
        df_set_session_id(session, "testclient");
        df_set_debug(session, 1);
        df_set_use_external_endpointer(session, !single_utterance);

        if (!session) {
            test_log(LOG_ERROR, "Error creating session\n");
            return 30;
        }

        if (df_set_project_id(session, projectid)) {
            test_log(LOG_ERROR, "Error setting project id\n");
            return 40;
        }

        if (audiofile) {
            FILE *audio;
            int responseCount = 0;

            audio = fopen(audiofile, "rb");
            if (!audio) {
                test_log(LOG_ERROR, "Error opening audio file %s -- %d\n", audiofile, errno);
                return 15;
            }

            df_set_request_sentiment_analysis(session, 1);

            if (df_start_recognition(session, NULL, 0, hints, hints_count)) {
                test_log(LOG_ERROR, "Error starting recognition\n");
                return 50;
            }

            test_log(LOG_DEBUG, "Recognition started -- %d\n", df_get_rpc_state(session));

            while (!feof(audio) && !ferror(audio)) {
                enum dialogflow_session_state state;
                char buffer[160];
                int newResponseCount;
                size_t read = fread(buffer, sizeof(char), sizeof(buffer), audio);
                if (read < sizeof(buffer)) {
                    memset(buffer + read, 0x7f, sizeof(buffer) - read);
                }
                /* dc - this doesn't seem necessary anymore */
                usleep(20 * 1000);
                if (++i % 100 == 1) {
                    test_log(LOG_DEBUG, "Writing audio...\n");
                }
                state = df_write_audio(session, buffer, sizeof(buffer));

                if (state != DF_STATE_STARTED) {
                    break;
                }

                newResponseCount = df_get_response_count(session);
                if (responseCount == 0 && newResponseCount > 0) {
                    test_log(LOG_DEBUG, "Got %d response(s)\n", newResponseCount);
                }
                responseCount = newResponseCount;
            }

            fclose(audio);

            test_log(LOG_DEBUG, "Finished writing audio\n");

            if (df_stop_recognition(session)) {
                test_log(LOG_ERROR, "Error stopping recognition\n");
                return 70;
            }
        } else {
            if (df_recognize_event(session, event, NULL, 0)) {
                test_log(LOG_ERROR, "Failure recognizing event\n");
                return 80;
            }
        } 

        int resultcount = df_get_result_count(session);
        test_log(LOG_DEBUG, "Found %d results:\n", resultcount);
        for (i = 0; i < resultcount; i++) {
            struct dialogflow_result *result = df_get_result(session, i);
            if (strcasecmp(result->slot, "output_audio")) {
                test_log(LOG_DEBUG, "Result %d: '%s' = '%s' (%d)\n", i, result->slot, result->value, result->score);
            } else {
                test_log(LOG_DEBUG, "Result %d: '%s' = %d bytes of audio\n", i, result->slot, result->valueLen);
            }
        }

        df_close_session(session);

        session = NULL;
    }

    df_shutdown();

    return 0;
}