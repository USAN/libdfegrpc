#include <iostream>
#include <vector>
#include <string>
#include <cstdarg>
#include <cstring>
#include <thread>

#include <unistd.h>

#include <grpc/grpc.h>
#include <grpc/support/log.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <google/cloud/dialogflow/v2beta1/session.grpc.pb.h>

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientReaderWriter;
using grpc::ClientWriter;
using grpc::Status;
using google::cloud::dialogflow::v2beta1::Sessions;
using google::cloud::dialogflow::v2beta1::StreamingDetectIntentResponse;
using google::cloud::dialogflow::v2beta1::StreamingDetectIntentRequest;

enum dialogflow_log_level {
    test_log_LEVEL_DEBUG,
    test_log_LEVEL_INFO,
    test_log_LEVEL_WARNING,
    test_log_LEVEL_ERROR
};

#define LOG_DEBUG   test_log_LEVEL_DEBUG, __FILE__, __LINE__
#define LOG_INFO    test_log_LEVEL_INFO, __FILE__, __LINE__
#define LOG_WARNING test_log_LEVEL_WARNING, __FILE__, __LINE__
#define LOG_ERROR   test_log_LEVEL_ERROR, __FILE__, __LINE__

static void test_log(enum dialogflow_log_level level, const char *file, int line, const char *fmt, ...)
{
    va_list args;
    const char *levels[] = { "DEBUG", "INFO", "WARNING", "ERROR" };

    if (level < test_log_LEVEL_DEBUG || level > test_log_LEVEL_ERROR) {
        level = test_log_LEVEL_ERROR;
    }

    va_start(args, fmt);
    printf("[%s] (%s:%d) ", levels[level], file, line);
    vprintf(fmt, args);
    va_end(args);
}

static void wrapper_grpc_log(gpr_log_func_args *args)
{
    enum dialogflow_log_level df_level = args->severity == GPR_LOG_SEVERITY_DEBUG ? test_log_LEVEL_DEBUG :
                                        args->severity == GPR_LOG_SEVERITY_INFO ? test_log_LEVEL_INFO :
                                            test_log_LEVEL_ERROR;
    test_log(df_level, args->file, args->line, "%s\n", args->message);
}

static std::string format(const std::string& format, ...)
{
    va_list args;
    va_start (args, format);
    size_t len = std::vsnprintf(NULL, 0, format.c_str(), args);
    va_end (args);
    std::vector<char> vec(len + 1);
    va_start (args, format);
    std::vsnprintf(&vec[0], len + 1, format.c_str(), args);
    va_end (args);
    return &vec[0];
}

/* program keyfile projectid audiofile */
int main(int argc, const char *argv[])
{
    char keybuffer[4096];
    const char *keyfile;
    const char *projectid;
    const char *audiofile;
    FILE *audio;

    if (argc != 4) {
        test_log(LOG_ERROR, "Usage: test_client keyfile projectid audiofile\n");
        return 1;
    }

    keyfile = argv[1];
    projectid = argv[2];
    audiofile = argv[3];

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

    audio = fopen(audiofile, "rb");
    if (!audio) {
        test_log(LOG_ERROR, "Error opening audio file %s -- %d\n", audiofile, errno);
        return 15;
    }

    grpc_init();
    gpr_set_log_function(wrapper_grpc_log);
    gpr_set_log_verbosity(GPR_LOG_SEVERITY_DEBUG);

    const char *endpoint = "dialogflow.googleapis.com";
    const char *session_id = "test_client2";

    auto creds = grpc::CompositeChannelCredentials(grpc::SslCredentials(grpc::SslCredentialsOptions()), 
       grpc::ServiceAccountJWTAccessCredentials(keyfile, 3600));

    test_log(LOG_INFO, "Creating DF session to %s for %s\n", endpoint, session_id);

    std::shared_ptr<Channel> channel = grpc::CreateChannel(endpoint, creds);
    std::unique_ptr<Sessions::Stub> session = Sessions::NewStub(channel);

    test_log(LOG_DEBUG, "Channel to %s for %s created\n", endpoint, session_id);

    ClientContext context;

    std::string session_path = format("projects/%s/agent/sessions/%s", projectid, session_id);

    test_log(LOG_DEBUG, "Session %s starting recognition to %s\n", session_id, session_path.c_str());
   
    /* it didn't like assigning this to the session structure location */
    std::shared_ptr<ClientReaderWriter<StreamingDetectIntentRequest, StreamingDetectIntentResponse>> current_request =
        session->StreamingDetectIntent(&context);

    StreamingDetectIntentRequest request;
    request.set_session(session_path);
    request.mutable_query_input()->mutable_audio_config()->set_audio_encoding(google::cloud::dialogflow::v2beta1::AUDIO_ENCODING_MULAW);
    request.mutable_query_input()->mutable_audio_config()->set_sample_rate_hertz(8000);
    request.mutable_query_input()->mutable_audio_config()->set_language_code("en");

    if (!current_request->Write(request)) {
        test_log(LOG_WARNING, "Session %s got error writing initial data packet to %s\n", session_id, projectid);
        return -1;
    }

    test_log(LOG_DEBUG, "Recognition started -- %d\n", channel->GetState(false));

    std::thread readThread([current_request]() {
        StreamingDetectIntentResponse response;
        while (current_request->Read(&response)) {
            test_log(LOG_INFO, "Got response:\n");
            test_log(LOG_INFO, "\tquery result: %s\n",
                !response.has_query_result() ? "(none)" :
                    response.query_result().query_text().c_str()
                );
            test_log(LOG_INFO, "\trecognition result: %s\n",
                response.has_recognition_result() ? 
                    (response.recognition_result().message_type() == google::cloud::dialogflow::v2beta1::StreamingRecognitionResult_MessageType::StreamingRecognitionResult_MessageType_END_OF_SINGLE_UTTERANCE ?
                        "(end of utterance)" :
                        response.recognition_result().transcript().c_str()) :
                    "none");
        }
    });

    int i = 0;

    while (!feof(audio) && !ferror(audio)) {
        char buffer[160];
        size_t read = fread(buffer, sizeof(char), sizeof(buffer), audio);
        if (read < sizeof(buffer)) {
            memset(buffer + read, 0x7f, sizeof(buffer) - read);
        }
        usleep(18 * 1000);
        if (++i % 50 == 1) {
            test_log(LOG_DEBUG, "Writing audio...\n");
        }

        StreamingDetectIntentRequest audioRequest;
        audioRequest.set_input_audio(buffer, sizeof(buffer));

        if (!current_request->Write(audioRequest)) {
            test_log(LOG_WARNING, "Session %s got error writing initial data packet to %s\n", session_id, projectid);
        }
    }
    fclose(audio);
    current_request->WritesDone();

    readThread.join();

    Status status = current_request->Finish();
    if (!status.ok()) {
        test_log(LOG_ERROR, "RPC failed\n");
    } else {
        test_log(LOG_DEBUG, "RPC successful\n");
    }

    return 0;
}