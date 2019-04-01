#include <iostream>
#include <vector>
#include <string>
#include <cstdarg>
#include <cstring>
#include <thread>
#include <mutex>
#include <fstream>
#include <iostream>
#include <sys/time.h>

#include "libdfegrpc.h"
#include "libdfegrpc_internal.h"

#include <grpc/grpc.h>
#include <grpc/support/log.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <google/cloud/dialogflow/v2beta1/session.grpc.pb.h>

#include <google/cloud/texttospeech/v1beta1/cloud_tts.grpc.pb.h>

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientReaderWriter;
using grpc::ClientReaderWriterInterface;
using grpc::ClientWriter;
using grpc::Status;
using google::cloud::dialogflow::v2beta1::Sessions;
using google::cloud::dialogflow::v2beta1::StreamingDetectIntentResponse;
using google::cloud::dialogflow::v2beta1::StreamingDetectIntentRequest;
using google::cloud::dialogflow::v2beta1::DetectIntentResponse;
using google::cloud::dialogflow::v2beta1::DetectIntentRequest;
using google::cloud::dialogflow::v2beta1::QueryResult;

using google::cloud::texttospeech::v1beta1::TextToSpeech;
using google::cloud::texttospeech::v1beta1::SynthesizeSpeechRequest;
using google::cloud::texttospeech::v1beta1::SynthesizeSpeechResponse;

#define LOG_DEBUG   DF_LOG_LEVEL_DEBUG, __FILE__, __LINE__, __PRETTY_FUNCTION__
#define LOG_INFO    DF_LOG_LEVEL_INFO, __FILE__, __LINE__, __PRETTY_FUNCTION__
#define LOG_WARNING DF_LOG_LEVEL_WARNING, __FILE__, __LINE__, __PRETTY_FUNCTION__
#define LOG_ERROR   DF_LOG_LEVEL_ERROR, __FILE__, __LINE__, __PRETTY_FUNCTION__

#define cstrlen_zero(str)   (((str) == nullptr) || ((*str) == '\0'))
#define cstr_or(str, alt)   (cstrlen_zero(str) ? (alt) : (str))

#define ARRAY_LEN(a) (size_t) (sizeof(a) / sizeof(0[a]))

static void noop_log(enum dialogflow_log_level level, const char *file, int line, const char *function, const char *fmt, va_list args)
{
}

static void noop_call_log(void *user_data, const char *event, size_t log_data_size, const struct dialogflow_log_data *data)
{
}

static DF_LOG_FUNC parent_df_log = noop_log;
static DF_CALL_LOG_FUNC df_log_call = noop_call_log;

static void df_log(enum dialogflow_log_level level, const char *file, int line, const char *function, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    parent_df_log(level, file, line, function, fmt, args);
    va_end(args);
}

static void wrapper_grpc_log(gpr_log_func_args *args)
{
    enum dialogflow_log_level df_level = args->severity == GPR_LOG_SEVERITY_DEBUG ? DF_LOG_LEVEL_DEBUG :
                                        args->severity == GPR_LOG_SEVERITY_INFO ? DF_LOG_LEVEL_INFO :
                                            DF_LOG_LEVEL_ERROR;
    df_log(df_level, args->file, args->line, "grpc", "%s\n", args->message);
}

static timeval tvnow(void)
{
	timeval t;
	gettimeofday(&t, NULL);
	return t;
}

int df_init(DF_LOG_FUNC log_function, DF_CALL_LOG_FUNC call_log_function)
{
    grpc_init();
    if (log_function) {
        parent_df_log = log_function;
    }
    if (call_log_function) {
        df_log_call = call_log_function;
    }
    gpr_set_log_function(wrapper_grpc_log);
    gpr_set_log_verbosity(GPR_LOG_SEVERITY_DEBUG);
    return 0;
}

int df_shutdown(void)
{
    grpc_shutdown();
    return 0;
}

static std::shared_ptr<Channel> create_grpc_channel(const std::string& endpoint, const std::string& auth_key)
{
    std::shared_ptr<grpc::ChannelCredentials> creds;

    if (auth_key.length() > 0) {
        auto svcAcct = grpc::ServiceAccountJWTAccessCredentials(auth_key, 3600);
        if (svcAcct == nullptr) {
            df_log(LOG_ERROR, "Service account credentials failed to load, will attempt to use default credentials.\n");
            creds = grpc::GoogleDefaultCredentials();
        } else {
            creds = grpc::CompositeChannelCredentials(grpc::SslCredentials(grpc::SslCredentialsOptions()), svcAcct);
        }
    } else {
        creds = grpc::GoogleDefaultCredentials();
    }

    df_log(LOG_INFO, "Creating DF session to %s\n", endpoint.c_str());

    return grpc::CreateChannel(endpoint, creds);
}

