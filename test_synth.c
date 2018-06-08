#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/types.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "libdialogflow.h"

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

/* program keyfile destfile text */
int main(int argc, const char *argv[])
{
    char keybuffer[4096];
    const char *keyfile;
    const char *destfile;
    const char *text;

    if (argc != 4) {
        test_log(LOG_ERROR, "Usage: test_synth keyfile destfile text\n");
        return 1;
    }

    keyfile = argv[1];
    destfile = argv[2];
    text = argv[3];

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

    if (df_init(vtest_log)) {
        test_log(LOG_ERROR, "Failure initializing library\n");
        return 20;
    }

    if (google_synth_speech(NULL, keyfile, text, NULL, NULL, destfile)) {
        test_log(LOG_ERROR, "Synthesis failed\n");
        return 30;
    }

    return 0;
}