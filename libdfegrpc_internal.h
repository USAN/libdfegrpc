#include <vector>
#include <string>
#include <cstdarg>
#include <thread>
#include <mutex>

#include "libdfegrpc.h"

#include <grpcpp/channel.h>
#include <google/cloud/dialogflow/v2beta1/session.grpc.pb.h>
#include <google/cloud/texttospeech/v1beta1/cloud_tts.grpc.pb.h>

class df_result
{
    public:
    struct dialogflow_result result;
    const std::string slot;
    const std::string value;
    const int score;

    df_result(const std::string& slot, const std::string& value, int score) : slot(slot), value(value), score(score)
    {
        this->result.slot = this->slot.c_str();
        this->result.valueLen = this->value.length();
        this->result.value = this->value.c_str();
        this->result.score = score;
    } 
    df_result(const std::string& slot, const char *buffer, size_t bufferLen, int score) : slot(slot), value(buffer, bufferLen), score(score)
    {
        this->result.slot = this->slot.c_str();
        this->result.valueLen = bufferLen;
        this->result.value = this->value.c_str();
        this->result.score = score;
    } 
};

struct dialogflow_session {
    std::mutex lock;
    std::string auth_key;
    std::string endpoint;
    std::string session_id;
    std::string project_id;
    enum dialogflow_session_state state;
	std::shared_ptr<grpc::Channel> channel;
	std::shared_ptr<google::cloud::dialogflow::v2beta1::Sessions::Sessions::StubInterface> session;
    std::shared_ptr<grpc::ClientReaderWriterInterface<google::cloud::dialogflow::v2beta1::StreamingDetectIntentRequest, google::cloud::dialogflow::v2beta1::StreamingDetectIntentResponse>> current_request;
    std::unique_ptr<grpc::ClientContext> context;
    std::shared_ptr<google::cloud::dialogflow::v2beta1::StreamingDetectIntentResponse> transcription_response;
    std::shared_ptr<google::cloud::dialogflow::v2beta1::StreamingDetectIntentResponse> final_response;
    std::shared_ptr<google::cloud::dialogflow::v2beta1::StreamingDetectIntentResponse> audio_response;
    std::vector<std::unique_ptr<df_result>> results;
    std::thread read_thread;
    size_t bytesWritten;
    size_t packetsWritten;
    int responsesReceived;
    bool request_sentiment_analysis;
    bool debug;
    void *user_data;
};