struct dialogflow_session *df_create_session(void *user_data)
{
    struct dialogflow_session *session = new dialogflow_session();
    
    if (session == nullptr) {
        df_log(LOG_ERROR, "Failed to create session object\n");
        return nullptr;
    }

    session->state = DF_STATE_READY;
    session->user_data = user_data;
    session->endpoint = "dialogflow.googleapis.com";

    df_log_call(session->user_data, "create", 0, nullptr);

    return session;
}

static void df_disconnect_locked(struct dialogflow_session *session)
{
    session->session = nullptr;
    session->channel = nullptr;
}

int df_set_endpoint(struct dialogflow_session *session, const char *endpoint)
{
    std::lock_guard<std::mutex> lock(session->lock);
    if (cstrlen_zero(endpoint)) {
        endpoint = "dialogflow.googleapis.com";
    }

    if (strcasecmp(session->endpoint.c_str(), endpoint)) {
        session->endpoint = endpoint;
        df_disconnect_locked(session);
    }

    return 0;
}

int df_set_auth_key(struct dialogflow_session *session, const char *auth_key)
{
    std::lock_guard<std::mutex> lock(session->lock);
    if (strcasecmp(session->auth_key.c_str(), auth_key)) {
        session->auth_key = auth_key;
        df_disconnect_locked(session);
    }

    return 0;
}

int df_close_session(struct dialogflow_session *session)
{
    std::unique_lock<std::mutex> lock(session->lock);

    if (session->state != DF_STATE_READY) {
        lock.unlock();
        df_stop_recognition(session);
        lock.lock();
    }
    df_log(LOG_DEBUG, "Destroying channel to %s for %s\n", session->endpoint.c_str(), session->session_id.c_str());
    lock.unlock();
    df_log_call(session->user_data, "destroy", 0, NULL);

    delete session;

    return 0;
}

int df_set_session_id(struct dialogflow_session *session, const char *session_id)
{
    std::lock_guard<std::mutex> lock(session->lock);
    session->session_id = session_id;
    return 0;
}

const char *df_get_session_id(struct dialogflow_session *session)
{
    std::lock_guard<std::mutex> lock(session->lock);
    return session->session_id.c_str();
}

int df_set_project_id(struct dialogflow_session *session, const char *project_id)
{
    std::lock_guard<std::mutex> lock(session->lock);
    session->project_id = project_id;
    return 0;
}

const char *df_get_project_id(struct dialogflow_session *session)
{
    std::lock_guard<std::mutex> lock(session->lock);
    return session->project_id.c_str();
}

int df_set_request_sentiment_analysis(struct dialogflow_session *session, int request_sentiment_analysis)
{
    std::lock_guard<std::mutex> lock(session->lock);
    session->request_sentiment_analysis = (request_sentiment_analysis != 0);
    return 0;
}

/*!! Indicate that the client program will provide end of speech indication */
int df_set_use_external_endpointer(struct dialogflow_session *session, int use_external_endpointer)
{
    std::lock_guard<std::mutex> lock(session->lock);
    session->use_external_endpointer = (use_external_endpointer != 0);
    return 0;
}

/*!! Set the model for use by the intent detection request */
int df_set_model(struct dialogflow_session *session, const char *model)
{
    std::lock_guard<std::mutex> lock(session->lock);
    session->model = model;
    return 0;
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

static std::string make_indexed_name(std::string name, int index)
{
    if (index == 0) {
        return name;
    } else {
        return format("%s_%d", name.c_str(), index);
    }
}

static void push_parameter_result(std::vector<std::unique_ptr<df_result>> &results, const std::string &name, const ::google::protobuf::Value &value, int score)
{
    ::google::protobuf::Value::KindCase kind = value.kind_case();
    if (kind == ::google::protobuf::Value::KindCase::kStructValue) {
        /* nested */
        ::google::protobuf::Map<std::string, ::google::protobuf::Value> parameters = value.struct_value().fields();
        for (auto iterator = parameters.begin(); iterator != parameters.end(); ++iterator) {
            const std::string subname = iterator->first;
            const ::google::protobuf::Value subvalue = iterator->second;
            push_parameter_result(results, name + "_" + subname, subvalue, score);
        }
    } else if (kind == ::google::protobuf::Value::KindCase::kListValue) {
        /* nested-ish */
        const ::google::protobuf::ListValue list_value = value.list_value();
        int list_size = list_value.values_size();
        for (int i = 0; i < list_size; i++) {
            const ::google::protobuf::Value sub_value = list_value.values(i);
            push_parameter_result(results, name + "_" + std::to_string(i), sub_value, score);
        }
    } else {
        /* simple value */
        std::string result;
        switch (kind) {
            case ::google::protobuf::Value::KindCase::kNullValue:
                result = "null";
                break;
            case ::google::protobuf::Value::KindCase::kNumberValue:
                result = std::to_string(value.number_value());
                break;
            case ::google::protobuf::Value::KindCase::kStringValue:
                result = value.string_value();
                break;
            case ::google::protobuf::Value::KindCase::kBoolValue:
                result = value.bool_value() ? "true" : "false";
                break;
            default: 
                result = "unknown type";
                break;
        }
        results.push_back(std::unique_ptr<df_result>(new df_result(name, result, score)));
    }
}

static void make_query_result_responses(struct dialogflow_session *session, const QueryResult &query_result, int score)
{
    int text_count = 0;
    int simple_response_count = 0;
    int play_audio_count = 0;
    int synthesize_speech_count = 0;
    int transfer_call_count = 0;
    int terminate_call_count = 0;

    session->results.push_back(std::unique_ptr<df_result>(new df_result("query_text", query_result.query_text(), score)));
    session->results.push_back(std::unique_ptr<df_result>(new df_result("language_code", query_result.language_code(), score)));
    session->results.push_back(std::unique_ptr<df_result>(new df_result("action", query_result.action(), score)));
    session->results.push_back(std::unique_ptr<df_result>(new df_result("fulfillment_text", query_result.fulfillment_text(), score)));
    session->results.push_back(std::unique_ptr<df_result>(new df_result("intent_name", query_result.intent().name(), score)));
    session->results.push_back(std::unique_ptr<df_result>(new df_result("intent_display_name", query_result.intent().display_name(), score)));
    session->results.push_back(std::unique_ptr<df_result>(new df_result("intent_detection_confidence", format("%f", query_result.intent_detection_confidence()), score)));

    int msgs = query_result.fulfillment_messages_size();
    for (int i = 0; i < msgs; i++) {
        const ::google::cloud::dialogflow::v2beta1::Intent_Message& msg = query_result.fulfillment_messages(i);
        if (msg.has_text()) {
            int texts = msg.text().text_size();
            for (int j = 0; j < texts; j++) {
                session->results.push_back(std::unique_ptr<df_result>(new df_result(make_indexed_name("text", text_count++), msg.text().text(j), score)));
            }
        } else if (msg.has_simple_responses()) {
            int rspns = msg.simple_responses().simple_responses_size();
            for (int j = 0; j < rspns; j++) {
                const std::string& tts = msg.simple_responses().simple_responses(j).text_to_speech();
                const std::string& ssml = msg.simple_responses().simple_responses(j).ssml();

                session->results.push_back(std::unique_ptr<df_result>(new df_result(make_indexed_name("simple_response", simple_response_count++),
                    tts.length() ? tts : ssml, score)));
            }
        } else if (msg.has_telephony_play_audio()) {
            session->results.push_back(std::unique_ptr<df_result>(new df_result(make_indexed_name("play_audio", play_audio_count++),
                msg.telephony_play_audio().audio_uri(), score)));
        } else if (msg.has_telephony_synthesize_speech()) {
            const std::string& tts = msg.telephony_synthesize_speech().text();
            const std::string& ssml = msg.telephony_synthesize_speech().ssml();
            session->results.push_back(std::unique_ptr<df_result>(new df_result(make_indexed_name("synthesize_speech", synthesize_speech_count++),
                tts.length() ? tts : ssml, score)));
        } else if (msg.has_telephony_transfer_call()) {
            session->results.push_back(std::unique_ptr<df_result>(new df_result(make_indexed_name("transfer_call", transfer_call_count++),
                msg.telephony_transfer_call().phone_number(), score)));
        } else if (msg.has_telephony_terminate_call()) {
            session->results.push_back(std::unique_ptr<df_result>(new df_result(make_indexed_name("terminate_call", terminate_call_count++),
                "true", score)));
        }
    }

    ::google::protobuf::Map<std::string, ::google::protobuf::Value> parameters = query_result.parameters().fields();
    for (auto iterator = parameters.begin(); iterator != parameters.end(); ++iterator) {
        const std::string name = iterator->first;
        const ::google::protobuf::Value value = iterator->second;
        push_parameter_result(session->results, name, value, score);
    }

    if (query_result.has_sentiment_analysis_result() && query_result.sentiment_analysis_result().has_query_text_sentiment()) {
        auto sentiment = query_result.sentiment_analysis_result().query_text_sentiment();
        session->results.push_back(std::unique_ptr<df_result>(new df_result("sentiment_score", format("%f", sentiment.score()), score)));
        session->results.push_back(std::unique_ptr<df_result>(new df_result("sentiment_magnitude", format("%f", sentiment.magnitude()), score)));
    }
}

template<typename T> static void make_audio_result(struct dialogflow_session *session, T& response, int score)
{
    if (response.output_audio().length() > 0) {
        const char *audio = response.output_audio().c_str();
        if (!strncmp(audio, "RIFF", 4)) {
            /* looks like a wave file */
            int32_t chunkSize = *((int32_t*)(audio + 4));
            if (chunkSize > 0) {
                /* so far so good */
                try {
                    /* verify the array is valid... */
                    char a = audio[chunkSize + 8 - 1];
                    a = a; /* prevent the compiler complaining about the unused variable */
                    session->results.push_back(std::unique_ptr<df_result>(new df_result("output_audio", audio, chunkSize + 8, score)));
                } catch (const std::exception& e) {
                    df_log(LOG_WARNING, "Got exception poking end of audio array for output_audio for %s\n", session->session_id.c_str());
                }
            } else {
                df_log(LOG_WARNING, "Got output_audio for %s with a non-positive RIFF chunk size - %d", session->session_id.c_str(), chunkSize);
            }
        } else {
            df_log(LOG_WARNING, "Got output_audio for %s without a RIFF header\n", session->session_id.c_str());
        }
    }
}

static void log_responses(struct dialogflow_session *session, int score)
{
    std::unique_lock<std::mutex> lock(session->lock);
    size_t response_count = session->results.size();
    size_t log_data_size = response_count + 1; /* for score */
    struct dialogflow_log_data log_data[log_data_size];
    size_t i;
    std::string score_string = std::to_string(score);

    log_data[0].name = "score";
    log_data[0].value = score_string.c_str();
    log_data[0].value_type = dialogflow_log_data_value_type_string;
    
    for (i = 0; i < response_count; i++) {
        log_data[i + 1].value_type = dialogflow_log_data_value_type_string;
        log_data[i + 1].name = session->results[i]->slot.c_str();
        if (session->results[i]->slot == "output_audio") {
            log_data[i + 1].value = "audio data";
        } else {
            log_data[i + 1].value = session->results[i]->value.c_str();
        }
    }

    lock.unlock();
    df_log_call(session->user_data, "results", log_data_size, log_data);
}

static void make_streaming_responses(struct dialogflow_session *session)
{
    std::unique_lock<std::mutex> lock(session->lock);

    /* a standard final response has:
        response_id
        query_result.query_text (with score)
        query_result.language_code
        query_result.action
        query_result.fulfillment_text
        query_result.intent.name
        query_result.intent.display_name
        
        some number of:
        query_result.fulfillment_messages
    */
    if (session->final_response) {
        int score = int(session->final_response->query_result().intent_detection_confidence() * 100);
        session->results.clear();
        session->results.push_back(std::unique_ptr<df_result>(new df_result("response_id", session->final_response->response_id(), score)));
        
        if (session->audio_response) {
            make_audio_result<StreamingDetectIntentResponse>(session, *session->audio_response, score);
        }
        make_query_result_responses(session, session->final_response->query_result(), score);
        if (session->transcription_response) {
            float speech_score = session->transcription_response->recognition_result().confidence();
            session->results.push_back(std::unique_ptr<df_result>(new df_result("speech_score", std::to_string(speech_score), score)));
        }
        lock.unlock();
        log_responses(session, score);
    }
}

static void make_synchronous_responses(struct dialogflow_session *session, DetectIntentResponse& response)
{
    std::unique_lock<std::mutex> lock(session->lock);
    int score = int(response.query_result().intent_detection_confidence() * 100);
    session->results.clear();
    session->results.push_back(std::unique_ptr<df_result>(new df_result("response_id", response.response_id(), score)));
    
    make_audio_result<DetectIntentResponse>(session, response, score);
    make_query_result_responses(session, response.query_result(), score);
    lock.unlock();
    log_responses(session, score);
}

static bool is_session_connected(struct dialogflow_session *session)
{
    return (session->channel != nullptr);
}

static void ensure_connected(struct dialogflow_session *session)
{
    std::unique_lock<std::mutex> lock(session->lock);
    if (!is_session_connected(session)) {
        session->channel = create_grpc_channel(session->endpoint, session->auth_key);
        if (session->channel == nullptr) {
            df_log(LOG_ERROR, "Failed to create channel to %s for %s\n", session->endpoint.c_str(), session->session_id.c_str());
        } else {
            session->channel->GetState(true);
            session->session = std::move(Sessions::NewStub(session->channel));

            df_log(LOG_DEBUG, "Channel to %s created for %s\n", session->endpoint.c_str(), session->session_id.c_str());
            struct dialogflow_log_data create_data[] = { { "endpoint", session->endpoint.c_str() } };
            lock.unlock();
            df_log_call(session->user_data, "connect", 1, create_data);
        }
    } else {
        session->channel->GetState(true);
    }
}

void df_connect(struct dialogflow_session *session)
{
    ensure_connected(session);
}

int df_recognize_event(struct dialogflow_session *session, const char *event, const char *language, int request_audio)
{
    std::unique_lock<std::mutex> lock(session->lock);

    if (session->state != DF_STATE_READY) {
        lock.unlock();
        df_stop_recognition(session);
        lock.lock();
    }

    lock.unlock();
    ensure_connected(session);
    lock.lock();

    if (!is_session_connected(session)) {
        session->results.clear();
        session->results.push_back(std::unique_ptr<df_result>(new df_result("error", "Failed to connect", 100)));
        return -1;
    }

    if (cstrlen_zero(language)) {
        language = "en";
    }

    std::string session_path = format("projects/%s/agent/sessions/%s", session->project_id.c_str(), session->session_id.c_str());

    df_log(LOG_DEBUG, "Session %s performing event recognition on %s\n", session->session_id.c_str(), session_path.c_str());

    DetectIntentRequest request;
    DetectIntentResponse response;
    ClientContext context;

    request.set_session(session_path);
    if (request_audio) {
        request.mutable_output_audio_config()->set_audio_encoding(google::cloud::dialogflow::v2beta1::OutputAudioEncoding::OUTPUT_AUDIO_ENCODING_LINEAR_16);
        request.mutable_output_audio_config()->set_sample_rate_hertz(8000);
    }
    request.mutable_query_input()->mutable_event()->set_name(event);
    request.mutable_query_input()->mutable_event()->set_language_code(language);

    if (session->debug) {
        df_log(LOG_DEBUG, "REQUEST: %s\n", request.ShortDebugString().c_str());
    }

    struct dialogflow_log_data log_data[] = {
        { "event", event },
        { "language", language },
        { "session_path", session_path.c_str() }
    };
    lock.unlock();
    df_log_call(session->user_data, "detect_event", 3, log_data);
    lock.lock();

    session->session_start_time = tvnow();
    session->last_transcription_time = tvnow();
    Status status = session->session->DetectIntent(&context, request, &response);
    session->intent_detected_time = tvnow();
    if (!status.ok()) {
        df_log(LOG_WARNING, "Session %s got error performing event detection on %s: %s (%d: %s)\n", session->session_id.c_str(), session->project_id.c_str(),
            status.error_message().c_str(), status.error_code(), status.error_details().c_str());
        session->state = DF_STATE_READY;
        std::string error_code_string = std::to_string(status.error_code());
        struct dialogflow_log_data log_data[] = {
            { "message", status.error_message().c_str() },
            { "details", status.error_details().c_str() }, 
            { "error_code", error_code_string.c_str() }
        };
        lock.unlock();
        df_log_call(session->user_data, "error", 3, log_data);
        lock.lock();
        session->results.clear();
        session->results.push_back(std::unique_ptr<df_result>(new df_result("error", status.error_message(), 100)));
        session->results.push_back(std::unique_ptr<df_result>(new df_result("error_details", status.error_details(), 100)));
        session->results.push_back(std::unique_ptr<df_result>(new df_result("error_code", error_code_string, 100)));
        return -1;
    }

    if (session->debug) {
        df_log(LOG_DEBUG, "RESPONSE: %s\n", response.ShortDebugString().c_str());
    }

    lock.unlock();

    make_synchronous_responses(session, response);
    df_log_call(session->user_data, "stop", 0, NULL);

    lock.lock();
    session->responsesReceived = 1;
    session->state = DF_STATE_READY;

    return 0;
}

static void df_read_exec(struct dialogflow_session *session)
{
    StreamingDetectIntentResponse response;
    bool debug;
    void *user_data;

    std::unique_lock<std::mutex> lock(session->lock);
    std::string sessionId(session->session_id);
    std::shared_ptr<ClientReaderWriterInterface<StreamingDetectIntentRequest, StreamingDetectIntentResponse>> 
        current_request(session->current_request);
    debug = session->debug;
    user_data = session->user_data;
    lock.unlock();
    while (current_request->Read(&response)) {
        lock.lock();
        session->responsesReceived++;
        debug = session->debug;
        lock.unlock();
        if (debug) {
            df_log(LOG_DEBUG, "RESPONSE: %s\n", response.ShortDebugString().c_str());
        }
        if (response.has_query_result()) {
            // this is the final response
            df_log(LOG_DEBUG, "Got final response '%s' (\"%s\" / \"%s\") for %s\n", 
                response.query_result().intent().display_name().c_str(),
                response.query_result().query_text().c_str(),
                response.query_result().fulfillment_text().c_str(),
                sessionId.c_str());
            grpc::string audio = response.output_audio();
            if (audio.length() > 0) {
                df_log(LOG_DEBUG, "Final response has audio\n");
            }
            if (response.has_output_audio_config()) {
                df_log(LOG_DEBUG, "Final response has audio config\n");
            }
            lock.lock();
            session->intent_detected_time = tvnow();
            session->final_response = std::make_shared<StreamingDetectIntentResponse>(response);
            lock.unlock();
            struct dialogflow_log_data log_data[] = {
                { "intent", response.query_result().intent().display_name().c_str() },
                { "action", response.query_result().action().c_str() },
                { "fulfillment_text", response.query_result().fulfillment_text().c_str() },
            };
            df_log_call(user_data, "query_result_received", 3, log_data);
        } else if (response.has_recognition_result()) {
            if (response.recognition_result().message_type() == 
                google::cloud::dialogflow::v2beta1::StreamingRecognitionResult_MessageType::
                StreamingRecognitionResult_MessageType_END_OF_SINGLE_UTTERANCE) {
                df_log(LOG_DEBUG, "Got end of single utterance event for %s\n",
                    sessionId.c_str());
                df_log_call(user_data, "end_of_utterance", 0, NULL);
            } else {
                df_log(LOG_DEBUG, "Got interim response '%s' for %s\n",
                    response.recognition_result().transcript().c_str(),
                    sessionId.c_str());
                if (response.output_audio().length() > 0) {
                    df_log(LOG_DEBUG, "Interim response has audio\n");
                }
                if (response.has_output_audio_config()) {
                    df_log(LOG_DEBUG, "Interim response has audio config\n");
                }
                if (response.recognition_result().is_final()) {
                    std::string score = std::to_string(response.recognition_result().confidence());
                    struct dialogflow_log_data log_data[] = { 
                        { "text", response.recognition_result().transcript().c_str() },
                        { "score", score.c_str() }
                    };
                    df_log_call(user_data, "final_transcription", 2, log_data);
                    lock.lock();
                    session->last_transcription_time = tvnow();
                    session->transcription_response = std::make_shared<StreamingDetectIntentResponse>(response);
                    lock.unlock();
                } else {
                    struct dialogflow_log_data log_data[] = { { "text", response.recognition_result().transcript().c_str() }};
                    df_log_call(user_data, "transcription", 1, log_data);
                    lock.lock();
                    session->last_transcription_time = tvnow();
                    lock.unlock();
                }
            }
        } else if (response.output_audio().length() == 0) { /* don't complain if it's got an audio bit */
            df_log(LOG_DEBUG, "Got unexpected response packet for %s\n", sessionId.c_str());
        }
        if (response.output_audio().length() > 0) { /* but have this outside the if/else clause in case it comes on another packet */
            df_log(LOG_DEBUG, "Got response with audio for %s\n", sessionId.c_str());
            lock.lock();
            session->audio_response = std::make_shared<StreamingDetectIntentResponse>(response);
            lock.unlock();
            df_log_call(user_data, "audio_data", 0, NULL);
        } 
    }
    
    make_streaming_responses(session);
    lock.lock();
    if (session->state != DF_STATE_ERROR) {
        session->state = DF_STATE_FINISHED;
    }
    
    return;
}

int df_start_recognition(struct dialogflow_session *session, const char *language, int request_audio,
    const char **hints, size_t hints_count)
{
    std::unique_lock<std::mutex> lock(session->lock);

    if (session->state != DF_STATE_READY) {
        lock.unlock();
        df_stop_recognition(session);
        lock.lock();
    }

    lock.unlock();
    ensure_connected(session);
    lock.lock();

    if (!is_session_connected(session)) {
        session->results.clear();
        session->results.push_back(std::unique_ptr<df_result>(new df_result("error", "Failed to connect", 100)));
        return -1;
    }

    std::string session_path = format("projects/%s/agent/sessions/%s", session->project_id.c_str(), session->session_id.c_str());

    df_log(LOG_DEBUG, "Session %s starting recognition to %s\n", session->session_id.c_str(), session_path.c_str());

    struct dialogflow_log_data log_data[] = {
        { "language", cstr_or(language, "en") },
        { "session_path", session_path.c_str() },
        { "hints", hints, dialogflow_log_data_value_type_array_of_string, hints_count },
        { "request_sentiment_analysis", session->request_sentiment_analysis ? "true" : "false" },
        { "request_audio", request_audio ? "true" : "false" },
        { "single_utterance", session->use_external_endpointer == false ? "true" : "false" },
        { "model", session->model.c_str() }
    };
    lock.unlock();
    df_log_call(session->user_data, "start", ARRAY_LEN(log_data), log_data);
    lock.lock();

    session->session_start_time = tvnow();
    /* it didn't like assigning this to the session structure location */
    session->context.reset(new ClientContext());
    session->current_request = std::move(session->session->StreamingDetectIntent(session->context.get()));

    StreamingDetectIntentRequest request;
    request.set_session(session_path);
    request.set_single_utterance(session->use_external_endpointer == false);
    request.mutable_query_input()->mutable_audio_config()->set_audio_encoding(google::cloud::dialogflow::v2beta1::AUDIO_ENCODING_MULAW);
    request.mutable_query_input()->mutable_audio_config()->set_sample_rate_hertz(8000);
    request.mutable_query_input()->mutable_audio_config()->set_language_code(cstr_or(language, "en-US"));
    if (!session->model.empty()) {
        request.mutable_query_input()->mutable_audio_config()->set_model(session->model);
    }
    for (size_t i = 0; i < hints_count; i++) {
        request.mutable_query_input()->mutable_audio_config()->add_phrase_hints(hints[i]);
    }
    if (request_audio) {
        request.mutable_output_audio_config()->set_audio_encoding(google::cloud::dialogflow::v2beta1::OutputAudioEncoding::OUTPUT_AUDIO_ENCODING_LINEAR_16);
        request.mutable_output_audio_config()->set_sample_rate_hertz(8000);
    }
    if (session->request_sentiment_analysis) {
        request.mutable_query_params()->mutable_sentiment_analysis_request_config()->set_analyze_query_text_sentiment(1);
        request.mutable_query_params()->mutable_sentiment_analysis_request_config()->set_analyze_conversation_text_sentiment(1);
    }

    if (!session->current_request->Write(request)) {
        df_log(LOG_WARNING, "Session %s got error writing initial data packet to %s\n", session->session_id.c_str(), session->project_id.c_str());
        session->state = DF_STATE_ERROR;
        lock.unlock();
        df_log_call(session->user_data, "write_error", 0, NULL); 
        return -1;
    }
    if (session->debug) {
        df_log(LOG_DEBUG, "REQUEST: %s\n", request.ShortDebugString().c_str());
    }

    session->state = DF_STATE_STARTED;
    session->bytesWritten = 0;
    session->packetsWritten = 1;
    session->responsesReceived = 0;

    session->read_thread = std::thread(df_read_exec, session);

    return 0;
}

int df_stop_recognition(struct dialogflow_session *session)
{
    std::unique_lock<std::mutex> lock(session->lock);
    df_log(LOG_DEBUG, "Session %s stopping recognition to %s\n", session->session_id.c_str(), session->project_id.c_str());

    if (session->state != DF_STATE_READY) {
        lock.unlock();
        df_log_call(session->user_data, "stopping", 0, NULL);
        lock.lock();

        session->current_request->WritesDone();

        if (session->read_thread.joinable()) {
            lock.unlock();
            session->read_thread.join();
            lock.lock();
        }

        if (session->state == DF_STATE_READY) {
            /* while unlocked someone beat us to it */
            return 0;
        }

        Status status = session->current_request->Finish();
        if (!status.ok()) {
            df_log(LOG_WARNING, "Session %s got error performing streaming detection on %s: %s (%d: %s)\n", session->session_id.c_str(), session->project_id.c_str(),
                status.error_message().c_str(), status.error_code(), status.error_details().c_str());
            std::string error_code_string = std::to_string(status.error_code());
            struct dialogflow_log_data log_data[] = {
                { "message", status.error_message().c_str() },
                { "details", status.error_details().c_str() }, 
                { "error_code", error_code_string.c_str() }
            };
            lock.unlock();
            df_log_call(session->user_data, "error", 3, log_data);
            lock.lock();
            session->results.clear();
            session->results.push_back(std::unique_ptr<df_result>(new df_result("error", status.error_message(), 100)));
            session->results.push_back(std::unique_ptr<df_result>(new df_result("error_details", status.error_details(), 100)));
            session->results.push_back(std::unique_ptr<df_result>(new df_result("error_code", error_code_string, 100)));
        }
        lock.unlock();
        df_log_call(session->user_data, "stop", 0, NULL);
        lock.lock();
        session->state = DF_STATE_READY;
    }
    return 0;
}

enum dialogflow_session_state df_write_audio(struct dialogflow_session *session, const char *samples, size_t sample_count)
{
    std::unique_lock<std::mutex> lock(session->lock);
    enum dialogflow_session_state state;

    state = session->state;
    lock.unlock();

    if (state != DF_STATE_STARTED) {
        return state;
    }

    StreamingDetectIntentRequest request;
    request.set_input_audio(samples, sample_count);

#ifdef DF_LOG_WRITES
    static int logcount = 0;
    if (++logcount % 50 == 1) {
        df_log(LOG_DEBUG, "Session %s writing %d bytes audio...\n", session->session_id.c_str(),
            (int) sample_count);
    }
#endif

    lock.lock();
    if (!session->current_request->Write(request)) {
        df_log(LOG_WARNING, "Session %s got error writing audio data packet to %s\n", session->session_id.c_str(), session->project_id.c_str());
        state = session->state = DF_STATE_ERROR;
        lock.unlock();
        df_log_call(session->user_data, "write_error", 0, NULL); 
    }
    if (session->debug) {
        lock.unlock();
        df_log(LOG_DEBUG, "REQUEST: %s\n", request.ShortDebugString().c_str());
    }

    return state;
}

enum dialogflow_session_state df_get_state(struct dialogflow_session *session)
{
    std::lock_guard<std::mutex> lock(session->lock);

    return session->state;
}

int df_get_rpc_state(struct dialogflow_session *session)
{
    std::lock_guard<std::mutex> lock(session->lock);

    return int(session->channel->GetState(false));
}

int df_get_result_count(struct dialogflow_session *session)
{
    std::lock_guard<std::mutex> lock(session->lock);

    return session->results.size();
}

struct dialogflow_result *df_get_result(struct dialogflow_session *session, int number)
{
    std::lock_guard<std::mutex> lock(session->lock);
    struct dialogflow_result *result = nullptr;

    if (number >= 0 && number < int(session->results.size())) {
        result = &(session->results[number]->result);
    }

    return result;
}

int df_get_response_count(struct dialogflow_session *session)
{
    std::lock_guard<std::mutex> lock(session->lock);
    return session->responsesReceived;
}

void df_set_debug(struct dialogflow_session *session, int debug)
{
    std::lock_guard<std::mutex> lock(session->lock);
    session->debug = (debug != 0);
}

struct timeval df_get_session_start_time(struct dialogflow_session *session)
{
    std::lock_guard<std::mutex> lock(session->lock);
    return session->session_start_time;
}

struct timeval df_get_session_last_transcription_time(struct dialogflow_session *session)
{
    std::lock_guard<std::mutex> lock(session->lock);
    return session->last_transcription_time;
}

struct timeval df_get_session_intent_detected_time(struct dialogflow_session *session)
{
    std::lock_guard<std::mutex> lock(session->lock);
    return session->intent_detected_time;
}



int google_synth_speech(const char *endpoint, const char *svc_key, const char *text, const char *language, const char *voice_name, const char *destination_filename)
{
    if (cstrlen_zero(endpoint)) {
        endpoint = "texttospeech.googleapis.com";
    }

    std::shared_ptr<Channel> channel = create_grpc_channel(endpoint, svc_key);
    
    if (channel == nullptr) {
        df_log(LOG_ERROR, "Failed to create synthesis channel to %s\n", endpoint);
        return -1;
    }

    std::unique_ptr<TextToSpeech::Stub> tts = TextToSpeech::NewStub(channel);

    std::string strText(text);
    
    SynthesizeSpeechRequest request;
    SynthesizeSpeechResponse response;
    ClientContext context;

    if (strText.find("<speak") != std::string::npos) {
        request.mutable_input()->set_text(strText);
    } else {
        request.mutable_input()->set_ssml(strText);
    }
    request.mutable_voice()->set_language_code(cstr_or(language, "en"));
    if (!cstrlen_zero(voice_name)) {
        request.mutable_voice()->set_name(voice_name);
    }
    request.mutable_audio_config()->set_audio_encoding(google::cloud::texttospeech::v1beta1::AudioEncoding::LINEAR16);
    request.mutable_audio_config()->set_sample_rate_hertz(8000);

    Status status = tts->SynthesizeSpeech(&context, request, &response);
    if (!status.ok()) {
        df_log(LOG_WARNING, "Speech synthesis failed: %s (%d)\n", status.error_message().c_str(), status.error_code());
        df_log(LOG_DEBUG, "Error details: %s\n", status.error_details().c_str());
        return -1;
    }

    const std::string& audio = response.audio_content();

    if (audio.length() == 0) {
        df_log(LOG_WARNING, "Got 0 bytes of audio back from synthesis call\n");
    }

    std::ofstream wavefile(destination_filename, std::ofstream::binary);
    wavefile.write(audio.c_str(), sizeof(char) * audio.length());
    /* will auto-close */

    return 0;
}
